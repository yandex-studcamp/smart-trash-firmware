idf_component_register(SRCS "main.cpp" "espdl_model_runner.cpp"
                       INCLUDE_DIRS ".")

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

set(MODEL_IMAGE_FILE "${PROJECT_DIR}/models/model.espdl")
if(NOT EXISTS "${MODEL_IMAGE_FILE}")
    message(FATAL_ERROR
            "Model file not found: ${MODEL_IMAGE_FILE}\n"
            "Put your exported .espdl model at models/model.espdl")
endif()

esptool_py_flash_to_partition(flash "model" "${MODEL_IMAGE_FILE}")
