#include "r2d2_forecast.h"

#include "r2d2_face.h"  // Rgb, kBg, kText, kTextDim, kTextFaint

#include <algorithm>
#include <cstdio>

LV_FONT_DECLARE(font_noto_sans_basic_16_4);

namespace r2d2 {

namespace {

constexpr uint32_t kHairline = 0x262E3F;
constexpr float kHours = 6.0f;
constexpr int kLabelH = 20;   // value labels above the plot / tick labels below (detailed)

lv_point_precise_t P(float x, float y) {
    return {(lv_value_precise_t)x, (lv_value_precise_t)y};
}

float Num(const cJSON* item, float fallback = NAN) {
    return cJSON_IsNumber(item) ? (float)item->valuedouble : fallback;
}

}  // namespace

bool Forecast::Parse(const cJSON* json) {
    points.clear();
    axis.clear();
    now = NAN;
    anchored = false;
    if (!cJSON_IsObject(json)) return false;
    now = Num(cJSON_GetObjectItem(json, "now"));
    anchored = cJSON_IsTrue(cJSON_GetObjectItem(json, "anchored"));
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(json, "points")) {
        if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) < 2) continue;
        Point p{Num(cJSON_GetArrayItem(entry, 0)), Num(cJSON_GetArrayItem(entry, 1)), NAN, NAN};
        p.lo = Num(cJSON_GetArrayItem(entry, 2), p.t);
        p.hi = Num(cJSON_GetArrayItem(entry, 3), p.t);
        if (std::isfinite(p.h) && std::isfinite(p.t) && p.h > 0 && p.h <= kHours) points.push_back(p);
    }
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.h < b.h; });
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(json, "axis")) {
        const cJSON* h = cJSON_GetArrayItem(entry, 0);
        const cJSON* label = cJSON_GetArrayItem(entry, 1);
        if (cJSON_IsNumber(h) && cJSON_IsString(label)) axis.emplace_back((float)h->valuedouble, label->valuestring);
    }
    return !points.empty();
}

ForecastCurve::ForecastCurve(lv_obj_t* parent, int w, int h, bool detailed) : w_(w), h_(h), detailed_(detailed) {
    box_ = lv_obj_create(parent);
    lv_obj_remove_style_all(box_);
    lv_obj_set_size(box_, w, h);
    lv_obj_remove_flag(box_, LV_OBJ_FLAG_CLICKABLE);
    buf_ = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    canvas_ = lv_canvas_create(box_);
    lv_canvas_set_draw_buf(canvas_, buf_);
    lv_canvas_fill_bg(canvas_, Rgb(kBg), LV_OPA_COVER);
}

ForecastCurve::~ForecastCurve() {
    if (box_) lv_obj_delete(box_);  // the canvas and labels go with it
    if (buf_) lv_draw_buf_destroy(buf_);
}

