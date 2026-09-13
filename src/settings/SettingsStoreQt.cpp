#include "settings/SettingsStoreQt.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>

#include "Unicode.h"

namespace cnote::settings {

namespace {

constexpr std::string_view kAsciiWhitespace = " \t\n\v\f\r";

// Same trimming as cnote::ui::TrimAscii (C-locale isspace).
std::string_view TrimAsciiView(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && kAsciiWhitespace.find(value[start]) != std::string_view::npos) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && kAsciiWhitespace.find(value[end - 1U]) != std::string_view::npos) {
        --end;
    }

    return value.substr(start, end - start);
}

constexpr std::array<std::uint32_t, 5> kDefaultPalette {
    0xFF000000U,
    0xFF2563EBU,
    0xFFDC2626U,
    0xFF059669U,
    0xFFF59E0BU,
};

constexpr std::array<std::uint32_t, 5> kLegacyDefaultPalette {
    0xFF1F2937U,
    0xFF2563EBU,
    0xFFDC2626U,
    0xFF059669U,
    0xFFF59E0BU,
};

std::optional<unsigned int> ParseUnsignedInt(std::string_view value, int base = 10);
std::optional<float> ParseFloat(std::string_view value);

std::vector<std::string_view> SplitAscii(std::string_view value, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            parts.push_back(TrimAsciiView(value.substr(start)));
            break;
        }
        parts.push_back(TrimAsciiView(value.substr(start, end - start)));
        start = end + 1U;
    }
    return parts;
}

