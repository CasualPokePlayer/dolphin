// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// See CPP file for comments.

#pragma once

#include "Common/CommonTypes.h"

class PointerWrap;
namespace MMIO
{
class Mapping;
}

namespace AudioInterface
{
void Init();
void Shutdown();
void DoState(PointerWrap& p);
bool IsPlaying();

void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

// Get the audio rate (48000 or 32000 only)
u32 GetAIDSampleRateDivisor();
u32 GetAISSampleRateDivisor();

u32 Get32KHzSampleRateDivisor();
u32 Get48KHzSampleRateDivisor();

void GenerateAISInterrupt();

}  // namespace AudioInterface
