// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Image.h"

#include <string>
#include <vector>

//#include <png.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/ImageC.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"

namespace Common
{
bool LoadPNG(const std::vector<u8>& input, std::vector<u8>* data_out, u32* width_out,
             u32* height_out)
{
  return false;
}

bool SavePNG(const std::string& path, const u8* input, ImageByteFormat format, u32 width,
             u32 height, int stride, int level)
{
  return false;
}

bool ConvertRGBAToRGBAndSavePNG(const std::string& path, const u8* input, u32 width, u32 height,
                                int stride, int level)
{
  const std::vector<u8> data = RGBAToRGB(input, width, height, stride);
  return SavePNG(path, data.data(), ImageByteFormat::RGB, width, height, width * 3, level);
}

std::vector<u8> RGBAToRGB(const u8* input, u32 width, u32 height, int row_stride)
{
  std::vector<u8> buffer;
  buffer.reserve(width * height * 3);

  for (u32 y = 0; y < height; ++y)
  {
    const u8* pos = input + y * row_stride;
    for (u32 x = 0; x < width; ++x)
    {
      buffer.push_back(pos[x * 4]);
      buffer.push_back(pos[x * 4 + 1]);
      buffer.push_back(pos[x * 4 + 2]);
    }
  }
  return buffer;
}
}  // namespace Common
