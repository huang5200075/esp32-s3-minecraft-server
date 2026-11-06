#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define WIFI_CONNECTED_BIT BIT0

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t start_webserver(EventGroupHandle_t event_group);

esp_err_t stop_webserver(void);

#ifdef __cplusplus
}
#endif
