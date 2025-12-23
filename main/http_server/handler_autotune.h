#pragma once

#include "esp_http_server.h"

esp_err_t POST_autotune_start(httpd_req_t *req);
esp_err_t POST_autotune_stop(httpd_req_t *req);
esp_err_t GET_autotune_status(httpd_req_t *req);
