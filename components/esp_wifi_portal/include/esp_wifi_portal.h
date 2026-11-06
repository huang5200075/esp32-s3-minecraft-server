#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {


#endif

esp_err_t esp_wifi_portal_init(void);

esp_err_t esp_wifi_portal_deinit(void);

esp_err_t esp_wifi_portal_start(void);

esp_err_t esp_wifi_portal_stop(void);

void esp_wifi_portal_set_auto_start(bool auto_start);

#ifdef __cplusplus
}
#endif
