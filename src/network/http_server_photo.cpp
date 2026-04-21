#include "http_server.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "http";
httpd_handle_t g_http_server = nullptr;

struct LatestPhotoState {
    uint8_t *data = nullptr;
    size_t len = 0;
    uint32_t seq = 0;
    uint32_t capture_ms = 0;
    SemaphoreHandle_t mutex = nullptr;
};

LatestPhotoState g_latest_photo = {};

esp_err_t health_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");

    if (g_latest_photo.mutex == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "not_ready");
    }

    if (xSemaphoreTake(g_latest_photo.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "busy");
    }

    const uint32_t seq = g_latest_photo.seq;
    const uint32_t capture_ms = g_latest_photo.capture_ms;
    xSemaphoreGive(g_latest_photo.mutex);

    if (seq == 0) {
        httpd_resp_set_status(req, "200 OK");
        return httpd_resp_sendstr(req, "waiting_photo");
    }

    char status[64] = {};
    std::snprintf(status, sizeof(status), "ok seq=%" PRIu32 " capture_ms=%" PRIu32, seq, capture_ms);
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_sendstr(req, status);
}

esp_err_t photo_handler(httpd_req_t *req)
{
    if (g_latest_photo.mutex == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "not_ready");
    }

    if (xSemaphoreTake(g_latest_photo.mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "busy");
    }

    if (g_latest_photo.data == nullptr || g_latest_photo.len == 0) {
        xSemaphoreGive(g_latest_photo.mutex);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "no_photo");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");

    char seq_header[16] = {};
    std::snprintf(seq_header, sizeof(seq_header), "%" PRIu32, g_latest_photo.seq);
    httpd_resp_set_hdr(req, "X-Photo-Seq", seq_header);

    const esp_err_t send_ret =
        httpd_resp_send(req, reinterpret_cast<const char *>(g_latest_photo.data), g_latest_photo.len);
    xSemaphoreGive(g_latest_photo.mutex);
    return send_ret;
}

} // namespace

namespace smart_bin {

esp_err_t http_server_set_latest_photo(const uint8_t *jpeg, size_t len, uint32_t seq, uint32_t capture_ms)
{
    if (jpeg == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_latest_photo.mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        copy = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_8BIT));
    }
    if (copy == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    std::memcpy(copy, jpeg, len);

    if (xSemaphoreTake(g_latest_photo.mutex, portMAX_DELAY) != pdTRUE) {
        heap_caps_free(copy);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t *old = g_latest_photo.data;
    g_latest_photo.data = copy;
    g_latest_photo.len = len;
    g_latest_photo.seq = seq;
    g_latest_photo.capture_ms = capture_ms;

    xSemaphoreGive(g_latest_photo.mutex);

    if (old != nullptr) {
        heap_caps_free(old);
    }

    return ESP_OK;
}

esp_err_t start_http_server()
{
    if (g_http_server != nullptr) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_SMART_HTTP_SERVER_PORT;
    config.max_uri_handlers = 6;

    if (g_latest_photo.mutex == nullptr) {
        g_latest_photo.mutex = xSemaphoreCreateMutex();
        if (g_latest_photo.mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(httpd_start(&g_http_server, &config), kTag, "httpd_start failed");

    static httpd_uri_t health_uri = {};
    health_uri.uri = "/health";
    health_uri.method = HTTP_GET;
    health_uri.handler = health_handler;
    health_uri.user_ctx = nullptr;

    static httpd_uri_t photo_uri = {};
    photo_uri.uri = "/photo.jpg";
    photo_uri.method = HTTP_GET;
    photo_uri.handler = photo_handler;
    photo_uri.user_ctx = nullptr;

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_http_server, &health_uri), kTag, "Failed to register /health");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_http_server, &photo_uri), kTag, "Failed to register /photo.jpg");

    ESP_LOGI(kTag,
             "HTTP server started on port %d. Endpoints: GET /health, GET /photo.jpg",
             CONFIG_SMART_HTTP_SERVER_PORT);
    return ESP_OK;
}

} // namespace smart_bin
