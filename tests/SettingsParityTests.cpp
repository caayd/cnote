// Windows parity harness for the Qt settings service (migration plan 6.4/6.5).
//
// Runs the real legacy parser (`App::LoadAppSettings`, src/AppSettings.cpp) and
// `cnote::settings::SettingsStoreQt::ParseText` over the same settings payload
// and diffs every parsed field.  Also checks that the Qt shortcut table matches
// src/ShortcutConfig.cpp one-for-one, that the legacy writer and the Qt writer
// agree on every shared key, that the Windows settings path is unchanged, and
// that a settings persist never clears the shared document dirty flag.
//
// Windows-only: it links the legacy cnote_core target.

#include <windows.h>

#include "App.h"
#include "ShortcutConfig.h"
#include "Unicode.h"
#include "settings/SettingsStoreQt.h"

#include <QGuiApplication>
#include <QKeySequence>
#include <QString>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace cnote {

// Friend of App (declared in App.h).  Defined only in this test executable to
// read the legacy parser's private state; no other binary links this TU.
struct ProjectSessionTests {
    struct Snapshot {
        std::vector<std::uint32_t> palette;
        std::vector<std::pair<std::wstring, std::vector<std::uint32_t>>> savedPalettes;
        std::size_t colorIndex = 0;
        std::size_t paletteIndex = 0;
        std::uint32_t defaultPageColor = 0;
        std::vector<std::filesystem::path> recent;
        float drawBrushSize = 0.0f;
        float eraseBrushSize = 0.0f;
        float quickSizeSliderMax = 0.0f;
        std::vector<float> brushSizePresets;
        App::DrawCursorKind drawCursorKind = App::DrawCursorKind::Dot;
        float drawCursorSize = 0.0f;
        float inputSpacing = 0.0f;
        float inputSmoothing = 0.0f;
        float strokeSmoothing = 0.0f;
        float dashLength = 0.0f;
        float dashGap = 0.0f;
        unsigned int autoSaveIntervalSeconds = 0;
        bool autoHideToolbar = false;
        bool embedProjectInExportedPdfs = false;
        std::array<float, App::kZoomPresetCount> zoomPresets {};
        std::vector<App::Tool> wheelTools;
        std::vector<std::pair<App::ShortcutAction, App::KeyBinding>> shortcuts;
    };

    static Snapshot Capture(const App& app) {
        Snapshot snapshot;
        snapshot.palette = app.palette_;
        for (const App::NamedPalette& palette : app.savedPalettes_) {
            snapshot.savedPalettes.push_back({ palette.name, palette.colors });
        }
        snapshot.colorIndex = app.colorIndex_;
        snapshot.paletteIndex = app.settings_.paletteIndex;
        snapshot.defaultPageColor = app.settings_.defaultPageColor;
        snapshot.recent = app.recentDocumentPaths_;
        snapshot.drawBrushSize = app.settings_.drawBrushSize;
        snapshot.eraseBrushSize = app.settings_.eraseBrushSize;
        snapshot.quickSizeSliderMax = app.settings_.quickSizeSliderMax;
        snapshot.brushSizePresets = app.settings_.brushSizePresets;
        snapshot.drawCursorKind = app.settings_.drawCursorKind;
        snapshot.drawCursorSize = app.settings_.drawCursorSize;
        snapshot.inputSpacing = app.settings_.inputSpacing;
        snapshot.inputSmoothing = app.settings_.inputSmoothing;
        snapshot.strokeSmoothing = app.settings_.strokeSmoothing;
        snapshot.dashLength = app.settings_.dashLength;
        snapshot.dashGap = app.settings_.dashGap;
        snapshot.autoSaveIntervalSeconds = app.settings_.autoSaveIntervalSeconds;
        snapshot.autoHideToolbar = app.settings_.autoHideToolbar;
        snapshot.embedProjectInExportedPdfs = app.settings_.embedProjectInExportedPdfs;
        snapshot.zoomPresets = app.settings_.zoomPresets;
        snapshot.wheelTools = app.wheel_.tools;
        snapshot.shortcuts = app.shortcuts_.bindings;
        return snapshot;
    }

