#pragma once

#include <stdint.h>

typedef struct {
    uint16_t id;                // 11 bit standard id
    uint8_t dlc;
    uint8_t data[8];
} can_frame_t;
