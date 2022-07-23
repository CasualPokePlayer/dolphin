// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include <OptionParser.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <Windows.h>
#endif

#include "AudioCommon/AudioCommon.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/Host.h"
#include "Core/Movie.h"
#include "Core/State.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/PowerPC/MMU.h"

#include "UICommon/CommandLineParse.h"
#ifdef USE_DISCORD_PRESENCE
#include "UICommon/DiscordPresence.h"
#endif
#include "UICommon/UICommon.h"

#include "InputCommon/GCAdapter.h"

#include "VideoCommon/RenderBase.h"
#include "VideoCommon/VideoBackendBase.h"

#include "blip_buf.h"

static std::unique_ptr<Platform> s_platform;

static void signal_handler(int)
{
  const char message[] = "A signal was received. A second signal will force Dolphin to stop.\n";
#ifdef _WIN32
  puts(message);
#else
  if (write(STDERR_FILENO, message, sizeof(message)) < 0)
  {
  }
#endif

  s_platform->RequestShutdown();
}

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_NotifyMapLoaded()
{
}

void Host_RefreshDSPDebuggerWindow()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

static Common::Event s_update_main_frame_event;
void Host_Message(HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    s_platform->Stop();
}

void Host_UpdateTitle(const std::string& title)
{
  s_platform->SetTitle(title);
}

void Host_UpdateDisasmDialog()
{
}

void Host_UpdateMainFrame()
{
  s_update_main_frame_event.Set();
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return s_platform->IsWindowFocused();
}

bool Host_RendererHasFullFocus()
{
  // Mouse capturing isn't implemented
  return Host_RendererHasFocus();
}

bool Host_RendererIsFullscreen()
{
  return s_platform->IsWindowFullscreen();
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}

static std::unique_ptr<Platform> GetPlatform(const optparse::Values& options)
{
  std::string platform_name = static_cast<const char*>(options.get("platform"));

#if HAVE_X11
  if (platform_name == "x11" || platform_name.empty())
    return Platform::CreateX11Platform();
#endif

#ifdef __linux__
  if (platform_name == "fbdev" || platform_name.empty())
    return Platform::CreateFBDevPlatform();
#endif

#ifdef _WIN32
  if (platform_name == "win32" || platform_name.empty())
    return Platform::CreateWin32Platform();
#endif

  if (platform_name == "headless" || platform_name.empty())
    return Platform::CreateHeadlessPlatform();

  return nullptr;
}

#ifdef _WIN32
#define main app_main
#endif

