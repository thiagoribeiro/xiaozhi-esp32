#include "r2d2_nav.h"

#include "audio/audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "r2d2_core_client.h"
#include "r2d2_face.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <material_symbols.h>
#include <wifi_manager.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_20_4);
LV_FONT_DECLARE(font_material_symbols_16_4);
LV_FONT_DECLARE(font_material_symbols_20_4);

#define TAG "R2d2Nav"

namespace r2d2 {

namespace {

constexpr int kRows = 3;                      // list items per page on the round screen
constexpr int kRowTop[kRows] = {60, 102, 144};
constexpr int kRowH = 36, kRowW = 184;
constexpr uint32_t kHairline = 0x262E3F;
constexpr uint32_t kRowBg = 0x171C27;
constexpr int64_t kReloadAfterActionUs = 900 * 1000;  // let a switch report its new state
constexpr int64_t kDeviceRefreshUs = 2 * 1000 * 1000;
constexpr int64_t kLiveRefreshUs = 5 * 1000 * 1000;
constexpr int64_t kLoadTimeoutUs = 8 * 1000 * 1000;
constexpr int64_t kPlayerRefreshUs = 3 * 1000 * 1000;   // the queue may move on its own
constexpr int64_t kPlayerProgressUs = 1000 * 1000;      // arc and clock, interpolated locally
// player tap zones (screen pixels): the controls row; above it, play/pause
constexpr int kControlsTop = 146, kControlsBottom = 194;

std::string MinSec(float seconds) {
    int s = std::max(0, (int)seconds);
    char text[16];
    snprintf(text, sizeof(text), "%d:%02d", s / 60, s % 60);
    return text;
}

lv_obj_t* Plain(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, Rgb(color), 0);
    lv_label_set_text(label, "");
    return label;
}

uint32_t StateColor(const std::string& state) {
    if (state == "on") return 0x5FB88A;
    if (state == "warn") return 0xE0A83E;
    if (state == "crit") return kVoiceError;
    return kTextFaint;  // off
}

std::string Str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

void Show(lv_obj_t* obj, bool visible) {
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

NavView::NavView(lv_obj_t* parent, CoreClient* client) : client_(client) {
    root_ = Plain(parent);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));

