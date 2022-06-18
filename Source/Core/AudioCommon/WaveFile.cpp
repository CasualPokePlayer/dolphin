// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/WaveFile.h"
#include "AudioCommon/Mixer.h"

#include <string>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"

constexpr size_t WaveFileWriter::BUFFER_SIZE;

WaveFileWriter::WaveFileWriter()
{
}

WaveFileWriter::~WaveFileWriter()
{
  Stop();
}

bool WaveFileWriter::Start(const std::string& filename, unsigned int HLESampleRateDivisor)
{
  // Ask to delete file
  if (File::Exists(filename))
  {
    if (Config::Get(Config::MAIN_DUMP_AUDIO_SILENT) ||
        AskYesNoFmtT("Delete the existing file '{0}'?", filename))
    {
      File::Delete(filename);
    }
    else
    {
      // Stop and cancel dumping the audio
      return false;
    }
  }

  // Check if the file is already open
  if (file)
  {
    PanicAlertFmtT("The file {0} was already open, the file header will not be written.", filename);
    return false;
  }

  file.Open(filename, "wb");
  if (!file)
  {
    PanicAlertFmtT(
        "The file {0} could not be opened for writing. Please check if it's already opened "
        "by another program.",
        filename);
    return false;
  }

  audio_size = 0;

  if (basename.empty())
    SplitPath(filename, nullptr, &basename, nullptr);

  current_sample_rate_divisor = HLESampleRateDivisor;
  frac = 0;

  // -----------------
  // Write file header
  // -----------------
  Write4("RIFF");
  Write(100 * 1000 * 1000);  // write big value in case the file gets truncated
  Write4("WAVE");
  Write4("fmt ");

  Write(16);          // size of fmt block
  Write(0x00020001);  // two channels, uncompressed

  const u32 sample_rate = Mixer::FIXED_SAMPLE_RATE_DIVIDEND / HLESampleRateDivisor;
  Write(sample_rate);
  Write(sample_rate * 2 * 2);  // two channels, 16bit

  Write(0x00100004);
  Write4("data");
  Write(100 * 1000 * 1000 - 32);

  // We are now at offset 44
  if (file.Tell() != 44)
    PanicAlertFmt("Wrong offset: {}", file.Tell());

  return true;
}

void WaveFileWriter::Stop()
{
  file.Seek(4, File::SeekOrigin::Begin);
  Write(audio_size + 36);

  file.Seek(40, File::SeekOrigin::Begin);
  Write(audio_size);

  file.Close();
}

void WaveFileWriter::Write(u32 value)
{
  file.WriteArray(&value, 1);
}

void WaveFileWriter::Write4(const char* ptr)
{
  file.WriteBytes(ptr, 4);
}

void WaveFileWriter::AddStereoSamplesBE(const short* sample_data, u32 count,
                                        int sample_rate_divisor, int l_volume, int r_volume)
{
  if (!file)
    ERROR_LOG_FMT(AUDIO, "WaveFileWriter - file not open.");

  if (count > BUFFER_SIZE * 2)
    ERROR_LOG_FMT(AUDIO, "WaveFileWriter - buffer too small (count = {}).", count);

  if (skip_silence)
  {
    bool all_zero = true;

    for (u32 i = 0; i < count * 2; i++)
    {
      if (sample_data[i])
        all_zero = false;
    }

    if (all_zero)
      return;
  }

  const u32 ratio =
      (u32)(65536.0f *
            (Mixer::FIXED_SAMPLE_RATE_DIVIDEND / static_cast<float>(sample_rate_divisor)) /
            static_cast<float>(OUT_SAMPLE_RATE));
  u32 current_sample = 0;

  for (u32 r_index = 0; current_sample < count * 2 && (count * 2 - r_index) > 2;
       current_sample += 2)
  {
    u32 r2_index = r_index + 2;  // next sample

    s16 l1 = Common::swap16(sample_data[r_index + 1]);   // current
    s16 l2 = Common::swap16(sample_data[r2_index + 1]);  // next
    int sample_l = ((l1 << 16) + (l2 - l1) * static_cast<u16>(frac)) >> 16;
    sample_l = (sample_l * l_volume) >> 8;
    sample_l += out_buffer[current_sample + 1];
    out_buffer[current_sample + 1] = std::clamp(sample_l, -32768, 32767);

    s16 r1 = Common::swap16(sample_data[r_index]);   // current
    s16 r2 = Common::swap16(sample_data[r2_index]);  // next
    int sample_r = ((r1 << 16) + (r2 - r1) * static_cast<u16>(frac)) >> 16;
    sample_r = (sample_r * r_volume) >> 8;
    sample_r += out_buffer[current_sample];
    out_buffer[current_sample] = std::clamp(sample_r, -32768, 32767);

    frac += ratio;
    r_index += 2 * static_cast<u16>(frac >> 16);
    frac &= 0xffff;
  }

  file.WriteBytes(out_buffer.data(), current_sample * 2);
  audio_size += current_sample * 2;
}