int main(int argc, char* argv[])
{
  auto parser = CommandLineParse::CreateParser(CommandLineParse::ParserOptions::OmitGUIOptions);
  parser->add_option("-p", "--platform")
      .action("store")
      .help("Window platform to use [%choices]")
      .choices({
        "headless"
#ifdef __linux__
            ,
            "fbdev"
#endif
#if HAVE_X11
            ,
            "x11"
#endif
#ifdef _WIN32
            ,
            "win32"
#endif
      });

  optparse::Values& options = CommandLineParse::ParseArguments(parser.get(), argc, argv);
  std::vector<std::string> args = parser->args();

  std::optional<std::string> save_state_path;
  if (options.is_set("save_state"))
  {
    save_state_path = static_cast<const char*>(options.get("save_state"));
  }

  std::unique_ptr<BootParameters> boot;
  bool game_specified = false;
  if (options.is_set("exec"))
  {
    const std::list<std::string> paths_list = options.all("exec");
    const std::vector<std::string> paths{std::make_move_iterator(std::begin(paths_list)),
                                         std::make_move_iterator(std::end(paths_list))};
    boot = BootParameters::GenerateFromFile(
        paths, BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    game_specified = true;
  }
  else if (options.is_set("nand_title"))
  {
    const std::string hex_string = static_cast<const char*>(options.get("nand_title"));
    if (hex_string.length() != 16)
    {
      fprintf(stderr, "Invalid title ID\n");
      parser->print_help();
      return 1;
    }
    const u64 title_id = std::stoull(hex_string, nullptr, 16);
    boot = std::make_unique<BootParameters>(BootParameters::NANDTitle{title_id});
  }
  else if (args.size())
  {
    boot = BootParameters::GenerateFromFile(
        args.front(), BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    args.erase(args.begin());
    game_specified = true;
  }
  else
  {
    parser->print_help();
    return 0;
  }

  std::string user_directory;
  if (options.is_set("user"))
    user_directory = static_cast<const char*>(options.get("user"));

  UICommon::SetUserDirectory(user_directory);
  UICommon::Init();
  GCAdapter::Init();

  s_platform = GetPlatform(options);
  if (!s_platform || !s_platform->Init())
  {
    fprintf(stderr, "No platform found, or failed to initialize.\n");
    return 1;
  }

  if (save_state_path && !game_specified)
  {
    fprintf(stderr, "A save state cannot be loaded without specifying a game to launch.\n");
    return 1;
  }

  Core::AddOnStateChangedCallback([](Core::State state) {
    if (state == Core::State::Uninitialized)
      s_platform->Stop();
  });
/*
#ifdef _WIN32
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
#else
  // Shut down cleanly on SIGINT and SIGTERM
  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif
*/
  DolphinAnalytics::Instance().ReportDolphinStart("nogui");

  if (!BootManager::BootCore(std::move(boot), s_platform->GetWindowSystemInfo()))
  {
    fprintf(stderr, "Could not boot the specified file\n");
    return 1;
  }

#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
  Common::SetCurrentThreadName("Host thread");
  s_platform->MainLoop();
  Core::Stop();

  Core::Shutdown();
  s_platform.reset();
  UICommon::Shutdown();

  return 0;
}

#ifdef _WIN32
int wmain(int, wchar_t*[], wchar_t*[])
{
  std::vector<std::string> args = CommandLineToUtf8Argv(GetCommandLineW());
  const int argc = static_cast<int>(args.size());
  std::vector<char*> argv(args.size());
  for (size_t i = 0; i < args.size(); ++i)
    argv[i] = args[i].data();

  return main(argc, argv.data());
}
#endif

#ifdef _WIN32
#define DOLPHINEXPORT extern "C" __declspec(dllexport)
#define unreachable() std::unreachable()
#else
#define DOLPHINEXPORT extern "C" __attribute__((visibility("default")))
#define unreachable() __builtin_unreachable()
#endif

class AudioProvider
{
public:
  AudioProvider()
  : m_blip_l(blip_new(1024 * 2))
  , m_blip_r(blip_new(1024 * 2))
  , m_sample_rate(32000)
  , m_nsamps(0)
  , m_latch_l(0)
  , m_latch_r(0)
  , m_samples()
  {
    blip_set_rates(m_blip_l, m_sample_rate, 44100);
    blip_set_rates(m_blip_r, m_sample_rate, 44100);
  }

  ~AudioProvider()
  {
    blip_delete(m_blip_l);
    blip_delete(m_blip_r);
  }

  void AddSamples(const short* samples, unsigned int num_samples, int sample_rate, int l_volume, int r_volume)
  {
    if (m_sample_rate != sample_rate)
    {
      FlushSamples();
      m_sample_rate = sample_rate;
      blip_set_rates(m_blip_l, sample_rate, 44100);
      blip_set_rates(m_blip_r, sample_rate, 44100);
    }

    // sent samples are interleved big endian samples with right sample preceding the left
    // blip_buf expects interleaved littlen endian samples with left samples preceding the right, so convert it
    for (u32 i = 0; i < num_samples; i++)
    {
      short samp = Common::swap16(*samples++);
      samp = samp * r_volume / 256;
      if (m_latch_r != samp)
      {
        blip_add_delta(m_blip_r, m_nsamps + i, m_latch_r - samp);
        m_latch_r = samp;
      }

      samp = Common::swap16(*samples++);
      samp = samp * l_volume / 256;
      if (m_latch_l != samp)
      {
        blip_add_delta(m_blip_l, m_nsamps + i, m_latch_l - samp);
        m_latch_l = samp;
      }
    }

    m_nsamps += num_samples;
  }

  void FlushSamples()
  {
    if (m_nsamps == 0)
    {
      return;  // don't bother
    }

    blip_end_frame(m_blip_l, m_nsamps);
    blip_end_frame(m_blip_r, m_nsamps);
    m_nsamps = 0;

    int nsamps = blip_samples_avail(m_blip_l);
    ASSERT(nsamps == blip_samples_avail(m_blip_r));
    int pos = m_samples.size();
    m_samples.resize(pos + nsamps * 2);
    blip_read_samples(m_blip_l, m_samples.data() + pos + 0, nsamps, 1);
    blip_read_samples(m_blip_r, m_samples.data() + pos + 1, nsamps, 1);
  }

  std::vector<short>& GetSamples()
  {
    return m_samples;
  }

private:
  blip_t* m_blip_l;
  blip_t* m_blip_r;
  int m_sample_rate;
  int m_nsamps;
  short m_latch_l;
  short m_latch_r;
  std::vector<short> m_samples;
};

using AddSamplesFunction = std::function<void(const short*, unsigned int, int, int, int)>;
AddSamplesFunction g_dsp_add_samples_func;
AddSamplesFunction g_dtk_add_samples_func;

static std::unique_ptr<AudioProvider> s_dsp_audio_provider;
static std::unique_ptr<AudioProvider> s_dtk_audio_provider;

// this should be called in a separate thread
// as the host here just spinloops executing jobs given to it
DOLPHINEXPORT int Dolphin_Main(int argc, char* argv[])
{
  s_dsp_audio_provider.reset(new AudioProvider);
  g_dsp_add_samples_func = std::bind(&AudioProvider::AddSamples, s_dsp_audio_provider.get(),
    std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5);

  s_dtk_audio_provider.reset(new AudioProvider);
  g_dtk_add_samples_func = std::bind(&AudioProvider::AddSamples, s_dtk_audio_provider.get(),
    std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5);

  return main(argc, argv);
}

#ifdef _WIN32
#undef main
#endif

// wait for the Dolphin_Main thread to exit after calling this
DOLPHINEXPORT void Dolphin_Shutdown()
{
  s_platform->Stop();
}

#ifdef _WIN32 // windows can safely just callback on the cpu thread

#define DO_CALLBACK(callback, ...) do { \
  callback(__VA_ARGS__); \
} while (0)

#define TRY_CALLBACK() do {} while (0)

#else // linux is different, it seems like mono doesn't like fastmem on the cpu thread, so make it use the main thread to callback

static std::function<void()> s_main_thread_job = nullptr;
static std::mutex s_main_thread_job_lock;

#define DO_CALLBACK(callback, ...) do { \
  std::atomic_bool jobCompleted(false); \
  s_main_thread_job_lock.lock(); \
  s_main_thread_job = [&] { \
    callback(__VA_ARGS__); \
    jobCompleted.store(true); \
  }; \
  s_main_thread_job_lock.unlock(); \
  while (!jobCompleted.load()) {}; \
} while (0)

