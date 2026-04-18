#include "espdl_model_runner.hpp"

extern "C" void app_main(void) {
    espdl_boot::run_model_boot_flow();
}
