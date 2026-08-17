#include "skin_manager.h"
#include "common/com_utils.h"

#include <Windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

#pragma comment(lib, "gdiplus.lib")

namespace shuru {
namespace {

COLORREF ParseColor(const std::string& str, COLORREF fallback) {
    if (str.empty()) return fallback;
    try {
        std::string s = str;
        // 去除空格和引号
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) {
            return c == ' ' || c == '\t' || c == '\"' || c == '\'';
        }), s.end());

        if (s.rfind("#", 0) == 0) s = "0x" + s.substr(1);
        unsigned long val = 0;
        if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
            val = std::stoul(s.substr(2), nullptr, 16);
        } else {
            val = std::stoul(s, nullptr, 10);
        }
        // 如果是 6 位十六进制，按 0xRRGGBB 解析
        uint8_t r = static_cast<uint8_t>((val >> 16) & 0xFF);
        uint8_t g = static_cast<uint8_t>((val >> 8) & 0xFF);
        uint8_t b = static_cast<uint8_t>(val & 0xFF);
        return RGB(r, g, b);
    } catch (...) {
        return fallback;
    }
}

std::vector<int> ParseIntList(const std::string& str) {
    std::vector<int> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        try {
            item.erase(std::remove_if(item.begin(), item.end(), [](char c) {
                return c == ' ' || c == '\t';
            }), item.end());
            if (!item.empty()) {
                result.push_back(std::stoi(item));
            }
        } catch (...) {}
    }
    return result;
}

bool ParseBool(const std::string& value, bool fallback) {
    std::string normalized = value;
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
        [](unsigned char c) { return std::isspace(c) != 0; }), normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized == "1" || normalized == "true" || normalized == "yes") return true;
    if (normalized == "0" || normalized == "false" || normalized == "no") return false;
    return fallback;
}

bool IsValidSkinId(const std::wstring& value) {
    if (value.empty() || value.size() > 128 || value == L"." || value == L"..") {
        return false;
    }
    return value.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos &&
           std::none_of(value.begin(), value.end(), [](wchar_t ch) {
               return ch < 0x20;
           });
}

int ParseBoundedInt(const std::string& value, int fallback, int minimum, int maximum) {
    try {
        return (std::max)(minimum, (std::min)(maximum, std::stoi(value)));
    } catch (...) {
        return fallback;
    }
}

std::wstring GetModuleDirectory() {
    HMODULE hModule = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetModuleDirectory),
        &hModule);
    if (hModule == nullptr) return {};
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(hModule, path, MAX_PATH);
    std::wstring str(path);
    const auto pos = str.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring{} : str.substr(0, pos);
}

std::wstring GetUserDataSkinsDirectory() {
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (length == 0) return {};
    std::wstring root(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), length);
    if (written == 0 || written >= length) return {};
    root.resize(written);
    return root + L"\\CaishenPinyin\\skins";
}

}  // namespace

SkinManager& SkinManager::Instance() {
    static SkinManager s_instance;
    return s_instance;
}

SkinManager::SkinManager() {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
        gdiplus_token_ = reinterpret_cast<void*>(token);
    }
}

SkinManager::~SkinManager() {
    CleanupGdiResources();
    if (gdiplus_token_ != nullptr) {
        Gdiplus::GdiplusShutdown(reinterpret_cast<ULONG_PTR>(gdiplus_token_));
        gdiplus_token_ = nullptr;
    }
}

void SkinManager::CleanupGdiResources() {
    for (void* frame : bg_frames_) {
        delete reinterpret_cast<Gdiplus::Bitmap*>(frame);
    }
    bg_frames_.clear();
    frame_delays_ms_.clear();
    current_frame_ = 0;
    current_theme_.has_bg_image = false;
}

void SkinManager::EnsureSkin(const std::wstring& skin_id) {
    const std::wstring target_id = IsValidSkinId(skin_id)
        ? skin_id : std::wstring(L"classic_blue");
    if (target_id == current_skin_id_ && (!bg_frames_.empty() || !current_theme_.has_bg_image)) {
        return;
    }

    CleanupGdiResources();
    current_skin_id_ = target_id;

    // 内置资源优先，避免用户目录中的同名旧副本覆盖受版本管理的官方皮肤。
    std::wstring mod_dir = GetModuleDirectory();
    std::vector<std::wstring> candidates = {
        mod_dir + L"\\data\\skins\\" + target_id,
        mod_dir + L"\\..\\data\\skins\\" + target_id,
        mod_dir + L"\\skins\\" + target_id
    };

    for (const auto& dir : candidates) {
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            LoadFromDirectory(dir, target_id);
            return;
        }
    }

    const std::wstring user_dir = GetUserDataSkinsDirectory() + L"\\" + target_id;
    if (GetFileAttributesW(user_dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        LoadFromDirectory(user_dir, target_id);
        return;
    }

    // 配置被外部破坏时仍提供可用候选框；设置页会同步修正 SkinId。
    if (target_id != L"classic_blue") {
        for (const auto& dir : std::vector<std::wstring> {
                 mod_dir + L"\\data\\skins\\classic_blue",
                 mod_dir + L"\\..\\data\\skins\\classic_blue",
                 mod_dir + L"\\skins\\classic_blue"}) {
            if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
                LoadFromDirectory(dir, L"classic_blue");
                return;
            }
        }
    }

    current_theme_ = SkinTheme{};
    current_theme_.id = target_id;
}

