// mini-jarvis power policy (Thiago, 2026-11-08: the battery lasted ~7.5 h by day).
//
// - Screen: R2d2Display owns the backlight. It computes the brightness the
//   sun curve gives, keeps at least kAwakeMinBrightness for a few seconds
//   after a touch or a conversation, halves the result on battery
//   (kBatteryBrightnessFactor), and drops to kAsleepBrightness while the power
//   save timer has the board asleep (on battery, 60 s idle).
// - CPU: 240 MHz in a conversation, while media plays, or on the charger.
//   On battery and idle it runs at r2d2/idle_mhz (NVS, 80/160/240,
//   default 160). Only the wake word runs then.
// - Telemetry: asleep, cpu_mhz and the raw battery ADC go out with the status
//   poll (r2d2_core_client.cc → r2d2 agent_core/channels/satellite/metrics.py).
#pragma once

#include <atomic>

namespace r2d2::power {

constexpr float kBatteryBrightnessFactor = 0.5f;
constexpr int kAwakeMinBrightness = 20;  // legible after a touch at night
constexpr int kAsleepBrightness = 1;
constexpr int kDefaultIdleMhz = 160;

// The power save timer put the board to sleep (any task).
extern std::atomic<bool> asleep;
// Last averaged battery ADC reading, 0 = unknown (any task).
extern std::atomic<int> battery_adc;

// A conversation or media started (true) or ended (false).
void SetActive(bool active);
// The charge pin changed (or was first read).
void SetOnBattery(bool on_battery);
bool OnBattery();
int CpuMhz();  // the frequency in force

}  // namespace r2d2::power
