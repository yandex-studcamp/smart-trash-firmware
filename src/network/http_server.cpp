#include "http_server.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "board/board_config.hpp"
#include "board/servo_control.hpp"
#include "inference/inference.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "http";
httpd_handle_t g_http_server = nullptr;
uint32_t g_servo_test_cycle_counter = 0;

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

#if CONFIG_SMART_HTTP_DEBUG_ENABLE_SERVO_ENDPOINTS
esp_err_t send_servo_http_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, message);
}

esp_err_t parse_query_int(httpd_req_t *req, const char *key, int min_value, int max_value, int *out_value)
{
    if (out_value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char query[96] = {};
    if (query_len >= sizeof(query)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return ESP_FAIL;
    }

    char value[16] = {};
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    char *end_ptr = nullptr;
    const long parsed = std::strtol(value, &end_ptr, 10);
    if (end_ptr == value || *end_ptr != '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (parsed < min_value || parsed > max_value) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_value = static_cast<int>(parsed);
    return ESP_OK;
}

esp_err_t ensure_servo_ready()
{
    if (smart_bin::servo_is_ready()) {
        return ESP_OK;
    }
    return smart_bin::servo_init_all();
}

esp_err_t servo_home_handler(httpd_req_t *req)
{
    const esp_err_t init_ret = ensure_servo_ready();
    if (init_ret != ESP_OK) {
        return send_servo_http_error(req, "500 Internal Server Error", "servo_init_failed");
    }

    const esp_err_t home_ret = smart_bin::servo_set_safe();
    if (home_ret != ESP_OK) {
        return send_servo_http_error(req, "500 Internal Server Error", "servo_home_failed");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t servo_test_handler(httpd_req_t *req)
{
    const esp_err_t init_ret = ensure_servo_ready();
    if (init_ret != ESP_OK) {
        return send_servo_http_error(req, "500 Internal Server Error", "servo_init_failed");
    }

    ++g_servo_test_cycle_counter;
    const esp_err_t test_ret = smart_bin::servo_run_test_cycle(g_servo_test_cycle_counter);
    if (test_ret != ESP_OK) {
        return send_servo_http_error(req, "500 Internal Server Error", "servo_test_failed");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t servo_set_handler(httpd_req_t *req)
{
    int servo_id_raw = 0;
    int angle = 0;
    if (parse_query_int(req, "id", 1, 2, &servo_id_raw) != ESP_OK) {
        return send_servo_http_error(req, "400 Bad Request", "invalid_id");
    }
    if (parse_query_int(req, "angle", 0, static_cast<int>(smart_bin::board::kServoMaxAngleDeg), &angle) != ESP_OK) {
        return send_servo_http_error(req, "400 Bad Request", "invalid_angle");
    }

    const esp_err_t init_ret = ensure_servo_ready();
    if (init_ret != ESP_OK) {
        return send_servo_http_error(req, "500 Internal Server Error", "servo_init_failed");
    }

    const smart_bin::servo_id_t servo_id =
        (servo_id_raw == 1) ? smart_bin::servo_id_t::kServo1 : smart_bin::servo_id_t::kServo2;
    const esp_err_t set_ret = smart_bin::servo_set_angle(servo_id, static_cast<uint16_t>(angle));
    if (set_ret != ESP_OK) {
        return send_servo_http_error(req, "500 Internal Server Error", "servo_set_failed");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_sendstr(req, "ok");
}
#endif

} // namespace

namespace smart_bin {

esp_err_t http_server_set_latest_photo(const uint8_t *jpeg, size_t len, uint32_t seq, uint32_t capture_ms)
{
    (void)jpeg;
    (void)len;
    (void)seq;
    (void)capture_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

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

#if CONFIG_SMART_HTTP_DEBUG_ENABLE_SERVO_ENDPOINTS
    static httpd_uri_t servo_home_uri = {};
    servo_home_uri.uri = "/servo/home";
    servo_home_uri.method = HTTP_POST;
    servo_home_uri.handler = servo_home_handler;
    servo_home_uri.user_ctx = nullptr;

    static httpd_uri_t servo_test_uri = {};
    servo_test_uri.uri = "/servo/test";
    servo_test_uri.method = HTTP_POST;
    servo_test_uri.handler = servo_test_handler;
    servo_test_uri.user_ctx = nullptr;

    static httpd_uri_t servo_set_uri = {};
    servo_set_uri.uri = "/servo/set";
    servo_set_uri.method = HTTP_POST;
    servo_set_uri.handler = servo_set_handler;
    servo_set_uri.user_ctx = nullptr;

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_http_server, &servo_home_uri), kTag, "Failed to register /servo/home");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_http_server, &servo_test_uri), kTag, "Failed to register /servo/test");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_http_server, &servo_set_uri), kTag, "Failed to register /servo/set");
#endif

    ESP_LOGI(kTag,
             "HTTP server started on port %d. Endpoints: GET /health, POST /infer%s",
             CONFIG_SMART_HTTP_SERVER_PORT,
#if CONFIG_SMART_HTTP_DEBUG_ENABLE_SERVO_ENDPOINTS
             ", POST /servo/home, POST /servo/test, POST /servo/set?id=1&angle=90"
#else
             ""
#endif
    );
#if CONFIG_SMART_HTTP_DEBUG_ENABLE_SERVO_ENDPOINTS
    ESP_LOGI(kTag, "Servo debug endpoints enabled: POST /servo/home, /servo/test, /servo/set?id=1&angle=90");
#endif
    return ESP_OK;
}

} // namespace smart_bin
