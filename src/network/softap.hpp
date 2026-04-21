#pragma once

#include "esp_err.h"

struct esp_netif_obj;
typedef struct esp_netif_obj esp_netif_t;

namespace smart_bin {

esp_err_t start_softap(esp_netif_t **out_ap_netif);

} // namespace smart_bin
