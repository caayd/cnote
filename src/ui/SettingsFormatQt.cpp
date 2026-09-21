#include "ui/SettingsFormatQt.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cnote::ui {

namespace {

constexpr std::array<DimensionPreset, 6> kDimensionPresets {{
    { "Letter Portrait (612 x 792)", 612.0f, 792.0f },
    { "Letter Landscape (792 x 612)", 792.0f, 612.0f },
    { "Legal Portrait (612 x 1008)", 612.0f, 1008.0f },
    { "A4 Portrait (595 x 842)", 595.0f, 842.0f },
    { "A4 Landscape (842 x 595)", 842.0f, 595.0f },
    { "Tabloid Landscape (1224 x 792)", 1224.0f, 792.0f },
}};

bool NearlyEqual(float left, float right, float epsilon = 0.5f) {
    return std::abs(left - right) <= epsilon;
}

QString ToQString(const char* text) {
    return QString::fromUtf8(text);
}

std::optional<float> ParseFloatWholeString(const QString& text) {
    if (text.isEmpty()) {
        return std::nullopt;
    }

    const QByteArray utf8 = text.toUtf8();
    char* end = nullptr;
    const float parsed = std::strtof(utf8.constData(), &end);
    if (end == utf8.constData() || *end != '\0') {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace

std::span<const DimensionPreset> DimensionPresets() {
    return std::span<const DimensionPreset>(kDimensionPresets);
}

QString DimensionPresetLabelFor(float width, float height) {
    for (const DimensionPreset& preset : kDimensionPresets) {
        if (NearlyEqual(width, preset.width) && NearlyEqual(height, preset.height)) {
            return ToQString(preset.label);
        }
    }
    return QStringLiteral("Custom");
}

QString FormatHexColor(std::uint32_t color) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "#%06X", color & 0xFFFFFFU);
    return ToQString(buffer);
}

std::optional<std::uint32_t> ParseHexColor(const QString& text) {
    QString value = text;
    value.remove(QChar(' '));
    value.remove(QChar('\t'));
    value.remove(QChar('\n'));
    value.remove(QChar('\r'));
    value.remove(QChar('\v'));
    value.remove(QChar('\f'));

    if (!value.isEmpty() && value.front() == QLatin1Char('#')) {
        value.remove(0, 1);
    }
    if (value.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        value.remove(0, 2);
    }

    if (value.size() != 6 && value.size() != 8) {
        return std::nullopt;
    }
    for (const QChar ch : value) {
        if (!ch.isDigit() && !(ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))
            && !(ch >= QLatin1Char('A') && ch <= QLatin1Char('F'))) {
            return std::nullopt;
        }
    }

    bool ok = false;
    const uint parsed = value.toUInt(&ok, 16);
    if (!ok) {
        return std::nullopt;
    }
    return value.size() == 6 ? (0xFF000000U | parsed) : static_cast<std::uint32_t>(parsed);
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

QString FormatHueLabel(int hue) {
    return QStringLiteral("Hue: %1 deg").arg(hue);
}

QString FormatPercentLabel(const QString& prefix, int value) {
    return QStringLiteral("%1: %2%").arg(prefix).arg(value);
}

QString FormatFloatSliderLabel(const QString& prefix, float value, int multiplier) {
    return QStringLiteral("%1: %2").arg(prefix).arg(static_cast<int>(std::lround(value * static_cast<float>(multiplier))));
}

QString FormatSpacingLabel(float spacing) {
    return QStringLiteral("Point Spacing: %1").arg(static_cast<double>(spacing), 0, 'f', 2);
}

QString FormatDashLabel(const QString& prefix, float value) {
    return QStringLiteral("%1: %2x").arg(prefix).arg(static_cast<double>(value), 0, 'f', 1);
}

QString FormatPixelLabel(const QString& prefix, float value) {
    return QStringLiteral("%1: %2 px").arg(prefix).arg(static_cast<double>(value), 0, 'f', 0);
}

QString FormatAutoSaveLabel(unsigned int seconds) {
    if (seconds == 0U) {
        return QStringLiteral("Auto Save: Off");
    }
    if (seconds < 60U) {
        return QStringLiteral("Auto Save: %1 sec").arg(seconds);
    }
    if (seconds % 60U == 0U) {
        const unsigned int minutes = seconds / 60U;
        return QStringLiteral("Auto Save: %1 %2").arg(minutes).arg(minutes == 1U ? QStringLiteral("min") : QStringLiteral("mins"));
    }
    return QStringLiteral("Auto Save: %1 sec").arg(seconds);
}

QString FormatZoomPresetValue(float zoom) {
    if (zoom <= 0.0f) {
        return QString();
    }

    const float percent = zoom * 100.0f;
    QString value;
    if (std::abs(percent - std::round(percent)) < 0.05f) {
        value = QString::number(static_cast<int>(std::lround(percent)));
    } else {
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(percent));
        value = ToQString(buffer);
        while (!value.isEmpty() && value.back() == QLatin1Char('0')) {
            value.chop(1);
        }
        if (!value.isEmpty() && value.back() == QLatin1Char('.')) {
            value.chop(1);
        }
    }
    return value;
}

QString FormatPaletteEntry(std::size_t index, std::uint32_t color) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%zu. #%06X", index + 1U, color & 0xFFFFFFU);
    return ToQString(buffer);
}

QString FormatDimensionValue(float value) {
    char buffer[32] = {};
    if (std::abs(value - std::round(value)) < 0.01f) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", static_cast<double>(value));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
    }
    return ToQString(buffer);
}

std::optional<float> ParseStrictFloat(const QString& text) {
    if (text.isEmpty()) {
        return std::nullopt;
    }
    return ParseFloatWholeString(text);
}

std::optional<float> ParsePositiveFloat(const QString& text) {
    QString value = text;
    value.remove(QChar(' '));
    value.remove(QChar('\t'));
    value.remove(QChar('\n'));
    value.remove(QChar('\r'));
    value.remove(QChar('\v'));
    value.remove(QChar('\f'));

    const auto parsed = ParseFloatWholeString(value);
    if (!parsed.has_value() || !std::isfinite(*parsed) || *parsed <= 0.0f) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<float> ParseZoomPresetPercent(const QString& text) {
    QString value = text;
    value.remove(QChar(' '));
    value.remove(QChar('\t'));
    value.remove(QChar('\n'));
    value.remove(QChar('\r'));
    value.remove(QChar('\v'));
    value.remove(QChar('\f'));
    value.remove(QChar('%'));

    const auto parsed = ParseFloatWholeString(value);
    if (!parsed.has_value() || *parsed <= 0.0f) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace cnote::ui
