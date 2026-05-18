#include "wifi_scan.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern const char *TAG;

#define SCAN_MAX_APS 20

static int json_escape_ssid(char *dst, size_t dst_size, const uint8_t *src) {
    size_t out = 0;
    for (int i = 0; i < 32 && src[i] != 0 && out + 2 < dst_size; i++) {
        uint8_t c = src[i];
        if (c == '"' || c == '\\') {
            dst[out++] = '\\';
            dst[out++] = c;
        } else if (c >= 0x20) {
            dst[out++] = c;
        }
    }
    dst[out] = '\0';
    return (int)out;
}

esp_err_t wifi_scan_http_handler(httpd_req_t *req) {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    // Scanning needs a STA interface; temporarily promote AP-only to APSTA.
    // The AP stays up during the channel hops — clients may stall briefly.
    bool promoted = (mode == WIFI_MODE_AP);
    if (promoted) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = { .min = 100, .max = 300 }
        }
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);

    if (promoted) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    uint16_t ap_count = SCAN_MAX_APS;
    wifi_ap_record_t *aps = calloc(SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    if (!aps) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&ap_count, aps);

    // 96 bytes per entry is generous: prefix comma + {"ssid":"<64 escaped>","rssi":-100}
    const size_t json_size = SCAN_MAX_APS * 96 + 4;
    char *json = malloc(json_size);
    if (!json) {
        free(aps);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    int pos = 0;
    json[pos++] = '[';
    bool first = true;
    for (uint16_t i = 0; i < ap_count; i++) {
        if (aps[i].ssid[0] == 0) continue;
        // Scan results are RSSI-sorted; first occurrence of an SSID is strongest
        bool dup = false;
        for (uint16_t j = 0; j < i; j++) {
            if (strcmp((char *)aps[i].ssid, (char *)aps[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        char escaped[72];
        json_escape_ssid(escaped, sizeof(escaped), aps[i].ssid);
        int n = snprintf(json + pos, json_size - pos - 2,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                         first ? "" : ",", escaped, (int)aps[i].rssi);
        if (n > 0) pos += n;
        first = false;
    }
    json[pos++] = ']';
    json[pos] = '\0';

    free(aps);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    return ESP_OK;
}