lv_obj_t* ForecastCurve::AddLabel(const char* text, uint32_t color, int x, int y, int width, lv_text_align_t align) {
    lv_obj_t* label = lv_label_create(box_);
    lv_obj_set_style_text_font(label, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(label, Rgb(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_width(label, width);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    labels_.push_back(label);
    return label;
}

void ForecastCurve::Draw(const Forecast& f) {
    for (auto label : labels_) lv_obj_delete(label);
    labels_.clear();
    lv_canvas_fill_bg(canvas_, Rgb(kBg), LV_OPA_COVER);
    if (f.empty()) return;

    // now + the forecast; without an anchor (no fresh station reading) the
    // curve starts at the first forecast point, as on the HUD
    std::vector<Forecast::Point> s;
    float start = (f.anchored && std::isfinite(f.now)) ? f.now : f.points.front().t;
    s.push_back({0, start, start, start});
    s.insert(s.end(), f.points.begin(), f.points.end());

    float lo = INFINITY, hi = -INFINITY;
    for (const auto& p : s) {
        lo = std::min(lo, p.lo);
        hi = std::max(hi, p.hi);
    }
    float span = std::max(hi - lo, 1.0f);
    float vlo = lo - span * 0.05f, vhi = hi + span * 0.05f;
    int inset = detailed_ ? 4 : 1;   // room for the dots at the ends
    float x0 = inset, x1 = w_ - 1 - inset;
    float top = detailed_ ? kLabelH : 1, bottom = detailed_ ? h_ - kLabelH - 2 : h_ - 2;
    auto px = [&](float h) { return x0 + h / kHours * (x1 - x0); };
    auto py = [&](float v) { return bottom - (v - vlo) / (vhi - vlo) * (bottom - top); };

    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);

    // the 80 % band: two triangles per segment
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = Rgb(f.anchored ? kTempBand : kHairline);
    tri.opa = LV_OPA_COVER;
    for (size_t i = 0; i + 1 < s.size(); i++) {
        float xa = px(s[i].h), xb = px(s[i + 1].h);
        float ua = py(s[i].hi), ub = py(s[i + 1].hi), la = py(s[i].lo), lb = py(s[i + 1].lo);
        tri.p[0] = P(xa, ua);
        tri.p[1] = P(xb, ub);
        tri.p[2] = P(xb, lb);
        lv_draw_triangle(&layer, &tri);
        tri.p[0] = P(xa, ua);
        tri.p[1] = P(xb, lb);
        tri.p[2] = P(xa, la);
        lv_draw_triangle(&layer, &tri);
    }

    if (detailed_) {  // baseline under the plot
        lv_draw_line_dsc_t base;
        lv_draw_line_dsc_init(&base);
        base.color = Rgb(kHairline);
        base.width = 1;
        base.p1 = P(x0, bottom + 2);
        base.p2 = P(x1, bottom + 2);
        lv_draw_line(&layer, &base);
    }

    uint32_t line_color = f.anchored ? kTempTint : kTextDim;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = Rgb(line_color);
    line.width = detailed_ ? 3 : 2;
    line.round_start = 1;
    line.round_end = 1;
    for (size_t i = 0; i + 1 < s.size(); i++) {
        line.p1 = P(px(s[i].h), py(s[i].t));
        line.p2 = P(px(s[i + 1].h), py(s[i + 1].t));
        lv_draw_line(&layer, &line);
    }

    if (detailed_) {
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.radius = LV_RADIUS_CIRCLE;
        for (size_t i = 0; i < s.size(); i++) {
            int x = (int)std::lround(px(s[i].h)), y = (int)std::lround(py(s[i].t));
            dot.bg_color = Rgb(i == 0 ? kText : line_color);
            lv_area_t area = {(int32_t)(x - 3), (int32_t)(y - 3), (int32_t)(x + 3), (int32_t)(y + 3)};
            lv_draw_rect(&layer, &dot, &area);
        }
    }
    lv_canvas_finish_layer(canvas_, &layer);

    if (!detailed_) return;
    char text[16];
    for (const auto& p : f.points) {
        int h = (int)std::lround(p.h);
        if (h != 1 && h != 3 && h != 6) continue;   // +2 h would crowd +1 h at this width
        snprintf(text, sizeof(text), "%.0f°", p.t);
        int x = (int)std::lround(px(p.h));
        int y = (int)std::lround(std::min(py(p.t), py(p.hi))) - kLabelH;
        bool right = p.h >= kHours - 0.01f;
        AddLabel(text, f.anchored ? kText : kTextDim, right ? x - 44 + 4 : x - 22, std::max(0, y), 44,
                 right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER);
    }
    for (const auto& [h, label] : f.axis) {
        int x = (int)std::lround(px(h));
        lv_text_align_t align = h <= 0.01f ? LV_TEXT_ALIGN_LEFT
                              : h >= kHours - 0.01f ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
        int lx = align == LV_TEXT_ALIGN_LEFT ? 0 : align == LV_TEXT_ALIGN_RIGHT ? w_ - 60 : x - 30;
        AddLabel(label.c_str(), kTextFaint, lx, h_ - kLabelH, 60, align);
    }
    if (!f.anchored) AddLabel("modelo", kTextFaint, w_ - 60, 0, 60, LV_TEXT_ALIGN_RIGHT);
}

}  // namespace r2d2
