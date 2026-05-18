/* Captive Portal Example

    This example code is in the Public Domain (or CC0 licensed, at your option.)

    Unless required by applicable law or agreed to in writing, this
    software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
    CONDITIONS OF ANY KIND, either express or implied.
*/

#include <sys/param.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "esp_wifi.h"



extern "C" esp_err_t dev_wifi_init(wifi_init_config_t *config);
#include "esp_netif.h"
#include "lwip/inet.h"

#include "esp_http_server.h"
#include "dns_server.h"
#include "ota.h"
#include "wifi_scan.h"

#include "wifi-captiveportal.h"

#define EXAMPLE_MAX_STA_CONN 4

extern "C" const char *TAG;

static char portal_redirect_url[40] = "http://192.168.4.1/";

const char* get_portal_redirect_url(void) {
    return portal_redirect_url;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

static void wifi_init_softap(const char *ssid)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(dev_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {};
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.max_connection = EXAMPLE_MAX_STA_CONN;

    memcpy(wifi_config.ap.ssid, ssid, sizeof (wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:'%s'", ssid);
}

// Advertises the captive portal via DHCP Option 114 (RFC 8910).
// Android 11+ and Samsung One UI read this before HTTP probing, so this is
// the most reliable trigger on modern devices. Also populates portal_redirect_url
// for use in HTTP redirect responses.
static void dhcp_set_captiveportal_url(void) {
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));

    // Store root URL used by all HTTP redirect handlers
    snprintf(portal_redirect_url, sizeof(portal_redirect_url), "http://%s/", ip_addr);

    // DHCP Option 114 points to the RFC 8908 JSON API endpoint (not the root page)
    char api_uri[48];
    snprintf(api_uri, sizeof(api_uri), "http://%s/api/captive", ip_addr);

    ESP_LOGI(TAG, "Captive portal: redirect=%s api=%s", portal_redirect_url, api_uri);

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           api_uri, strlen(api_uri)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
}

// HTTP Error (404) Handler - Redirects all requests to the anyGet page
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    // Absolute URL required — Android/Samsung reject private-IP portals from relative redirects
    httpd_resp_set_hdr(req, "Location", portal_redirect_url);
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Redirecting to /");
    return ESP_OK;
}

static HttpGetHandler *handler;
static httpd_handle_t server = NULL;
static dns_server_handle_t dns_handle = NULL;
static bool captive_mode = false;

bool is_captive_portal_active(void) {
    return captive_mode;
}

static esp_err_t getHandler(httpd_req_t *req) {
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    ESP_LOGI(TAG, "HTTP %s (host:%s)", req->uri, host);

    if (!captive_mode) {
        return handler->getHandler(req);
    }

    // RFC 8908 Captive Portal API — fetched by Android 11+ / Samsung One UI after reading DHCP Option 114
    if (strncmp(req->uri, "/api/captive", 12) == 0) {
        char json[96];
        snprintf(json, sizeof(json),
                 "{\"captive\":true,\"user-portal-url\":\"%s\"}", portal_redirect_url);
        httpd_resp_set_type(req, "application/captive+json");
        httpd_resp_set_hdr(req, "Cache-Control", "private");
        httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // OS captive-portal probe URLs — explicit 302 to absolute portal URL
    if (strncmp(req->uri, "/generate_204",    13) == 0 ||
        strncmp(req->uri, "/hotspot-detect",  15) == 0 ||
        strncmp(req->uri, "/ncsi.txt",         9) == 0 ||
        strncmp(req->uri, "/connecttest.txt", 16) == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", portal_redirect_url);
        httpd_resp_send(req, "", 0);
        return ESP_OK;
    }

    // Any request whose Host header doesn't match our AP IP arrived via DNS spoofing
    // (e.g. Samsung's http://www.samsung.com/ probe, or any other vendor-specific URL
    // whose path happens to be "/"). Return a 302 so the OS recognises it as a captive
    // portal redirect rather than treating the served page as a successful internet fetch.
    if (host[0]) {
        const char *ap_ip = portal_redirect_url + 7;  // skip "http://"
        size_t ip_len = strlen(ap_ip) - 1;             // exclude trailing "/"
        if (strncmp(host, ap_ip, ip_len) != 0) {
            ESP_LOGI(TAG, "Redirect spoofed host '%s' -> portal", host);
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", portal_redirect_url);
            httpd_resp_send(req, "", 0);
            return ESP_OK;
        }
    }

    return handler->getHandler(req);
}

void start_web_server(HttpGetHandler *_handler) {
  if (handler) {
    ESP_LOGI(TAG, "Web server already started");
    return;
  }
  handler = _handler;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_open_sockets = 13;
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;

  // Start the httpd server
  ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
  if (httpd_start(&server, &config) == ESP_OK) {
    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    // handle = handler;
    static const httpd_uri_t ota_uri = {
      .uri       = "/ota",
      .method    = HTTP_POST,
      .handler   = ota_post_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &ota_uri);
    static const httpd_uri_t scan_uri = {
      .uri       = "/scan",
      .method    = HTTP_GET,
      .handler   = wifi_scan_http_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &scan_uri);
    static const httpd_uri_t anyGet = {
      .uri = "*",
      .method = HTTP_GET,
      .handler = getHandler,
      .user_ctx = NULL
    };

    httpd_register_uri_handler(server, &anyGet);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
  }
}

void start_captive_portal(HttpGetHandler *_handler, const char *ssid) {
  /*
      Turn of warnings from HTTP server as redirecting traffic will yield
      lots of invalid requests
  */
  esp_log_level_set("httpd_uri", ESP_LOG_WARN);
  esp_log_level_set("httpd_txrx", ESP_LOG_WARN);
  esp_log_level_set("httpd_parse", ESP_LOG_WARN);

  // Initialize Wi-Fi including netif with default config
  esp_netif_create_default_wifi_ap();

  // Initialise ESP32 in SoftAP mode
  wifi_init_softap(ssid);

  captive_mode = true;

  // DHCP Option 114 (RFC 8910) — always enabled; also populates portal_redirect_url
  dhcp_set_captiveportal_url();

  // Start the http server
  start_web_server(_handler);

  // Start the DNS server that will redirect all queries to the softAP IP
  dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
  dns_handle = start_dns_server(&dns_config);
}

void stop_web_server(void) {
    if (server) {
        ESP_LOGI(TAG, "Stopping web server");
        httpd_stop(server);
        server = NULL;
        handler = NULL;
    } else {
        ESP_LOGI(TAG, "Web server not running");
    }
}

void stop_captive_portal(void) {
    ESP_LOGI(TAG, "Stopping captive portal");
    captive_mode = false;

    // Stop DNS server
    stop_dns_server(dns_handle);

    // Stop HTTP server
    stop_web_server();

    // Deinit Wi-Fi SoftAP
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    // Destroy default Wi-Fi AP netif
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_destroy(ap_netif);
        ESP_LOGI(TAG, "Destroyed Wi-Fi AP netif");
    }

    ESP_LOGI(TAG, "Captive portal stopped and resources released");
}