    static void SaveSettings(App& app) { app.SaveAppSettings(); }
    static void SetDirty(App& app, bool dirty) { app.session_->dirty = dirty; }
    static bool IsDirty(const App& app) { return app.session_->dirty; }
};

}  // namespace cnote

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// The legacy App reads APPDATA with `_wgetenv` (the executable's CRT) while
// Qt's own helpers read it through QtCore's CRT; on toolchains that do not
// share one CRT instance both must be updated.
void SetAppData(const std::filesystem::path& path) {
    _wputenv_s(L"APPDATA", path.c_str());
    qputenv("APPDATA", path.string().c_str());
}

void ClearAppData() {
    _wputenv_s(L"APPDATA", L"");
    qputenv("APPDATA", "");
}

const char* LegacyToolName(cnote::App::Tool tool) {
    switch (tool) {
    case cnote::App::Tool::Select:
        return "Select";
    case cnote::App::Tool::Move:
        return "Move";
    case cnote::App::Tool::Text:
        return "Text";
    case cnote::App::Tool::Latex:
        return "Latex";
    case cnote::App::Tool::Draw:
        return "Draw";
    case cnote::App::Tool::Pan:
        return "Pan";
    case cnote::App::Tool::Erase:
        return "Erase";
    case cnote::App::Tool::Zoom:
        return "Zoom";
    default:
        return "";
    }
}

const char* SettingsToolName(cnote::settings::Tool tool) {
    switch (tool) {
    case cnote::settings::Tool::Select:
        return "Select";
    case cnote::settings::Tool::Move:
        return "Move";
    case cnote::settings::Tool::Text:
        return "Text";
    case cnote::settings::Tool::Latex:
        return "Latex";
    case cnote::settings::Tool::Draw:
        return "Draw";
    case cnote::settings::Tool::Pan:
        return "Pan";
    case cnote::settings::Tool::Erase:
        return "Erase";
    case cnote::settings::Tool::Zoom:
        return "Zoom";
    default:
        return "";
    }
}

const cnote::settings::ShortcutBinding* Binding(
    const cnote::settings::Settings& settings,
    cnote::settings::ShortcutAction action) {
    for (const auto& binding : settings.shortcuts) {
        if (binding.action == action) {
            return &binding;
        }
    }
    return nullptr;
}

// Drops the shortcut lines (and, for the Qt text, the format marker) so the
// remaining keys can be compared byte-for-byte between writers.
std::string WithoutShortcutLines(const std::string& text, bool dropMarker) {
    std::istringstream input(text);
    std::string line;
    std::string result;
    while (std::getline(input, line)) {
        if (line.starts_with("shortcut.")) {
            continue;
        }
        if (dropMarker && line.starts_with("shortcut_format=")) {
            continue;
        }
        result += line;
        result += '\n';
    }
    return result;
}

std::string ShortcutLines(const std::string& text) {
    std::istringstream input(text);
    std::string line;
    std::string result;
    while (std::getline(input, line)) {
        if (line.starts_with("shortcut.")) {
            result += line;
            result += '\n';
        }
    }
    return result;
}

