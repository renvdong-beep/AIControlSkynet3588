// Copyright (c) 2023 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _RKNN_YOLOV5_DEMO_PREPROCESS_RGA_H_
#define _RKNN_YOLOV5_DEMO_PREPROCESS_RGA_H_

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rga/im2d.h"
#include "rga/rga.h"
#include "postprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 使用 RGA 进行 letterbox 缩放
 * 
 * @param src_fd 源图像 DMABUF fd
 * @param src_virt 源图像虚拟地址（如果 fd 无效则使用）
 * @param src_width 源图像宽度
 * @param src_height 源图像高度
 * @param src_format 源图像格式 (RK_FORMAT_*)
 * @param dst_virt 目标缓冲区虚拟地址
 * @param dst_width 目标宽度
 * @param dst_height 目标高度
 * @param pads 输出填充信息
 * @return int 0 成功，-1 失败
 */
int letterbox_rga(int src_fd, void* src_virt, int src_width, int src_height, int src_format,
                  void* dst_virt, int dst_width, int dst_height, BOX_RECT* pads);

/**
 * @brief 使用 RGA 进行简单缩放
 * 
 * @param src_fd 源图像 DMABUF fd
 * @param src_virt 源图像虚拟地址
 * @param src_width 源图像宽度
 * @param src_height 源图像高度
 * @param src_format 源图像格式
 * @param dst_virt 目标缓冲区虚拟地址
 * @param dst_width 目标宽度
 * @param dst_height 目标高度
 * @return int 0 成功，-1 失败
 */
int resize_rga_simple(int src_fd, void* src_virt, int src_width, int src_height, int src_format,
                      void* dst_virt, int dst_width, int dst_height);

#ifdef __cplusplus
}
#endif

#endif //_RKNN_YOLOV5_DEMO_PREPROCESS_RGA_H_
