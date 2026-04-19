set(SMART_SRC_FILES
    "app/main.cpp"
    "app/app_boot.cpp")

set(SMART_REQUIRES
    esp_timer)

if(CONFIG_SMART_BOOT_PROFILE_SERVO_TEST)
    list(APPEND SMART_SRC_FILES "test/servo_smoke_test.cpp")
    list(APPEND SMART_REQUIRES esp_driver_ledc)
endif()

if(CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE)
    list(APPEND SMART_SRC_FILES
        "camera/smart_camera.cpp"
        "inference/smart_inference.cpp")
    list(APPEND SMART_REQUIRES esp32-camera)
endif()

if(CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE AND CONFIG_SMART_ENABLE_NETWORK_DEBUG_API)
    list(APPEND SMART_SRC_FILES
        "network/smart_http_server.cpp"
        "network/smart_softap.cpp")
    list(APPEND SMART_REQUIRES
        esp_event
        esp_http_server
        esp_netif
        esp_wifi
        nvs_flash)
endif()

idf_component_register(
    SRCS ${SMART_SRC_FILES}
    INCLUDE_DIRS "."
    REQUIRES ${SMART_REQUIRES})

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

if(CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE AND CONFIG_SMART_FLASH_MODEL_WITH_APP)
    set(MODEL_IMAGE_FILE "${PROJECT_DIR}/models/model.espdl")
    if(NOT EXISTS "${MODEL_IMAGE_FILE}")
        message(FATAL_ERROR
                "Model file not found: ${MODEL_IMAGE_FILE}\n"
                "Put your exported .espdl model at models/model.espdl")
    endif()

    esptool_py_flash_to_partition(flash "model" "${MODEL_IMAGE_FILE}")
endif()