std::string BuildPayload(const std::filesystem::path& scratch) {
    const std::filesystem::path first = scratch / "alpha.cnote";
    const std::filesystem::path second = scratch / "beta.cnote";

    std::ostringstream payload;
    payload << "palette=FF1A2B3C,ff4D5E6F,FF778899\n";
    payload << "saved_palette_1_name=Warm\n";
    payload << "saved_palette_1_colors=FF111111,FF222222\n";
    payload << "saved_palette_2_name=Cool\n";
    payload << "saved_palette_2_colors=FF333333\n";
    payload << "saved_palette_3_colors=FF444444\n";
    payload << "active_color=2\n";
    payload << "settings_palette_index=7\n";
    payload << "default_page_color=FFABCDEF\n";
    payload << "recent_cnote_2=" << second.string() << "\n";
    payload << "recent_document_1=" << first.string() << "\n";
    payload << "input_smoothing=0.99\n";
    payload << "draw_brush_size=7.5\n";
    payload << "erase_brush_size=0.1\n";
    payload << "quick_size_slider_max=9\n";
    payload << "brush_size_presets= 4 , 1.25 ,1.25, 2 \n";
    payload << "draw_cursor_kind=precision_crosshair\n";
    payload << "draw_cursor_size=1\n";
    payload << "input_spacing=9\n";
    payload << "stroke_smoothing=2\n";
    payload << "dash_length=0.1\n";
    payload << "dash_gap=99\n";
    payload << "auto_save_interval_seconds=5000\n";
    payload << "auto_hide_toolbar=1\n";
    payload << "embed_project_in_exported_pdfs=0\n";
    payload << "zoom_preset_1=2.25\n";
    payload << "zoom_preset_3=0\n";
    payload << "zoom_preset_4=abc\n";
    payload << "wheel_tools=Draw, Zoom ,Draw,Text\n";
    // Legacy four-number records, no marker: this is the on-disk Windows form.
    payload << "shortcut.ToolDraw=68,0,0,0\n";
    payload << "shortcut.Undo=90,1,0,0\n";
    payload << "shortcut.Redo=0,0,0,0\n";
    payload << "shortcut.ExportPdf=65,1,1,1\n";
    payload << "shortcut.PreviousPage=33,0,0,0\n";
    payload << "shortcut.NextPage=34,0,0,0\n";
    payload << "shortcut.TogglePdfSidebar=66,1,0,0\n";
    return payload.str();
}

void CompareParsers(
    const cnote::ProjectSessionTests::Snapshot& legacy,
    const cnote::settings::Settings& parsed) {
    namespace s = cnote::settings;

    Check(legacy.palette == parsed.palette, "palette matches the legacy parser");
    Check(legacy.savedPalettes.size() == parsed.savedPalettes.size(), "saved palette count matches");
    for (std::size_t index = 0; index < legacy.savedPalettes.size() && index < parsed.savedPalettes.size(); ++index) {
        const std::wstring& legacyName = legacy.savedPalettes[index].first;
        const std::u16string legacyNameUtf16(reinterpret_cast<const char16_t*>(legacyName.data()), legacyName.size());
        Check(cnote::Utf16ToUtf8(legacyNameUtf16) == cnote::Utf16ToUtf8(parsed.savedPalettes[index].name),
            "saved palette name matches at index " + std::to_string(index));
        Check(legacy.savedPalettes[index].second == parsed.savedPalettes[index].colors,
            "saved palette colors match at index " + std::to_string(index));
    }
    Check(legacy.colorIndex == parsed.colorIndex, "active_color matches");
    Check(legacy.paletteIndex == parsed.paletteIndex, "settings_palette_index matches");
    Check(legacy.defaultPageColor == parsed.defaultPageColor, "default_page_color matches");
    Check(legacy.recent == parsed.recentDocumentPaths, "recent files match");
    Check(legacy.drawBrushSize == parsed.drawBrushSize, "draw_brush_size matches");
    Check(legacy.eraseBrushSize == parsed.eraseBrushSize, "erase_brush_size matches");
    Check(legacy.quickSizeSliderMax == parsed.quickSizeSliderMax, "quick_size_slider_max matches");
    Check(legacy.brushSizePresets == parsed.brushSizePresets, "brush_size_presets matches");
    Check((legacy.drawCursorKind == cnote::App::DrawCursorKind::PrecisionCrosshair)
            == (parsed.drawCursorKind == s::DrawCursorKind::PrecisionCrosshair),
        "draw_cursor_kind matches");
    Check(legacy.drawCursorSize == parsed.drawCursorSize, "draw_cursor_size matches");
    Check(legacy.inputSpacing == parsed.inputSpacing, "input_spacing matches");
    Check(legacy.inputSmoothing == parsed.inputSmoothing, "input_smoothing matches");
    Check(legacy.strokeSmoothing == parsed.strokeSmoothing, "stroke_smoothing matches");
    Check(legacy.dashLength == parsed.dashLength, "dash_length matches");
    Check(legacy.dashGap == parsed.dashGap, "dash_gap matches");
    Check(legacy.autoSaveIntervalSeconds == parsed.autoSaveIntervalSeconds, "auto_save_interval_seconds matches");
    Check(legacy.autoHideToolbar == parsed.autoHideToolbar, "auto_hide_toolbar matches");
    Check(legacy.embedProjectInExportedPdfs == parsed.embedProjectInExportedPdfs, "embed_project_in_exported_pdfs matches");
    for (std::size_t index = 0; index < legacy.zoomPresets.size() && index < parsed.zoomPresets.size(); ++index) {
        Check(legacy.zoomPresets[index] == parsed.zoomPresets[index],
            "zoom_preset_" + std::to_string(index + 1) + " matches");
    }
    Check(legacy.wheelTools.size() == parsed.wheelTools.size(), "wheel_tools count matches");
    for (std::size_t index = 0; index < legacy.wheelTools.size() && index < parsed.wheelTools.size(); ++index) {
        Check(std::strcmp(LegacyToolName(legacy.wheelTools[index]), SettingsToolName(parsed.wheelTools[index])) == 0,
            "wheel_tools entry matches at index " + std::to_string(index));
    }

    // The legacy parser really consumed the custom shortcut records.
    const auto legacyBinding = [&](cnote::App::ShortcutAction action) -> const cnote::App::KeyBinding* {
        for (const auto& [boundAction, binding] : legacy.shortcuts) {
            if (boundAction == action) {
                return &binding;
            }
        }
        return nullptr;
    };
    const cnote::App::KeyBinding* undo = legacyBinding(cnote::App::ShortcutAction::Undo);
    Check(undo != nullptr && undo->key == 90U && undo->ctrl && !undo->shift && !undo->alt,
        "legacy parser read the Undo record");
    const cnote::App::KeyBinding* redo = legacyBinding(cnote::App::ShortcutAction::Redo);
    Check(redo != nullptr && redo->key == 0U, "legacy parser read the unbound Redo record");

    Check(legacy.shortcuts.size() == parsed.shortcuts.size(), "shortcut count matches");
    for (const auto& [action, binding] : legacy.shortcuts) {
        const char* name = cnote::ShortcutActionConfigName(action);
        const auto parsedAction = s::ParseShortcutActionConfigName(name);
        Check(parsedAction.has_value(), std::string("parsed action for ") + name);
        if (!parsedAction.has_value()) {
            continue;
        }
        const s::ShortcutBinding* parsedBinding = Binding(parsed, *parsedAction);
        const auto expected = s::KeySequenceFromLegacyRecord({ binding.key, binding.ctrl, binding.shift, binding.alt });
        Check(parsedBinding != nullptr, std::string("binding present for ") + name);
        if (parsedBinding == nullptr) {
            continue;
        }
        Check(expected.has_value() && parsedBinding->sequence == *expected,
            std::string("shortcut ") + name + " converts from the legacy record");
    }
}

