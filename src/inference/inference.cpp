#include "inference.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <new>

#include "dl_image.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "inference";
constexpr int kClassCount = 3;
constexpr std::array<const char *, kClassCount> kClassLabels = {"other", "paper", "plastic"};

struct InferenceState {
    dl::Model *model = nullptr;
    dl::image::ImagePreprocessor *preprocessor = nullptr;
    dl::TensorBase *output_tensor = nullptr;
    std::string output_name;
    int input_width = 0;
    int input_height = 0;
    SemaphoreHandle_t mutex = nullptr;
    bool initialized = false;
    bool ready = false;
};

InferenceState g_state;

float exp_to_scale(int exponent)
{
    return std::ldexp(1.0f, exponent);
}

void clear_state(bool keep_mutex)
{
    if (g_state.preprocessor != nullptr) {
        delete g_state.preprocessor;
        g_state.preprocessor = nullptr;
    }

    if (g_state.model != nullptr) {
        delete g_state.model;
        g_state.model = nullptr;
    }

    g_state.output_tensor = nullptr;
    g_state.output_name.clear();
    g_state.input_width = 0;
    g_state.input_height = 0;
    if (!keep_mutex && g_state.mutex != nullptr) {
        vSemaphoreDelete(g_state.mutex);
        g_state.mutex = nullptr;
    }

    g_state.ready = false;
}

bool is_tensor_shape_supported(const dl::TensorBase *input_tensor)
{
    if (input_tensor == nullptr || input_tensor->shape.size() != 4) {
        return false;
    }

    return input_tensor->shape[0] == 1 && input_tensor->shape[1] > 0 && input_tensor->shape[2] > 0 &&
           input_tensor->shape[3] == 3;
}

std::string format_tensor_shape(const dl::TensorBase *tensor)
{
    if (tensor == nullptr) {
        return "<null>";
    }

    std::string shape = "[";
    for (size_t i = 0; i < tensor->shape.size(); ++i) {
        if (i > 0) {
            shape += ",";
        }
        shape += std::to_string(tensor->shape[i]);
    }
    shape += "]";
    return shape;
}

void log_tensor_map(const char *title, const std::map<std::string, dl::TensorBase *> &tensors)
{
    ESP_LOGE(kTag, "%s count=%u", title, static_cast<unsigned>(tensors.size()));
    for (const auto &entry : tensors) {
        const dl::TensorBase *tensor = entry.second;
        ESP_LOGE(kTag,
                 "  %s: name=%s shape=%s size=%d dtype=%s exponent=%d",
                 title,
                 entry.first.c_str(),
                 format_tensor_shape(tensor).c_str(),
                 tensor ? tensor->size : -1,
                 tensor ? dl::dtype_to_string(tensor->dtype) : "<null>",
                 (tensor && tensor->exponent.channel_size() > 0) ? tensor->exponent.get(0) : 0);
    }
}

void log_model_io_summary(dl::Model *model)
{
    if (model == nullptr) {
        return;
    }

    log_tensor_map("input", model->get_inputs());
    log_tensor_map("output", model->get_outputs());
}

void log_partition_prefix(const esp_partition_t *partition)
{
    if (partition == nullptr) {
        return;
    }

    uint8_t bytes[16] = {};
    const esp_err_t read_ret = esp_partition_read(partition, 0, bytes, sizeof(bytes));
    if (read_ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to read model partition prefix: 0x%x", read_ret);
        return;
    }

    char hex[(sizeof(bytes) * 3) + 1] = {};
    size_t pos = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        pos += std::snprintf(hex + pos, sizeof(hex) - pos, "%s%02x", i == 0 ? "" : " ", bytes[i]);
    }
    ESP_LOGE(kTag, "Model partition first %u bytes: %s", static_cast<unsigned>(sizeof(bytes)), hex);
}

bool is_class_output_candidate(const dl::TensorBase *tensor)
{
    if (tensor == nullptr || tensor->size != kClassCount) {
        return false;
    }

    if (!tensor->shape.empty() && tensor->shape.back() == kClassCount) {
        return true;
    }

    return tensor->shape.empty() || tensor->shape.size() == 1;
}

