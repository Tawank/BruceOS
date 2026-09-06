#pragma once

#include "core_sdk/ir.h"

#define IR_CODE_VALUE(protocol_, value_, bits_)                                                              \
    {.name = "Power", .type = IR_CODE_PARSED_VALUE, .protocol = protocol_, .value = value_, .bits = bits_}
#define IR_CODE_ADDRESS(protocol_, address_, command_)                                                       \
    {.name = "Power",                                                                                        \
     .type = IR_CODE_PARSED_ADDRESS,                                                                         \
     .protocol = protocol_,                                                                                  \
     .address = address_,                                                                                    \
     .command = command_}

static const bruce_ir_code_t s_power_na[] = {
    IR_CODE_VALUE("NEC", "20DF10EF", 32),
    IR_CODE_VALUE("NEC", "E0E040BF", 32),
    IR_CODE_VALUE("NEC", "04FB08F7", 32),
    IR_CODE_VALUE("NEC", "40BF12ED", 32),
    IR_CODE_VALUE("NEC", "C1AA09F6", 32),
    IR_CODE_VALUE("NEC", "F50A03FC", 32),
    IR_CODE_VALUE("Samsung32", "E0E040BF", 32),
    IR_CODE_VALUE("SIRC", "A90", 12),
    IR_CODE_VALUE("SIRC15", "540C", 15),
};

static const bruce_ir_code_t s_power_eu[] = {
    IR_CODE_VALUE("NEC", "20DF10EF", 32),
    IR_CODE_VALUE("NEC", "E0E040BF", 32),
    IR_CODE_VALUE("NEC", "10EF38C7", 32),
    IR_CODE_VALUE("NEC", "02FD48B7", 32),
    IR_CODE_VALUE("NEC", "08F7C03F", 32),
    IR_CODE_VALUE("NEC", "807F02FD", 32),
    IR_CODE_VALUE("Samsung32", "E0E040BF", 32),
    IR_CODE_VALUE("SIRC", "A90", 12),
    IR_CODE_VALUE("SIRC20", "000A90", 20),
};

static const bruce_ir_code_t s_power_universal[] = {
    IR_CODE_VALUE("NEC", "00FF02FD", 32),
    IR_CODE_VALUE("NEC", "00FF38C7", 32),
    IR_CODE_VALUE("NEC", "00FFA25D", 32),
    IR_CODE_VALUE("NEC", "00FFE21D", 32),
    IR_CODE_VALUE("NEC", "FF00FD02", 32),
    IR_CODE_VALUE("Samsung32", "707000FF", 32),
};

static const bruce_ir_code_t s_power_universal_ext[] = {
    IR_CODE_ADDRESS("NECext", "01 72", "1E"), IR_CODE_ADDRESS("NECext", "01 3E", "0A"),
    IR_CODE_ADDRESS("NECext", "04 F4", "08"), IR_CODE_ADDRESS("NECext", "85 7C", "80"),
    IR_CODE_ADDRESS("NECext", "83 7A", "08"), IR_CODE_ADDRESS("NECext", "00 F7", "0C"),
    IR_CODE_ADDRESS("NECext", "72 DD", "0E"), IR_CODE_ADDRESS("NECext", "72 DD", "10"),
    IR_CODE_ADDRESS("NECext", "04 B9", "00"), IR_CODE_ADDRESS("NECext", "00 DF", "1C"),
    IR_CODE_ADDRESS("NECext", "00 BF", "03"), IR_CODE_ADDRESS("NECext", "A0 B7", "E9"),
    IR_CODE_ADDRESS("NECext", "00 BF", "00"), IR_CODE_ADDRESS("NECext", "00 FB", "0A"),
    IR_CODE_ADDRESS("NECext", "84 E0", "20"), IR_CODE_ADDRESS("NECext", "86 05", "0F"),
    IR_CODE_ADDRESS("NECext", "40 40", "0A"), IR_CODE_ADDRESS("Samsung32", "3E", "0C"),
    IR_CODE_ADDRESS("Samsung32", "0E", "0C"),
};

static const uint32_t s_power_tcl_roku_timings[] = {
    440, 8150, 450, 8150, 450, 8150, 450, 5280, 450, 8150, 450, 5280, 450, 8150, 450, 8150,
    450, 8150, 450, 5280, 450, 8150, 450, 8150, 450, 8150, 450, 5280, 450, 8150, 450, 50000,
    450, 8150, 450, 8150, 450, 8150, 450, 5280, 450, 8150, 450, 5280, 450, 8150, 450, 8150,
};

static const bruce_ir_code_t s_power_universal_raw[] = {
    {
     .name = "Power",
     .type = IR_CODE_RAW,
     .frequency_hz = 34483,
     .duty_cycle = 0.33f,
     .data = s_power_tcl_roku_timings,
     .data_count = sizeof(s_power_tcl_roku_timings) / sizeof(s_power_tcl_roku_timings[0]),
     }
};

#undef IR_CODE_ADDRESS
#undef IR_CODE_VALUE
