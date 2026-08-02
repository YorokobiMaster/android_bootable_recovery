// SPDX-License-Identifier: GPL-3.0-or-later

#include "screenshot_utils.h"

#include <array>

#include <gtest/gtest.h>

TEST(ScreenshotUtilsTest, ConvertsRgb565PrimaryColors) {
  const std::array<uint8_t, 10> src = {
    0x00, 0x00,  // black
    0xff, 0xff,  // white
    0x00, 0xf8,  // red
    0xe0, 0x07,  // green
    0x1f, 0x00,  // blue
  };
  std::array<uint8_t, 20> dst{};

  ASSERT_TRUE(ConvertRgb565ToRgba8888(src.data(), src.size(), 5, 1, dst.data(), dst.size()));
  EXPECT_EQ(dst,
            (std::array<uint8_t, 20>{
                0, 0, 0, 255, 255, 255, 255, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
            }));
}

TEST(ScreenshotUtilsTest, HonorsSourceAndDestinationRowPadding) {
  const std::array<uint8_t, 8> src = {
    0x00, 0xf8, 0xaa, 0xbb, 0x1f, 0x00, 0xcc, 0xdd,
  };
  std::array<uint8_t, 12> dst;
  dst.fill(0x5a);

  ASSERT_TRUE(ConvertRgb565ToRgba8888(src.data(), 4, 1, 2, dst.data(), 6));
  EXPECT_EQ((std::array<uint8_t, 4>{ dst[0], dst[1], dst[2], dst[3] }),
            (std::array<uint8_t, 4>{ 255, 0, 0, 255 }));
  EXPECT_EQ((std::array<uint8_t, 4>{ dst[6], dst[7], dst[8], dst[9] }),
            (std::array<uint8_t, 4>{ 0, 0, 255, 255 }));
  EXPECT_EQ(dst[4], 0x5a);
  EXPECT_EQ(dst[5], 0x5a);
  EXPECT_EQ(dst[10], 0x5a);
  EXPECT_EQ(dst[11], 0x5a);
}

TEST(ScreenshotUtilsTest, RejectsNullPointersAndShortRows) {
  std::array<uint8_t, 8> buffer{};
  EXPECT_FALSE(ConvertRgb565ToRgba8888(nullptr, 2, 1, 1, buffer.data(), 4));
  EXPECT_FALSE(ConvertRgb565ToRgba8888(buffer.data(), 2, 1, 1, nullptr, 4));
  EXPECT_FALSE(ConvertRgb565ToRgba8888(buffer.data(), 1, 1, 1, buffer.data(), 4));
  EXPECT_FALSE(ConvertRgb565ToRgba8888(buffer.data(), 2, 1, 1, buffer.data(), 3));
}
