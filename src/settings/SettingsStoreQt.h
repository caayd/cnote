#pragma once

#include "Document.h"
#include "settings/KeyBindingQt.h"

#include <QKeySequence>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cnote::settings {

inline constexpr std::size_t kZoomPresetCount = 5;
inline constexpr std::size_t kMaxRecentDocuments = 10;
inline constexpr std::size_t kMaxPaletteColors = 12;
inline constexpr float kMinZoom = 0.35f;
inline constexpr float kMaxZoom = 4.0f;
inline constexpr float kMaxBrushSize = 20.0f;

inline constexpr std::array<std::uint32_t, 10> kDefaultPageColors {
    0xFFFFFFFFU,
    0xFFFFFBF5U,
    0xFFF4F0E6U,
    0xFFE3E7EFU,
    0xFFBFC7D4U,
    0xFF1C2430U,
    0xFF161C26U,
    0xFF12171FU,
    0xFF10161FU,
    0xFF090D12U,
};

// Portable restatements of the legacy App enums that the settings file stores.
enum class Tool {
    Select,
    Move,
    Text,
    Latex,
    Draw,
    Pan,
    Erase,
    Zoom,
};

enum class DrawCursorKind {
    Dot,
    PrecisionCrosshair,
};

struct NamedPalette {
    std::u16string name {};
    std::vector<std::uint32_t> colors {};
};

struct ShortcutBinding {
    ShortcutAction action = ShortcutAction::ToolDraw;
    QKeySequence sequence {};
};

std::vector<std::uint32_t> DefaultPalette();
std::array<ShortcutBinding, kShortcutActionCount> DefaultShortcutBindings();

// Brush-size value codecs shared by the settings file and the dialogs, so no
// snapping/formatting rule is duplicated between the store and the UI.
[[nodiscard]] float SnapBrushSize(float size);
bool TryParseBrushSizePresets(std::string_view value, std::vector<float>& presets);
[[nodiscard]] std::string FormatBrushSizePresets(const std::vector<float>& presets);
[[nodiscard]] std::string FormatBrushSizeValue(float size);

// In-memory image of `settings.ini`; the defaults are the legacy member
// initializers in src/App.h, so a missing file or unparsable field keeps them.
struct Settings {
    std::vector<std::uint32_t> palette = DefaultPalette();
    std::vector<NamedPalette> savedPalettes {};
    std::size_t colorIndex = 0;
    std::size_t paletteIndex = 0;
    std::uint32_t defaultPageColor = kDefaultPageColors.front();
    std::vector<std::filesystem::path> recentDocumentPaths {};
    float drawBrushSize = 3.0f;
    float eraseBrushSize = 3.0f;
    float quickSizeSliderMax = 20.0f;
    std::vector<float> brushSizePresets {};
    // The text tool's pending style: the Ctrl+W text-format popup edits these
    // before a text object exists and AddTextObject seeds them, mirroring how
    // the brush sizes persist per tool.  textFontSize is the on-screen point
    // size at zoom 1; creation divides it by zoom (legacy zoom-relative rule)
    // so new text lands at a constant screen size.
    TextFontFamily textFontFamily = TextFontFamily::ShantellSans;
    bool textBold = false;
    bool textItalic = false;
    bool textUnderline = false;
    TextAlignment textAlignment = TextAlignment::Left;
    float textFontSize = 18.0f;
    DrawCursorKind drawCursorKind = DrawCursorKind::Dot;
    float drawCursorSize = 8.0f;
    float inputSpacing = 0.85f;
    float inputSmoothing = 0.3f;
    float strokeSmoothing = 0.55f;
    float dashLength = 3.0f;
    float dashGap = 2.0f;
    unsigned int autoSaveIntervalSeconds = 60;
    bool autoHideToolbar = false;
    bool embedProjectInExportedPdfs = true;
    std::array<float, kZoomPresetCount> zoomPresets {};
    std::vector<Tool> wheelTools {};
    std::array<ShortcutBinding, kShortcutActionCount> shortcuts = DefaultShortcutBindings();
};

// Portable settings storage for the Qt frontend (plan steps 6.4/6.5).
//
// Path rules:
//   Windows  %APPDATA%/cnote/settings.ini, or `cnote.settings.ini` in the
//            working directory when APPDATA is unset (unchanged legacy rule).
//   other    QStandardPaths::AppConfigLocation plus settings.ini, once the
//            application is named "cnote" with no organization component.
//
// Parse and serialize mirror src/AppSettings.cpp key-for-key.  Persistence goes
// through QSaveFile, so a failed write/commit leaves the previous destination
// untouched.  The store has no document state: nothing here can change a
// document's dirty flag.
class SettingsStoreQt {
public:
    SettingsStoreQt();
    explicit SettingsStoreQt(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    // Missing file: defaults.  Unreadable file: defaults.  Invalid fields keep
    // the legacy ignore/clamp semantics.
    [[nodiscard]] Settings Load() const;

    // Writes `settings`, creating the destination directory first.  Returns
    // false when nothing was written; the previous file is never replaced by a
    // partial or failed write.
    bool Save(const Settings& settings) const;

    static std::filesystem::path ResolveSettingsPath();

    // Exposed for the parity harness and tests: parse/serialize the exact text
    // form shared with the legacy Win32 writer.
    [[nodiscard]] static Settings ParseText(std::string_view text);
    [[nodiscard]] static std::string Serialize(const Settings& settings);

private:
    std::filesystem::path path_ {};
};

}  // namespace cnote::settings
