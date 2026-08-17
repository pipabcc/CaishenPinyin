#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace shuru {

enum class CandidateTextWeight {
    Regular,
    SemiBold,
    Bold,
};

enum class CandidateTextAlignment {
    Near,
    Center,
};

struct CandidateTextStyle {
    std::wstring font_family = L"Microsoft YaHei UI";
    float font_size = 15.0f;
    CandidateTextWeight weight = CandidateTextWeight::Regular;
    CandidateTextAlignment alignment = CandidateTextAlignment::Near;
    bool trim_with_ellipsis = true;
};

// DirectWrite 使用 DIP 进行排版，再按窗口 DPI 生成灰度覆盖率并合成到
// 预乘 Alpha 的候选窗表面。初始化或绘制失败时调用方可回退到 GDI+。
class DirectWriteTextRenderer {
public:
    DirectWriteTextRenderer();
    ~DirectWriteTextRenderer();

    DirectWriteTextRenderer(const DirectWriteTextRenderer&) = delete;
    DirectWriteTextRenderer& operator=(const DirectWriteTextRenderer&) = delete;

    bool IsAvailable() const noexcept;
    bool IsFontFamilyAvailable(const std::wstring& family) const;
    std::wstring ResolveFontFamily(const std::wstring& requested) const;

    float MeasureText(
        const std::wstring& text,
        const CandidateTextStyle& style,
        UINT dpi) const;

    bool DrawText(
        std::uint8_t* destination_pixels,
        int destination_width,
        int destination_height,
        const RECT& pixel_rect,
        const std::wstring& text,
        COLORREF color,
        const CandidateTextStyle& style,
        UINT dpi);

    bool BeginFrame(int width, int height, UINT dpi);
    bool DrawTextInFrame(
        const RECT& pixel_rect,
        const std::wstring& text,
        COLORREF color,
        const CandidateTextStyle& style);
    bool CompositeFrame(
        std::uint8_t* destination_pixels,
        int destination_width,
        int destination_height);
    void EndFrame() noexcept;

private:
    void* factory_ = nullptr;
    void* gdi_interop_ = nullptr;
    void* frame_target_ = nullptr;
    void* frame_rendering_params_ = nullptr;
    UINT frame_dpi_ = 96;
};

int CandidateVModeListFontSize(int candidate_font_size) noexcept;

}  // namespace shuru
