#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t http_server_start(void);
void http_server_stop(void);

#ifdef __cplusplus
}
#endif
