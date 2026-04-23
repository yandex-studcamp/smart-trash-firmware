#include "inference.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <climits>
#include <string>
#include <new>
#include <map>
#include <cstdlib>
#include <vector>

#include "dl_image.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_SMART_SAMPLE_DUMP_ENABLE
#include "app/sample_dump.hpp"
#endif

#ifndef CONFIG_SMART_PRESENCE_MSE_THRESHOLD
#define CONFIG_SMART_PRESENCE_MSE_THRESHOLD "0.008"
#endif

namespace {

constexpr char kTag[] = "inference";
constexpr int kBaseClassCount = 3;
constexpr int kMaxClassCount = 4;
constexpr std::array<const char *, kBaseClassCount> kBaseClassLabels = {"other", "paper", "plastic"};
constexpr const char *kNothingLabel = "nothing";
constexpr const char *kMainModelPartitionLabel = "model";
constexpr const char *kGateModelPartitionLabel = "premodel";

#if CONFIG_SMART_PREDICTION_POLICY_EXPLICIT_NOTHING_CLASS
constexpr int kExpectedOutputClassCount = 4;
#else
constexpr int kExpectedOutputClassCount = 3;
#endif

enum class InputLayout {
    UNKNOWN = 0,
    NHWC,
    NCHW,
};

struct InferenceState {
    dl::Model *model = nullptr;
    dl::TensorBase *input_tensor = nullptr;
    dl::image::ImagePreprocessor *preprocessor = nullptr;
    dl::TensorBase *output_tensor = nullptr;
    std::string output_name;
    dl::Model *gate_model = nullptr;
    dl::image::ImagePreprocessor *gate_preprocessor = nullptr;
    dl::TensorBase *gate_input_tensor = nullptr;
    dl::TensorBase *gate_output_tensor = nullptr;
    std::string gate_output_name;
    int input_width = 0;
    int input_height = 0;
    int gate_input_width = 0;
    int gate_input_height = 0;
    InputLayout gate_input_layout = InputLayout::UNKNOWN;
    int class_count = 0;
    int nothing_class_id = -1;
    bool gate_enabled = false;
    std::array<const char *, kMaxClassCount> class_labels = {};
    SemaphoreHandle_t mutex = nullptr;
    bool initialized = false;
    bool ready = false;
};

InferenceState g_state;

float get_presence_mse_threshold()
{
    constexpr float kFallback = 0.008f;
    const char *threshold_cfg = CONFIG_SMART_PRESENCE_MSE_THRESHOLD;
    char *parse_end = nullptr;
    const float parsed = std::strtof(threshold_cfg, &parse_end);
    if (parse_end == threshold_cfg || !std::isfinite(parsed) || parsed < 0.0f) {
        ESP_LOGW(kTag,
                 "Invalid SMART_PRESENCE_MSE_THRESHOLD='%s', fallback=%.9f",
                 threshold_cfg,
                 kFallback);
        return kFallback;
    }
    return parsed;
}

float exp_to_scale(int exponent)
{
    return std::ldexp(1.0f, exponent);
}

const char *active_prediction_policy_name()
{
#if CONFIG_SMART_PREDICTION_POLICY_LEGACY_3_CLASS
    return "LEGACY_3_CLASS";
#elif CONFIG_SMART_PREDICTION_POLICY_EXPLICIT_NOTHING_CLASS
    return "EXPLICIT_NOTHING_CLASS";
#elif CONFIG_SMART_PREDICTION_POLICY_CONFIDENCE_THRESHOLD
    return "CONFIDENCE_THRESHOLD";
#elif CONFIG_SMART_PREDICTION_POLICY_PRESENCE_PREMODEL
    return "PRESENCE_PREMODEL";
#else
    return "UNKNOWN";
#endif
}

void log_runtime_scenario_summary()
{
    ESP_LOGI(kTag,
             "Runtime scenario: policy=%s main_partition=%s premodel_partition=%s premodel_enabled=%s class_count=%d "
             "nothing_class_id=%d",
             active_prediction_policy_name(),
             kMainModelPartitionLabel,
             kGateModelPartitionLabel,
             g_state.gate_enabled ? "true" : "false",
             g_state.class_count,
             g_state.nothing_class_id);

#if CONFIG_SMART_PREDICTION_POLICY_PRESENCE_PREMODEL
    ESP_LOGI(kTag,
             "Presence premodel params: mse_threshold=%.9f output_tensor=%s",
             get_presence_mse_threshold(),
             g_state.gate_output_name.empty() ? "<not-selected-yet>" : g_state.gate_output_name.c_str());
#elif CONFIG_SMART_PREDICTION_POLICY_CONFIDENCE_THRESHOLD
    ESP_LOGI(kTag,
             "Confidence-threshold params: threshold=%d%% synthetic_nothing_id=%d",
             CONFIG_SMART_CONFIDENCE_THRESHOLD_PERCENT,
             g_state.nothing_class_id);
#endif
}

esp_err_t configure_prediction_policy()
{
    g_state.class_labels.fill(nullptr);
    g_state.class_count = kExpectedOutputClassCount;
    g_state.nothing_class_id = -1;
    g_state.gate_enabled = false;

#if CONFIG_SMART_PREDICTION_POLICY_LEGACY_3_CLASS
    for (int i = 0; i < kBaseClassCount; ++i) {
        g_state.class_labels[i] = kBaseClassLabels[i];
    }
    ESP_LOGI(kTag, "Prediction policy: LEGACY_3_CLASS");
#elif CONFIG_SMART_PREDICTION_POLICY_EXPLICIT_NOTHING_CLASS
    g_state.nothing_class_id = CONFIG_SMART_NOTHING_CLASS_ID;
    if (g_state.nothing_class_id < 0 || g_state.nothing_class_id >= g_state.class_count) {
        ESP_LOGE(kTag,
                 "Invalid SMART_NOTHING_CLASS_ID=%d for explicit policy (expected range [0..%d])",
                 g_state.nothing_class_id,
                 g_state.class_count - 1);
        return ESP_ERR_INVALID_ARG;
    }

    int base_label_index = 0;
    for (int class_id = 0; class_id < g_state.class_count; ++class_id) {
        if (class_id == g_state.nothing_class_id) {
            g_state.class_labels[class_id] = kNothingLabel;
        } else if (base_label_index < kBaseClassCount) {
            g_state.class_labels[class_id] = kBaseClassLabels[base_label_index++];
        } else {
            g_state.class_labels[class_id] = "unknown";
        }
    }

    ESP_LOGI(kTag, "Prediction policy: EXPLICIT_NOTHING_CLASS (nothing_id=%d)", g_state.nothing_class_id);
#elif CONFIG_SMART_PREDICTION_POLICY_CONFIDENCE_THRESHOLD
    g_state.nothing_class_id = CONFIG_SMART_NOTHING_CLASS_ID;
    if (g_state.nothing_class_id < 0) {
        ESP_LOGE(kTag, "Invalid SMART_NOTHING_CLASS_ID=%d", g_state.nothing_class_id);
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < kBaseClassCount; ++i) {
        g_state.class_labels[i] = kBaseClassLabels[i];
    }
    ESP_LOGI(kTag,
             "Prediction policy: CONFIDENCE_THRESHOLD (threshold=%d%%, synthetic_nothing_id=%d)",
             CONFIG_SMART_CONFIDENCE_THRESHOLD_PERCENT,
             g_state.nothing_class_id);
#elif CONFIG_SMART_PREDICTION_POLICY_PRESENCE_PREMODEL
    g_state.nothing_class_id = CONFIG_SMART_NOTHING_CLASS_ID;
    if (g_state.nothing_class_id < 0) {
        ESP_LOGE(kTag, "Invalid SMART_NOTHING_CLASS_ID=%d", g_state.nothing_class_id);
        return ESP_ERR_INVALID_ARG;
    }
    g_state.gate_enabled = true;
    for (int i = 0; i < kBaseClassCount; ++i) {
        g_state.class_labels[i] = kBaseClassLabels[i];
    }
    ESP_LOGI(kTag,
             "Prediction policy: PRESENCE_PREMODEL (synthetic_nothing_id=%d, mse_threshold=%.9f)",
             g_state.nothing_class_id,
             get_presence_mse_threshold());
#else
#error "Unknown prediction policy"
#endif

    for (int i = 0; i < g_state.class_count; ++i) {
        ESP_LOGI(kTag, "Class map: id=%d label=%s", i, g_state.class_labels[i] ? g_state.class_labels[i] : "unknown");
    }

    return ESP_OK;
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
    if (g_state.gate_preprocessor != nullptr) {
        delete g_state.gate_preprocessor;
        g_state.gate_preprocessor = nullptr;
    }
    if (g_state.gate_model != nullptr) {
        delete g_state.gate_model;
        g_state.gate_model = nullptr;
    }

    g_state.output_tensor = nullptr;
    g_state.output_name.clear();
    g_state.input_tensor = nullptr;
    g_state.gate_input_tensor = nullptr;
    g_state.gate_output_tensor = nullptr;
    g_state.gate_output_name.clear();
    g_state.input_width = 0;
    g_state.input_height = 0;
    g_state.gate_input_width = 0;
    g_state.gate_input_height = 0;
    g_state.gate_input_layout = InputLayout::UNKNOWN;
    g_state.class_count = 0;
    g_state.nothing_class_id = -1;
    g_state.gate_enabled = false;
    g_state.class_labels.fill(nullptr);
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

InputLayout detect_input_layout(const dl::TensorBase *input_tensor)
{
    if (input_tensor == nullptr || input_tensor->shape.size() != 4) {
        return InputLayout::UNKNOWN;
    }

    const int n = input_tensor->shape[0];
    const int dim1 = input_tensor->shape[1];
    const int dim2 = input_tensor->shape[2];
    const int dim3 = input_tensor->shape[3];
    if (n != 1) {
        return InputLayout::UNKNOWN;
    }

    if (dim3 == 3 && dim1 > 0 && dim2 > 0) {
        return InputLayout::NHWC;
    }

    if (dim1 == 3 && dim2 > 0 && dim3 > 0) {
        return InputLayout::NCHW;
    }

    return InputLayout::UNKNOWN;
}

bool extract_hw_from_input_tensor(const dl::TensorBase *input_tensor,
                                  InputLayout *layout_out,
                                  int *height_out,
                                  int *width_out)
{
    if (input_tensor == nullptr || height_out == nullptr || width_out == nullptr) {
        return false;
    }

    const InputLayout layout = detect_input_layout(input_tensor);
    if (layout == InputLayout::UNKNOWN) {
        return false;
    }

    if (layout_out != nullptr) {
        *layout_out = layout;
    }

    if (layout == InputLayout::NHWC) {
        *height_out = input_tensor->shape[1];
        *width_out = input_tensor->shape[2];
    } else {
        *height_out = input_tensor->shape[2];
        *width_out = input_tensor->shape[3];
    }

    return true;
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
    if (tensor == nullptr || tensor->size != kExpectedOutputClassCount) {
        return false;
    }

    if (!tensor->shape.empty() && tensor->shape.back() == kExpectedOutputClassCount) {
        return true;
    }

    return tensor->shape.empty() || tensor->shape.size() == 1;
}

bool is_scalar_output_candidate(const dl::TensorBase *tensor)
{
    if (tensor == nullptr || tensor->size != 1) {
        return false;
    }

    return tensor->shape.empty() || tensor->shape.back() == 1;
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
        if (entry.first == "main_logits") {
            selected = tensor;
            selected_name = entry.first.c_str();
            if (selected_name_out != nullptr) {
                *selected_name_out = entry.first;
            }
            continue;
        }

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
                 "Found %u classifier-like outputs, selected: %s shape=%s size=%d",
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

dl::TensorBase *select_scalar_output_tensor(dl::Model *model, std::string *selected_name_out)
{
    if (model == nullptr) {
        return nullptr;
    }

    auto &outputs = model->get_outputs();
    dl::TensorBase *selected = nullptr;
    const char *selected_name = nullptr;

    for (const auto &entry : outputs) {
        dl::TensorBase *tensor = entry.second;
        if (!is_scalar_output_candidate(tensor)) {
            continue;
        }

        if (entry.first == "mse" || entry.first == "mse_logit" || entry.first == "error" || entry.first == "output") {
            selected = tensor;
            selected_name = entry.first.c_str();
            if (selected_name_out != nullptr) {
                *selected_name_out = entry.first;
            }
            break;
        }

        if (selected == nullptr) {
            selected = tensor;
            selected_name = entry.first.c_str();
            if (selected_name_out != nullptr) {
                *selected_name_out = entry.first;
            }
        }
    }

    if (selected == nullptr) {
        ESP_LOGE(kTag, "No scalar output tensor found in gate model");
        log_model_io_summary(model);
        return nullptr;
    }

    ESP_LOGE(kTag,
             "Selected gate output tensor: %s shape=%s size=%d",
             selected_name,
             format_tensor_shape(selected).c_str(),
             selected->size);
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

void log_scalar_tensor_quant_details(const char *tag, dl::TensorBase *tensor, int index, float dequantized)
{
#if !CONFIG_SMART_INFERENCE_VERBOSE_LOGGING
    (void)tag;
    (void)tensor;
    (void)index;
    (void)dequantized;
    return;
#else
    if (tag == nullptr || tensor == nullptr || index < 0 || index >= tensor->size) {
        return;
    }

    const int exponent = (tensor->exponent.channel_size() > 0) ? tensor->exponent.get(0) : 0;
    const float scale = (tensor->exponent.channel_size() > 0) ? exp_to_scale(exponent) : NAN;

    switch (tensor->dtype) {
    case dl::DATA_TYPE_INT8: {
        const int raw = static_cast<int>(tensor->get_element_ptr<int8_t>()[index]);
        ESP_LOGE(kTag,
                 "%s scalar quant: dtype=%s index=%d raw=%d exponent=%d scale=%.12f dequant=%.10f",
                 tag,
                 dl::dtype_to_string(tensor->dtype),
                 index,
                 raw,
                 exponent,
                 scale,
                 dequantized);
        break;
    }
    case dl::DATA_TYPE_UINT8: {
        const unsigned raw = static_cast<unsigned>(tensor->get_element_ptr<uint8_t>()[index]);
        ESP_LOGE(kTag,
                 "%s scalar quant: dtype=%s index=%d raw=%u exponent=%d scale=%.12f dequant=%.10f",
                 tag,
                 dl::dtype_to_string(tensor->dtype),
                 index,
                 raw,
                 exponent,
                 scale,
                 dequantized);
        break;
    }
    case dl::DATA_TYPE_INT16: {
        const int raw = static_cast<int>(tensor->get_element_ptr<int16_t>()[index]);
        ESP_LOGE(kTag,
                 "%s scalar quant: dtype=%s index=%d raw=%d exponent=%d scale=%.12f dequant=%.10f",
                 tag,
                 dl::dtype_to_string(tensor->dtype),
                 index,
                 raw,
                 exponent,
                 scale,
                 dequantized);
        break;
    }
    case dl::DATA_TYPE_UINT16: {
        const unsigned raw = static_cast<unsigned>(tensor->get_element_ptr<uint16_t>()[index]);
        ESP_LOGE(kTag,
                 "%s scalar quant: dtype=%s index=%d raw=%u exponent=%d scale=%.12f dequant=%.10f",
                 tag,
                 dl::dtype_to_string(tensor->dtype),
                 index,
                 raw,
                 exponent,
                 scale,
                 dequantized);
        break;
    }
    case dl::DATA_TYPE_INT32: {
        const int raw = tensor->get_element_ptr<int32_t>()[index];
        ESP_LOGE(kTag,
                 "%s scalar quant: dtype=%s index=%d raw=%d exponent=%d scale=%.12f dequant=%.10f",
                 tag,
                 dl::dtype_to_string(tensor->dtype),
                 index,
                 raw,
                 exponent,
                 scale,
                 dequantized);
        break;
    }
    case dl::DATA_TYPE_UINT32: {
        const unsigned raw = tensor->get_element_ptr<uint32_t>()[index];
        ESP_LOGE(kTag,
                 "%s scalar quant: dtype=%s index=%d raw=%u exponent=%d scale=%.12f dequant=%.10f",
                 tag,
                 dl::dtype_to_string(tensor->dtype),
                 index,
                 raw,
                 exponent,
                 scale,
                 dequantized);
        break;
    }
    case dl::DATA_TYPE_FLOAT: {
        const float raw = tensor->get_element_ptr<float>()[index];
        ESP_LOGE(kTag,
                 "%s scalar quant: dtype=%s index=%d raw_float=%.10f dequant=%.10f",
                 tag,
                 dl::dtype_to_string(tensor->dtype),
                 index,
                 raw,
                 dequantized);
        break;
    }
    case dl::DATA_TYPE_DOUBLE: {
        const double raw = tensor->get_element_ptr<double>()[index];
        ESP_LOGE(kTag,
                 "%s scalar quant: dtype=%s index=%d raw_double=%.10f dequant=%.10f",
                 tag,
                 dl::dtype_to_string(tensor->dtype),
                 index,
                 raw,
                 dequantized);
        break;
    }
    default:
        ESP_LOGE(kTag,
                 "%s scalar quant: dtype=%s index=%d exponent=%d scale=%.12f dequant=%.10f",
                 tag,
                 dl::dtype_to_string(tensor->dtype),
                 index,
                 exponent,
                 scale,
                 dequantized);
        break;
    }
#endif
}

bool get_tensor_value_bounds(const dl::TensorBase *tensor, float *min_out, float *max_out)
{
    if (tensor == nullptr || min_out == nullptr || max_out == nullptr) {
        return false;
    }

    if (tensor->exponent.channel_size() <= 0) {
        return false;
    }

    const float scale = exp_to_scale(tensor->exponent.get(0));
    switch (tensor->dtype) {
    case dl::DATA_TYPE_INT8:
        *min_out = static_cast<float>(INT8_MIN) * scale;
        *max_out = static_cast<float>(INT8_MAX) * scale;
        return true;
    case dl::DATA_TYPE_UINT8:
        *min_out = 0.0f;
        *max_out = static_cast<float>(UINT8_MAX) * scale;
        return true;
    case dl::DATA_TYPE_INT16:
        *min_out = static_cast<float>(INT16_MIN) * scale;
        *max_out = static_cast<float>(INT16_MAX) * scale;
        return true;
    case dl::DATA_TYPE_UINT16:
        *min_out = 0.0f;
        *max_out = static_cast<float>(UINT16_MAX) * scale;
        return true;
    case dl::DATA_TYPE_INT32:
        *min_out = static_cast<float>(INT32_MIN) * scale;
        *max_out = static_cast<float>(INT32_MAX) * scale;
        return true;
    case dl::DATA_TYPE_UINT32:
        *min_out = 0.0f;
        *max_out = static_cast<float>(UINT32_MAX) * scale;
        return true;
    default:
        return false;
    }
}

void log_all_output_tensor_values(dl::Model *model)
{
#if !CONFIG_SMART_INFERENCE_VERBOSE_LOGGING
    (void)model;
    return;
#else
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
#endif
}

void normalize_scores(float *scores, int score_count)
{
    if (scores == nullptr || score_count <= 0) {
        return;
    }

    bool looks_like_probabilities = true;
    float sum = 0.0f;

    for (int i = 0; i < score_count; ++i) {
        const float value = scores[i];
        if (!std::isfinite(value) || value < -0.001f || value > 1.001f) {
            looks_like_probabilities = false;
        }
        sum += value;
    }

    if (looks_like_probabilities && sum > 0.001f && sum < 1.999f) {
        for (int i = 0; i < score_count; ++i) {
            scores[i] = std::max(0.0f, scores[i] / sum);
        }
        return;
    }

    const float max_logit = *std::max_element(scores, scores + score_count);
    float exp_sum = 0.0f;
    for (int i = 0; i < score_count; ++i) {
        scores[i] = std::exp(scores[i] - max_logit);
        exp_sum += scores[i];
    }

    if (exp_sum <= 0.0f) {
        for (int i = 0; i < score_count; ++i) {
            scores[i] = 0.0f;
        }
        return;
    }

    for (int i = 0; i < score_count; ++i) {
        scores[i] /= exp_sum;
    }
}

int argmax_index(const float *scores, int score_count)
{
    if (scores == nullptr || score_count <= 0) {
        return 0;
    }

    int best_idx = 0;
    float best_value = scores[0];
    for (int i = 1; i < score_count; ++i) {
        if (scores[i] > best_value) {
            best_value = scores[i];
            best_idx = i;
        }
    }
    return best_idx;
}

inline int clamp_int(int value, int lo, int hi)
{
    return (value < lo) ? lo : ((value > hi) ? hi : value);
}

void center_crop_resize_rgb888(const uint8_t *src,
                               int src_w,
                               int src_h,
                               uint8_t *dst,
                               int dst_w,
                               int dst_h)
{
    const int crop_side = std::min(src_w, src_h);
    const int crop_x = (src_w - crop_side) / 2;
    const int crop_y = (src_h - crop_side) / 2;

    for (int y = 0; y < dst_h; ++y) {
        const int src_y = crop_y + (y * crop_side) / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int src_x = crop_x + (x * crop_side) / dst_w;
            const uint8_t *src_px = src + ((src_y * src_w + src_x) * 3);
            uint8_t *dst_px = dst + ((y * dst_w + x) * 3);
            dst_px[0] = src_px[0];
            dst_px[1] = src_px[1];
            dst_px[2] = src_px[2];
        }
    }
}

esp_err_t preprocess_gate_input_locked(const dl::image::img_t &input_img, bool normalize_to_unit_range = true)
{
    if (g_state.gate_input_tensor == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (g_state.gate_preprocessor != nullptr) {
        g_state.gate_preprocessor->preprocess(input_img);
        return ESP_OK;
    }

    if (input_img.data == nullptr || input_img.width == 0 || input_img.height == 0 ||
        g_state.gate_input_width <= 0 || g_state.gate_input_height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int dst_w = g_state.gate_input_width;
    const int dst_h = g_state.gate_input_height;
    std::vector<uint8_t> resized_rgb(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 3U);
    center_crop_resize_rgb888(static_cast<const uint8_t *>(input_img.data),
                              input_img.width,
                              input_img.height,
                              resized_rgb.data(),
                              dst_w,
                              dst_h);

    const float unit_scale = normalize_to_unit_range ? (1.0f / 255.0f) : 1.0f;
    const auto dtype = g_state.gate_input_tensor->dtype;
    const InputLayout layout = g_state.gate_input_layout;

    if (layout == InputLayout::UNKNOWN) {
        return ESP_ERR_INVALID_STATE;
    }

    if (dtype == dl::DATA_TYPE_FLOAT) {
        float *dst = g_state.gate_input_tensor->get_element_ptr<float>();
        if (dst == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        if (layout == InputLayout::NCHW) {
            const int hw = dst_h * dst_w;
            for (int y = 0; y < dst_h; ++y) {
                for (int x = 0; x < dst_w; ++x) {
                    const uint8_t *src_px = resized_rgb.data() + ((y * dst_w + x) * 3);
                    dst[(0 * hw) + (y * dst_w + x)] = static_cast<float>(src_px[0]) * unit_scale;
                    dst[(1 * hw) + (y * dst_w + x)] = static_cast<float>(src_px[1]) * unit_scale;
                    dst[(2 * hw) + (y * dst_w + x)] = static_cast<float>(src_px[2]) * unit_scale;
                }
            }
        } else {
            for (int y = 0; y < dst_h; ++y) {
                for (int x = 0; x < dst_w; ++x) {
                    const uint8_t *src_px = resized_rgb.data() + ((y * dst_w + x) * 3);
                    const int base = (y * dst_w + x) * 3;
                    dst[base + 0] = static_cast<float>(src_px[0]) * unit_scale;
                    dst[base + 1] = static_cast<float>(src_px[1]) * unit_scale;
                    dst[base + 2] = static_cast<float>(src_px[2]) * unit_scale;
                }
            }
        }
        return ESP_OK;
    }

    if (g_state.gate_input_tensor->exponent.channel_size() <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const float scale = exp_to_scale(g_state.gate_input_tensor->exponent.get(0));
    if (scale == 0.0f || !std::isfinite(scale)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (dtype == dl::DATA_TYPE_INT8) {
        int8_t *dst = g_state.gate_input_tensor->get_element_ptr<int8_t>();
        if (dst == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        const int hw = dst_h * dst_w;
        for (int y = 0; y < dst_h; ++y) {
            for (int x = 0; x < dst_w; ++x) {
                    const uint8_t *src_px = resized_rgb.data() + ((y * dst_w + x) * 3);
                    for (int c = 0; c < 3; ++c) {
                        const float normalized = static_cast<float>(src_px[c]) * unit_scale;
                        const int q = clamp_int(static_cast<int>(std::lround(normalized / scale)), INT8_MIN, INT8_MAX);
                        if (layout == InputLayout::NCHW) {
                            dst[(c * hw) + (y * dst_w + x)] = static_cast<int8_t>(q);
                    } else {
                        dst[((y * dst_w + x) * 3) + c] = static_cast<int8_t>(q);
                    }
                }
            }
        }
        return ESP_OK;
    }

    if (dtype == dl::DATA_TYPE_INT16) {
        int16_t *dst = g_state.gate_input_tensor->get_element_ptr<int16_t>();
        if (dst == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        const int hw = dst_h * dst_w;
        for (int y = 0; y < dst_h; ++y) {
            for (int x = 0; x < dst_w; ++x) {
                    const uint8_t *src_px = resized_rgb.data() + ((y * dst_w + x) * 3);
                    for (int c = 0; c < 3; ++c) {
                        const float normalized = static_cast<float>(src_px[c]) * unit_scale;
                        const int q =
                            clamp_int(static_cast<int>(std::lround(normalized / scale)), INT16_MIN, INT16_MAX);
                        if (layout == InputLayout::NCHW) {
                        dst[(c * hw) + (y * dst_w + x)] = static_cast<int16_t>(q);
                    } else {
                        dst[((y * dst_w + x) * 3) + c] = static_cast<int16_t>(q);
                    }
                }
            }
        }
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

bool read_scalar_raw_value(dl::TensorBase *tensor, int index, int32_t *raw_out)
{
    if (tensor == nullptr || raw_out == nullptr || index < 0 || index >= tensor->size) {
        return false;
    }

    switch (tensor->dtype) {
    case dl::DATA_TYPE_INT8:
        *raw_out = static_cast<int32_t>(tensor->get_element_ptr<int8_t>()[index]);
        return true;
    case dl::DATA_TYPE_UINT8:
        *raw_out = static_cast<int32_t>(tensor->get_element_ptr<uint8_t>()[index]);
        return true;
    case dl::DATA_TYPE_INT16:
        *raw_out = static_cast<int32_t>(tensor->get_element_ptr<int16_t>()[index]);
        return true;
    case dl::DATA_TYPE_UINT16:
        *raw_out = static_cast<int32_t>(tensor->get_element_ptr<uint16_t>()[index]);
        return true;
    case dl::DATA_TYPE_INT32:
        *raw_out = tensor->get_element_ptr<int32_t>()[index];
        return true;
    case dl::DATA_TYPE_UINT32:
        *raw_out = static_cast<int32_t>(tensor->get_element_ptr<uint32_t>()[index]);
        return true;
    default:
        return false;
    }
}

#if CONFIG_SMART_SAMPLE_DUMP_ENABLE
float read_tensor_channel_value(dl::TensorBase *tensor, int index)
{
    if (tensor == nullptr) {
        return NAN;
    }

    const int exp_idx = (tensor->exponent.channel_size() > 0) ? 0 : -1;
    const float scale = (exp_idx >= 0) ? exp_to_scale(tensor->exponent.get(exp_idx)) : 1.0f;

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

bool tensor_input_to_rgb888(dl::TensorBase *tensor,
                            int width,
                            int height,
                            InputLayout layout,
                            std::vector<uint8_t> *out_rgb)
{
    if (tensor == nullptr || out_rgb == nullptr || width <= 0 || height <= 0 ||
        tensor->shape.size() != 4 || layout == InputLayout::UNKNOWN) {
        return false;
    }

    out_rgb->assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U, 0);
    const int hw = width * height;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                int idx = 0;
                if (layout == InputLayout::NCHW) {
                    idx = (c * hw) + (y * width + x);
                } else {
                    idx = ((y * width + x) * 3) + c;
                }

                float normalized = read_tensor_channel_value(tensor, idx);
                if (!std::isfinite(normalized)) {
                    normalized = 0.0f;
                }

                // Model inputs are normalized to [0..1] in current pipeline.
                int pixel = static_cast<int>(std::lround(normalized * 255.0f));
                pixel = clamp_int(pixel, 0, 255);
                (*out_rgb)[static_cast<size_t>((y * width + x) * 3 + c)] = static_cast<uint8_t>(pixel);
            }
        }
    }

    return true;
}

void dump_rgb_artifact(const char *kind,
                       uint32_t inference_id,
                       const uint8_t *rgb,
                       int width,
                       int height,
                       int original_width,
                       int original_height)
{
    if (rgb == nullptr || width <= 0 || height <= 0) {
        return;
    }

    const esp_err_t ret = smart_bin::dump_rgb888_sample_with_meta_to_log(rgb,
                                                                          static_cast<uint16_t>(width),
                                                                          static_cast<uint16_t>(height),
                                                                          kind,
                                                                          inference_id,
                                                                          static_cast<uint16_t>(original_width),
                                                                          static_cast<uint16_t>(original_height));
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Sample dump failed for kind=%s: 0x%x", kind ? kind : "unknown", ret);
    }
}
#endif

esp_err_t run_gate_premodel_locked(const dl::image::img_t &input_img,
                                   bool *object_present_out,
                                   float *mse_out,
                                   float *preprocess_ms_out,
                                   float *infer_ms_out,
                                   uint32_t inference_id)
{
    static bool s_threshold_range_warning_emitted = false;

    if (object_present_out == nullptr || mse_out == nullptr || preprocess_ms_out == nullptr || infer_ms_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_state.gate_enabled) {
        *object_present_out = true;
        *mse_out = NAN;
        *preprocess_ms_out = 0.0f;
        *infer_ms_out = 0.0f;
        return ESP_OK;
    }

    if (g_state.gate_model == nullptr || g_state.gate_input_tensor == nullptr || g_state.gate_output_tensor == nullptr) {
        ESP_LOGE(kTag, "Gate model is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t gate_preprocess_start_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(preprocess_gate_input_locked(input_img, true), kTag, "Gate preprocess failed");
    *preprocess_ms_out = static_cast<float>(esp_timer_get_time() - gate_preprocess_start_us) / 1000.0f;

    const int64_t gate_infer_start_us = esp_timer_get_time();
    g_state.gate_model->run(dl::RUNTIME_MODE_SINGLE_CORE);
    *infer_ms_out = static_cast<float>(esp_timer_get_time() - gate_infer_start_us) / 1000.0f;
    log_all_output_tensor_values(g_state.gate_model);

    *mse_out = read_output_value(g_state.gate_output_tensor, 0);
    int32_t primary_raw = 0;
    bool primary_raw_ok = read_scalar_raw_value(g_state.gate_output_tensor, 0, &primary_raw);

    if (g_state.gate_input_tensor != nullptr &&
        g_state.gate_input_tensor->dtype == dl::DATA_TYPE_FLOAT &&
        primary_raw_ok &&
        primary_raw == 0) {
        static bool s_zero_raw_warning_emitted = false;
        if (!s_zero_raw_warning_emitted) {
            const int exponent = (g_state.gate_output_tensor->exponent.channel_size() > 0)
                                     ? g_state.gate_output_tensor->exponent.get(0)
                                     : 0;
            const float scale = exp_to_scale(exponent);
            ESP_LOGW(kTag,
                     "Gate output raw stays zero (FLOAT input path). "
                     "Possible preprocessing mismatch vs training wrapper or strongly quantized gate head. "
                     "Gate dtype=%s exponent=%d scale=%.12f",
                     dl::dtype_to_string(g_state.gate_output_tensor->dtype),
                     exponent,
                     scale);
            s_zero_raw_warning_emitted = true;
        }
    }

    log_scalar_tensor_quant_details("Gate output", g_state.gate_output_tensor, 0, *mse_out);
    const float configured_threshold = get_presence_mse_threshold();
    float effective_threshold = configured_threshold;

    float min_possible = NAN;
    float max_possible = NAN;
    if (get_tensor_value_bounds(g_state.gate_output_tensor, &min_possible, &max_possible)) {
        if (effective_threshold > max_possible) {
            if (!s_threshold_range_warning_emitted) {
                ESP_LOGW(kTag,
                         "Presence threshold %.9f is above max representable gate value %.9f (dtype=%s exp=%d). "
                         "Clamping to %.9f",
                         effective_threshold,
                         max_possible,
                         dl::dtype_to_string(g_state.gate_output_tensor->dtype),
                         g_state.gate_output_tensor->exponent.get(0),
                         max_possible);
                s_threshold_range_warning_emitted = true;
            }
            effective_threshold = max_possible;
        } else if (effective_threshold < min_possible) {
            if (!s_threshold_range_warning_emitted) {
                ESP_LOGW(kTag,
                         "Presence threshold %.9f is below min representable gate value %.9f (dtype=%s exp=%d). "
                         "Clamping to %.9f",
                         effective_threshold,
                         min_possible,
                         dl::dtype_to_string(g_state.gate_output_tensor->dtype),
                         g_state.gate_output_tensor->exponent.get(0),
                         min_possible);
                s_threshold_range_warning_emitted = true;
            }
            effective_threshold = min_possible;
        }
    }

    const bool object_present = (*mse_out > effective_threshold);

    *object_present_out = object_present;
    ESP_LOGE(kTag,
             "Presence premodel: mse=%.10f threshold=%.10f (configured=%.10f) object_present=%s",
             *mse_out,
             effective_threshold,
             configured_threshold,
             object_present ? "true" : "false");
#if CONFIG_SMART_INFERENCE_VERBOSE_LOGGING
    ESP_LOGE(kTag,
             "Presence premodel detailed: mse=%.10f threshold=%.10f configured=%.10f object_present=%s",
             *mse_out,
             effective_threshold,
             configured_threshold,
             object_present ? "true" : "false");
#endif

#if CONFIG_SMART_SAMPLE_DUMP_ENABLE
    std::vector<uint8_t> gate_rgb;
    if (tensor_input_to_rgb888(g_state.gate_input_tensor,
                               g_state.gate_input_width,
                               g_state.gate_input_height,
                               g_state.gate_input_layout,
                               &gate_rgb)) {
        dump_rgb_artifact("gate_input",
                          inference_id,
                          gate_rgb.data(),
                          g_state.gate_input_width,
                          g_state.gate_input_height,
                          input_img.width,
                          input_img.height);
    } else {
        ESP_LOGE(kTag, "Failed to convert gate input tensor to RGB dump");
    }
#else
    (void)inference_id;
#endif
    return ESP_OK;
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

#if CONFIG_SMART_SAMPLE_DUMP_ENABLE
    static uint32_t s_inference_dump_id = 0;
    const uint32_t inference_dump_id = ++s_inference_dump_id;
    dump_rgb_artifact("raw_rgb", inference_dump_id, rgb_data, width, height, width, height);
#else
    const uint32_t inference_dump_id = 0;
#endif

    float preprocess_ms = 0.0f;
    float infer_ms = 0.0f;

    bool gate_object_present = true;
    float gate_mse = NAN;
    float gate_preprocess_ms = 0.0f;
    float gate_infer_ms = 0.0f;
#if CONFIG_SMART_PREDICTION_POLICY_PRESENCE_PREMODEL
    ESP_RETURN_ON_ERROR(run_gate_premodel_locked(
                            input_img, &gate_object_present, &gate_mse, &gate_preprocess_ms, &gate_infer_ms, inference_dump_id),
                        kTag,
                        "Gate premodel inference failed");
    preprocess_ms += gate_preprocess_ms;
    infer_ms += gate_infer_ms;

    if (!gate_object_present) {
        const float total_ms = static_cast<float>(esp_timer_get_time() - total_start_us) / 1000.0f;
        *out = {};
        out->predicted_class = g_state.nothing_class_id;
        out->predicted_label = kNothingLabel;
        out->predicted_nothing = true;
        out->confidence = 1.0f;
        out->score_count = 0;
        out->input_width = width;
        out->input_height = height;
        out->decode_ms = decode_ms;
        out->preprocess_ms = preprocess_ms;
        out->infer_ms = infer_ms;
        out->total_ms = total_ms;

        ESP_LOGE(kTag,
                 "Prediction via %s: class=%d label=%s nothing=true confidence=1.0000 gate_mse=%.10f",
                 g_state.gate_output_name.empty() ? "<gate>" : g_state.gate_output_name.c_str(),
                 out->predicted_class,
                 out->predicted_label,
                 gate_mse);
        return ESP_OK;
    }
#endif

    const int64_t preprocess_start_us = esp_timer_get_time();
    g_state.preprocessor->preprocess(input_img);
    preprocess_ms += static_cast<float>(esp_timer_get_time() - preprocess_start_us) / 1000.0f;

#if CONFIG_SMART_SAMPLE_DUMP_ENABLE
    std::vector<uint8_t> main_rgb;
    if (tensor_input_to_rgb888(g_state.input_tensor,
                               g_state.input_width,
                               g_state.input_height,
                               InputLayout::NHWC,
                               &main_rgb)) {
        dump_rgb_artifact("main_input",
                          inference_dump_id,
                          main_rgb.data(),
                          g_state.input_width,
                          g_state.input_height,
                          width,
                          height);
    } else {
        ESP_LOGE(kTag, "Failed to convert main input tensor to RGB dump");
    }
#endif

    const int64_t infer_start_us = esp_timer_get_time();
    g_state.model->run(dl::RUNTIME_MODE_SINGLE_CORE);
    infer_ms += static_cast<float>(esp_timer_get_time() - infer_start_us) / 1000.0f;
    log_all_output_tensor_values(g_state.model);

    dl::TensorBase *output_tensor = g_state.output_tensor;
    if (output_tensor == nullptr || output_tensor->size < g_state.class_count || g_state.class_count <= 0 ||
        g_state.class_count > kMaxClassCount) {
        ESP_LOGE(kTag, "Model output tensor is invalid");
        return ESP_FAIL;
    }

    std::array<float, kMaxClassCount> probabilities = {};
    for (int i = 0; i < g_state.class_count; ++i) {
        probabilities[i] = read_output_value(output_tensor, i);
    }
    normalize_scores(probabilities.data(), g_state.class_count);

    const int raw_predicted_class = argmax_index(probabilities.data(), g_state.class_count);
    int effective_predicted_class = raw_predicted_class;
    const float confidence = probabilities[raw_predicted_class];
    bool predicted_nothing = false;
    const char *predicted_label = g_state.class_labels[raw_predicted_class];

#if CONFIG_SMART_PREDICTION_POLICY_CONFIDENCE_THRESHOLD
    const float threshold = static_cast<float>(CONFIG_SMART_CONFIDENCE_THRESHOLD_PERCENT) / 100.0f;
    if (confidence < threshold) {
        effective_predicted_class = g_state.nothing_class_id;
        predicted_label = kNothingLabel;
        predicted_nothing = true;
    }
#endif

#if CONFIG_SMART_PREDICTION_POLICY_EXPLICIT_NOTHING_CLASS
    predicted_nothing = (raw_predicted_class == g_state.nothing_class_id);
#endif

#if CONFIG_SMART_PREDICTION_POLICY_LEGACY_3_CLASS
    predicted_nothing = false;
#endif

    if (predicted_label == nullptr) {
        predicted_label = "unknown";
    }

    const float total_ms = static_cast<float>(esp_timer_get_time() - total_start_us) / 1000.0f;

    *out = {};
    out->predicted_class = effective_predicted_class;
    out->predicted_label = predicted_label;
    out->predicted_nothing = predicted_nothing;
    out->confidence = confidence;
    out->score_count = static_cast<uint8_t>(g_state.class_count);
    for (int i = 0; i < kMaxClassCount; ++i) {
        out->scores[i] = (i < g_state.class_count) ? probabilities[i] : 0.0f;
    }
    out->input_width = width;
    out->input_height = height;
    out->decode_ms = decode_ms;
    out->preprocess_ms = preprocess_ms;
    out->infer_ms = infer_ms;
    out->total_ms = total_ms;

    char score_buf[256] = {};
    size_t pos = 0;
    for (int i = 0; i < g_state.class_count; ++i) {
        const char *label = g_state.class_labels[i] ? g_state.class_labels[i] : "class";
        const int written = std::snprintf(score_buf + pos,
                                          sizeof(score_buf) - pos,
                                          "%s%s=%.4f",
                                          (i == 0) ? "" : " ",
                                          label,
                                          probabilities[i]);
        if (written <= 0) {
            break;
        }
        if (static_cast<size_t>(written) >= (sizeof(score_buf) - pos)) {
            pos = sizeof(score_buf) - 1;
            break;
        }
        pos += static_cast<size_t>(written);
    }

    ESP_LOGE(kTag,
             "Prediction via %s: raw_class=%d class=%d label=%s nothing=%s confidence=%.4f scores=[%s]",
             g_state.output_name.empty() ? "<unknown>" : g_state.output_name.c_str(),
             raw_predicted_class,
             out->predicted_class,
             out->predicted_label,
             out->predicted_nothing ? "true" : "false",
             out->confidence,
             score_buf);
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
    ESP_RETURN_ON_ERROR(configure_prediction_policy(), kTag, "Prediction policy configuration failed");
    log_runtime_scenario_summary();

    const esp_partition_t *model_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x82), kMainModelPartitionLabel);
    if (model_partition == nullptr) {
        ESP_LOGE(kTag, "Partition '%s' was not found in partition table", kMainModelPartitionLabel);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(kTag,
             "Model partition found: address=0x%08x size=0x%x (%u bytes)",
             model_partition->address,
             model_partition->size,
             static_cast<unsigned>(model_partition->size));
    log_partition_prefix(model_partition);

    ESP_LOGI(kTag, "Loading ESP-DL model from partition '%s'...", kMainModelPartitionLabel);
    g_state.model = new (std::nothrow)
        dl::Model(kMainModelPartitionLabel, fbs::MODEL_LOCATION_IN_FLASH_PARTITION, 0, dl::MEMORY_MANAGER_GREEDY, nullptr, false);
    if (g_state.model == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate dl::Model");
        return ESP_ERR_NO_MEM;
    }

    if (g_state.model->get_fbs_model() == nullptr) {
        ESP_LOGE(kTag, "Model is not loaded. Check partition size/content for '%s'", kMainModelPartitionLabel);
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
    if (g_state.output_tensor->size != g_state.class_count) {
        ESP_LOGE(kTag,
                 "Output tensor class count mismatch: expected=%d got=%d",
                 g_state.class_count,
                 g_state.output_tensor->size);
        clear_state(true);
        return ESP_ERR_INVALID_SIZE;
    }

    dl::TensorBase *input_tensor = inputs.begin()->second;
    g_state.input_tensor = input_tensor;
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

#if CONFIG_SMART_PREDICTION_POLICY_PRESENCE_PREMODEL
    const esp_partition_t *gate_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x83), kGateModelPartitionLabel);
    if (gate_partition == nullptr) {
        ESP_LOGE(kTag, "Partition '%s' was not found in partition table", kGateModelPartitionLabel);
        clear_state(true);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(kTag,
             "Gate model partition found: address=0x%08x size=0x%x (%u bytes)",
             gate_partition->address,
             gate_partition->size,
             static_cast<unsigned>(gate_partition->size));
    log_partition_prefix(gate_partition);

    ESP_LOGI(kTag, "Loading gate model from partition '%s'...", kGateModelPartitionLabel);
    g_state.gate_model = new (std::nothrow)
        dl::Model(kGateModelPartitionLabel, fbs::MODEL_LOCATION_IN_FLASH_PARTITION, 0, dl::MEMORY_MANAGER_GREEDY, nullptr, false);
    if (g_state.gate_model == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate gate dl::Model");
        clear_state(true);
        return ESP_ERR_NO_MEM;
    }

    if (g_state.gate_model->get_fbs_model() == nullptr) {
        ESP_LOGE(kTag, "Gate model is not loaded. Check partition size/content for '%s'", kGateModelPartitionLabel);
        log_partition_prefix(gate_partition);
        g_state.gate_model = nullptr;
        g_state.gate_preprocessor = nullptr;
        g_state.gate_output_tensor = nullptr;
        g_state.gate_output_name.clear();
        g_state.ready = false;
        return ESP_ERR_INVALID_RESPONSE;
    }

    auto &gate_inputs = g_state.gate_model->get_inputs();
    if (gate_inputs.size() != 1 || gate_inputs.begin()->second == nullptr) {
        ESP_LOGE(kTag, "Gate model must have exactly one valid input tensor, got %u", static_cast<unsigned>(gate_inputs.size()));
        log_model_io_summary(g_state.gate_model);
        clear_state(true);
        return ESP_ERR_INVALID_RESPONSE;
    }

    log_model_io_summary(g_state.gate_model);
    g_state.gate_output_tensor = select_scalar_output_tensor(g_state.gate_model, &g_state.gate_output_name);
    if (g_state.gate_output_tensor == nullptr) {
        clear_state(true);
        return ESP_ERR_INVALID_RESPONSE;
    }

    dl::TensorBase *gate_input_tensor = gate_inputs.begin()->second;
    g_state.gate_input_tensor = gate_input_tensor;
    int gate_h = 0;
    int gate_w = 0;
    InputLayout gate_layout = InputLayout::UNKNOWN;
    if (!extract_hw_from_input_tensor(gate_input_tensor, &gate_layout, &gate_h, &gate_w)) {
        ESP_LOGE(kTag,
                 "Unsupported gate model input shape. Expected [1,H,W,3] or [1,3,H,W], got: [%d,%d,%d,%d]",
                 gate_input_tensor ? gate_input_tensor->shape[0] : -1,
                 gate_input_tensor ? gate_input_tensor->shape[1] : -1,
                 gate_input_tensor ? gate_input_tensor->shape[2] : -1,
                 gate_input_tensor ? gate_input_tensor->shape[3] : -1);
        clear_state(true);
        return ESP_ERR_INVALID_SIZE;
    }

    g_state.gate_input_width = gate_w;
    g_state.gate_input_height = gate_h;
    g_state.gate_input_layout = gate_layout;

    ESP_LOGI(kTag,
             "Gate input layout=%s shape=%s dtype=%s",
             gate_layout == InputLayout::NCHW ? "NCHW" : "NHWC",
             format_tensor_shape(gate_input_tensor).c_str(),
             gate_input_tensor->get_dtype_string());

    if (gate_input_tensor->dtype != dl::DATA_TYPE_INT8 && gate_input_tensor->dtype != dl::DATA_TYPE_INT16 &&
        gate_input_tensor->dtype != dl::DATA_TYPE_FLOAT) {
        ESP_LOGE(kTag, "Unsupported gate model input dtype: %s", gate_input_tensor->get_dtype_string());
        clear_state(true);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const bool gate_uses_builtin_preprocessor =
        (gate_layout == InputLayout::NHWC) &&
        (gate_input_tensor->dtype == dl::DATA_TYPE_INT8 || gate_input_tensor->dtype == dl::DATA_TYPE_INT16);

    if (gate_uses_builtin_preprocessor) {
        g_state.gate_preprocessor = new (std::nothrow) dl::image::ImagePreprocessor(g_state.gate_model, mean, std, false);
        if (g_state.gate_preprocessor == nullptr) {
            clear_state(true);
            return ESP_ERR_NO_MEM;
        }
    } else {
        g_state.gate_preprocessor = nullptr;
        ESP_LOGI(kTag, "Gate model uses manual preprocess path");
    }
#else
    ESP_LOGI(kTag,
             "Premodel loading skipped: policy=%s uses only main model partition '%s'",
             active_prediction_policy_name(),
             kMainModelPartitionLabel);
#endif

#if CONFIG_SMART_INFERENCE_ENABLE_SELF_TEST
    const esp_err_t test_ret = g_state.model->test();
    if (test_ret != ESP_OK) {
        ESP_LOGW(kTag, "model->test() failed: 0x%x", test_ret);
    }
    g_state.model->profile_module(true);
#if CONFIG_SMART_PREDICTION_POLICY_PRESENCE_PREMODEL
    const esp_err_t gate_test_ret = g_state.gate_model->test();
    if (gate_test_ret != ESP_OK) {
        ESP_LOGW(kTag, "gate_model->test() failed: 0x%x", gate_test_ret);
    }
    g_state.gate_model->profile_module(true);
#endif
#endif

    g_state.ready = true;
    ESP_LOGE(kTag,
             "Inference service is ready (%d classes, %dx%d input, output=%s)",
             g_state.class_count,
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
#if !CONFIG_SMART_ENABLE_HTTP_DEBUG_API
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