void CompareDefinitions() {
    const auto legacyDefinitions = cnote::ShortcutDefinitions();
    const auto settingsDefinitions = cnote::settings::ShortcutDefinitions();
    Check(legacyDefinitions.size() == settingsDefinitions.size(), "shortcut definition count matches ShortcutConfig.cpp");
    for (const cnote::ShortcutDefinition& definition : legacyDefinitions) {
        const char* name = cnote::ShortcutActionConfigName(definition.action);
        const auto parsedAction = cnote::settings::ParseShortcutActionConfigName(name);
        Check(parsedAction.has_value(), std::string("Qt table has action ") + name);
        if (!parsedAction.has_value()) {
            continue;
        }
        const cnote::settings::ShortcutDefinition* ours = cnote::settings::FindShortcutDefinition(*parsedAction);
        Check(ours != nullptr, std::string("Qt definition for ") + name);
        if (ours == nullptr) {
            continue;
        }
        const std::u16string legacyLabel(reinterpret_cast<const char16_t*>(definition.label));
        Check(cnote::Utf16ToUtf8(legacyLabel) == ours->label, std::string("label matches for ") + name);
        Check(ours->defaultRecord.key == definition.defaultBinding.key
                && ours->defaultRecord.ctrl == definition.defaultBinding.ctrl
                && ours->defaultRecord.shift == definition.defaultBinding.shift
                && ours->defaultRecord.alt == definition.defaultBinding.alt,
            std::string("default record matches for ") + name);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    const std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / (L"cnote-settings-parity-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    std::filesystem::create_directories(scratch, error);
    SetAppData(scratch);

    const std::string payload = BuildPayload(scratch);
    WriteFile(scratch / L"cnote" / L"settings.ini", payload);

    cnote::App legacy(GetModuleHandleW(nullptr));  // constructor runs LoadAppSettings()
    const cnote::ProjectSessionTests::Snapshot snapshot = cnote::ProjectSessionTests::Capture(legacy);
    const cnote::settings::Settings parsed = cnote::settings::SettingsStoreQt::ParseText(payload);

    CompareParsers(snapshot, parsed);
    CompareDefinitions();

    // Both writers agree on every shared key; only the shortcut representation
    // differs (legacy four-number records vs PortableText plus the marker).
    cnote::ProjectSessionTests::SaveSettings(legacy);
    const std::string legacyText = ReadFile(scratch / L"cnote" / L"settings.ini");
    const std::string qtText = cnote::settings::SettingsStoreQt::Serialize(parsed);
    Check(WithoutShortcutLines(legacyText, false) == WithoutShortcutLines(qtText, true),
        "legacy and Qt writers serialize identical non-shortcut keys");
    Check(qtText.find("shortcut_format=qt-portable-v1\n") != std::string::npos, "Qt writer emits the format marker");
    const auto countLines = [](const std::string& text) {
        return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    };
    Check(countLines(ShortcutLines(legacyText)) == countLines(ShortcutLines(qtText))
            && countLines(ShortcutLines(qtText)) == cnote::settings::ShortcutDefinitions().size(),
        "both writers emit one shortcut line per action");

    std::string expectedShortcutLines;
    for (const auto& [action, binding] : snapshot.shortcuts) {
        const auto sequence = cnote::settings::KeySequenceFromLegacyRecord({ binding.key, binding.ctrl, binding.shift, binding.alt });
        Check(sequence.has_value(), "legacy record for the written shortcut converts");
        expectedShortcutLines += std::string("shortcut.") + cnote::ShortcutActionConfigName(action) + "=";
        expectedShortcutLines += sequence.has_value() ? sequence->toString(QKeySequence::PortableText).toStdString() : std::string();
        expectedShortcutLines += '\n';
    }
    Check(ShortcutLines(qtText) == expectedShortcutLines,
        "Qt writer emits portable text for every legacy record");
    Check(static_cast<std::size_t>(std::count(qtText.begin(), qtText.end(), '\n'))
            == static_cast<std::size_t>(std::count(legacyText.begin(), legacyText.end(), '\n')) + 1U,
        "Qt writer adds exactly the format marker line");

    // Windows settings path is unchanged by the Qt helper.
    const std::filesystem::path resolved = cnote::settings::SettingsStoreQt::ResolveSettingsPath();
    std::printf(
        "parity settings path: %s (APPDATA='%s')\n",
        resolved.string().c_str(),
        qEnvironmentVariable("APPDATA").toStdString().c_str());
    Check(resolved == scratch / L"cnote" / L"settings.ini",
        "Qt settings path matches %APPDATA%/cnote/settings.ini");
    ClearAppData();
    Check(cnote::settings::SettingsStoreQt::ResolveSettingsPath() == std::filesystem::current_path() / L"cnote.settings.ini",
        "Qt settings path falls back to the working directory");
    SetAppData(scratch);

    // A settings persist never changes the document dirty flag, whether it
    // succeeds or fails.
    cnote::ProjectSessionTests::SetDirty(legacy, true);
    cnote::ProjectSessionTests::SaveSettings(legacy);
    Check(cnote::ProjectSessionTests::IsDirty(legacy), "successful settings save leaves the document dirty");

    const std::filesystem::path blocked = scratch / L"blocked";
    WriteFile(blocked / L"cnote", "not a directory");
    SetAppData(blocked);
    cnote::ProjectSessionTests::SetDirty(legacy, true);
    cnote::ProjectSessionTests::SaveSettings(legacy);
    Check(cnote::ProjectSessionTests::IsDirty(legacy), "failed settings save leaves the document dirty");
    SetAppData(scratch);

    std::filesystem::remove_all(scratch, error);
    std::printf(g_failures == 0 ? "ALL SETTINGS PARITY CHECKS PASSED\n" : "%d SETTINGS PARITY CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
