#include "ui/UiUtil.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <memory>

#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#include "DpiUtil.h"

namespace cnote::ui {

namespace {

std::unique_ptr<Gdiplus::GraphicsPath> CreateRoundedRectPath(const RECT& rect, float radius) {
    const float left = static_cast<float>(rect.left);
    const float top = static_cast<float>(rect.top);
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    const float diameter = std::min(radius * 2.0f, std::min(width, height));

    auto path = std::make_unique<Gdiplus::GraphicsPath>();
    if (diameter <= 0.0f) {
        path->AddRectangle(Gdiplus::RectF(left, top, width, height));
        return path;
    }

    path->AddArc(left, top, diameter, diameter, 180.0f, 90.0f);
    path->AddArc(left + width - diameter, top, diameter, diameter, 270.0f, 90.0f);
    path->AddArc(left + width - diameter, top + height - diameter, diameter, diameter, 0.0f, 90.0f);
    path->AddArc(left, top + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path->CloseFigure();
    return path;
}

template <typename ColorFn>
void FillTrackWithColorFunction(HDC hdc, const RECT& trackRect, ColorFn&& colorFn) {
    const int width = std::max(1, static_cast<int>(trackRect.right - trackRect.left));
    const int height = std::max(1, static_cast<int>(trackRect.bottom - trackRect.top));

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    graphics.SetClip(Gdiplus::Rect(trackRect.left, trackRect.top, width, height));

    for (int offset = 0; offset < width; ++offset) {
        const float normalized = width > 1 ? static_cast<float>(offset) / static_cast<float>(width - 1) : 0.0f;
        const std::uint32_t color = colorFn(normalized);
        Gdiplus::SolidBrush brush(Gdiplus::Color(
            255,
            static_cast<BYTE>((color >> 16U) & 0xFFU),
            static_cast<BYTE>((color >> 8U) & 0xFFU),
            static_cast<BYTE>(color & 0xFFU)));
        graphics.FillRectangle(&brush, Gdiplus::Rect(trackRect.left + offset, trackRect.top, 1, height));
    }

    graphics.ResetClip();
}

}  // namespace

std::wstring WideFromUtf16(std::u16string_view value) {
    std::wstring wide;
    wide.reserve(value.size());
    for (char16_t unit : value) {
        wide.push_back(static_cast<wchar_t>(unit));
    }
    return wide;
}

std::u16string Utf16FromWide(std::wstring_view value) {
    std::u16string text;
    text.reserve(value.size());
    for (wchar_t unit : value) {
        text.push_back(static_cast<char16_t>(unit));
    }
    return text;
}

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect { left, top, right, bottom };
    return rect;
}

RECT MakeRectFromFloatBounds(float left, float top, float right, float bottom) {
    return MakeRect(
        static_cast<int>(std::floor(left)),
        static_cast<int>(std::floor(top)),
        static_cast<int>(std::ceil(right)),
        static_cast<int>(std::ceil(bottom)));
}

bool HasRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

std::wstring TrimWide(std::wstring_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::iswspace(value[start]) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::iswspace(value[end - 1U]) != 0) {
        --end;
    }

    return std::wstring(value.substr(start, end - start));
}

std::string TrimAscii(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

std::wstring FormatHexColor(std::uint32_t color) {
    wchar_t buffer[16] = {};
    std::swprintf(buffer, std::size(buffer), L"#%06X", color & 0xFFFFFFU);
    return buffer;
}

std::optional<std::uint32_t> ParseHexColor(std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }), value.end());

    if (!value.empty() && value.front() == L'#') {
        value.erase(value.begin());
    }
    if (value.rfind(L"0x", 0) == 0 || value.rfind(L"0X", 0) == 0) {
        value.erase(0, 2);
    }

    if (value.size() != 6U && value.size() != 8U) {
        return std::nullopt;
    }
    if (!std::all_of(value.begin(), value.end(), [](wchar_t ch) { return std::iswxdigit(ch) != 0; })) {
        return std::nullopt;
    }

    try {
        const std::uint32_t parsed = static_cast<std::uint32_t>(std::stoul(value, nullptr, 16));
        return value.size() == 6U ? (0xFF000000U | parsed) : parsed;
    } catch (...) {
        return std::nullopt;
    }
}

HslColor RgbToHsl(std::uint32_t color) {
    const float red = static_cast<float>((color >> 16U) & 0xFFU) / 255.0f;
    const float green = static_cast<float>((color >> 8U) & 0xFFU) / 255.0f;
    const float blue = static_cast<float>(color & 0xFFU) / 255.0f;
    const float maxChannel = std::max({ red, green, blue });
    const float minChannel = std::min({ red, green, blue });
    const float delta = maxChannel - minChannel;

    HslColor hsl {};
    hsl.lightness = (maxChannel + minChannel) * 0.5f;
    if (delta <= 0.0001f) {
        return hsl;
    }

    hsl.saturation = delta / (1.0f - std::abs((2.0f * hsl.lightness) - 1.0f));
    if (maxChannel == red) {
        hsl.hue = 60.0f * std::fmod(((green - blue) / delta), 6.0f);
    } else if (maxChannel == green) {
        hsl.hue = 60.0f * (((blue - red) / delta) + 2.0f);
    } else {
        hsl.hue = 60.0f * (((red - green) / delta) + 4.0f);
    }
    if (hsl.hue < 0.0f) {
        hsl.hue += 360.0f;
    }
    return hsl;
}