#define TRY_CALLBACK() do { \
  s_main_thread_job_lock.lock(); \
  if (s_main_thread_job) s_main_thread_job(); \
  s_main_thread_job = nullptr; \
  s_main_thread_job_lock.unlock(); \
} while (0)

#endif

DOLPHINEXPORT bool Dolphin_BootupSuccessful()
{
  TRY_CALLBACK();
  return Core::IsRunningAndStarted();
}

void (*g_frame_callback)(const u8* buf, u32 width, u32 height, u32 pitch);
static u32* s_frame_buffer;
static std::atomic<u32> s_width, s_height;
static std::atomic_bool s_gpu_lagged;

static void FrameCallback(const u8* buf, u32 width, u32 height, u32 pitch)
{
  s_width.store(width);
  s_height.store(height);
  s_gpu_lagged.store(false);

  const u32* src = reinterpret_cast<const u32*>(buf);
  u32* dst = s_frame_buffer;
  for (u32 i = 0; i < height; i++)
  {
    for (u32 j = 0; j < width; j++)
    {
      dst[j] = Common::swap32(src[j]) >> 8;
    }

    dst += width;
    src += pitch / sizeof(u32);
  }
}

DOLPHINEXPORT void Dolphin_SetFrameBuffer(u32* fb)
{
  g_frame_callback = fb ? FrameCallback : nullptr;
  s_frame_buffer = fb;
  s_width.store(640);
  s_height.store(480);
}