std::optional<unsigned int> ParseUnsignedInt(std::string_view value, int base) {
    const std::string_view trimmed = TrimAsciiView(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(std::string(trimmed), &consumed, base);
        if (consumed != trimmed.size()) {
            return std::nullopt;
        }
        return static_cast<unsigned int>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<float> ParseFloat(std::string_view value) {
    const std::string_view trimmed = TrimAsciiView(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    try {
        std::size_t consumed = 0;
        const float parsed = std::stof(std::string(trimmed), &consumed);
        if (consumed != trimmed.size()) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<std::uint32_t>> ParsePaletteColors(std::string_view value) {
    std::vector<std::uint32_t> colors;
    for (const std::string_view token : SplitAscii(value, ',')) {
        const auto color = ParseUnsignedInt(token, 16);
        if (!color.has_value()) {
            return std::nullopt;
        }
        colors.push_back(*color);
        if (colors.size() >= 12U) {
            break;
        }
    }

    if (colors.empty()) {
        return std::nullopt;
    }
    return colors;
}

void WritePaletteColors(std::ostream& output, const std::vector<std::uint32_t>& colors) {
    for (std::size_t index = 0; index < colors.size(); ++index) {
        if (index > 0U) {
            output << ',';
        }
        std::ostringstream colorValue;
        colorValue << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << colors[index];
        output << colorValue.str();
    }
}

float SnapBrushSize(float size) {
    constexpr float kMinBrushSize = 0.5f;
    constexpr float kBrushSizeStep = 0.25f;
    const float clamped = std::clamp(size, kMinBrushSize, kMaxBrushSize);
    const float steps = std::round((clamped - kMinBrushSize) / kBrushSizeStep);
    return kMinBrushSize + (steps * kBrushSizeStep);
}

std::string FormatBrushSizePresetValue(float size) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(SnapBrushSize(size)));
    std::string value = buffer;
    while (!value.empty() && value.back() == '0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    return value;
}

std::vector<float> NormalizeBrushSizePresets(std::vector<float> presets) {
    for (float& preset : presets) {
        preset = SnapBrushSize(preset);
    }

    std::sort(presets.begin(), presets.end());
    presets.erase(std::unique(presets.begin(), presets.end(), [](float left, float right) {
        return std::abs(left - right) < 0.001f;
    }), presets.end());
    if (presets.size() > 16U) {
        presets.resize(16U);
    }
    return presets;
}

std::string FormatBrushSizePresets(const std::vector<float>& presets) {
    const std::vector<float> normalized = NormalizeBrushSizePresets(presets);
    std::string formatted;
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        if (index > 0U) {
            formatted += ", ";
        }
        formatted += FormatBrushSizePresetValue(normalized[index]);
    }
    return formatted;
}

bool TryParseBrushSizePresets(std::string_view value, std::vector<float>& presets) {
    const QString text = value.empty()
        ? QString()
        : QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())).trimmed();
    if (text.isEmpty()) {
        presets.clear();
        return true;
    }

    std::vector<float> parsedPresets;
    const QStringList tokens = text.split(QLatin1Char(','));
    for (const QString& rawToken : tokens) {
        const QString token = rawToken.trimmed();
        if (token.isEmpty()) {
            return false;
        }

        const QByteArray utf8 = token.toUtf8();
        char* parseEnd = nullptr;
        const float parsedValue = std::strtof(utf8.constData(), &parseEnd);
        if (parseEnd == utf8.constData() || *parseEnd != '\0' || parsedValue <= 0.0f) {
            return false;
        }
        parsedPresets.push_back(parsedValue);
    }

    presets = NormalizeBrushSizePresets(std::move(parsedPresets));
    return true;
}

const char* DrawCursorKindConfigName(DrawCursorKind kind) {
    switch (kind) {
    case DrawCursorKind::PrecisionCrosshair:
        return "precision_crosshair";
    case DrawCursorKind::Dot:
    default:
        return "dot";
    }
}

std::optional<DrawCursorKind> ParseDrawCursorKindConfigName(std::string_view name) {
    if (name == "dot") {
        return DrawCursorKind::Dot;
    }
    if (name == "precision_crosshair") {
        return DrawCursorKind::PrecisionCrosshair;
    }
    return std::nullopt;
}

const char* ToolConfigName(Tool tool) {
    switch (tool) {
    case Tool::Select:
        return "Select";
    case Tool::Move:
        return "Move";
    case Tool::Text:
        return "Text";
    case Tool::Latex:
        return "Latex";
    case Tool::Draw:
        return "Draw";
    case Tool::Pan:
        return "Pan";
    case Tool::Erase:
        return "Erase";
    case Tool::Zoom:
        return "Zoom";
    default:
        return "";
    }
}

std::optional<Tool> ParseToolConfigName(std::string_view name) {
    if (name == "Select") {
        return Tool::Select;
    }
    if (name == "Move") {
        return Tool::Move;
    }
    if (name == "Text") {
        return Tool::Text;
    }
    if (name == "Latex") {
        return Tool::Latex;
    }
    if (name == "Draw") {
        return Tool::Draw;
    }
    if (name == "Pan") {
        return Tool::Pan;
    }
    if (name == "Erase") {
        return Tool::Erase;
    }
    if (name == "Zoom") {
        return Tool::Zoom;
    }
    return std::nullopt;
}

bool EqualPaletteNames(std::u16string_view left, std::u16string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    const QString leftText = QString::fromUtf16(left.data(), static_cast<qsizetype>(left.size()));
    const QString rightText = QString::fromUtf16(right.data(), static_cast<qsizetype>(right.size()));
    return leftText.compare(rightText, Qt::CaseInsensitive) == 0;
}

std::u16string TrimWide(std::u16string_view value) {
    if (value.empty()) {
        return {};
    }
    return QString::fromUtf16(value.data(), static_cast<qsizetype>(value.size())).trimmed().toStdU16String();
}

std::u16string Utf8ToUtf16(std::string_view value) {
    return cnote::Utf8ToUtf16(value);
}

// -- recent files -------------------------------------------------------------

bool HasExtension(const std::filesystem::path& path, std::u16string_view extension) {
    const std::u16string actual = path.extension().u16string();
    if (actual.size() != extension.size()) {
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const char16_t a = actual[index];
        const char16_t b = extension[index];
        const char16_t lowerA = (a >= u'A' && a <= u'Z') ? static_cast<char16_t>(a + 32) : a;
        const char16_t lowerB = (b >= u'A' && b <= u'Z') ? static_cast<char16_t>(b + 32) : b;
        if (lowerA != lowerB) {
            return false;
        }
    }
    return true;
}

std::filesystem::path NormalizeRecentPath(const std::filesystem::path& filePath) {
    if (filePath.empty()) {
        return {};
    }

    std::filesystem::path path = filePath;
    if (path.is_relative()) {
        std::error_code error;
        const std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
        if (!error) {
            path = absolutePath;
        }
    }

    return path.lexically_normal();
}

bool PathsEqual(const std::filesystem::path& a, const std::filesystem::path& b) {
#ifdef _WIN32
    return QString::fromStdU16String(a.u16string()).compare(
               QString::fromStdU16String(b.u16string()), Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

// Mirrors App::RecordRecentDocumentPath ordering, filtering and 10-entry cap.
class RecentDocumentCollector {
public:
    void Record(const std::filesystem::path& filePath) {
        const std::filesystem::path normalizedPath = NormalizeRecentPath(filePath);
        if (normalizedPath.empty()
            || (!HasExtension(normalizedPath, u".cnote") && !HasExtension(normalizedPath, u".pdf"))) {
            return;
        }

        const auto existing = std::find_if(paths_.begin(), paths_.end(), [&](const std::filesystem::path& recentPath) {
            return PathsEqual(recentPath, normalizedPath);
        });

        if (existing != paths_.end() && existing == paths_.begin()) {
            if (*existing != normalizedPath) {
                *existing = normalizedPath;
            }
            return;
        }

        if (existing != paths_.end()) {
            paths_.erase(existing);
        }

        paths_.insert(paths_.begin(), normalizedPath);
        if (paths_.size() > kMaxRecentDocuments) {
            paths_.resize(kMaxRecentDocuments);
        }
    }

    [[nodiscard]] std::vector<std::filesystem::path> Take() { return std::move(paths_); }

private:
    std::vector<std::filesystem::path> paths_ {};
};

// -- storage location ---------------------------------------------------------

QString PathToQString(const std::filesystem::path& path) {
    return QString::fromStdString(cnote::PathToUtf8(path));
}

std::filesystem::path PathFromQString(const QString& text) {
    return cnote::Utf8ToPath(text.toStdString());
}

#ifndef _WIN32
void EnsureApplicationIdentity() {
    if (QCoreApplication::instance() == nullptr) {
        return;
    }
    if (QCoreApplication::applicationName() != QStringLiteral("cnote")) {
        QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    }
    if (!QCoreApplication::organizationName().isEmpty()) {
        QCoreApplication::setOrganizationName(QString());
    }
}
#endif

ShortcutBinding* FindBinding(std::array<ShortcutBinding, kShortcutActionCount>& bindings, ShortcutAction action) {
    for (ShortcutBinding& binding : bindings) {
        if (binding.action == action) {
            return &binding;
        }
    }
    return nullptr;
}

}  // namespace

std::vector<std::uint32_t> DefaultPalette() {
    return std::vector<std::uint32_t>(kDefaultPalette.begin(), kDefaultPalette.end());
}

std::array<ShortcutBinding, kShortcutActionCount> DefaultShortcutBindings() {
    std::array<ShortcutBinding, kShortcutActionCount> bindings {};
    std::size_t index = 0;
    for (const ShortcutDefinition& definition : ShortcutDefinitions()) {
        if (index >= bindings.size()) {
            break;
        }
        bindings[index].action = definition.action;
        bindings[index].sequence = DefaultKeySequence(definition.action);
        ++index;
    }
    return bindings;
}

SettingsStoreQt::SettingsStoreQt() : path_(ResolveSettingsPath()) {}

SettingsStoreQt::SettingsStoreQt(std::filesystem::path path) : path_(std::move(path)) {}

std::filesystem::path SettingsStoreQt::ResolveSettingsPath() {
#ifdef _WIN32
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) {
        return PathFromQString(appData) / "cnote" / "settings.ini";
    }
    return std::filesystem::current_path() / "cnote.settings.ini";
#else
    EnsureApplicationIdentity();
    return PathFromQString(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)) / "settings.ini";
#endif
}

