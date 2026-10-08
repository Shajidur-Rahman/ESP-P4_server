#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ethernet_start(void);
bool ethernet_has_ip(void);
const char *ethernet_get_ip_str(void);
const char *ethernet_get_mac_str(void);

#ifdef __cplusplus
}
#endif