static std::vector<short> s_samples;

DOLPHINEXPORT bool Dolphin_FrameStep(u32* width, u32* height)
{
  s_gpu_lagged.store(true);

  Core::DoFrameStep();

  // cpu will be "inactive" for a little time after requesting a frame step
  // so we use this to wait until the frame step variable is unset
  // (as the frame step variable will be set after a frame step req)
  while (Core::IsFrameStepping())
  {
    TRY_CALLBACK();
  }

  // cpu thread will still be doing stuff, and potentially will even poll inputs
  // let's wait until it's in a "safe" place (i.e. "inactive")
  while (CPU::IsCPUActive())
  {
    TRY_CALLBACK();
  }

  TRY_CALLBACK(); // just to be safe (i.e. in a case a new job is pending and cpu became inactive before the prev job returns)

  // since cpu thread gives jobs and it's inactive there can't be any more jobs to execute
  // we can continue now

  s_dsp_audio_provider->FlushSamples();
  s_dtk_audio_provider->FlushSamples();

  auto& dsp_samples = s_dsp_audio_provider->GetSamples();
  auto& dtk_samples = s_dtk_audio_provider->GetSamples();
  u32 sz = std::min(dsp_samples.size(), dtk_samples.size());
  s_samples.clear();

  for (u32 i = 0; i < sz; i++)
  {
    int sample = dsp_samples[i] / 2 + dtk_samples[i] / 2;
    s_samples.push_back(sample);
  }

  int samp_rm = dsp_samples.size() - sz;
  std::memmove(&dsp_samples[0], &dsp_samples[sz], samp_rm * 2);
  dsp_samples.resize(samp_rm);

  samp_rm = dtk_samples.size() - sz;
  std::memmove(&dtk_samples[0], &dtk_samples[sz], samp_rm * 2);
  dtk_samples.resize(samp_rm);

  *width = s_width.load();
  *height = s_height.load();
  return s_gpu_lagged.load();
}

static void (*s_gcpad_callback)(GCPadStatus* padStatus, int controllerID);

static void GCPadTrampoline(GCPadStatus* padStatus, int controllerID)
{
  DO_CALLBACK(s_gcpad_callback, padStatus, controllerID);
}

DOLPHINEXPORT void Dolphin_SetGCPadCallback(void (*callback)(GCPadStatus*, int))
{
  s_gcpad_callback = callback;
  Movie::SetGCInputManip(callback ? GCPadTrampoline : nullptr);
}

enum class WiimoteInputReq
{
  CORE_BUTTONS = 0,
  CORE_ACCEL = 1,
  CORE_IR_BASIC = 2,
  CORE_IR_EXTENDED = 3,
  CORE_IR_FULL = 4,
  END_INPUT = 0xFF,
};

void (*s_wiipad_callback)(void* p, WiimoteInputReq which, int controllerID);