Settings SettingsStoreQt::Load() const {
    QFile file(PathToQString(path_));
    if (!file.open(QIODevice::ReadOnly)) {
        return Settings {};
    }

    const QByteArray bytes = file.readAll();
    return ParseText(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}

Settings SettingsStoreQt::ParseText(std::string_view text) {
    Settings settings;

    struct PendingNamedPalette {
        std::u16string name {};
        std::vector<std::uint32_t> colors {};
        bool hasName = false;
        bool hasColors = false;
    };

    std::optional<std::size_t> loadedColorIndex;
    std::optional<std::size_t> loadedSettingsPaletteIndex;
    std::map<unsigned int, PendingNamedPalette> pendingNamedPalettes;
    std::map<unsigned int, std::filesystem::path> pendingRecentDocumentPaths;

    std::vector<std::string_view> lines;
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const std::size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string_view::npos) {
            lines.push_back(text.substr(lineStart));
            break;
        }
        lines.push_back(text.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1U;
    }

    // The shortcut representation is chosen once for the whole file so that a
    // hand-edited marker after the records still selects the portable format.
    bool portableShortcuts = false;
    for (const std::string_view line : lines) {
        const std::string_view trimmed = TrimAsciiView(line);
        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }
        const std::size_t separator = trimmed.find('=');
        if (separator == std::string_view::npos) {
            continue;
        }
        const std::string_view key = TrimAsciiView(trimmed.substr(0, separator));
        const std::string_view value = TrimAsciiView(trimmed.substr(separator + 1U));
        if (key == "shortcut_format" && value == kShortcutFormatValue) {
            portableShortcuts = true;
            break;
        }
    }

    for (const std::string_view line : lines) {
        const std::string_view trimmed = TrimAsciiView(line);
        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }

        const std::size_t separator = trimmed.find('=');
        if (separator == std::string_view::npos) {
            continue;
        }

        const std::string_view key = TrimAsciiView(trimmed.substr(0, separator));
        const std::string_view value = TrimAsciiView(trimmed.substr(separator + 1U));

        if (key == "palette") {
            if (const auto loadedPalette = ParsePaletteColors(value); loadedPalette.has_value()) {
                settings.palette = std::move(*loadedPalette);
            }
            continue;
        }

        if (key.starts_with("saved_palette_")) {
            const std::string_view suffix = key.substr(14U);
            const std::size_t fieldSeparator = suffix.find('_');
            if (fieldSeparator == std::string_view::npos) {
                continue;
            }

            const auto paletteIndex = ParseUnsignedInt(suffix.substr(0, fieldSeparator));
            if (!paletteIndex.has_value() || *paletteIndex == 0U) {
                continue;
            }

            PendingNamedPalette& pendingPalette = pendingNamedPalettes[*paletteIndex];
            const std::string_view field = suffix.substr(fieldSeparator + 1U);
            if (field == "name") {
                pendingPalette.name = TrimWide(Utf8ToUtf16(value));
                pendingPalette.hasName = !pendingPalette.name.empty();
            } else if (field == "colors") {
                if (const auto colors = ParsePaletteColors(value); colors.has_value()) {
                    pendingPalette.colors = std::move(*colors);
                    pendingPalette.hasColors = true;
                }
            }
            continue;
        }

        if (key == "active_color") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                loadedColorIndex = static_cast<std::size_t>(*parsed);
            }
            continue;
        }

        if (key == "settings_palette_index") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                loadedSettingsPaletteIndex = static_cast<std::size_t>(*parsed);
            }
            continue;
        }

        if (key == "default_page_color") {
            if (const auto parsed = ParseUnsignedInt(value, 16); parsed.has_value()) {
                settings.defaultPageColor = *parsed;
            }
            continue;
        }

        if (key.starts_with("recent_document_")) {
            if (const auto index = ParseUnsignedInt(key.substr(16)); index.has_value()
                && *index >= 1U
                && *index <= kMaxRecentDocuments) {
                std::filesystem::path recentPath = cnote::Utf8ToPath(value);
                if (!recentPath.empty()) {
                    pendingRecentDocumentPaths[*index] = std::move(recentPath);
                }
            }
            continue;
        }

        if (key.starts_with("recent_cnote_")) {
            if (const auto index = ParseUnsignedInt(key.substr(13)); index.has_value()
                && *index >= 1U
                && *index <= kMaxRecentDocuments) {
                std::filesystem::path recentPath = cnote::Utf8ToPath(value);
                if (!recentPath.empty() && pendingRecentDocumentPaths.find(*index) == pendingRecentDocumentPaths.end()) {
                    pendingRecentDocumentPaths[*index] = std::move(recentPath);
                }
            }
            continue;
        }

        if (key == "input_smoothing") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings.inputSmoothing = std::clamp(*parsed, 0.05f, 0.95f);
            }
            continue;
        }

        if (key == "draw_brush_size") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings.drawBrushSize = std::clamp(*parsed, 0.5f, 20.0f);
            }
            continue;
        }

        if (key == "erase_brush_size") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings.eraseBrushSize = std::clamp(*parsed, 0.5f, 20.0f);
            }
            continue;
        }

        if (key == "quick_size_slider_max") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings.quickSizeSliderMax = std::clamp(*parsed, 1.0f, 20.0f);
            }
            continue;
        }

        if (key == "brush_size_presets") {
            std::vector<float> parsedPresets;
            if (TryParseBrushSizePresets(value, parsedPresets)) {
                settings.brushSizePresets = std::move(parsedPresets);
            }
            continue;
        }

        if (key == "draw_cursor_size") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings.drawCursorSize = std::clamp(*parsed, 4.0f, 24.0f);
            }
            continue;
        }

        if (key == "draw_cursor_kind") {
            if (const auto parsed = ParseDrawCursorKindConfigName(value); parsed.has_value()) {
                settings.drawCursorKind = *parsed;
            }
            continue;
        }

        if (key == "input_spacing") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings.inputSpacing = std::clamp(*parsed, 0.05f, 4.05f);
            }
            continue;
        }

        if (key == "stroke_smoothing") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings.strokeSmoothing = std::clamp(*parsed, 0.0f, 1.0f);
            }
            continue;
        }

        if (key == "dash_length") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings.dashLength = std::clamp(*parsed, 0.5f, 8.0f);
            }
            continue;
        }

        if (key == "dash_gap") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings.dashGap = std::clamp(*parsed, 0.5f, 8.0f);
            }
            continue;
        }

        if (key == "auto_save_interval_seconds") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                settings.autoSaveIntervalSeconds = std::min(*parsed, 300U);
            }
            continue;
        }

        if (key == "auto_hide_toolbar") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                settings.autoHideToolbar = *parsed != 0U;
            }
            continue;
        }

        if (key == "embed_project_in_exported_pdfs") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                settings.embedProjectInExportedPdfs = *parsed != 0U;
            }
            continue;
        }

        if (key.starts_with("zoom_preset_")) {
            if (const auto index = ParseUnsignedInt(key.substr(12)); index.has_value()
                && *index >= 1U
                && *index <= settings.zoomPresets.size()) {
                if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                    settings.zoomPresets[*index - 1U] = *parsed <= 0.0f ? 0.0f : std::clamp(*parsed, kMinZoom, kMaxZoom);
                }
            }
            continue;
        }

        if (key == "wheel_tools") {
            std::vector<Tool> loadedTools;
            for (const std::string_view token : SplitAscii(value, ',')) {
                const auto tool = ParseToolConfigName(token);
                if (!tool.has_value()) {
                    continue;
                }
                if (std::find(loadedTools.begin(), loadedTools.end(), *tool) == loadedTools.end()) {
                    loadedTools.push_back(*tool);
                }
            }
            settings.wheelTools = std::move(loadedTools);
            continue;
        }

        if (key.starts_with("shortcut.")) {
            const auto action = ParseShortcutActionConfigName(key.substr(9));
            if (!action.has_value()) {
                continue;
            }

            ShortcutBinding* binding = FindBinding(settings.shortcuts, *action);
            if (binding == nullptr) {
                continue;
            }

            if (portableShortcuts) {
                if (value.empty()) {
                    binding->sequence = QKeySequence {};
                    continue;
                }
                if (const auto parsed = FromPortableText(value); parsed.has_value()) {
                    binding->sequence = *parsed;
                }
                continue;
            }

            const std::vector<std::string_view> parts = SplitAscii(value, ',');
            if (parts.size() != 4U) {
                continue;
            }

            const auto parsedKey = ParseUnsignedInt(parts[0]);
            const auto parsedCtrl = ParseUnsignedInt(parts[1]);
            const auto parsedShift = ParseUnsignedInt(parts[2]);
            const auto parsedAlt = ParseUnsignedInt(parts[3]);
            if (!parsedKey.has_value() || !parsedCtrl.has_value() || !parsedShift.has_value() || !parsedAlt.has_value()) {
                continue;
            }

            const LegacyKeyRecord record {
                *parsedKey,
                *parsedCtrl != 0U,
                *parsedShift != 0U,
                *parsedAlt != 0U,
            };
            if (const auto sequence = KeySequenceFromLegacyRecord(record); sequence.has_value()) {
                binding->sequence = *sequence;
            }
            continue;
        }
    }

    settings.savedPalettes.clear();
    for (auto& [unusedIndex, pendingPalette] : pendingNamedPalettes) {
        static_cast<void>(unusedIndex);
        if (!pendingPalette.hasName || !pendingPalette.hasColors || pendingPalette.colors.empty()) {
            continue;
        }

        const std::u16string trimmedName = TrimWide(pendingPalette.name);
        if (trimmedName.empty()) {
            continue;
        }

        const auto existingPalette = std::find_if(
            settings.savedPalettes.begin(),
            settings.savedPalettes.end(),
            [&](const NamedPalette& palette) {
                return EqualPaletteNames(palette.name, trimmedName);
            });
        if (existingPalette != settings.savedPalettes.end()) {
            existingPalette->name = trimmedName;
            existingPalette->colors = pendingPalette.colors;
        } else {
            settings.savedPalettes.push_back({ trimmedName, pendingPalette.colors });
        }
    }

    RecentDocumentCollector recentCollector;
    for (auto it = pendingRecentDocumentPaths.rbegin(); it != pendingRecentDocumentPaths.rend(); ++it) {
        recentCollector.Record(it->second);
    }
    settings.recentDocumentPaths = recentCollector.Take();

    if (settings.palette == std::vector<std::uint32_t>(kLegacyDefaultPalette.begin(), kLegacyDefaultPalette.end())) {
        settings.palette = DefaultPalette();
    }
    if (settings.palette.empty()) {
        settings.palette = DefaultPalette();
    }
    settings.quickSizeSliderMax = std::clamp(settings.quickSizeSliderMax, 1.0f, kMaxBrushSize);
    settings.drawBrushSize = std::min(settings.drawBrushSize, settings.quickSizeSliderMax);
    settings.eraseBrushSize = std::min(settings.eraseBrushSize, settings.quickSizeSliderMax);
    settings.colorIndex = std::min(loadedColorIndex.value_or(settings.colorIndex), settings.palette.size() - 1U);
    settings.paletteIndex = std::min(loadedSettingsPaletteIndex.value_or(settings.paletteIndex), settings.palette.size() - 1U);
    return settings;
}

