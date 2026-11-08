#include "r2d2_power.h"

#include "settings.h"

#include <esp_log.h>
#include <sdkconfig.h>
#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

#include <mutex>

#define TAG "R2d2Power"

namespace r2d2::power {

std::atomic<bool> asleep{false};
std::atomic<int> battery_adc{0};

namespace {

std::mutex mutex_;
bool active_ = true;       // until the app first says LOW_POWER (boot, activation)
bool on_battery_ = false;
std::atomic<int> cpu_mhz_{CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ};

int IdleMhz() {
    int mhz = Settings("r2d2", false).GetInt("idle_mhz", kDefaultIdleMhz);
    return (mhz == 80 || mhz == 160 || mhz == 240) ? mhz : kDefaultIdleMhz;
}

void Apply() {  // mutex_ held
    int mhz = (active_ || !on_battery_) ? 240 : IdleMhz();
    if (mhz == cpu_mhz_) return;
#if CONFIG_PM_ENABLE
    // Fixed frequency (min = max), no light sleep: the mic and Wi-Fi never stop.
    esp_pm_config_t config = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CPU %d MHz refused: %s", mhz, esp_err_to_name(err));
        return;
    }
    cpu_mhz_ = mhz;
    ESP_LOGI(TAG, "CPU %d MHz (%s, %s)", mhz, active_ ? "active" : "idle",
             on_battery_ ? "battery" : "charger");
#endif
}

}  // namespace

void SetActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = active;
    Apply();
}

void SetOnBattery(bool on_battery) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_battery_ = on_battery;
    Apply();
}

bool OnBattery() {
    std::lock_guard<std::mutex> lock(mutex_);
    return on_battery_;
}

int CpuMhz() { return cpu_mhz_; }

}  // namespace r2d2::power