static void WiiPadTrampoline(WiimoteCommon::DataReportBuilder& rpt, int controllerID, int ext, const WiimoteEmu::EncryptionKey& key)
{
  if (rpt.HasCore())
  {
    WiimoteCommon::DataReportBuilder::CoreData core;
    rpt.GetCoreData(&core);
    DO_CALLBACK(s_wiipad_callback, &core.hex, WiimoteInputReq::CORE_BUTTONS, controllerID);
    rpt.SetCoreData(core);
  }

  if (rpt.HasAccel())
  {
    WiimoteCommon::AccelData accel;
    rpt.GetAccelData(&accel);
    DO_CALLBACK(s_wiipad_callback, &accel.value.data, WiimoteInputReq::CORE_ACCEL, controllerID);
    rpt.SetAccelData(accel);
  }

  if (rpt.HasIR())
  {
    u8* const ir_data = rpt.GetIRDataPtr();
    if (rpt.GetIRDataSize() == sizeof(WiimoteEmu::IRBasic) * 2)
    {
      memset(ir_data, 0xFF, sizeof(WiimoteEmu::IRBasic) * 2);
      DO_CALLBACK(s_wiipad_callback, ir_data, WiimoteInputReq::CORE_IR_BASIC, controllerID);
    }
    else if (rpt.GetIRDataSize() == sizeof(WiimoteEmu::IRExtended) * 4)
    {
      memset(ir_data, 0xFF, sizeof(WiimoteEmu::IRExtended) * 4);
      DO_CALLBACK(s_wiipad_callback, ir_data, WiimoteInputReq::CORE_IR_EXTENDED, controllerID);
    }
    else if (rpt.GetIRDataSize() == sizeof(WiimoteEmu::IRFull) * 2)
    {
      memset(ir_data, 0xFF, sizeof(WiimoteEmu::IRFull) * 2);
      DO_CALLBACK(s_wiipad_callback, ir_data, WiimoteInputReq::CORE_IR_FULL, controllerID);
    }
    else
    {
      ASSERT(false);
    }
  }

  if (rpt.HasExt())
  {
    if (ext == 1) // nunchuk
    {
      // todo
    }
    else if (ext == 2) // classic controller
    {
      // todo
    }
  }

  DO_CALLBACK(s_wiipad_callback, rpt.GetDataPtr(), WiimoteInputReq::END_INPUT, controllerID);
}

DOLPHINEXPORT void Dolphin_SetWiiPadCallback(void (*callback)(void*, WiimoteInputReq, int))
{
  s_wiipad_callback = callback;
  Movie::SetWiiInputManip(callback ? WiiPadTrampoline : nullptr);
}

DOLPHINEXPORT short* Dolphin_GetAudio(u32* sz)
{
  *sz = s_samples.size();
  return s_samples.data();
}

static std::vector<u8> s_state_buffer;

DOLPHINEXPORT u32 Dolphin_StateSize(bool compressed)
{
  if (compressed)
  {
    State::BizSaveStateCompressed(s_state_buffer);
    return s_state_buffer.size();
  }
  else
  {
    return State::BizStateSize();
  }
}

DOLPHINEXPORT void Dolphin_SaveState(u8* buf, u32 sz, bool compressed)
{
  if (compressed)
  {
    std::memcpy(buf, s_state_buffer.data(), sz);
  }
  else
  {
    State::BizSaveState(buf, sz);
  }
}

DOLPHINEXPORT void Dolphin_LoadState(u8* buf, u32 sz, bool compressed)
{
  if (compressed)
  {
    State::BizLoadStateCompressed(buf, sz);
  }
  else
  {
    State::BizLoadState(buf, sz);
  }
}

enum class MEMPTR_IDS
{
  RAM = 0,
  EXRAM = 1,
  L1Cache = 2,
  FakeVMEM = 3,
};

DOLPHINEXPORT bool Dolphin_GetMemPtr(MEMPTR_IDS which, u8** ptr, u32* sz)
{
  switch (which)
  {
    case MEMPTR_IDS::RAM:
      if (ptr)
        *ptr = Memory::m_pRAM;
      if (sz)
        *sz = Memory::GetRamSize();
      return true;
    case MEMPTR_IDS::EXRAM:
      if (ptr)
        *ptr = Memory::m_pEXRAM;
      if (sz)
        *sz = Memory::GetExRamSize();
      return true;
    case MEMPTR_IDS::L1Cache:
      if (ptr)
        *ptr = Memory::m_pL1Cache;
      if (sz)
        *sz = Memory::GetL1CacheSize();
      return true;
    case MEMPTR_IDS::FakeVMEM:
      if (ptr)
        *ptr = Memory::m_pFakeVMEM;
      if (sz)
        *sz = Memory::GetFakeVMemSize();
      return true;
  }

  return false;
}

