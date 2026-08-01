#include "input_app.h"

#include "core_sdk/input.h"

int input_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return input__init();
}
