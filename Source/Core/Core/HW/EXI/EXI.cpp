// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/EXI/EXI.h"

#include <array>
#include <memory>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/IniFile.h"

#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI_Channel.h"
#include "Core/HW/EXI/EXI_DeviceMemoryCard.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/Sram.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Movie.h"

#include "DiscIO/Enums.h"

Sram g_SRAM;
bool g_SRAM_netplay_initialized = false;

namespace ExpansionInterface
{
static CoreTiming::EventType* changeDevice;
static CoreTiming::EventType* updateInterrupts;

static std::array<std::unique_ptr<CEXIChannel>, MAX_EXI_CHANNELS> g_Channels;

static void ChangeDeviceCallback(u64 userdata, s64 cyclesLate);
static void UpdateInterruptsCallback(u64 userdata, s64 cycles_late);

namespace
{
void AddMemoryCards(int i)
{
  TEXIDevices memorycard_device;
  if (Movie::IsPlayingInput() && Movie::IsConfigSaved())
  {
    if (Movie::IsUsingMemcard(i))
    {
      if (SConfig::GetInstance().m_EXIDevice[i] == EXIDEVICE_MEMORYCARDFOLDER)
        memorycard_device = EXIDEVICE_MEMORYCARDFOLDER;
      else
        memorycard_device = EXIDEVICE_MEMORYCARD;
    }
    else
    {
      memorycard_device = EXIDEVICE_NONE;
    }
  }
  else
  {
    memorycard_device = SConfig::GetInstance().m_EXIDevice[i];
  }

  g_Channels[i]->AddDevice(memorycard_device, 0);
}
}  // namespace

void Init()
{
  if (!g_SRAM_netplay_initialized)
  {
    InitSRAM();
  }

  CEXIMemoryCard::Init();

  {
    bool use_memcard_251;
    IniFile gameIni = SConfig::GetInstance().LoadGameIni();
    gameIni.GetOrCreateSection("Core")->Get("MemoryCard251", &use_memcard_251, false);
    const u16 size_mbits =
        use_memcard_251 ? Memcard::MBIT_SIZE_MEMORY_CARD_251 : Memcard::MBIT_SIZE_MEMORY_CARD_2043;
    const bool shift_jis =
        SConfig::ToGameCubeRegion(SConfig::GetInstance().m_region) == DiscIO::Region::NTSC_J;
    const CardFlashId& flash_id = g_SRAM.settings_ex.flash_id[Memcard::SLOT_A];
    const u32 rtc_bias = g_SRAM.settings.rtc_bias;
    const u32 sram_language = static_cast<u32>(g_SRAM.settings.language);
    const u64 format_time =
        Common::Timer::GetLocalTimeSinceJan1970() - ExpansionInterface::CEXIIPL::GC_EPOCH;

    for (u32 i = 0; i < MAX_EXI_CHANNELS; i++)
    {
      Memcard::HeaderData header_data;
      Memcard::InitializeHeaderData(&header_data, flash_id, size_mbits, shift_jis, rtc_bias,
                                    sram_language, format_time + i);
      g_Channels[i] = std::make_unique<CEXIChannel>(i, header_data);
    }
  }

  for (int i = 0; i < MAX_MEMORYCARD_SLOTS; i++)
    AddMemoryCards(i);

  g_Channels[0]->AddDevice(EXIDEVICE_MASKROM, 1);
  g_Channels[0]->AddDevice(SConfig::GetInstance().m_EXIDevice[2], 2);  // Serial Port 1
  g_Channels[2]->AddDevice(EXIDEVICE_AD16, 0);

  changeDevice = CoreTiming::RegisterEvent("ChangeEXIDevice", ChangeDeviceCallback);
  updateInterrupts = CoreTiming::RegisterEvent("EXIUpdateInterrupts", UpdateInterruptsCallback);
}

void Shutdown()
{
  for (auto& channel : g_Channels)
    channel.reset();

  CEXIMemoryCard::Shutdown();
}

void DoState(PointerWrap& p)
{
  for (auto& channel : g_Channels)
    channel->DoState(p);
}

void PauseAndLock(bool doLock, bool unpauseOnUnlock)
{
  for (auto& channel : g_Channels)
    channel->PauseAndLock(doLock, unpauseOnUnlock);
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  for (int i = 0; i < MAX_EXI_CHANNELS; ++i)
  {
    DEBUG_ASSERT(g_Channels[i] != nullptr);
    // Each channel has 5 32 bit registers assigned to it. We offset the
    // base that we give to each channel for registration.
    //
    // Be careful: this means the base is no longer aligned on a page
    // boundary and using "base | FOO" is not valid!
    g_Channels[i]->RegisterMMIO(mmio, base + 5 * 4 * i);
  }
}

static void ChangeDeviceCallback(u64 userdata, s64 cyclesLate)
{
  u8 channel = (u8)(userdata >> 32);
  u8 type = (u8)(userdata >> 16);
  u8 num = (u8)userdata;

  g_Channels.at(channel)->AddDevice((TEXIDevices)type, num);
}

void ChangeDevice(const u8 channel, const TEXIDevices device_type, const u8 device_num,
                  CoreTiming::FromThread from_thread)
{
  // Let the hardware see no device for 1 second
  CoreTiming::ScheduleEvent(0, changeDevice,
                            ((u64)channel << 32) | ((u64)EXIDEVICE_NONE << 16) | device_num,
                            from_thread);
  CoreTiming::ScheduleEvent(SystemTimers::GetTicksPerSecond(), changeDevice,
                            ((u64)channel << 32) | ((u64)device_type << 16) | device_num,
                            from_thread);
}

CEXIChannel* GetChannel(u32 index)
{
  return g_Channels.at(index).get();
}

IEXIDevice* FindDevice(TEXIDevices device_type, int customIndex)
{
  for (auto& channel : g_Channels)
  {
    IEXIDevice* device = channel->FindDevice(device_type, customIndex);
    if (device)
      return device;
  }
  return nullptr;
}

void UpdateInterrupts()
{
  // Interrupts are mapped a bit strangely:
  // Channel 0 Device 0 generates interrupt on channel 0
  // Channel 0 Device 2 generates interrupt on channel 2
  // Channel 1 Device 0 generates interrupt on channel 1
  g_Channels[2]->SetEXIINT(g_Channels[0]->GetDevice(4)->IsInterruptSet());

  bool causeInt = false;
  for (auto& channel : g_Channels)
    causeInt |= channel->IsCausingInterrupt();

  ProcessorInterface::SetInterrupt(ProcessorInterface::INT_CAUSE_EXI, causeInt);
}

static void UpdateInterruptsCallback(u64 userdata, s64 cycles_late)
{
  UpdateInterrupts();
}

void ScheduleUpdateInterrupts(CoreTiming::FromThread from, int cycles_late)
{
  CoreTiming::ScheduleEvent(cycles_late, updateInterrupts, 0, from);
}

}  // namespace ExpansionInterface