template<typename T>
static T ReadMMU(u32 addr)
{
  switch (sizeof(T))
  {
    case 1:
    {
      auto ret = PowerPC::HostTryReadU8(addr);
      return ret.has_value() ? ret.value().value : 0;
    }
    case 2:
    {
      auto ret = PowerPC::HostTryReadU16(addr);
      return ret.has_value() ? ret.value().value : 0;
    }
    case 4:
    {
      auto ret = PowerPC::HostTryReadU32(addr);
      return ret.has_value() ? ret.value().value : 0;
    }
    default:
      unreachable();
  }
}

DOLPHINEXPORT u8 Dolphin_ReadU8(u32 addr)
{
  return ReadMMU<u8>(addr);
}

DOLPHINEXPORT u16 Dolphin_ReadU16(u32 addr, bool bigEndian)
{
  return bigEndian ? ReadMMU<u16>(addr) : Common::swap16(ReadMMU<u16>(addr));
}

DOLPHINEXPORT u32 Dolphin_ReadU32(u32 addr, bool bigEndian)
{
  return bigEndian ? ReadMMU<u32>(addr) : Common::swap32(ReadMMU<u32>(addr));
}

DOLPHINEXPORT void Dolphin_ReadBulkU8(u32 start, u32 num, u8* buf)
{
  for (u32 i = 0; i < num; i++)
  {
    buf[i] = ReadMMU<u8>(start + i);
  }
}

DOLPHINEXPORT void Dolphin_ReadBulkU16(u32 start, u32 num, u16* buf, bool bigEndian)
{
  for (u32 i = 0; i < num; i++)
  {
    buf[i] = bigEndian ? ReadMMU<u16>(start + i * 2) : Common::swap16(ReadMMU<u16>(start + i * 2));
  }
}

DOLPHINEXPORT void Dolphin_ReadBulkU32(u32 start, u32 num, u32* buf, bool bigEndian)
{
  for (u32 i = 0; i < num; i++)
  {
    buf[i] = bigEndian ? ReadMMU<u32>(start + i * 4) : Common::swap32(ReadMMU<u32>(start + i * 4));
  }
}

template <typename T>
static void WriteMMU(u32 addr, T val)
{
  switch (sizeof(T))
  {
  case 1:
    PowerPC::HostTryWriteU8(val, addr);
    break;
  case 2:
    PowerPC::HostTryWriteU16(val, addr);
    break;
  case 4:
    PowerPC::HostTryWriteU32(val, addr);
    break;
  default:
    unreachable();
  }
}

DOLPHINEXPORT void Dolphin_WriteU8(u32 addr, u8 val)
{
  WriteMMU<u8>(addr, val);
}

DOLPHINEXPORT void Dolphin_WriteU16(u32 addr, u16 val, bool bigEndian)
{
  WriteMMU<u16>(addr, bigEndian ? val : Common::swap16(val));
}

DOLPHINEXPORT void Dolphin_WriteU32(u32 addr, u32 val, bool bigEndian)
{
  WriteMMU<u32>(addr, bigEndian ? val : Common::swap32(val));
}

DOLPHINEXPORT u32 Dolphin_GetVSyncNumerator()
{
  return VideoInterface::GetTargetRefreshRateNumerator();
}

DOLPHINEXPORT u32 Dolphin_GetVSyncDenominator()
{
  return VideoInterface::GetTargetRefreshRateDenominator();
}

bool (*g_mplus_config_callback)(int);
WiimoteEmu::ExtensionNumber (*g_extension_config_callback)(int);

DOLPHINEXPORT void Dolphin_SetConfigCallbacks(bool (*mplus)(int), WiimoteEmu::ExtensionNumber (*extension)(int))
{
  g_mplus_config_callback = mplus;
  g_extension_config_callback = extension;
}

DOLPHINEXPORT u64 Dolphin_GetTicks()
{
  std::atomic_uint64_t ret(0);
  Core::RunAsCPUThread([&ret] { ret.store(CoreTiming::GetTicks()); });
  return ret.load();
}
