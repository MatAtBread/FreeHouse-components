#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_scan_http_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif
