#include "app_boot.hpp"

extern "C" void app_main(void) {
    smart_bin::run_boot_flow();
}
