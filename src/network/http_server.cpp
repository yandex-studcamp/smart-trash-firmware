#include "http_server.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "inference/inference.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "http";
httpd_handle_t g_http_server = nullptr;

bool header_is_supported_content_type(httpd_req_t *req)
{
    char content_type[64] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        return false;
    }

    const char *semicolon = std::strchr(content_type, ';');
    if (semicolon != nullptr) {
        content_type[semicolon - content_type] = '\0';
    }

    const bool is_jpeg = std::strcmp(content_type, "image/jpeg") == 0;
    const bool is_binary = std::strcmp(content_type, "application/octet-stream") == 0;
    return is_jpeg || is_binary;
}

esp_err_t health_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    if (inference_is_ready()) {
        httpd_resp_set_status(req, "200 OK");
        return httpd_resp_sendstr(req, "ok");
    }
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_sendstr(req, "not_ready");
}

esp_err_t infer_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");

    if (!inference_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "not_ready");
    }

    if (!header_is_supported_content_type(req)) {
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        return httpd_resp_sendstr(req, "unsupported_content_type");
    }

    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "empty_body");
    }

    if (req->content_len > CONFIG_SMART_HTTP_MAX_UPLOAD_BYTES) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "payload_too_large");
    }

    uint8_t *buffer =
        static_cast<uint8_t *>(heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "alloc_failed");
    }

    int remaining = req->content_len;
    int received_total = 0;
    while (remaining > 0) {
        const int ret = httpd_req_recv(req, reinterpret_cast<char *>(buffer + received_total), remaining);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            heap_caps_free(buffer);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "read_failed");
        }
        remaining -= ret;
        received_total += ret;
    }

    inference_result_t result = {};
    const esp_err_t infer_ret = inference_run_jpeg(buffer, static_cast<size_t>(received_total), &result);
    heap_caps_free(buffer);

    if (infer_ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "not_ready");
    }
    if (infer_ret == ESP_ERR_INVALID_ARG || infer_ret == ESP_ERR_INVALID_RESPONSE) {
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        return httpd_resp_sendstr(req, "invalid_jpeg");
    }
    if (infer_ret != ESP_OK) {
        ESP_LOGE(kTag, "Inference failed: 0x%x", infer_ret);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "inference_failed");
    }

    char response[64] = {};
    std::snprintf(response, sizeof(response), "%s %.3f", result.predicted_label, result.confidence);
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_sendstr(req, response);
}

} // namespace

namespace smart_bin {

esp_err_t start_http_server()
{
    if (g_http_server != nullptr) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_SMART_HTTP_SERVER_PORT;
    config.max_uri_handlers = 8;

    ESP_RETURN_ON_ERROR(httpd_start(&g_http_server, &config), kTag, "httpd_start failed");

    static httpd_uri_t health_uri = {};
    health_uri.uri = "/health";
    health_uri.method = HTTP_GET;
    health_uri.handler = health_handler;
    health_uri.user_ctx = nullptr;

    static httpd_uri_t infer_uri = {};
    infer_uri.uri = "/infer";
    infer_uri.method = HTTP_POST;
    infer_uri.handler = infer_handler;
    infer_uri.user_ctx = nullptr;

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_http_server, &health_uri), kTag, "Failed to register /health");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_http_server, &infer_uri), kTag, "Failed to register /infer");

    ESP_LOGI(kTag,
             "HTTP server started on port %d. Endpoints: GET /health, POST /infer",
             CONFIG_SMART_HTTP_SERVER_PORT);
    return ESP_OK;
}

} // namespace smart_bin
