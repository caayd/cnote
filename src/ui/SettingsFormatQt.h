#pragma once

#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace cnote::ui {

// Text, colour and dimension value codecs shared by the Qt settings dialogs.
// They are direct ports of the legacy helpers in src/ui/UiUtil.cpp,
// src/SettingsWindow.cpp and src/NewPageSettingsWindow.cpp so both frontends
// format and validate identically (plan step 6.2, no settings-policy redesign).

struct HslColor {
    float hue = 0.0f;
    float saturation = 0.0f;
    float lightness = 0.0f;
};

struct DimensionPreset {
    const char* label;
    float width;
    float height;
};

std::span<const DimensionPreset> DimensionPresets();

// "Custom" when the draft size matches no preset (tolerance 0.5pt).
QString DimensionPresetLabelFor(float width, float height);

QString FormatHexColor(std::uint32_t color);
std::optional<std::uint32_t> ParseHexColor(const QString& text);

HslColor RgbToHsl(std::uint32_t color);
std::uint32_t HslToColor(float hueDegrees, float saturation, float lightness);

QString FormatHueLabel(int hue);
QString FormatPercentLabel(const QString& prefix, int value);
QString FormatFloatSliderLabel(const QString& prefix, float value, int multiplier);
QString FormatSpacingLabel(float spacing);
QString FormatDashLabel(const QString& prefix, float value);
QString FormatPixelLabel(const QString& prefix, float value);
QString FormatAutoSaveLabel(unsigned int seconds);
QString FormatZoomPresetValue(float zoom);
QString FormatPaletteEntry(std::size_t index, std::uint32_t color);
QString FormatDimensionValue(float value);

// Whole-string strict float parse: whitespace inside the text is rejected
// (callers trim first), matching the legacy std::stof + full-consumption check.
std::optional<float> ParseStrictFloat(const QString& text);

// Legacy ParsePositiveFloat: surrounding whitespace removed, finite, > 0.
std::optional<float> ParsePositiveFloat(const QString& text);

// Legacy zoom-preset edit parse: whitespace and '%' removed, then > 0.
std::optional<float> ParseZoomPresetPercent(const QString& text);

}  // namespace cnote::ui
