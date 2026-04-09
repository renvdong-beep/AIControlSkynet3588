#ifndef _RKNN_YOLOV5_DEMO_PREPROCESS_H_
#define _RKNN_YOLOV5_DEMO_PREPROCESS_H_

#include <stdio.h>
#include <stdint.h>
#include "rga/im2d.h"
#include "rga/rga.h"
#include "postprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

// RGA 版本的预处理函数（无 OpenCV 依赖）
int letterbox_rga(int src_fd, void* src_virt, int src_width, int src_height, int src_format,
                  void* dst_virt, int dst_width, int dst_height, BOX_RECT* pads);

int resize_rga_simple(int src_fd, void* src_virt, int src_width, int src_height, int src_format,
                      void* dst_virt, int dst_width, int dst_height);

#ifdef __cplusplus
}
#endif

#endif //_RKNN_YOLOV5_DEMO_PREPROCESS_H_
