#ifndef TIMELAPSE_LIB_H
#define TIMELAPSE_LIB_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    __fp16 x;
    __fp16 y;
} motion_vec_t;

typedef struct {
    uint8_t* img_arr[16];
    motion_vec_t* motion_arr[15];
    uint8_t* alpha_ch;
    uint8_t cir_size;
    uint16_t width;
    uint16_t height;
} circular_buf_t;

extern "C" int motion_analysis_and_stack (circular_buf_t cir_buf_dscr, float * mean_x, float * mean_y);

#endif // TIMELAPSE_LIB_H
