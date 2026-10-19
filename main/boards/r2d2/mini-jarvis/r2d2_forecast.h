// The next 6 h of temperature at the R2D2's weather station, as on the HUD's
// weather card: the Core's anchored forecast (r2d2 repo, agent_core/forecast/
// thermal_baselines.py) drawn as a line over its 80 % band.
//
// Used twice: the "Próximas 6 h" nav screen (detailed: dots, values, hour
// ticks) and a small curve next to the temperature on the idle screen.
//
// JSON (the Core's satellite screens.thermal_forecast / status weather.forecast):
//   {"now": 24.3, "anchored": true,
//    "points": [[h, temp, lo, hi], ...],          h in hours, 0 < h <= 6
//    "axis":   [[h, "label"], ...]}               tick labels, text from the Core
#pragma once

#include <cJSON.h>
#include <lvgl.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace r2d2 {

constexpr uint32_t kTempTint = 0xE87A4C;  // HUD colors.TEMP_TINT
constexpr uint32_t kTempBand = 0x3C2A28;  // HUD colors.TEMP_BAND (TEMP_TINT ~20 % over the background)

struct Forecast {
    struct Point {
        float h, t, lo, hi;
    };
    float now = NAN;
    bool anchored = false;
    std::vector<Point> points;
    std::vector<std::pair<float, std::string>> axis;

    bool Parse(const cJSON* json);  // false (and empty) when there is nothing to draw
    bool empty() const { return points.empty(); }
};

class ForecastCurve {
public:
    // detailed: dots, the value over +1/+3/+6 h and the axis labels (needs
    // ~22 px above and below the plot inside w x h); otherwise just band + line.
    ForecastCurve(lv_obj_t* parent, int w, int h, bool detailed);
    ~ForecastCurve();

    void Draw(const Forecast& forecast);
    lv_obj_t* obj() const { return box_; }

private:
    lv_obj_t* AddLabel(const char* text, uint32_t color, int x, int y, int width, lv_text_align_t align);

    int w_, h_;
    bool detailed_;
    lv_obj_t* box_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_draw_buf_t* buf_ = nullptr;
    std::vector<lv_obj_t*> labels_;
};

}  // namespace r2d2
