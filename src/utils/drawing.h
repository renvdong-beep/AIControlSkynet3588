#ifndef DRAWING_MODULE_
#define DRAWING_MODULE_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void draw_rectangle_yuv420sp(unsigned char* yuv420sp, int w, int h, int rx, int ry, int rw, int rh, unsigned int color, int thickness);

void draw_image_yuv420sp(unsigned char* yuv420sp, int w, int h, unsigned char* draw_img, int rx, int ry, int rw, int rh);

#ifdef __cplusplus
}
#endif

#endif 