std::string SettingsStoreQt::Serialize(const Settings& settings) {
    if (settings.palette.empty()) {
        return {};
    }

    std::ostringstream output;

    output << "palette=";
    WritePaletteColors(output, settings.palette);
    output << '\n';
    for (std::size_t index = 0; index < settings.savedPalettes.size(); ++index) {
        const NamedPalette& palette = settings.savedPalettes[index];
        if (palette.name.empty() || palette.colors.empty()) {
            continue;
        }
        output << "saved_palette_" << (index + 1U) << "_name=" << cnote::Utf16ToUtf8(palette.name) << '\n';
        output << "saved_palette_" << (index + 1U) << "_colors=";
        WritePaletteColors(output, palette.colors);
        output << '\n';
    }
    output << "active_color=" << settings.colorIndex << '\n';
    output << "settings_palette_index=" << settings.paletteIndex << '\n';
    output << "default_page_color=" << std::hex << std::uppercase << settings.defaultPageColor
           << std::dec << std::nouppercase << '\n';
    for (std::size_t index = 0; index < settings.recentDocumentPaths.size(); ++index) {
        output << "recent_document_" << (index + 1U) << '=' << cnote::PathToUtf8(settings.recentDocumentPaths[index]) << '\n';
    }
    output << "draw_brush_size=" << settings.drawBrushSize << '\n';
    output << "erase_brush_size=" << settings.eraseBrushSize << '\n';
    output << "quick_size_slider_max=" << settings.quickSizeSliderMax << '\n';
    output << "brush_size_presets=" << FormatBrushSizePresets(settings.brushSizePresets) << '\n';
    output << "draw_cursor_kind=" << DrawCursorKindConfigName(settings.drawCursorKind) << '\n';
    output << "draw_cursor_size=" << settings.drawCursorSize << '\n';
    output << "input_smoothing=" << settings.inputSmoothing << '\n';
    output << "input_spacing=" << settings.inputSpacing << '\n';
    output << "stroke_smoothing=" << settings.strokeSmoothing << '\n';
    output << "dash_length=" << settings.dashLength << '\n';
    output << "dash_gap=" << settings.dashGap << '\n';
    output << "auto_save_interval_seconds=" << settings.autoSaveIntervalSeconds << '\n';
    output << "auto_hide_toolbar=" << (settings.autoHideToolbar ? 1 : 0) << '\n';
    output << "embed_project_in_exported_pdfs=" << (settings.embedProjectInExportedPdfs ? 1 : 0) << '\n';
    for (std::size_t index = 0; index < settings.zoomPresets.size(); ++index) {
        output << "zoom_preset_" << (index + 1U) << '=' << settings.zoomPresets[index] << '\n';
    }

    output << "wheel_tools=";
    for (std::size_t index = 0; index < settings.wheelTools.size(); ++index) {
        if (index > 0U) {
            output << ',';
        }
        output << ToolConfigName(settings.wheelTools[index]);
    }
    output << '\n';

    output << "shortcut_format=" << kShortcutFormatValue << '\n';
    for (const ShortcutBinding& binding : settings.shortcuts) {
        output << "shortcut." << ShortcutActionConfigName(binding.action) << '='
               << ToPortableText(binding.sequence).toStdString() << '\n';
    }

    return output.str();
}

bool SettingsStoreQt::Save(const Settings& settings) const {
    if (settings.palette.empty()) {
        return false;
    }

    const std::string text = Serialize(settings);
    if (text.empty()) {
        return false;
    }

    std::error_code error;
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) {
            return false;
        }
    }

    QSaveFile file(PathToQString(path_));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    const qsizetype size = static_cast<qsizetype>(text.size());
    if (file.write(text.data(), size) != size) {
        file.cancelWriting();
        return false;
    }

    return file.commit();
}

}  // namespace cnote::settings
