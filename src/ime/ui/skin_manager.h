#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace shuru {

struct SkinMargin {
    int top = 8;
    int bottom = 8;
    int left = 12;
    int right = 12;
};

struct SkinSlice {
    int left = 16;
    int right = 16;
    int top = 16;
    int bottom = 16;
};

enum class SkinOverlayAnchor {
    Start,
    Center,
    End
};

struct SkinAnimationOverlay {
    SkinOverlayAnchor horizontal_anchor = SkinOverlayAnchor::Start;
    SkinOverlayAnchor vertical_anchor = SkinOverlayAnchor::Start;
    int margin_left = 0;
    int margin_top = 0;
    int margin_right = 0;
    int margin_bottom = 0;
    std::vector<void*> frames;
    std::vector<UINT> frame_delays_ms;
};

struct SkinTheme {
    std::wstring id = L"classic_gold";
    std::wstring name = L"财神金韵";
    std::wstring author = L"财神输入法官方";
    std::wstring info = L"经典国风主题";

    std::wstring font_family = L"Microsoft YaHei UI";
    int font_size = 18;

    COLORREF pinyin_color = RGB(139, 30, 15);
    COLORREF candidate_color = RGB(44, 36, 22);
    COLORREF highlight_color = RGB(255, 255, 255);
    COLORREF highlight_bg_color = RGB(200, 22, 29);
    COLORREF index_color = RGB(184, 134, 11);
    COLORREF status_text_color = RGB(153, 136, 119);
    COLORREF separator_color = RGB(240, 228, 210);
    COLORREF utility_background_color = RGB(248, 250, 252);

    SkinSlice slice;
    SkinMargin pinyin_margin {10, 6, 16, 16};
    SkinMargin candidate_margin {6, 10, 16, 16};

    int corner_radius = 8;
    bool has_shadow = true;
    bool native_appearance = false;
    bool show_separator = true;
    bool has_bg_image = false;
    bool is_user_skin = false;
    int native_width = 0;
    int native_height = 0;
};

COLORREF EstimateCandidateBackgroundColorFromPixels(
    const std::uint8_t* bgra_pixels,
    int width,
    int height,
    int stride,
    const SkinTheme& theme) noexcept;

int ResolveSkinOverlayPosition(
    SkinOverlayAnchor anchor,
    int extent,
    int item_extent,
    int start_margin,
    int end_margin) noexcept;

class SkinManager {
public:
    static SkinManager& Instance();

    SkinManager();
    ~SkinManager();

    // 禁用拷贝
    SkinManager(const SkinManager&) = delete;
    SkinManager& operator=(const SkinManager&) = delete;

    // 检查并按需加载指定皮肤
    void EnsureSkin(const std::wstring& skin_id);

    // 配置变更通知到达时强制释放旧素材并重新加载。
    void ReloadSkin(const std::wstring& skin_id);

    // 获取当前皮肤配置
    const SkinTheme& CurrentTheme() const { return current_theme_; }

    // 绘制九宫格背景到 Gdiplus::Graphics（目标矩形从 (0,0) 到 (width, height)）
    // 返回 true 表示使用背景图成功绘制，false 表示应使用默认矢量渲染
    bool DrawBackground(
        void* graphics, int width, int height, UINT dpi = 96);

    // 绘制九宫格背景到目标 DC（目标矩形从 (0,0) 到 (width, height)）
    // 返回 true 表示使用背景图成功绘制，false 表示应使用默认矢量渲染
    bool DrawBackground(
        HDC hdc, int width, int height, UINT dpi = 96,
        uint8_t* destination_pixels = nullptr,
        int destination_bitmap_width = 0,
        int destination_bitmap_height = 0,
        int destination_offset = 0);

    bool HasAnimation() const noexcept;
    UINT CurrentFrameDelayMs() const noexcept;
    bool AdvanceFrame() noexcept;
    void ResetAnimation() noexcept { current_frame_ = 0; }

private:
    void LoadFromDirectory(
        const std::wstring& dir_path,
        const std::wstring& skin_id,
        bool is_user_skin);
    void CleanupGdiResources();

    SkinTheme current_theme_;
    std::wstring current_skin_id_;
    
    // GDI+ 相关内部对象指针（void* 避免头文件依赖 gdiplus.h）
    void* gdiplus_token_ = nullptr;
    std::vector<void*> bg_frames_; // Gdiplus::Bitmap*
    std::vector<UINT> frame_delays_ms_;
    std::vector<SkinAnimationOverlay> animation_overlays_;
    size_t current_frame_ = 0;
};

}  // namespace shuru
