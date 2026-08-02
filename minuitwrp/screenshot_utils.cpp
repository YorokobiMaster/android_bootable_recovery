// SPDX-License-Identifier: GPL-3.0-or-later

#include "screenshot_utils.h"

bool ConvertRgb565ToRgba8888(const uint8_t* src, size_t src_row_bytes, size_t width, size_t height,
                             uint8_t* dst, size_t dst_row_bytes) {
  constexpr size_t kRgb565PixelBytes = 2;
  constexpr size_t kRgba8888PixelBytes = 4;

  if (src == nullptr || dst == nullptr || width > src_row_bytes / kRgb565PixelBytes ||
      width > dst_row_bytes / kRgba8888PixelBytes) {
    return false;
  }

  for (size_t y = 0; y < height; ++y) {
    const uint8_t* src_row = src + y * src_row_bytes;
    uint8_t* dst_row = dst + y * dst_row_bytes;
    for (size_t x = 0; x < width; ++x) {
      const uint16_t pixel =
          static_cast<uint16_t>(src_row[x * 2]) | (static_cast<uint16_t>(src_row[x * 2 + 1]) << 8);
      const uint8_t red = (pixel >> 11) & 0x1f;
      const uint8_t green = (pixel >> 5) & 0x3f;
      const uint8_t blue = pixel & 0x1f;

      dst_row[x * 4] = (red << 3) | (red >> 2);
      dst_row[x * 4 + 1] = (green << 2) | (green >> 4);
      dst_row[x * 4 + 2] = (blue << 3) | (blue >> 2);
      dst_row[x * 4 + 3] = 0xff;
    }
  }
  return true;
}
