set(APP_SRC_FILES
    "app/app_boot.cpp")

set(APP_REQUIRES
    esp_timer
    esp_event
    esp_http_server
    esp_netif
    esp_wifi
    nvs_flash)

if(CONFIG_SMART_BOOT_PROFILE_SERVO_TEST)
    list(APPEND APP_SRC_FILES "test/servo_smoke_test.cpp")
endif()

if(CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE)
    list(APPEND APP_SRC_FILES
        "camera/camera.cpp"
        "inference/inference.cpp")
    list(APPEND APP_REQUIRES esp32-camera)
endif()

if(CONFIG_SMART_BOOT_PROFILE_CAMERA_CAPTURE_SERVICE)
    list(APPEND APP_SRC_FILES
        "camera/camera.cpp")
    list(APPEND APP_REQUIRES esp32-camera)
endif()

if(CONFIG_SMART_BOOT_PROFILE_SERVO_TEST OR CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE)
    list(APPEND APP_SRC_FILES "board/servo_control.cpp")
    list(APPEND APP_REQUIRES esp_driver_ledc)
endif()

if(CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE AND CONFIG_SMART_SAMPLE_DUMP_ENABLE)
    list(APPEND APP_SRC_FILES "app/sample_dump.cpp")
endif()

if(CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE AND CONFIG_SMART_ENABLE_HTTP_DEBUG_API)
    list(APPEND APP_SRC_FILES
        "network/http_server.cpp"
        "network/softap.cpp")
endif()

if(CONFIG_SMART_BOOT_PROFILE_CAMERA_CAPTURE_SERVICE)
    list(APPEND APP_SRC_FILES
        "network/http_server_photo.cpp"
        "network/softap.cpp")
endif()

idf_component_register(
    SRCS ${APP_SRC_FILES}
    INCLUDE_DIRS "."
    REQUIRES ${APP_REQUIRES})

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

if(CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE AND CONFIG_SMART_FLASH_MODEL_WITH_APP)
    set(MODEL_IMAGE_FILE "${PROJECT_DIR}/models/model.espdl")
    math(EXPR MODEL_PARTITION_MAX_BYTES "0x180000")
    if(NOT EXISTS "${MODEL_IMAGE_FILE}")
        message(FATAL_ERROR
                "Model file not found: ${MODEL_IMAGE_FILE}\n"
                "Put your exported .espdl model at models/model.espdl")
    endif()
    file(SIZE "${MODEL_IMAGE_FILE}" MODEL_IMAGE_SIZE)
    if(MODEL_IMAGE_SIZE GREATER MODEL_PARTITION_MAX_BYTES)
        message(FATAL_ERROR
                "Model file is too large for the model partition: ${MODEL_IMAGE_SIZE} bytes > ${MODEL_PARTITION_MAX_BYTES} bytes\n"
                "Shrink models/model.espdl or increase the model partition in partitions.csv")
    endif()

    esptool_py_flash_to_partition(flash "model" "${MODEL_IMAGE_FILE}")
endif()
