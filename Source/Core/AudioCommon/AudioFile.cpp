// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/AudioFile.h"
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

constexpr size_t AudioFileWriter::BUFFER_SIZE;

AudioFileWriter::AudioFileWriter()
{
}

AudioFileWriter::~AudioFileWriter()
{
  Stop();
}

bool AudioFileWriter::Start(const std::string& filename, unsigned int hle_sample_rate_divisor,
                           bool aiff)
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

  current_sample_rate_divisor = hle_sample_rate_divisor;
  use_aiff = aiff;

  if (use_aiff)
  {
    // -----------------
    // Write aiff file header
    // -----------------
    Write4("FORM");
    Write<u32>(100 * 1000 * 1000);  // write big value in case the file gets truncated
    Write4("AIFC");

    Write4("FVER");
    Write<u32>(4);           // size of fver block
    Write<u32>(0xA2805140);  // AIFCVersion1

    Write4("COMM");
    Write<u32>(0x18);  // size of comm block

    Write<u16>(2);                      // channels
    Write<u32>(100 * 1000 * 1000 / 2);  // write big value in case the file gets truncated
    Write<u16>(16);                     // bit depth

    // sample rate is stored as a 80 bit IEEE Standard 754 floating point number
    // on x86 systems, this is generally long double, so we can directly use the result from that
    // (probably)

    // on other systems, we'll try to use a normal double, and convert it over to a 80
    // bit float

    // ok, this is just a typical long double on x86, we can directly use it
    if (sizeof(long double) == 10 && std::numeric_limits<long double>::is_iec559)
    {
      // evil type punning ahead
#pragma pack(push, 1)
      union IEEE80BitFloat
      {
        long double f;
        struct
        {
          u64 significand;
          u16 exponent;
        };
      };
#pragma pack(pop)

      IEEE80BitFloat sample_rate = {};
      sample_rate.f =
          Mixer::FIXED_SAMPLE_RATE_DIVIDEND / static_cast<long double>(hle_sample_rate_divisor);

      Write<u16>(sample_rate.exponent);
      Write<u64>(sample_rate.significand);
    }
    // fall back on double (probably precice enough)
    else if (sizeof(double) == 8 && std::numeric_limits<double>::is_iec559)
    {
      // more evil type punning ahead
#pragma pack(push, 1)
      union IEEE64BitFloat
      {
        double f;
        struct
        {
          u64 significand : 52;
          u64 exponent : 12;
        };
      };
#pragma pack(pop)

      IEEE64BitFloat sample_rate = {};
      sample_rate.f =
          Mixer::FIXED_SAMPLE_RATE_DIVIDEND / static_cast<double>(hle_sample_rate_divisor);

      // 11 -> 15 bit exponent (we probably can assume the sign bit is 0?)
      u16 exponent = 0x3FFF + (sample_rate.exponent - 0x3FF);
      u64 significand = sample_rate.significand;
      // 52 -> 63 bit significand
      significand <<= 63 - 52;
      // "normalize" the float
      significand |= 0x8000000000000000ULL;

      Write<u16>(exponent);
      Write<u64>(significand);
    }
    else
    // well this is weird? is_iec559 probably returned false somehow, so we're on a very strange
    // machine, generic integer to 80-bit float conversion ahead (is this needed?)
    {
      u16 exponent = 0x3FFF + 63;
      u64 significand = Mixer::FIXED_SAMPLE_RATE_DIVIDEND / hle_sample_rate_divisor;

      while ((significand & (1ULL << 63)) == 0)
      {
        significand <<= 1;
        exponent--;
      }

      Write<u16>(exponent);
      Write<u64>(significand);
    }

    Write4("sowt");  // little endian samples
    Write<u16>(0);   // compression name (don't bother)

    Write4("SSND");
    Write<u32>(100 * 1000 * 1000);  // write big value in case the file gets truncated
    Write<u32>(0);
    Write<u32>(0);

    // We are now at offset 72
    if (file.Tell() != 72)
      PanicAlertFmt("Wrong offset: {}", file.Tell());
  }
  else  // wav
  {
    // -----------------
    // Write wav file header
    // -----------------
    Write4("RIFF");
    Write<u32>(100 * 1000 * 1000);  // write big value in case the file gets truncated
    Write4("WAVE");
    Write4("fmt ");

    Write<u32>(16);          // size of fmt block
    Write<u32>(0x00020001);  // two channels, uncompressed

    const u32 sample_rate = Mixer::FIXED_SAMPLE_RATE_DIVIDEND / hle_sample_rate_divisor;
    Write<u32>(sample_rate);
    Write<u32>(sample_rate * 2 * 2);  // two channels, 16bit

    Write<u32>(0x00100004);
    Write4("data");
    Write<u32>(100 * 1000 * 1000 - 32);

    // We are now at offset 44
    if (file.Tell() != 44)
      PanicAlertFmt("Wrong offset: {}", file.Tell());
  }

  return true;
}

void AudioFileWriter::Stop()
{
  if (use_aiff)
  {
    file.Seek(4, File::SeekOrigin::Begin);
    Write<u32>(audio_size + 72 - 8);

    file.Seek(34, File::SeekOrigin::Begin);
    Write<u32>(audio_size / 4);

    file.Seek(60, File::SeekOrigin::Begin);
    Write<u32>(audio_size - 8);
  }
  else  // wav
  {
    file.Seek(4, File::SeekOrigin::Begin);
    Write<u32>(audio_size + 36);

    file.Seek(40, File::SeekOrigin::Begin);
    Write<u32>(audio_size);
  }

  file.Close();
}

template <typename T>
void AudioFileWriter::Write(T value)
{
  if (use_aiff)  // AIFF uses BE for its values instead of LE (value is assumed LE)
  {
    value = Common::FromBigEndian(value);
  }

  file.WriteArray(&value, 1);
}

void AudioFileWriter::Write4(const char* ptr)
{
  file.WriteBytes(ptr, 4);
}

void AudioFileWriter::AddStereoSamplesBE(const short* sample_data, u32 count,
                                        int sample_rate_divisor, int l_volume, int r_volume)
{
  if (!file)
    ERROR_LOG_FMT(AUDIO, "AudioFileWriter - file not open.");

  if (count > BUFFER_SIZE * 2)
    ERROR_LOG_FMT(AUDIO, "AudioFileWriter - buffer too small (count = {}).", count);

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

  for (u32 i = 0; i < count; i++)
  {
    // Flip the audio channels from RL to LR
    conv_buffer[2 * i] = Common::swap16((u16)sample_data[2 * i + 1]);
    conv_buffer[2 * i + 1] = Common::swap16((u16)sample_data[2 * i]);

    // Apply volume (volume ranges from 0 to 256)
    conv_buffer[2 * i] = conv_buffer[2 * i] * l_volume / 256;
    conv_buffer[2 * i + 1] = conv_buffer[2 * i + 1] * r_volume / 256;
  }

  if (sample_rate_divisor != current_sample_rate_divisor)
  {
    Stop();
    file_index++;
    std::ostringstream filename;
    filename << File::GetUserPath(D_DUMPAUDIO_IDX) << basename << file_index
             << (use_aiff ? ".aiff" : ".wav");
    Start(filename.str(), sample_rate_divisor, use_aiff);
    current_sample_rate_divisor = sample_rate_divisor;
  }

  file.WriteBytes(conv_buffer.data(), count * 4);
  audio_size += count * 4;
}