dl::TensorBase *select_class_output_tensor(dl::Model *model, std::string *selected_name_out)
{
    if (model == nullptr) {
        return nullptr;
    }

    auto &outputs = model->get_outputs();
    dl::TensorBase *selected = nullptr;
    const char *selected_name = nullptr;
    unsigned candidate_count = 0;

    for (const auto &entry : outputs) {
        dl::TensorBase *tensor = entry.second;
        if (!is_class_output_candidate(tensor)) {
            continue;
        }

        ++candidate_count;
        if (selected == nullptr) {
            selected = tensor;
            selected_name = entry.first.c_str();
            if (selected_name_out != nullptr) {
                *selected_name_out = entry.first;
            }
        }
    }

    if (selected == nullptr) {
        ESP_LOGE(kTag, "No classifier-like output tensor found among %u outputs", static_cast<unsigned>(outputs.size()));
        log_model_io_summary(model);
        return nullptr;
    }

    if (candidate_count > 1) {
        ESP_LOGE(kTag,
                 "Found %u classifier-like outputs, selecting the first: %s shape=%s size=%d",
                 candidate_count,
                 selected_name,
                 format_tensor_shape(selected).c_str(),
                 selected->size);
    } else {
        ESP_LOGE(kTag,
                 "Selected output tensor: %s shape=%s size=%d",
                 selected_name,
                 format_tensor_shape(selected).c_str(),
                 selected->size);
    }

    return selected;
}

float read_output_value(dl::TensorBase *tensor, int index)
{
    const int exponent = tensor->exponent.get(0);
    const float scale = exp_to_scale(exponent);

    switch (tensor->dtype) {
    case dl::DATA_TYPE_FLOAT:
        return tensor->get_element_ptr<float>()[index];
    case dl::DATA_TYPE_DOUBLE:
        return static_cast<float>(tensor->get_element_ptr<double>()[index]);
    case dl::DATA_TYPE_INT8:
        return dl::dequantize<int8_t, float>(tensor->get_element_ptr<int8_t>()[index], scale);
    case dl::DATA_TYPE_UINT8:
        return static_cast<float>(tensor->get_element_ptr<uint8_t>()[index]) * scale;
    case dl::DATA_TYPE_INT16:
        return dl::dequantize<int16_t, float>(tensor->get_element_ptr<int16_t>()[index], scale);
    case dl::DATA_TYPE_UINT16:
        return static_cast<float>(tensor->get_element_ptr<uint16_t>()[index]) * scale;
    case dl::DATA_TYPE_INT32:
        return static_cast<float>(tensor->get_element_ptr<int32_t>()[index]) * scale;
    case dl::DATA_TYPE_UINT32:
        return static_cast<float>(tensor->get_element_ptr<uint32_t>()[index]) * scale;
    default:
        return NAN;
    }
}

void log_all_output_tensor_values(dl::Model *model)
{
    if (model == nullptr) {
        return;
    }

    auto &outputs = model->get_outputs();
    for (const auto &entry : outputs) {
        dl::TensorBase *tensor = entry.second;
        if (tensor == nullptr) {
            ESP_LOGE(kTag, "output values: name=%s <null>", entry.first.c_str());
            continue;
        }

        ESP_LOGE(kTag,
                 "output values: name=%s shape=%s size=%d",
                 entry.first.c_str(),
                 format_tensor_shape(tensor).c_str(),
                 tensor->size);

        std::string line;
        for (int i = 0; i < tensor->size; ++i) {
            char value_buf[32];
            std::snprintf(value_buf, sizeof(value_buf), "%s%.6f", (i == 0 || (i % 8) != 0) ? "" : "",
                          read_output_value(tensor, i));

            if (!line.empty()) {
                line += ' ';
            }
            line += value_buf;

            if ((i % 8) == 7 || i == tensor->size - 1) {
                ESP_LOGE(kTag, "  [%d..%d] %s", i - static_cast<int>((i % 8)), i, line.c_str());
                line.clear();
            }
        }
    }
}

void normalize_scores(std::array<float, kClassCount> &scores)
{
    bool looks_like_probabilities = true;
    float sum = 0.0f;

    for (float value : scores) {
        if (!std::isfinite(value) || value < -0.001f || value > 1.001f) {
            looks_like_probabilities = false;
        }
        sum += value;
    }

    if (looks_like_probabilities && sum > 0.001f && sum < 1.999f) {
        for (float &value : scores) {
            value = std::max(0.0f, value / sum);
        }
        return;
    }

    const float max_logit = *std::max_element(scores.begin(), scores.end());
    float exp_sum = 0.0f;
    for (float &value : scores) {
        value = std::exp(value - max_logit);
        exp_sum += value;
    }

    if (exp_sum <= 0.0f) {
        for (float &value : scores) {
            value = 0.0f;
        }
        return;
    }

    for (float &value : scores) {
        value /= exp_sum;
    }
}

