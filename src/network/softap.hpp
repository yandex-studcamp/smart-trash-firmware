#pragma once

#include "esp_err.h"
#include "esp_netif.h"

namespace smart_bin {

esp_err_t start_softap(esp_netif_t **out_ap_netif);

} // namespace smart_bin