    area_dots_ = Plain(root_);
    lv_obj_set_size(area_dots_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(area_dots_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(area_dots_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(area_dots_, 5, 0);
    lv_obj_align(area_dots_, LV_ALIGN_TOP_MID, 0, 12);

    depth_dots_ = Plain(root_);
    lv_obj_set_size(depth_dots_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(depth_dots_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(depth_dots_, 5, 0);
    lv_obj_align(depth_dots_, LV_ALIGN_RIGHT_MID, -8, 0);

    title_ = Label(root_, &font_noto_sans_basic_16_4, kTextDim);
    lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 30);

    // Loading: a turning arc in the listening cyan, the destination already in the title.
    spinner_ = lv_arc_create(root_);
    lv_obj_set_size(spinner_, 40, 40);
    lv_obj_align(spinner_, LV_ALIGN_CENTER, 0, 4);
    lv_obj_remove_style(spinner_, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(spinner_, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(spinner_, 0, 360);
    lv_arc_set_angles(spinner_, 0, 80);
    lv_obj_set_style_arc_width(spinner_, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner_, Rgb(kHairline), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner_, Rgb(kVoiceListen), LV_PART_INDICATOR);
    lv_anim_t spin;
    lv_anim_init(&spin);
    lv_anim_set_var(&spin, spinner_);
    lv_anim_set_exec_cb(&spin, [](void* obj, int32_t v) { lv_arc_set_rotation((lv_obj_t*)obj, v); });
    lv_anim_set_values(&spin, 0, 360);
    lv_anim_set_duration(&spin, 900);
    lv_anim_set_repeat_count(&spin, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&spin);

    note_ = Label(root_, &font_noto_sans_basic_16_4, kTextDim);
    lv_obj_set_width(note_, 170);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(note_, LV_LABEL_LONG_WRAP);
    lv_obj_align(note_, LV_ALIGN_CENTER, 0, 0);

    for (int i = 0; i < kRows; i++) {
        lv_obj_t* row = Plain(root_);
        lv_obj_set_size(row, kRowW, kRowH);
        lv_obj_set_pos(row, (240 - kRowW) / 2, kRowTop[i]);
        lv_obj_set_style_radius(row, kRowH / 2, 0);
        lv_obj_set_style_bg_color(row, Rgb(kRowBg), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, Rgb(kHairline), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_t* label = Label(row, &font_noto_sans_basic_16_4, kText);
        lv_obj_set_width(label, kRowW - 64);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_t* value = Label(row, &font_noto_sans_basic_16_4, kTextDim);
        lv_obj_align(value, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_t* dot = Plain(row);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, 5, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -16, 0);
        rows_.push_back(row);
        row_labels_.push_back(label);
        row_values_.push_back(value);
        row_dots_.push_back(dot);
    }

    metrics_ = Plain(root_);
    lv_obj_set_size(metrics_, 200, 124);
    lv_obj_align(metrics_, LV_ALIGN_CENTER, 0, 8);
    for (int i = 0; i < 4; i++) {
        lv_obj_t* value = Label(metrics_, &font_noto_sans_basic_20_4, kText);
        lv_obj_t* label = Label(metrics_, &font_noto_sans_basic_16_4, kTextDim);
        int x = i % 2 == 0 ? -50 : 50, y = i < 2 ? -32 : 30;
        lv_obj_align(value, LV_ALIGN_CENTER, x, y - 10);
        lv_obj_align(label, LV_ALIGN_CENTER, x, y + 14);
        metric_values_.push_back(value);
        metric_labels_.push_back(label);
    }

    chart_box_ = Plain(root_);
    lv_obj_set_size(chart_box_, LV_PCT(100), LV_PCT(100));
    chart_value_ = Label(chart_box_, &font_noto_sans_basic_20_4, kText);
    lv_obj_align(chart_value_, LV_ALIGN_TOP_MID, 0, 56);
    chart_ = lv_chart_create(chart_box_);
    lv_obj_set_size(chart_, 180, 62);
    lv_obj_align(chart_, LV_ALIGN_CENTER, 0, 12);
    lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart_, 3, 0);
    lv_obj_set_style_bg_opa(chart_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart_, 0, 0);
    lv_obj_set_style_pad_all(chart_, 0, 0);
    lv_obj_set_style_line_color(chart_, Rgb(kHairline), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart_, 3, LV_PART_ITEMS);
    lv_obj_set_style_width(chart_, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart_, 0, LV_PART_INDICATOR);
    chart_series_ = lv_chart_add_series(chart_, Rgb(kRainTint), LV_CHART_AXIS_PRIMARY_Y);
    chart_range_ = Label(chart_box_, &font_noto_sans_basic_16_4, kTextFaint);
    lv_obj_align(chart_range_, LV_ALIGN_CENTER, 0, 58);

    image_ = lv_image_create(root_);
    lv_obj_align(image_, LV_ALIGN_CENTER, 0, -2);
    caption_ = Label(root_, &font_noto_sans_basic_16_4, kTextDim);
    lv_obj_align(caption_, LV_ALIGN_CENTER, 0, 70);

    // ── player: progress arc on the rim (open at the top, under the area dots), track, artist, time, controls, volume ──
    player_ = Plain(root_);
    lv_obj_set_size(player_, LV_PCT(100), LV_PCT(100));
    player_arc_ = lv_arc_create(player_);
    lv_obj_remove_style(player_arc_, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(player_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(player_arc_, 226, 226);
    lv_obj_center(player_arc_);
    // The gap goes at the top, where the area dots are: from ~1 o'clock round
    // to ~11 o'clock (LVGL angles: 0 = 3 o'clock, clockwise; 270 = top).
    lv_arc_set_rotation(player_arc_, 300);
    lv_arc_set_bg_angles(player_arc_, 0, 300);
    lv_arc_set_range(player_arc_, 0, 1000);
    lv_obj_set_style_arc_width(player_arc_, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(player_arc_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(player_arc_, Rgb(kHairline), LV_PART_MAIN);
    lv_obj_set_style_arc_color(player_arc_, Rgb(kAccent), LV_PART_INDICATOR);
    // One line each (a long title wrapped over the artist, 2026-10-18): the
    // title scrolls when it doesn't fit, the artist ends in dots.
    player_track_ = Label(player_, &font_noto_sans_basic_20_4, kText);
    lv_obj_set_width(player_track_, 170);
    lv_obj_set_style_text_align(player_track_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(player_track_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(player_track_, LV_ALIGN_CENTER, 0, -38);
    player_artist_ = Label(player_, &font_noto_sans_basic_16_4, kTextDim);
    lv_obj_set_size(player_artist_, 160, 20);
    lv_obj_set_style_text_align(player_artist_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(player_artist_, LV_LABEL_LONG_DOT);
    lv_obj_align(player_artist_, LV_ALIGN_CENTER, 0, -12);
    player_time_ = Label(player_, &font_noto_sans_basic_16_4, kTextFaint);
    lv_obj_align(player_time_, LV_ALIGN_CENTER, 0, 12);
    player_prev_ = Label(player_, &font_material_symbols_20_4, kTextDim);
    lv_label_set_text(player_prev_, MATERIAL_SYMBOLS_SKIP_PREVIOUS);
    lv_obj_align(player_prev_, LV_ALIGN_CENTER, -60, 50);
    player_next_ = Label(player_, &font_material_symbols_20_4, kTextDim);
    lv_label_set_text(player_next_, MATERIAL_SYMBOLS_SKIP_NEXT);
    lv_obj_align(player_next_, LV_ALIGN_CENTER, 60, 50);
    player_toggle_ = Plain(player_);
    lv_obj_set_size(player_toggle_, 46, 46);
    lv_obj_set_style_radius(player_toggle_, 23, 0);
    lv_obj_set_style_bg_opa(player_toggle_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(player_toggle_, Rgb(kAccent), 0);
    lv_obj_align(player_toggle_, LV_ALIGN_CENTER, 0, 50);
    player_toggle_icon_ = Label(player_toggle_, &font_material_symbols_20_4, kBg);
    lv_obj_center(player_toggle_icon_);
    // volume lives in the queue screen (swipe up), not here

    // ── queue: the next three tracks, then two BIG volume buttons (- / +) ──
    queue_ = Plain(root_);
    lv_obj_set_size(queue_, LV_PCT(100), LV_PCT(100));
    for (int i = 0; i < 3; i++) {
        lv_obj_t* label = Label(queue_, &font_noto_sans_basic_16_4, i == 0 ? kText : kTextDim);
        lv_obj_set_size(label, 176, 20);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, -56 + i * 24);
        queue_tracks_.push_back(label);
    }
    queue_volume_ = Label(queue_, &font_noto_sans_basic_16_4, kTextDim);
    lv_obj_align(queue_volume_, LV_ALIGN_CENTER, 0, 22);
    for (int side = -1; side <= 1; side += 2) {
        lv_obj_t* button = Plain(queue_);
        lv_obj_set_size(button, 84, 56);
        lv_obj_set_style_radius(button, 18, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(button, Rgb(kRowBg), 0);
        lv_obj_set_style_border_color(button, Rgb(kAccent), 0);
        lv_obj_set_style_border_width(button, 2, 0);
        lv_obj_align(button, LV_ALIGN_CENTER, side * 48, 64);
        lv_obj_t* icon = Label(button, &font_material_symbols_20_4, kText);
        lv_label_set_text(icon, side < 0 ? MATERIAL_SYMBOLS_VOLUME_DOWN : MATERIAL_SYMBOLS_VOLUME_UP);
        lv_obj_center(icon);
    }

    footer_arrow_ = Label(root_, &font_material_symbols_16_4, kTextFaint);
    lv_label_set_text(footer_arrow_, MATERIAL_SYMBOLS_KEYBOARD_ARROW_UP);
    lv_obj_align(footer_arrow_, LV_ALIGN_BOTTOM_MID, 0, -40);
    footer_ = Label(root_, &font_noto_sans_basic_16_4, kTextFaint);
    lv_obj_align(footer_, LV_ALIGN_BOTTOM_MID, 0, -22);

    Render();
}

// ── data from the Core ───────────────────────────────────────────────────────

void NavView::SetNav(const cJSON* nav) {
    areas_.clear();
    const cJSON* areas = cJSON_GetObjectItem(nav, "areas");
    const cJSON* area = nullptr;
    cJSON_ArrayForEach(area, areas) {
        areas_.push_back({Str(area, "name"), Str(area, "screen")});
    }
    const cJSON* home = cJSON_GetObjectItem(nav, "home");
    const cJSON* timeout = cJSON_GetObjectItem(nav, "home_timeout_s");
    home_ = cJSON_IsNumber(home) ? std::clamp(home->valueint, 0, (int)areas_.size() - 1) : 0;
    if (cJSON_IsNumber(timeout)) home_timeout_us_ = (int64_t)timeout->valueint * 1000 * 1000;
    ESP_LOGI(TAG, "nav: %d areas, home %d", (int)areas_.size(), home_);
    if (!areas_.empty()) GoArea(home_);
}

void NavView::OnScreen(uint32_t seq, const std::string& id, cJSON* json) {
    if (seq != seq_) {  // the user moved on
        cJSON_Delete(json);
        return;
    }
    Screen s;
    s.id = id;
    s.loading = false;
    if (json == nullptr) {
        s.kind = "note";
        s.title = screen_.title;
        s.text = "O r2d2 não respondeu. Toque para tentar de novo.";
        s.failed = true;
    } else {
        s.kind = Str(json, "kind");
        s.title = Str(json, "title");
        s.next = Str(json, "next");
        s.next_name = Str(json, "next_name");
        s.text = Str(json, "text");
        s.image = Str(json, "image");
        s.caption = Str(json, "caption");
        s.state = Str(json, "state");
        s.track = Str(json, "track");
        s.artist = Str(json, "artist");
        const cJSON* num = cJSON_GetObjectItem(json, "elapsed");
        if (cJSON_IsNumber(num)) s.elapsed = (float)num->valuedouble;
        num = cJSON_GetObjectItem(json, "duration");
        if (cJSON_IsNumber(num)) s.duration = (float)num->valuedouble;
        num = cJSON_GetObjectItem(json, "volume");
        if (cJSON_IsNumber(num)) s.volume = num->valueint;
        const cJSON* acts = cJSON_GetObjectItem(json, "acts");
        const cJSON* act_entry = nullptr;
        cJSON_ArrayForEach(act_entry, acts) {
            if (!cJSON_IsObject(act_entry) || act_entry->string == nullptr) continue;
            char* text = cJSON_PrintUnformatted(act_entry);
            if (text) s.acts[act_entry->string] = text;
            cJSON_free(text);
        }
        const cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, cJSON_GetObjectItem(json, "items")) {
            Item item{Str(entry, "label"), Str(entry, "value"), Str(entry, "state"), Str(entry, "open"),
                      Str(entry, "area"), Str(entry, "local"), ""};
            const cJSON* act = cJSON_GetObjectItem(entry, "act");
            if (cJSON_IsObject(act)) {
                char* text = cJSON_PrintUnformatted(act);
                item.act = text ? text : "";
                cJSON_free(text);
            }
            s.items.push_back(std::move(item));
        }
        cJSON_ArrayForEach(entry, cJSON_GetObjectItem(json, "metrics")) {
            s.metrics.emplace_back(Str(entry, "value"), Str(entry, "label"));
        }
        const cJSON* points = cJSON_GetObjectItem(json, "points");
        if (cJSON_IsArray(points)) {
            const cJSON* decimals = cJSON_GetObjectItem(json, "decimals");
            const cJSON* last = cJSON_GetObjectItem(json, "last");
            int dec = cJSON_IsNumber(decimals) ? std::clamp(decimals->valueint, 0, 3) : 1;
            float lo = INFINITY, hi = -INFINITY;
            cJSON_ArrayForEach(entry, points) {
                float v = cJSON_IsNumber(entry) ? (float)entry->valuedouble : NAN;
                s.chart.push_back(v);
                if (!std::isnan(v)) {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            }
            char text[48];
            std::string unit = Str(json, "unit");
            if (cJSON_IsNumber(last)) {
                snprintf(text, sizeof(text), "%.*f %s", dec, last->valuedouble, unit.c_str());
                s.chart_value = text;
            }
            if (std::isfinite(lo)) {
                snprintf(text, sizeof(text), "mín %.*f · máx %.*f", dec, lo, dec, hi);
                s.chart_range = text;
            }
        }
        cJSON_Delete(json);
    }
    screen_ = std::move(s);
    page_ = std::min(restore_page_, Pages() - 1);
    restore_page_ = 0;
    image_data_.reset();
    lv_image_set_src(image_, nullptr);
    if (!screen_.image.empty()) client_->FetchImage(screen_.image, seq_);
    int64_t now_us = esp_timer_get_time();
    next_refresh_us_ = now_us + (screen_.id == "camera_live" ? kLiveRefreshUs
                                 : screen_.kind == "player" ? kPlayerRefreshUs : kDeviceRefreshUs);
    player_loaded_us_ = now_us;
    next_progress_us_ = now_us + kPlayerProgressUs;
    Render();
}

void NavView::OnImage(uint32_t seq, std::unique_ptr<LvglAllocatedImage> image) {
    if (seq != seq_ || !image || !image->IsValid()) return;
    image_data_ = std::move(image);
    lv_image_set_src(image_, image_data_->image_dsc());
}

void NavView::OnAction(uint32_t seq, bool ok) {
    if (seq != seq_) return;
    if (!ok) ESP_LOGW(TAG, "action failed on %s", screen_.id.c_str());
    // Reload either way: on success to show the real new state, on failure to undo the optimistic one.
    next_refresh_us_ = esp_timer_get_time() + kReloadAfterActionUs;
    screen_.loading = false;
    reload_pending_ = true;
}

// ── navigation ───────────────────────────────────────────────────────────────

void NavView::ShowArea(const std::string& screen) {
    for (size_t a = 0; a < areas_.size(); a++) {
        if (areas_[a].screen == screen) {
            last_touch_us_ = esp_timer_get_time();
            GoArea(a);
            return;
        }
    }
}

bool NavView::AtHome() const { return areas_.empty() || (area_ == home_ && stack_.size() <= 1); }

void NavView::GoArea(int area, const std::string& open, const std::string& open_title) {
    if (area < 0 || area >= (int)areas_.size()) return;
    area_ = area;
    stack_ = {areas_[area].screen};
    if (!open.empty()) stack_.push_back(open);
    Load(stack_.back(), false, open.empty() ? areas_[area].name : open_title);
}

void NavView::Push(const std::string& id, const std::string& title) {
    stack_.push_back(id);
    Load(id, false, title);
}

void NavView::Pop() {
    if (stack_.size() <= 1) return;
    stack_.pop_back();
    Load(stack_.back(), false, stack_.size() == 1 ? areas_[area_].name : "");
}

void NavView::Load(const std::string& id, bool quiet, const std::string& title) {
    seq_++;
    reload_pending_ = false;
    client_->FetchScreen(id, seq_);
    if (quiet) {
        restore_page_ = page_;
        return;
    }
    page_ = 0;
    restore_page_ = 0;
    screen_ = Screen{};
    screen_.id = id;
    screen_.title = title;
    loading_since_us_ = esp_timer_get_time();
    image_data_.reset();
    lv_image_set_src(image_, nullptr);
    Render();
}

int NavView::Pages() const {
    if (screen_.kind != "list" || IsQueue()) return 1;
    return std::max<int>(1, (screen_.items.size() + kRows - 1) / kRows);
}

void NavView::OnGesture(Gesture gesture, int x, int y) {
    last_touch_us_ = esp_timer_get_time();
    if (areas_.empty()) {
        client_->FetchNav();
        return;
    }
    switch (gesture) {
        case Gesture::kLeft:  // the content follows the finger: the next area comes in from the right
            GoArea(area_ + 1);
            break;
        case Gesture::kRight:
            GoArea(area_ - 1);
            break;
        case Gesture::kUp:
            if (page_ + 1 < Pages()) {
                page_++;
                Render();
            } else if (!screen_.next.empty()) {
                Push(screen_.next, screen_.next_name);
            }
            break;
        case Gesture::kDown:
            if (page_ > 0) {
                page_--;
                Render();
            } else {
                Pop();
            }
            break;
        case Gesture::kTap:
            Tap(x, y);
            break;
    }
}

void NavView::Tap(int x, int y) {
    if (screen_.failed) {
        Load(screen_.id, false, screen_.title);
        return;
    }
    if (screen_.id == "camera_live") {  // tap: a fresh frame now
        next_refresh_us_ = 0;
        return;
    }
    if (screen_.kind == "player") {
        TapPlayer(x, y);
        return;
    }
    if (IsQueue()) {
        TapQueue(x, y);
        return;
    }
    if (screen_.kind != "list") return;
    for (int i = 0; i < kRows; i++) {
        if (y < kRowTop[i] || y > kRowTop[i] + kRowH) continue;
        size_t index = page_ * kRows + i;
        if (index >= screen_.items.size()) return;
        Item& item = screen_.items[index];
        ESP_LOGI(TAG, "tap %s", item.label.c_str());
        if (!item.area.empty()) {
            for (size_t a = 0; a < areas_.size(); a++) {
                if (areas_[a].screen == item.area) {
                    GoArea(a, item.open, item.label);
                    return;
                }
            }
        } else if (!item.open.empty()) {
            Push(item.open, item.label);
        } else if (!item.act.empty()) {
            if (item.state == "on" || item.state == "off") item.state = item.state == "on" ? "off" : "on";
            client_->Action(item.act, seq_);
            Render();
        } else if (!item.local.empty()) {
            RunLocal(item);
        }
        return;
    }
}

void NavView::TapPlayer(int x, int y) {
    const char* key;
    if (y > kControlsBottom) {
        return;  // below the controls: the "Fila" hint, nothing to press by accident
    } else if (y >= kControlsTop && y <= kControlsBottom && x < 92) {
        key = "previous";
    } else if (y >= kControlsTop && y <= kControlsBottom && x > 148) {
        key = "next";
    } else {
        key = "toggle";  // the big play/pause button and the whole middle of the screen
    }
    auto act = screen_.acts.find(key);
    if (act == screen_.acts.end()) return;
    ESP_LOGI(TAG, "player %s", key);
    if (act->second.find("\"local\"") != std::string::npos) {  // the device's own speaker
        Item local;
        cJSON* json = cJSON_Parse(act->second.c_str());
        local.local = json ? Str(json, "local") : "";
        cJSON_Delete(json);
        RunLocal(local);
        return;
    }
    if (std::string(key) == "toggle" && screen_.state != "stop") {  // optimistic: the reload confirms it
        screen_.elapsed += (esp_timer_get_time() - player_loaded_us_) / 1e6f * (screen_.state == "play");
        player_loaded_us_ = esp_timer_get_time();
        screen_.state = screen_.state == "play" ? "pause" : "play";
    }
    client_->Action(act->second, seq_);
    Render();
}

void NavView::TapQueue(int x, int y) {
    if (y < 120) return;  // the track list: nothing to tap
    for (auto& item : screen_.items) {  // the bottom half is all volume: left = -, right = +
        if (item.local == (x < 120 ? "volume-" : "volume+")) {
            RunLocal(item);
            return;
        }
    }
}

void NavView::RenderQueue() {
    size_t shown = 0;
    for (const auto& item : screen_.items) {
        if (!item.local.empty() || shown >= queue_tracks_.size()) continue;
        lv_label_set_text(queue_tracks_[shown++], item.label.c_str());
    }
    for (; shown < queue_tracks_.size(); shown++) lv_label_set_text(queue_tracks_[shown], "");
    char text[24];
    snprintf(text, sizeof(text), "volume %d%%", Board::GetInstance().GetAudioCodec()->output_volume());
    lv_label_set_text(queue_volume_, text);
}

void NavView::RunLocal(Item& item) {
    auto& board = Board::GetInstance();
    if (item.local == "volume+" || item.local == "volume-") {
        auto codec = board.GetAudioCodec();
        int v = std::clamp(codec->output_volume() + (item.local == "volume+" ? 10 : -10), 10, 100);
        codec->SetOutputVolume(v);
    } else if (item.local == "mic+" || item.local == "mic-") {
        auto codec = board.GetAudioCodec();
        int step = item.local == "mic+" ? kMicGainStepDb : -kMicGainStepDb;
        int gain = std::clamp((int)std::lround(codec->input_gain()) + step, kMinMicGainDb, kMaxMicGainDb);
        codec->SetInputGain(gain);
        Settings("r2d2", true).SetInt("mic_gain", gain);
    } else if (item.local == "bright+" || item.local == "bright-") {
        auto backlight = board.GetBacklight();
        if (backlight) {
            int b = std::clamp(backlight->brightness() + (item.local == "bright+" ? 15 : -15), 10, 100);
            backlight->SetBrightness(b, true);
        }
    }
    Render();
}

void NavView::Tick(int64_t now_us) {
    if (areas_.empty()) return;
    bool playing_here = screen_.kind == "player" && screen_.state == "play";  // the player stays while music plays
    if (!AtHome() && !playing_here && last_touch_us_ && now_us - last_touch_us_ > home_timeout_us_) {
        GoArea(home_);
        return;
    }
    if (screen_.loading && now_us - loading_since_us_ > kLoadTimeoutUs) {
        seq_++;  // a late answer no longer counts
        screen_.loading = false;
        screen_.kind = "note";
        screen_.text = "O r2d2 não respondeu. Toque para tentar de novo.";
        screen_.failed = true;
        Render();
        return;
    }
    if (screen_.kind == "player" && !screen_.loading && now_us >= next_progress_us_) {
        next_progress_us_ = now_us + kPlayerProgressUs;
        RenderPlayerProgress(now_us);
    }
    if (now_us < next_refresh_us_ || screen_.loading) return;
    if (reload_pending_) {
        Load(screen_.id, true);
    } else if (screen_.kind == "device") {
        next_refresh_us_ = now_us + kDeviceRefreshUs;
        Render();
    } else if (screen_.kind == "player") {
        next_refresh_us_ = now_us + kPlayerRefreshUs;
        Load(screen_.id, true);  // quiet: the player stays on screen while it reloads
    } else if (screen_.id == "camera_live" && !screen_.image.empty()) {
        next_refresh_us_ = now_us + kLiveRefreshUs;
        client_->FetchImage(screen_.image, seq_);
    }
}

// ── drawing ──────────────────────────────────────────────────────────────────

void NavView::RenderDots() {
    lv_obj_clean(area_dots_);
    for (size_t i = 0; i < areas_.size(); i++) {
        lv_obj_t* dot = Plain(area_dots_);
        bool on = (int)i == area_;
        lv_obj_set_size(dot, on ? 14 : 6, 6);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, Rgb(on ? ((int)i == home_ ? kAccent : kText) : kTextFaint), 0);
    }
    lv_obj_clean(depth_dots_);
    int depth = stack_.size() + Pages() - 1;
    if (depth > 1) {
        int current = stack_.size() - 1 + page_;
        for (int i = 0; i < depth && i < 6; i++) {
            lv_obj_t* dot = Plain(depth_dots_);
            lv_obj_set_size(dot, 6, i == current ? 14 : 6);
            lv_obj_set_style_radius(dot, 3, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, Rgb(i == current ? kText : kTextFaint), 0);
        }
    }
}

void NavView::Render() {
    RenderDots();
    const std::string& kind = screen_.kind;
    Show(note_, !screen_.loading && kind == "note");
    Show(spinner_, screen_.loading);
    Show(metrics_, kind == "metrics" || kind == "device");
    Show(chart_box_, kind == "chart");
    Show(image_, kind == "photo");
    Show(caption_, kind == "photo");
    Show(player_, !screen_.loading && kind == "player");
    Show(queue_, !screen_.loading && IsQueue());
    for (auto row : rows_) Show(row, false);

    std::string title = screen_.title;
    if (Pages() > 1) {
        char text[32];
        snprintf(text, sizeof(text), " · %d/%d", page_ + 1, Pages());
        title += text;
    }
    lv_label_set_text(title_, title.c_str());

    if (screen_.loading) {
        lv_label_set_text(footer_, "");
        Show(footer_arrow_, false);
        return;
    }
    if (kind == "note") lv_label_set_text(note_, screen_.text.c_str());
    if (kind == "list" && !IsQueue()) RenderList();
    if (IsQueue()) RenderQueue();
    if (kind == "metrics") RenderMetrics();
    if (kind == "device") RenderDevice();
    if (kind == "chart") RenderChart();
    if (kind == "photo") lv_label_set_text(caption_, screen_.caption.c_str());
    if (kind == "player") RenderPlayer();

    std::string footer = page_ + 1 < Pages() ? "mais" : screen_.next_name;
    lv_label_set_text(footer_, footer.c_str());
    Show(footer_arrow_, !footer.empty());
}

void NavView::RenderList() {
    auto& board = Board::GetInstance();
    for (int i = 0; i < kRows; i++) {
        size_t index = page_ * kRows + i;
        if (index >= screen_.items.size()) continue;
        const Item& item = screen_.items[index];
        Show(rows_[i], true);
        lv_label_set_text(row_labels_[i], item.label.c_str());
        std::string value = item.value;
        if (item.local == "volume+" || item.local == "volume-") {
            value = std::to_string(board.GetAudioCodec()->output_volume());
        } else if (item.local == "mic+" || item.local == "mic-") {
            value = std::to_string((int)std::lround(board.GetAudioCodec()->input_gain())) + " dB";
        } else if ((item.local == "bright+" || item.local == "bright-") && board.GetBacklight()) {
            value = std::to_string(board.GetBacklight()->brightness());
        } else if (!item.open.empty() || !item.area.empty()) {
            value = ">";
        }
        bool dot = !item.state.empty();
        lv_label_set_text(row_values_[i], dot ? "" : value.c_str());
        Show(row_dots_[i], dot);
        if (dot) lv_obj_set_style_bg_color(row_dots_[i], Rgb(StateColor(item.state)), 0);
    }
}

void NavView::RenderPlayer() {
    bool stopped = screen_.state != "play" && screen_.state != "pause";
    lv_label_set_text(player_track_, screen_.track.c_str());
    lv_label_set_text(player_artist_, screen_.artist.c_str());
    lv_obj_set_style_text_color(player_track_, Rgb(stopped ? kTextDim : kText), 0);
    lv_label_set_text(player_toggle_icon_, screen_.state == "play" ? MATERIAL_SYMBOLS_PAUSE : MATERIAL_SYMBOLS_PLAY_ARROW);
    lv_obj_set_style_bg_color(player_toggle_, Rgb(stopped ? kHairline : kAccent), 0);
    Show(player_prev_, !stopped);
    Show(player_next_, !stopped);
    Show(player_arc_, !stopped);
    lv_obj_set_style_arc_color(player_arc_, Rgb(screen_.state == "play" ? kAccent : kTextFaint), LV_PART_INDICATOR);
    RenderPlayerProgress(esp_timer_get_time());
}

void NavView::RenderPlayerProgress(int64_t now_us) {
    if (screen_.state != "play" && screen_.state != "pause") {
        lv_label_set_text(player_time_, "");
        return;
    }
    float elapsed = screen_.elapsed;
    if (screen_.state == "play") elapsed += (now_us - player_loaded_us_) / 1e6f;
    if (screen_.duration > 0) elapsed = std::min(elapsed, screen_.duration);
    std::string text = MinSec(elapsed);
    if (screen_.duration > 0) text += " / " + MinSec(screen_.duration);
    lv_label_set_text(player_time_, text.c_str());
    lv_arc_set_value(player_arc_, screen_.duration > 0 ? (int)(1000 * elapsed / screen_.duration) : 0);
}

void NavView::RenderMetrics() {
    for (size_t i = 0; i < metric_values_.size(); i++) {
        bool has = i < screen_.metrics.size();
        lv_label_set_text(metric_values_[i], has ? screen_.metrics[i].first.c_str() : "");
        lv_label_set_text(metric_labels_[i], has ? screen_.metrics[i].second.c_str() : "");
    }
}

void NavView::RenderDevice() {
    auto& board = Board::GetInstance();
    auto& wifi = WifiManager::GetInstance();
    char rssi[16], battery[16], uptime[16];
    snprintf(rssi, sizeof(rssi), "%d", wifi.IsConnected() ? wifi.GetRssi() : 0);
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        snprintf(battery, sizeof(battery), charging ? "%d%%+" : "%d%%", level);
    } else {
        snprintf(battery, sizeof(battery), "--");
    }
    int64_t minutes = esp_timer_get_time() / 60000000;
    if (minutes < 60) snprintf(uptime, sizeof(uptime), "%d min", (int)minutes);
    else snprintf(uptime, sizeof(uptime), "%d h", (int)(minutes / 60));
    screen_.metrics = {{rssi, "Wi-Fi dBm"}, {battery, "bateria"},
                       {client_->online() ? "online" : "offline", "r2d2"}, {uptime, "ligado há"}};
    RenderMetrics();
}

void NavView::RenderChart() {
    int n = screen_.chart.size();
    if (n < 2) return;
    float lo = INFINITY, hi = -INFINITY;
    for (float v : screen_.chart) {
        if (std::isnan(v)) continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (!std::isfinite(lo)) return;
    const float factor = 10.0f;  // one decimal is plenty at 180 px
    float pad = std::max((hi - lo) * 0.1f, 0.1f);
    lv_chart_set_point_count(chart_, n);
    lv_chart_set_axis_range(chart_, LV_CHART_AXIS_PRIMARY_Y, (int32_t)std::floor((lo - pad) * factor),
                            (int32_t)std::ceil((hi + pad) * factor));
    for (int i = 0; i < n; i++) {
        float v = screen_.chart[i];
        lv_chart_set_series_value_by_id(chart_, chart_series_, i,
                                        std::isnan(v) ? LV_CHART_POINT_NONE : (int32_t)std::lround(v * factor));
    }
    lv_chart_refresh(chart_);
    lv_label_set_text(chart_value_, screen_.chart_value.c_str());
    lv_label_set_text(chart_range_, screen_.chart_range.c_str());
}

}  // namespace r2d2