esp_err_t run_rgb_locked(const uint8_t *rgb_data,
                         uint16_t width,
                         uint16_t height,
                         float decode_ms,
                         inference_result_t *out)
{
    if (!g_state.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t total_start_us = esp_timer_get_time();
    dl::image::img_t input_img = {};
    input_img.data = const_cast<uint8_t *>(rgb_data);
    input_img.width = width;
    input_img.height = height;
    input_img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    const int64_t preprocess_start_us = esp_timer_get_time();
    g_state.preprocessor->preprocess(input_img);
    const float preprocess_ms = static_cast<float>(esp_timer_get_time() - preprocess_start_us) / 1000.0f;

    const int64_t infer_start_us = esp_timer_get_time();
    g_state.model->run(dl::RUNTIME_MODE_SINGLE_CORE);
    const float infer_ms = static_cast<float>(esp_timer_get_time() - infer_start_us) / 1000.0f;
    log_all_output_tensor_values(g_state.model);

    dl::TensorBase *output_tensor = g_state.output_tensor;
    if (output_tensor == nullptr || output_tensor->size < kClassCount) {
        ESP_LOGE(kTag, "Model output tensor is invalid");
        return ESP_FAIL;
    }

    std::array<float, kClassCount> probabilities = {};
    for (int i = 0; i < kClassCount; ++i) {
        probabilities[i] = read_output_value(output_tensor, i);
    }
    normalize_scores(probabilities);

    const int predicted_class = static_cast<int>(
        std::distance(probabilities.begin(), std::max_element(probabilities.begin(), probabilities.end())));
    const float total_ms = static_cast<float>(esp_timer_get_time() - total_start_us) / 1000.0f;

    *out = {};
    out->predicted_class = predicted_class;
    out->predicted_label = kClassLabels[predicted_class];
    out->confidence = probabilities[predicted_class];
    out->scores[0] = probabilities[0];
    out->scores[1] = probabilities[1];
    out->scores[2] = probabilities[2];
    out->input_width = width;
    out->input_height = height;
    out->decode_ms = decode_ms;
    out->preprocess_ms = preprocess_ms;
    out->infer_ms = infer_ms;
    out->total_ms = total_ms;
    ESP_LOGE(kTag,
             "Prediction via %s: class=%d label=%s confidence=%.4f scores=[%.4f %.4f %.4f]",
             g_state.output_name.empty() ? "<unknown>" : g_state.output_name.c_str(),
             out->predicted_class,
             out->predicted_label,
             out->confidence,
             out->scores[0],
             out->scores[1],
             out->scores[2]);
    return ESP_OK;
}

} // namespace

