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

#include "preprocess_rga.h"
#include <stdlib.h>
#include <string.h>

#define LETTERBOX_COLOR 128  // 灰色填充

int letterbox_rga(int src_fd, void* src_virt, int src_width, int src_height, int src_format,
                  void* dst_virt, int dst_width, int dst_height, BOX_RECT* pads)
{
    if (!dst_virt || !pads) {
        return -1;
    }

    // 计算缩放比例
    float scale_x = (float)dst_width / src_width;
    float scale_y = (float)dst_height / src_height;
    float scale = scale_x < scale_y ? scale_x : scale_y;

    int scaled_width = (int)(src_width * scale);
    int scaled_height = (int)(src_height * scale);

    // 计算填充
    pads->left = (dst_width - scaled_width) / 2;
    pads->right = dst_width - scaled_width - pads->left;
    pads->top = (dst_height - scaled_height) / 2;
    pads->bottom = dst_height - scaled_height - pads->top;

    // 先用灰色填充目标缓冲区
    memset(dst_virt, LETTERBOX_COLOR, dst_width * dst_height * 3);  // RGB888

    // 使用 RGA 缩放并放置到中心位置
    rga_buffer_t src_buf;
    rga_buffer_t dst_buf;
    im_rect src_rect;
    im_rect dst_rect;
    
    memset(&src_rect, 0, sizeof(src_rect));
    memset(&dst_rect, 0, sizeof(dst_rect));

    // 创建源缓冲区
    if (src_fd >= 0) {
        src_buf = wrapbuffer_fd(src_fd, src_width, src_height, src_format);
    } else {
        src_buf = wrapbuffer_virtualaddr(src_virt, src_width, src_height, src_format);
    }

    // 创建目标缓冲区（放置到中心位置）
    dst_buf = wrapbuffer_virtualaddr(dst_virt, dst_width, dst_height, RK_FORMAT_RGB_888);
    
    // 设置目标区域（中心位置）
    dst_rect.x = pads->left;
    dst_rect.y = pads->top;
    dst_rect.width = scaled_width;
    dst_rect.height = scaled_height;

    // 检查参数
    int ret = imcheck(src_buf, dst_buf, src_rect, dst_rect);
    if (IM_STATUS_NOERROR != ret) {
        fprintf(stderr, "letterbox_rga: imcheck error! %s\n", imStrError((IM_STATUS)ret));
        return -1;
    }

    // 执行缩放
    IM_STATUS status = imresize(src_buf, dst_buf);
    if (IM_STATUS_SUCCESS != status) {
        fprintf(stderr, "letterbox_rga: imresize failed! %s\n", imStrError(status));
        return -1;
    }

    return 0;
}

int resize_rga_simple(int src_fd, void* src_virt, int src_width, int src_height, int src_format,
                      void* dst_virt, int dst_width, int dst_height)
{
    if (!dst_virt) {
        return -1;
    }

    rga_buffer_t src_buf;
    rga_buffer_t dst_buf;
    im_rect src_rect;
    im_rect dst_rect;
    
    memset(&src_rect, 0, sizeof(src_rect));
    memset(&dst_rect, 0, sizeof(dst_rect));

    // 创建源缓冲区
    if (src_fd >= 0) {
        src_buf = wrapbuffer_fd(src_fd, src_width, src_height, src_format);
    } else {
        src_buf = wrapbuffer_virtualaddr(src_virt, src_width, src_height, src_format);
    }

    // 创建目标缓冲区
    dst_buf = wrapbuffer_virtualaddr(dst_virt, dst_width, dst_height, RK_FORMAT_RGB_888);

    // 检查参数
    int ret = imcheck(src_buf, dst_buf, src_rect, dst_rect);
    if (IM_STATUS_NOERROR != ret) {
        fprintf(stderr, "resize_rga_simple: imcheck error! %s\n", imStrError((IM_STATUS)ret));
        return -1;
    }

    // 执行缩放
    IM_STATUS status = imresize(src_buf, dst_buf);
    if (IM_STATUS_SUCCESS != status) {
        fprintf(stderr, "resize_rga_simple: imresize failed! %s\n", imStrError(status));
        return -1;
    }

    return 0;
}
