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
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/Host.h"
#include "Core/Movie.h"
#include "Core/State.h"
#include "Core/HW/Memmap.h"
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

  DolphinAnalytics::Instance().ReportDolphinStart("nogui");

  if (!BootManager::BootCore(std::move(boot), s_platform->GetWindowSystemInfo()))
  {
    fprintf(stderr, "Could not boot the specified file\n");
    return 1;
  }

#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
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
#else
#define DOLPHINEXPORT extern "C" __attribute__((visibility("default")))
#endif

static blip_t* s_blip_l = nullptr;
static blip_t* s_blip_r = nullptr;
static int s_sample_rate;
static int s_nsamps;
static short s_latch_l;
static short s_latch_r;
static std::vector<short> s_samples;

static void FlushSamples()
{
  if (s_nsamps == 0)
  {
    return; // don't bother
  }

  blip_end_frame(s_blip_l, s_nsamps);
  blip_end_frame(s_blip_r, s_nsamps);
  s_nsamps = 0;

  int nsamps = blip_samples_avail(s_blip_l);
  ASSERT(nsamps == blip_samples_avail(s_blip_r));
  int pos = s_samples.size();
  s_samples.resize(pos + nsamps * 2);
  blip_read_samples(s_blip_l, s_samples.data() + pos + 0, nsamps, 1);
  blip_read_samples(s_blip_r, s_samples.data() + pos + 1, nsamps, 1);
}

void (*g_audio_callback)(const short* samples, unsigned int num_samples, int sample_rate);

static void AudioCallback(const short* samples, unsigned int num_samples, int sample_rate)
{
  if (s_sample_rate != sample_rate)
  {
    FlushSamples();
    s_sample_rate = sample_rate;
    blip_set_rates(s_blip_l, sample_rate, 44100);
    blip_set_rates(s_blip_r, sample_rate, 44100);
  }

  for (int i = 0; i < num_samples; i++)
  {
    short samp = Common::swap16(samples[0]);
    if (s_latch_l != samp)
    {
      blip_add_delta(s_blip_l, s_nsamps, s_latch_l - samp);
      s_latch_l = samp;
    }

    samp = Common::swap16(samples[1]);
    if (s_latch_r != samp)
    {
      blip_add_delta(s_blip_r, s_nsamps, s_latch_r - samp);
      s_latch_r = samp;
    }

    s_nsamps++;
    samples += 2;
  }
}

// this should be called in a separate thread
// as the host here just spinloops executing jobs given to it
DOLPHINEXPORT int Dolphin_Main(int argc, char* argv[])
{
  g_audio_callback = AudioCallback;
  s_sample_rate = 32000;
  s_samples.clear();
  s_nsamps = 0;
  s_latch_l = 0;
  s_latch_r = 0;

  blip_delete(s_blip_l);
  s_blip_l = blip_new(1024 * 2);
  blip_set_rates(s_blip_l, s_sample_rate, 44100);

  blip_delete(s_blip_r);
  s_blip_r = blip_new(1024 * 2);
  blip_set_rates(s_blip_r, s_sample_rate, 44100);

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

DOLPHINEXPORT bool Dolphin_BootupSuccessful()
{
  return Core::IsRunningAndStarted();
}

void (*g_frame_callback)(const u8* buf, u32 width, u32 height, u32 pitch);

DOLPHINEXPORT void Dolphin_SetFrameCallback(void (*callback)(const u8*, u32, u32, u32))
{
  g_frame_callback = callback;
}

DOLPHINEXPORT void Dolphin_FrameStep()
{
  //Core::QueueHostJob(Core::DoFrameStep);
  //todo: running this on the host thread is probably safer, although need to add some mechanism for waiting for all jobs to be flushed
  s_samples.clear();
  Core::DoFrameStep();
  while (Core::IsFrameStepping()) {};
  FlushSamples();
}

void (*g_gcpad_callback)(GCPadStatus* padStatus, int controllerID);

static void GCPadTrampoline(GCPadStatus* padStatus, int controllerID)
{
  g_gcpad_callback(padStatus, controllerID);
}

DOLPHINEXPORT void Dolphin_SetGCPadCallback(void (*callback)(GCPadStatus*, int))
{
  g_gcpad_callback = callback;
  Movie::SetGCInputManip(callback ? GCPadTrampoline : nullptr);
}

DOLPHINEXPORT void Dolphin_GetAudio(short** data, u32* sz)
{
  *data = s_samples.data();
  *sz = s_samples.size();
}

static std::vector<u8> s_state_buffer;

DOLPHINEXPORT u8* Dolphin_SaveState(u32* sz)
{
  State::SaveToBuffer(s_state_buffer);
  *sz = s_state_buffer.size();
  return s_state_buffer.data();
}

DOLPHINEXPORT void Dolphin_LoadState(u8* buf, int sz)
{
  if (s_state_buffer.size() != sz)
  {
    s_state_buffer.resize(sz);
  }
  std::memcpy(s_state_buffer.data(), buf, sz);
  State::LoadFromBuffer(s_state_buffer);
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
      std::unreachable();
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
    std::unreachable();
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