void SkinManager::ReloadSkin(const std::wstring& skin_id) {
    current_skin_id_.clear();
    EnsureSkin(skin_id);
}

void SkinManager::LoadFromDirectory(const std::wstring& dir_path, const std::wstring& skin_id) {
    current_theme_ = SkinTheme{};
    current_theme_.id = skin_id;

    std::wstring ini_path = dir_path + L"\\skin.ini";
    std::ifstream file(ini_path, std::ios::binary);
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    // 简单解析 INI
    std::stringstream ss(content);
    std::string line;
    std::string section;
    std::map<std::string, std::map<std::string, std::string>> ini;

    while (std::getline(ss, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
        } else {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string k = line.substr(0, pos);
                std::string v = line.substr(pos + 1);
                k.erase(0, k.find_first_not_of(" \t"));
                k.erase(k.find_last_not_of(" \t") + 1);
                v.erase(0, v.find_first_not_of(" \t"));
                v.erase(v.find_last_not_of(" \t") + 1);
                ini[section][k] = v;
            }
        }
    }

    // 读取 [General]
    if (ini.count("General")) {
        auto& g = ini["General"];
        if (g.count("name")) current_theme_.name = Utf8ToWide(g["name"]);
        if (g.count("author")) current_theme_.author = Utf8ToWide(g["author"]);
        if (g.count("info")) current_theme_.info = Utf8ToWide(g["info"]);
    }

    // 读取 [Display]
    if (ini.count("Display")) {
        auto& d = ini["Display"];
        if (d.count("font_family")) current_theme_.font_family = Utf8ToWide(d["font_family"]);
        if (d.count("font_size")) {
            try { current_theme_.font_size = std::stoi(d["font_size"]); } catch (...) {}
        }
        if (d.count("pinyin_color")) current_theme_.pinyin_color = ParseColor(d["pinyin_color"], current_theme_.pinyin_color);
        if (d.count("candidate_color")) current_theme_.candidate_color = ParseColor(d["candidate_color"], current_theme_.candidate_color);
        if (d.count("highlight_color")) current_theme_.highlight_color = ParseColor(d["highlight_color"], current_theme_.highlight_color);
        if (d.count("highlight_bg_color")) current_theme_.highlight_bg_color = ParseColor(d["highlight_bg_color"], current_theme_.highlight_bg_color);
        if (d.count("index_color")) current_theme_.index_color = ParseColor(d["index_color"], current_theme_.index_color);
        if (d.count("status_text_color")) current_theme_.status_text_color = ParseColor(d["status_text_color"], current_theme_.status_text_color);
        if (d.count("separator_color")) current_theme_.separator_color = ParseColor(d["separator_color"], current_theme_.separator_color);
    }

    // 读取 [Scheme_H1]
    std::string bg_filename = "cand_bg.png";
    if (ini.count("Scheme_H1")) {
        auto& s = ini["Scheme_H1"];
        if (s.count("bg_image")) bg_filename = s["bg_image"];
        else if (s.count("pic")) bg_filename = s["pic"];

        if (s.count("layout_horizontal")) {
            auto list = ParseIntList(s["layout_horizontal"]);
            if (list.size() >= 3) {
                current_theme_.slice.left = list[1];
                current_theme_.slice.right = list[2];
            }
        }
        if (s.count("layout_vertical")) {
            auto list = ParseIntList(s["layout_vertical"]);
            if (list.size() >= 3) {
                current_theme_.slice.top = list[1];
                current_theme_.slice.bottom = list[2];
            }
        }
        if (s.count("pinyin_margin") || s.count("pinyin_marge")) {
            auto list = ParseIntList(s.count("pinyin_margin") ? s["pinyin_margin"] : s["pinyin_marge"]);
            if (list.size() >= 4) {
                current_theme_.pinyin_margin.top = list[0];
                current_theme_.pinyin_margin.bottom = list[1];
                current_theme_.pinyin_margin.left = list[2];
                current_theme_.pinyin_margin.right = list[3];
            }
        }
        if (s.count("candidate_margin") || s.count("zhongwen_marge")) {
            auto list = ParseIntList(s.count("candidate_margin") ? s["candidate_margin"] : s["zhongwen_marge"]);
            if (list.size() >= 4) {
                current_theme_.candidate_margin.top = list[0];
                current_theme_.candidate_margin.bottom = list[1];
                current_theme_.candidate_margin.left = list[2];
                current_theme_.candidate_margin.right = list[3];
            }
        }
        if (s.count("corner_radius")) {
            try { current_theme_.corner_radius = std::stoi(s["corner_radius"]); } catch (...) {}
        }
        if (s.count("has_shadow")) {
            current_theme_.has_shadow = ParseBool(s["has_shadow"], current_theme_.has_shadow);
        }
        if (s.count("native_appearance")) {
            current_theme_.native_appearance = ParseBool(
                s["native_appearance"], current_theme_.native_appearance);
        }
        if (s.count("show_separator")) {
            current_theme_.show_separator = ParseBool(
                s["show_separator"], current_theme_.show_separator);
        }
    }

    std::vector<std::pair<std::string, UINT>> configured_frames;
    if (ini.count("Animation")) {
        auto& animation = ini["Animation"];
        const int frame_count = animation.count("frame_count")
            ? ParseBoundedInt(animation["frame_count"], 0, 0, 240)
            : 0;
        for (int index = 0; index < frame_count; ++index) {
            const std::string frame_key = "frame_" + std::to_string(index);
            if (!animation.count(frame_key)) break;
            const std::string delay_key = "delay_" + std::to_string(index);
            const UINT delay = static_cast<UINT>(animation.count(delay_key)
                ? ParseBoundedInt(animation[delay_key], 100, 10, 10000)
                : 100);
            configured_frames.emplace_back(animation[frame_key], delay);
        }
    }
    if (configured_frames.empty()) configured_frames.emplace_back(bg_filename, 100);

    for (const auto& [file_name, delay] : configured_frames) {
        std::wstring img_path = dir_path + L"\\" + Utf8ToWide(file_name);
        if (GetFileAttributesW(img_path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        auto* bitmap = new Gdiplus::Bitmap(img_path.c_str());
        if (bitmap->GetLastStatus() != Gdiplus::Ok ||
            bitmap->GetWidth() == 0 || bitmap->GetHeight() == 0) {
            delete bitmap;
            continue;
        }
        if (bg_frames_.empty()) {
            current_theme_.native_width = static_cast<int>(bitmap->GetWidth());
            current_theme_.native_height = static_cast<int>(bitmap->GetHeight());
        }
        bg_frames_.push_back(bitmap);
        frame_delays_ms_.push_back(delay);
    }
    current_theme_.has_bg_image = !bg_frames_.empty();
}

UINT SkinManager::CurrentFrameDelayMs() const noexcept {
    if (frame_delays_ms_.empty()) return 100;
    return frame_delays_ms_[(std::min)(current_frame_, frame_delays_ms_.size() - 1)];
}

bool SkinManager::AdvanceFrame() noexcept {
    if (bg_frames_.size() <= 1) return false;
    current_frame_ = (current_frame_ + 1) % bg_frames_.size();
    return true;
}

bool SkinManager::DrawBackground(
    void* graphics_ptr, int width, int height, UINT dpi) {
    if (graphics_ptr == nullptr || bg_frames_.empty() || !current_theme_.has_bg_image || width <= 0 || height <= 0) {
        return false;
    }

    auto* bmp = reinterpret_cast<Gdiplus::Bitmap*>(
        bg_frames_[(std::min)(current_frame_, bg_frames_.size() - 1)]);
    const int src_w = static_cast<int>(bmp->GetWidth());
    const int src_h = static_cast<int>(bmp->GetHeight());
    if (src_w <= 0 || src_h <= 0) return false;

    auto& g = *reinterpret_cast<Gdiplus::Graphics*>(graphics_ptr);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const int source_left = (std::max)(0, (std::min)(current_theme_.slice.left, src_w));
    const int source_right = (std::max)(0,
        (std::min)(current_theme_.slice.right, src_w - source_left));
    const int source_top = (std::max)(0, (std::min)(current_theme_.slice.top, src_h));
    const int source_bottom = (std::max)(0,
        (std::min)(current_theme_.slice.bottom, src_h - source_top));
    const auto scaled = [dpi](int value) {
        return MulDiv(value, static_cast<int>((std::max)(UINT {1}, dpi)), 96);
    };
    int sl = scaled(source_left);
    int sr = scaled(source_right);
    int st = scaled(source_top);
    int sb = scaled(source_bottom);
    if (sl + sr > width) {
        const int total = sl + sr;
        sl = total == 0 ? 0 : width * sl / total;
        sr = width - sl;
    }
    if (st + sb > height) {
        const int total = st + sb;
        st = total == 0 ? 0 : height * st / total;
        sb = height - st;
    }

    const int mid_src_w = src_w - source_left - source_right;
    const int mid_src_h = src_h - source_top - source_bottom;
    const int mid_dst_w = width - sl - sr;
    const int mid_dst_h = height - st - sb;

    // 1. 四个角 (不拉伸)
    if (sl > 0 && st > 0) g.DrawImage(bmp, Gdiplus::Rect(0, 0, sl, st), 0, 0, source_left, source_top, Gdiplus::UnitPixel);
    if (sr > 0 && st > 0) g.DrawImage(bmp, Gdiplus::Rect(width - sr, 0, sr, st), src_w - source_right, 0, source_right, source_top, Gdiplus::UnitPixel);
    if (sl > 0 && sb > 0) g.DrawImage(bmp, Gdiplus::Rect(0, height - sb, sl, sb), 0, src_h - source_bottom, source_left, source_bottom, Gdiplus::UnitPixel);
    if (sr > 0 && sb > 0) g.DrawImage(bmp, Gdiplus::Rect(width - sr, height - sb, sr, sb), src_w - source_right, src_h - source_bottom, source_right, source_bottom, Gdiplus::UnitPixel);

    // 2. 四条边 (单向拉伸)
    if (mid_dst_w > 0 && st > 0 && mid_src_w > 0) {
        g.DrawImage(bmp, Gdiplus::Rect(sl, 0, mid_dst_w, st), source_left, 0, mid_src_w, source_top, Gdiplus::UnitPixel);
    }
    if (mid_dst_w > 0 && sb > 0 && mid_src_w > 0) {
        g.DrawImage(bmp, Gdiplus::Rect(sl, height - sb, mid_dst_w, sb), source_left, src_h - source_bottom, mid_src_w, source_bottom, Gdiplus::UnitPixel);
    }
    if (sl > 0 && mid_dst_h > 0 && mid_src_h > 0) {
        g.DrawImage(bmp, Gdiplus::Rect(0, st, sl, mid_dst_h), 0, source_top, source_left, mid_src_h, Gdiplus::UnitPixel);
    }
    if (sr > 0 && mid_dst_h > 0 && mid_src_h > 0) {
        g.DrawImage(bmp, Gdiplus::Rect(width - sr, st, sr, mid_dst_h), src_w - source_right, source_top, source_right, mid_src_h, Gdiplus::UnitPixel);
    }

    // 3. 中心区域 (双向拉伸)
    if (mid_dst_w > 0 && mid_dst_h > 0 && mid_src_w > 0 && mid_src_h > 0) {
        g.DrawImage(bmp, Gdiplus::Rect(sl, st, mid_dst_w, mid_dst_h), source_left, source_top, mid_src_w, mid_src_h, Gdiplus::UnitPixel);
    }

    return true;
}

bool SkinManager::DrawBackground(
    HDC hdc, int width, int height, UINT dpi,
    uint8_t* destination_pixels,
    int destination_bitmap_width,
    int destination_bitmap_height,
    int destination_offset) {
    if (bg_frames_.empty() || !current_theme_.has_bg_image || width <= 0 || height <= 0) {
        return false;
    }

    if (destination_pixels != nullptr &&
        destination_bitmap_width > 0 && destination_bitmap_height > 0 &&
        destination_offset >= 0 &&
        destination_offset + width <= destination_bitmap_width &&
        destination_offset + height <= destination_bitmap_height) {
        Gdiplus::Bitmap destination_bitmap(
            destination_bitmap_width, destination_bitmap_height,
            destination_bitmap_width * 4,
            PixelFormat32bppPARGB, destination_pixels);
        Gdiplus::Graphics alpha_graphics(&destination_bitmap);
        alpha_graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        alpha_graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        alpha_graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        alpha_graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        alpha_graphics.TranslateTransform(
            static_cast<Gdiplus::REAL>(destination_offset),
            static_cast<Gdiplus::REAL>(destination_offset));
        return DrawBackground(&alpha_graphics, width, height, dpi);
    }

    if (hdc != nullptr) {
        Gdiplus::Graphics hdc_graphics(hdc);
        return DrawBackground(&hdc_graphics, width, height, dpi);
    }

    return false;
}

}  // namespace shuru
