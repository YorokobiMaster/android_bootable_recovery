// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

bool ConvertRgb565ToRgba8888(const uint8_t* src, size_t src_row_bytes, size_t width, size_t height,
                             uint8_t* dst, size_t dst_row_bytes);