std::uint32_t HslToColor(float hueDegrees, float saturation, float lightness) {
    const float hue = std::fmod(std::max(0.0f, hueDegrees), 360.0f);
    const float clampedSaturation = std::clamp(saturation, 0.0f, 1.0f);
    const float clampedLightness = std::clamp(lightness, 0.0f, 1.0f);
    const float chroma = (1.0f - std::abs((2.0f * clampedLightness) - 1.0f)) * clampedSaturation;
    const float huePrime = hue / 60.0f;
    const float x = chroma * (1.0f - std::abs(std::fmod(huePrime, 2.0f) - 1.0f));
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;

    if (huePrime < 1.0f) {
        red = chroma;
        green = x;
    } else if (huePrime < 2.0f) {
        red = x;
        green = chroma;
    } else if (huePrime < 3.0f) {
        green = chroma;
        blue = x;
    } else if (huePrime < 4.0f) {
        green = x;
        blue = chroma;
    } else if (huePrime < 5.0f) {
        red = x;
        blue = chroma;
    } else {
        red = chroma;
        blue = x;
    }

    const float match = clampedLightness - (chroma * 0.5f);
    const auto outRed = static_cast<std::uint32_t>(std::clamp(std::lround((red + match) * 255.0f), 0L, 255L));
    const auto outGreen = static_cast<std::uint32_t>(std::clamp(std::lround((green + match) * 255.0f), 0L, 255L));
    const auto outBlue = static_cast<std::uint32_t>(std::clamp(std::lround((blue + match) * 255.0f), 0L, 255L));
    return 0xFF000000U | (outRed << 16U) | (outGreen << 8U) | outBlue;
}

std::wstring FormatHueLabel(int hue) {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"Hue: %d deg", hue);
    return buffer;
}

std::wstring FormatPercentLabel(const wchar_t* prefix, int value) {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"%ls: %d%%", prefix, value);
    return buffer;
}

std::wstring FormatZoomPercentValue(float zoom) {
    wchar_t buffer[32] = {};
    const float percent = zoom * 100.0f;
    const bool wholePercent = std::abs(percent - std::round(percent)) < 0.05f;
    if (wholePercent) {
        std::swprintf(buffer, std::size(buffer), L"%.0f", percent);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%.1f", percent);
    }
    std::wstring value = buffer;
    if (!wholePercent) {
        while (!value.empty() && value.back() == L'0') {
            value.pop_back();
        }
        if (!value.empty() && value.back() == L'.') {
            value.pop_back();
        }
    }
    return value;
}

void SetControlText(HWND control, const std::wstring& text) {
    if (control != nullptr) {
        SetWindowTextW(control, text.c_str());
    }
}

LRESULT CALLBACK EditReturnToParentSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_KEYDOWN && wParam == VK_RETURN) {
        SetFocus(GetParent(window));
        return 0;
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

void FillRoundedRect(HDC hdc, RECT rect, COLORREF color) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
    const auto path = CreateRoundedRectPath(rect, 6.0f);
    graphics.FillPath(&brush, path.get());
}

void StrokeRoundedRect(HDC hdc, RECT rect, COLORREF color, int width) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), static_cast<Gdiplus::REAL>(width));
    const auto path = CreateRoundedRectPath(rect, 6.0f);
    graphics.DrawPath(&pen, path.get());
}

RECT SliderTrackRect(const RECT& sliderRect, UINT dpi) {
    const int centerY = (sliderRect.top + sliderRect.bottom) / 2;
    return MakeRect(
        sliderRect.left + ScaleForDpi(10, dpi),
        centerY - ScaleForDpi(3, dpi),
        sliderRect.right - ScaleForDpi(10, dpi),
        centerY + ScaleForDpi(3, dpi));
}

int SliderThumbCenterX(const RECT& trackRect, int value, int minimum, int maximum) {
    const int width = std::max(1, static_cast<int>(trackRect.right - trackRect.left));
    const int range = std::max(1, maximum - minimum);
    const float normalized = static_cast<float>(std::clamp(value, minimum, maximum) - minimum) / static_cast<float>(range);
    return trackRect.left + static_cast<int>(std::lround(normalized * static_cast<float>(width - 1)));
}

void FillHuePreviewTrack(HDC hdc, const RECT& trackRect, float saturation, float lightness) {
    FillTrackWithColorFunction(hdc, trackRect, [=](float normalized) {
        return HslToColor(normalized * 360.0f, saturation, lightness);
    });
}

void FillSaturationPreviewTrack(HDC hdc, const RECT& trackRect, float hue, float lightness) {
    FillTrackWithColorFunction(hdc, trackRect, [=](float normalized) {
        return HslToColor(hue, normalized, lightness);
    });
}

void FillLightnessPreviewTrack(HDC hdc, const RECT& trackRect, float hue, float saturation) {
    FillTrackWithColorFunction(hdc, trackRect, [=](float normalized) {
        return HslToColor(hue, saturation, normalized);
    });
}

RECT UnionRectangles(RECT a, RECT b) {
    if (!HasRect(a)) {
        return b;
    }
    if (!HasRect(b)) {
        return a;
    }
    return MakeRect(
        std::min(a.left, b.left),
        std::min(a.top, b.top),
        std::max(a.right, b.right),
        std::max(a.bottom, b.bottom));
}

RECT InflateRectCopy(RECT rect, int amount) {
    rect.left -= amount;
    rect.top -= amount;
    rect.right += amount;
    rect.bottom += amount;
    return rect;
}

void DeferMoveControl(HDWP& deferred, HWND control, const RECT& rect, UINT flags) {
    if (deferred == nullptr || control == nullptr) {
        return;
    }

    deferred = DeferWindowPos(
        deferred,
        control,
        nullptr,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        flags);
}

}  // namespace cnote::ui
