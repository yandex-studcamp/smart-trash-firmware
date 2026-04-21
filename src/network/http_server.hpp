#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace smart_bin {

esp_err_t start_http_server();
esp_err_t http_server_set_latest_photo(const uint8_t *jpeg, size_t len, uint32_t seq, uint32_t capture_ms);

} // namespace smart_bin