extern "C" esp_err_t inference_init(void)
{
    if (g_state.mutex == nullptr) {
        g_state.mutex = xSemaphoreCreateMutex();
        if (g_state.mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_state.ready) {
        return ESP_OK;
    }

    clear_state(true);
    g_state.initialized = true;

    const esp_partition_t *model_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x82), "model");
    if (model_partition == nullptr) {
        ESP_LOGE(kTag, "Partition 'model' was not found in partition table");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(kTag,
             "Model partition found: address=0x%08x size=0x%x (%u bytes)",
             model_partition->address,
             model_partition->size,
             static_cast<unsigned>(model_partition->size));
    log_partition_prefix(model_partition);

    ESP_LOGI(kTag, "Loading ESP-DL model from partition 'model'...");
    g_state.model = new (std::nothrow)
        dl::Model("model", fbs::MODEL_LOCATION_IN_FLASH_PARTITION, 0, dl::MEMORY_MANAGER_GREEDY, nullptr, false);
    if (g_state.model == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate dl::Model");
        return ESP_ERR_NO_MEM;
    }

    if (g_state.model->get_fbs_model() == nullptr) {
        ESP_LOGE(kTag, "Model is not loaded. Check partition size/content for 'model'");
        log_partition_prefix(model_partition);
        // Avoid deleting partially initialized dl::Model here. In this failure path
        // some esp-dl versions can crash during cleanup and trigger reboot loops.
        g_state.model = nullptr;
        g_state.preprocessor = nullptr;
        g_state.output_tensor = nullptr;
        g_state.output_name.clear();
        g_state.input_width = 0;
        g_state.input_height = 0;
        g_state.ready = false;
        return ESP_ERR_INVALID_RESPONSE;
    }

    auto &inputs = g_state.model->get_inputs();
    if (inputs.size() != 1 || inputs.begin()->second == nullptr) {
        ESP_LOGE(kTag, "Model must have exactly one valid input tensor, got %u", static_cast<unsigned>(inputs.size()));
        log_model_io_summary(g_state.model);
        clear_state(true);
        return ESP_ERR_INVALID_RESPONSE;
    }

    log_model_io_summary(g_state.model);
    g_state.output_tensor = select_class_output_tensor(g_state.model, &g_state.output_name);
    if (g_state.output_tensor == nullptr) {
        clear_state(true);
        return ESP_ERR_INVALID_RESPONSE;
    }

    dl::TensorBase *input_tensor = inputs.begin()->second;
    if (!is_tensor_shape_supported(input_tensor)) {
        ESP_LOGE(kTag,
                 "Unsupported model input shape. Expected [1,H,W,3], got: [%d,%d,%d,%d]",
                 input_tensor ? input_tensor->shape[0] : -1,
                 input_tensor ? input_tensor->shape[1] : -1,
                 input_tensor ? input_tensor->shape[2] : -1,
                 input_tensor ? input_tensor->shape[3] : -1);
        clear_state(true);
        return ESP_ERR_INVALID_SIZE;
    }

    g_state.input_width = input_tensor->shape[2];
    g_state.input_height = input_tensor->shape[1];

    if (input_tensor->dtype != dl::DATA_TYPE_INT8 && input_tensor->dtype != dl::DATA_TYPE_INT16) {
        ESP_LOGE(kTag, "Unsupported model input dtype: %s", input_tensor->get_dtype_string());
        clear_state(true);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const std::array<float, 3> mean = {0.0f, 0.0f, 0.0f};
    const std::array<float, 3> std = {255.0f, 255.0f, 255.0f};
    g_state.preprocessor = new (std::nothrow) dl::image::ImagePreprocessor(g_state.model, mean, std, false);
    if (g_state.preprocessor == nullptr) {
        clear_state(true);
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_SMART_INFERENCE_ENABLE_SELF_TEST
    const esp_err_t test_ret = g_state.model->test();
    if (test_ret != ESP_OK) {
        ESP_LOGW(kTag, "model->test() failed: 0x%x", test_ret);
    }
    g_state.model->profile_module(true);
#endif

    g_state.ready = true;
    ESP_LOGE(kTag,
             "Inference service is ready (%d classes, %dx%d input, output=%s)",
             kClassCount,
             g_state.input_width,
             g_state.input_height,
             g_state.output_name.empty() ? "<unknown>" : g_state.output_name.c_str());
    return ESP_OK;
}

extern "C" bool inference_is_ready(void)
{
    return g_state.initialized && g_state.ready;
}

extern "C" esp_err_t inference_run_rgb888(const uint8_t *rgb,
                                          uint16_t width,
                                          uint16_t height,
                                          inference_result_t *out)
{
    if (rgb == nullptr || out == nullptr || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_state.ready || g_state.mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_state.mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t ret = run_rgb_locked(rgb, width, height, 0.0f, out);
    xSemaphoreGive(g_state.mutex);
    return ret;
}

extern "C" esp_err_t inference_run_jpeg(const uint8_t *data, size_t len, inference_result_t *out)
{
#if !CONFIG_SMART_INFERENCE_ENABLE_JPEG_INPUT
    (void)data;
    (void)len;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (data == nullptr || out == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_state.ready || g_state.mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    dl::image::jpeg_img_t jpeg_img = {};
    jpeg_img.data = const_cast<uint8_t *>(data);
    jpeg_img.data_len = len;

    const int64_t decode_start_us = esp_timer_get_time();
    dl::image::img_t decoded = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (decoded.data == nullptr || decoded.width == 0 || decoded.height == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const float decode_ms = static_cast<float>(esp_timer_get_time() - decode_start_us) / 1000.0f;

    if (xSemaphoreTake(g_state.mutex, portMAX_DELAY) != pdTRUE) {
        heap_caps_free(decoded.data);
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t ret =
        run_rgb_locked(static_cast<const uint8_t *>(decoded.data), decoded.width, decoded.height, decode_ms, out);

    xSemaphoreGive(g_state.mutex);
    heap_caps_free(decoded.data);
    return ret;
#endif
}
