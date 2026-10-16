// Qt settings/key-binding service test (migration plan steps 6.4/6.5).
//
// Self-contained: everything runs against temporary directories.  Defends the
// observable contract a plausible bug would break: every capturable legacy
// record round-trips through QKeySequence PortableText, malformed or unknown
// mappings keep the action default instead of binding a wrong key, the payload
// parses with the legacy defaults/clamping, the storage location follows the
// Windows/non-Windows rules, and a failed persist leaves the previous file
// intact without touching document dirty state.

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "settings/SettingsStoreQt.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

std::string Portable(const QKeySequence& sequence) {
    return sequence.toString(QKeySequence::PortableText).toStdString();
}

std::filesystem::path ScratchDirectory(const char* name) {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    const std::filesystem::path scratch = base / (std::string("cnote-settings-") + name);
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    std::filesystem::create_directories(scratch, error);
    Check(!error, std::string("create scratch directory ") + scratch.string());
    return scratch;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string RecordText(const cnote::settings::LegacyKeyRecord& record) {
    char buffer[64] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%u,%d,%d,%d",
        record.key,
        record.ctrl ? 1 : 0,
        record.shift ? 1 : 0,
        record.alt ? 1 : 0);
    return buffer;
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

bool SameSettings(const cnote::settings::Settings& a, const cnote::settings::Settings& b, std::string& difference) {
    const auto fail = [&](const std::string& field) {
        difference = field;
        return false;
    };
    if (a.palette != b.palette) return fail("palette");
    if (a.savedPalettes.size() != b.savedPalettes.size()) return fail("savedPalettes.size");
    for (std::size_t index = 0; index < a.savedPalettes.size(); ++index) {
        if (a.savedPalettes[index].name != b.savedPalettes[index].name) return fail("savedPalettes.name");
        if (a.savedPalettes[index].colors != b.savedPalettes[index].colors) return fail("savedPalettes.colors");
    }
    if (a.colorIndex != b.colorIndex) return fail("colorIndex");
    if (a.paletteIndex != b.paletteIndex) return fail("paletteIndex");
    if (a.defaultPageColor != b.defaultPageColor) return fail("defaultPageColor");
    if (a.defaultPageSizePreset != b.defaultPageSizePreset) return fail("defaultPageSizePreset");
    if (a.recentDocumentPaths != b.recentDocumentPaths) return fail("recentDocumentPaths");
    if (a.drawBrushSize != b.drawBrushSize) return fail("drawBrushSize");
    if (a.eraseBrushSize != b.eraseBrushSize) return fail("eraseBrushSize");
    if (a.quickSizeSliderMax != b.quickSizeSliderMax) return fail("quickSizeSliderMax");
    if (a.brushSizePresets != b.brushSizePresets) return fail("brushSizePresets");
    if (a.textFontFamily != b.textFontFamily) return fail("textFontFamily");
    if (a.textBold != b.textBold) return fail("textBold");
    if (a.textItalic != b.textItalic) return fail("textItalic");
    if (a.textUnderline != b.textUnderline) return fail("textUnderline");
    if (a.textAlignment != b.textAlignment) return fail("textAlignment");
    if (a.textSizePreset != b.textSizePreset) return fail("textSizePreset");
    if (a.textSizePresets != b.textSizePresets) return fail("textSizePresets");
    if (a.drawCursorKind != b.drawCursorKind) return fail("drawCursorKind");
    if (a.drawCursorSize != b.drawCursorSize) return fail("drawCursorSize");
    if (a.inputSpacing != b.inputSpacing) return fail("inputSpacing");
    if (a.inputSmoothing != b.inputSmoothing) return fail("inputSmoothing");
    if (a.strokeSmoothing != b.strokeSmoothing) return fail("strokeSmoothing");
    if (a.dashLength != b.dashLength) return fail("dashLength");
    if (a.dashGap != b.dashGap) return fail("dashGap");
    if (a.autoSaveIntervalSeconds != b.autoSaveIntervalSeconds) return fail("autoSaveIntervalSeconds");
    if (a.autoHideToolbar != b.autoHideToolbar) return fail("autoHideToolbar");
    if (a.embedProjectInExportedPdfs != b.embedProjectInExportedPdfs) return fail("embedProjectInExportedPdfs");
    if (a.zoomPresets != b.zoomPresets) return fail("zoomPresets");
    if (a.wheelTools != b.wheelTools) return fail("wheelTools");
    if (a.shortcuts.size() != b.shortcuts.size()) return fail("shortcuts.size");
    for (std::size_t index = 0; index < a.shortcuts.size(); ++index) {
        if (a.shortcuts[index].action != b.shortcuts[index].action) return fail("shortcuts.action");
        if (a.shortcuts[index].sequence != b.shortcuts[index].sequence) return fail("shortcuts.sequence");
    }
    return true;
}

void CheckSameSettings(const cnote::settings::Settings& a, const cnote::settings::Settings& b, const std::string& label) {
    std::string difference = "(none)";
    Check(SameSettings(a, b, difference), label + " (first difference: " + difference + ")");
}

// 1. Every capturable action: legacy record -> QKeySequence -> portable text -> QKeySequence.
void RoundTripActions() {
    std::printf("%-14s %-14s %-16s %s\n", "action", "legacy record", "portable text", "round trip");
    for (const cnote::settings::ShortcutDefinition& definition : cnote::settings::ShortcutDefinitions()) {
        const char* name = cnote::settings::ShortcutActionConfigName(definition.action);
        const auto sequence = cnote::settings::KeySequenceFromLegacyRecord(definition.defaultRecord);
        Check(sequence.has_value(), std::string(name) + " default record converts");
        if (!sequence.has_value()) {
            continue;
        }
        if (definition.defaultRecord.key == 0U) {
            Check(sequence->isEmpty(), std::string(name) + " unbound default is an empty sequence");
            std::printf("%-14s %-14s %-16s %s\n", name, "(unbound)", "", "unbound");
            continue;
        }

        const std::string portable = Portable(*sequence);
        const auto reparsed = cnote::settings::FromPortableText(portable);
        const bool roundTrips = reparsed.has_value() && *reparsed == *sequence;
        Check(roundTrips, std::string(name) + " portable text round trip");
        std::printf(
            "%-14s %-14s %-16s %s\n",
            name,
            RecordText(definition.defaultRecord).c_str(),
            portable.c_str(),
            roundTrips ? "ok" : "MISMATCH");
    }
}

// 2. Every virtual key in the conversion table round-trips through PortableText.
void RoundTripVirtualKeyTable() {
    std::vector<unsigned int> keys;

    constexpr unsigned int kNavigation[] {
        0x08, 0x09, 0x0C, 0x0D, 0x1B, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
        0x2C, 0x2D, 0x2E, 0x13, 0x14, 0x90, 0x91, 0x5B, 0x5D,
    };
    constexpr unsigned int kOem[] { 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE };
    constexpr unsigned int kNumpad[] { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F };

    for (unsigned int key = 'A'; key <= 'Z'; ++key) keys.push_back(key);
    for (unsigned int key = '0'; key <= '9'; ++key) keys.push_back(key);
    for (unsigned int key = 0x70; key <= 0x87; ++key) keys.push_back(key);
    keys.insert(keys.end(), std::begin(kNavigation), std::end(kNavigation));
    keys.insert(keys.end(), std::begin(kOem), std::end(kOem));
    keys.insert(keys.end(), std::begin(kNumpad), std::end(kNumpad));

    std::size_t roundTrips = 0;
    for (const unsigned int key : keys) {
        for (unsigned int modifiers = 0; modifiers < 4; ++modifiers) {
            const cnote::settings::LegacyKeyRecord record {
                key,
                (modifiers & 1U) != 0U,
                (modifiers & 2U) != 0U,
                false,
            };
            const auto sequence = cnote::settings::KeySequenceFromLegacyRecord(record);
            if (!sequence.has_value()) {
                Check(false, "VK " + std::to_string(key) + " has no Qt key");
                continue;
            }
            const std::string portable = Portable(*sequence);
            const auto reparsed = cnote::settings::FromPortableText(portable);
            Check(reparsed.has_value() && *reparsed == *sequence,
                "VK " + std::to_string(key) + " record " + RecordText(record) + " portable '" + portable + "' round trip");
            ++roundTrips;
        }
    }
    std::printf("virtual-key table: %zu keys, %zu round trips\n", keys.size(), roundTrips);
    Check(keys.size() >= 100, "table covers letters, digits, F1-F24, navigation/editing, OEM punctuation and numpad");

    // Unknown / modifier virtual keys never become a Qt key.
    constexpr unsigned int kUnmapped[] { 0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xDF, 0xE2, 0xFF, 0x1000 };
    for (const unsigned int key : kUnmapped) {
        Check(cnote::settings::QtKeyFromVirtualKey(key) == 0, "VK " + std::to_string(key) + " is unmapped");
        Check(
            !cnote::settings::KeySequenceFromLegacyRecord({ key, true, false, false }).has_value(),
            "VK " + std::to_string(key) + " record is rejected, not bound");
    }
}

// 3. Malformed / partial / unknown records keep the action default.
void MalformedRecordsKeepDefaults() {
    const std::string legacyPayload =
        "shortcut.Undo=90,1,0,0\n"
        "shortcut.Redo=255,0,0,0\n"
        "shortcut.Open=79,1\n"
        "shortcut.Save=abc,1,0,0\n"
        "shortcut.New=0,0,0,0\n"
        "shortcut.Copy=67,4,0,0\n"
        "shortcut.Cut=88,1,0,1\n"
        "shortcut.ZoomPreset1=112,0,0,0\n"
        "shortcut.ToolDraw=1,2,3\n"
        "shortcut.Bogus=65,1,0,0\n";
    const cnote::settings::Settings legacy = cnote::settings::SettingsStoreQt::ParseText(legacyPayload);

    Check(Binding(legacy, cnote::settings::ShortcutAction::Undo)->sequence == QKeySequence(Qt::CTRL | Qt::Key_Z),
        "well-formed legacy record binds Undo");
    Check(Binding(legacy, cnote::settings::ShortcutAction::Redo)->sequence
            == cnote::settings::DefaultKeySequence(cnote::settings::ShortcutAction::Redo),
        "unknown virtual key keeps the Redo default");
    Check(Binding(legacy, cnote::settings::ShortcutAction::Open)->sequence
            == cnote::settings::DefaultKeySequence(cnote::settings::ShortcutAction::Open),
        "partial legacy record keeps the Open default");
    Check(Binding(legacy, cnote::settings::ShortcutAction::Save)->sequence
            == cnote::settings::DefaultKeySequence(cnote::settings::ShortcutAction::Save),
        "non-numeric legacy record keeps the Save default");
    Check(Binding(legacy, cnote::settings::ShortcutAction::New)->sequence.isEmpty(),
        "all-zero legacy record means unbound");
    Check(Binding(legacy, cnote::settings::ShortcutAction::Copy)->sequence == QKeySequence(Qt::CTRL | Qt::Key_C),
        "nonzero modifier flag means Ctrl");
    Check(Binding(legacy, cnote::settings::ShortcutAction::Cut)->sequence
            == QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_X),
        "legacy modifier flags combine");
    Check(Binding(legacy, cnote::settings::ShortcutAction::ZoomPreset1)->sequence == QKeySequence(Qt::Key_F1),
        "F1 legacy record converts through the table");
    Check(Binding(legacy, cnote::settings::ShortcutAction::ToolDraw)->sequence
            == cnote::settings::DefaultKeySequence(cnote::settings::ShortcutAction::ToolDraw),
        "three-part record keeps the ToolDraw default");

    const std::string portablePayload =
        "shortcut_format=qt-portable-v1\n"
        "shortcut.Undo=Ctrl+Ply\n"
        "shortcut.Redo=NotAKey\n"
        "shortcut.New=\n"
        "shortcut.Save=Ctrl+Shift+S\n"
        "shortcut.Open=Ctrl+O\n";
    const cnote::settings::Settings portable = cnote::settings::SettingsStoreQt::ParseText(portablePayload);
    Check(Binding(portable, cnote::settings::ShortcutAction::Undo)->sequence
            == cnote::settings::DefaultKeySequence(cnote::settings::ShortcutAction::Undo),
        "malformed portable text keeps the Undo default");
    Check(Binding(portable, cnote::settings::ShortcutAction::Redo)->sequence
            == cnote::settings::DefaultKeySequence(cnote::settings::ShortcutAction::Redo),
        "unparsable portable text keeps the Redo default");
    Check(Binding(portable, cnote::settings::ShortcutAction::New)->sequence.isEmpty(),
        "empty portable text means unbound");
    Check(Binding(portable, cnote::settings::ShortcutAction::Save)->sequence
            == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S),
        "portable text binds Save");
    Check(Binding(portable, cnote::settings::ShortcutAction::Open)->sequence
            == QKeySequence(Qt::CTRL | Qt::Key_O),
        "portable text binds Open");

    const cnote::settings::Settings wrongMarker = cnote::settings::SettingsStoreQt::ParseText(
        "shortcut_format=qt-portable-v2\n"
        "shortcut.Undo=90,1,0,0\n");
    Check(Binding(wrongMarker, cnote::settings::ShortcutAction::Undo)->sequence
            == QKeySequence(Qt::CTRL | Qt::Key_Z),
        "a non-matching marker falls back to the legacy record format");
}

// 4. A real payload parses to the legacy defaults/clamping.
void ParsesRealPayload(const std::filesystem::path& scratch) {
    const std::filesystem::path first = scratch / "alpha.cnote";
    const std::filesystem::path second = scratch / "beta.cnote";
    const std::filesystem::path third = scratch / "gamma.cnote";
    const std::filesystem::path ignored = scratch / "notes.txt";

    std::ostringstream payload;
    payload << "palette=FF112233,ff445566\n";
    payload << "saved_palette_2_name=Beta\n";
    payload << "saved_palette_2_colors=FF000000\n";
    payload << "saved_palette_1_name=Alpha\n";
    payload << "saved_palette_1_colors=FF111111,FF222222\n";
    payload << "saved_palette_3_name=NoColors\n";
    payload << "active_color=5\n";
    payload << "settings_palette_index=9\n";
    payload << "default_page_color=FFEEDDCC\n";
    payload << "default_page_size_preset=4\n";
    payload << "recent_cnote_2=" << second.string() << "\n";
    payload << "recent_document_1=" << first.string() << "\n";
    payload << "recent_document_3=" << third.string() << "\n";
    payload << "recent_document_4=" << ignored.string() << "\n";
    payload << "recent_document_11=" << (scratch / "out-of-range.cnote").string() << "\n";
    payload << "input_smoothing=0.99\n";
    payload << "draw_brush_size=50\n";
    payload << "erase_brush_size=0.1\n";
    payload << "quick_size_slider_max=99\n";
    payload << "brush_size_presets= 3 , 1.5 ,1.5, 2 \n";
    payload << "draw_cursor_kind=precision_crosshair\n";
    payload << "draw_cursor_size=1\n";
    payload << "input_spacing=9\n";
    payload << "stroke_smoothing=2\n";
    payload << "dash_length=0.1\n";
    payload << "dash_gap=99\n";
    payload << "auto_save_interval_seconds=5000\n";
    payload << "auto_hide_toolbar=1\n";
    payload << "embed_project_in_exported_pdfs=0\n";
    payload << "zoom_preset_2=10\n";
    payload << "zoom_preset_3=0\n";
    payload << "zoom_preset_4=abc\n";
    payload << "text_font_family=monospace\n";
    payload << "text_bold=1\n";
    payload << "text_italic=0\n";
    payload << "text_underline=3\n";
    payload << "text_alignment=right\n";
    payload << "text_size_preset=9\n";
    payload << "text_size_small=2\n";
    payload << "text_size_medium=28\n";
    payload << "text_size_large=bogus\n";
    payload << "wheel_tools=Draw, Zoom ,Draw,Bogus\n";
    payload << "malformed-line-without-separator\n";
    payload << "# comment=ignored\n";
    const std::string text = payload.str();

    const cnote::settings::Settings settings = cnote::settings::SettingsStoreQt::ParseText(text);
    namespace s = cnote::settings;

    Check(settings.palette == std::vector<std::uint32_t>({ 0xFF112233U, 0xFF445566U }), "palette parsed");
    Check(settings.savedPalettes.size() == 2U, "saved palettes need both name and colors");
    Check(settings.savedPalettes.size() == 2U && settings.savedPalettes[0].name == u"Alpha"
            && settings.savedPalettes[0].colors == std::vector<std::uint32_t>({ 0xFF111111U, 0xFF222222U }),
        "saved palette 1 keeps name and colors");
    Check(settings.savedPalettes.size() == 2U && settings.savedPalettes[1].name == u"Beta"
            && settings.savedPalettes[1].colors == std::vector<std::uint32_t>({ 0xFF000000U }),
        "saved palette 2 keeps name and colors");
    Check(settings.colorIndex == 1U, "active_color clamps to the palette size");
    Check(settings.paletteIndex == 1U, "settings_palette_index clamps to the palette size");
    Check(settings.defaultPageColor == 0xFFEEDDCCU, "default_page_color parsed as hex");
    Check(settings.defaultPageSizePreset == 4U, "default_page_size_preset parses an in-range index");
    Check(settings.recentDocumentPaths.size() == 3U, "recent files keep index 1..10 and drop other extensions");
    Check(settings.recentDocumentPaths.size() == 3U
            && settings.recentDocumentPaths[0] == first
            && settings.recentDocumentPaths[1] == second
            && settings.recentDocumentPaths[2] == third,
        "recent files end up in index order with recent_cnote_ as fallback");
    Check(settings.inputSmoothing == 0.95f, "input_smoothing clamps to 0.95");
    Check(settings.drawBrushSize == 20.0f, "draw_brush_size clamps to 20");
    Check(settings.eraseBrushSize == 0.5f, "erase_brush_size clamps to 0.5");
    Check(settings.quickSizeSliderMax == 20.0f, "quick_size_slider_max clamps to 20");
    Check(settings.brushSizePresets == std::vector<float>({ 1.5f, 2.0f, 3.0f }), "brush presets snap, sort and dedupe");
    Check(settings.drawCursorKind == s::DrawCursorKind::PrecisionCrosshair, "draw_cursor_kind parsed");
    Check(settings.drawCursorSize == 4.0f, "draw_cursor_size clamps to 4");
    Check(settings.inputSpacing == 4.05f, "input_spacing clamps to 4.05");
    Check(settings.strokeSmoothing == 1.0f, "stroke_smoothing clamps to 1");
    Check(settings.dashLength == 0.5f, "dash_length clamps to 0.5");
    Check(settings.dashGap == 8.0f, "dash_gap clamps to 8");
    Check(settings.autoSaveIntervalSeconds == 300U, "auto_save_interval_seconds clamps to 300");
    Check(settings.autoHideToolbar, "auto_hide_toolbar parsed");
    Check(!settings.embedProjectInExportedPdfs, "embed_project_in_exported_pdfs parsed");
    Check(settings.zoomPresets[1] == 4.0f, "zoom preset clamps to kMaxZoom");
    Check(settings.zoomPresets[2] == 0.0f, "non-positive zoom preset stays unset");
    Check(settings.zoomPresets[3] == 0.0f, "unparsable zoom preset keeps the default");

    Check(settings.textFontFamily == cnote::TextFontFamily::Monospace, "text_font_family parsed");
    Check(settings.textBold && !settings.textItalic && settings.textUnderline, "text style flags parsed");
    Check(settings.textAlignment == cnote::TextAlignment::Right, "text_alignment parsed");
    Check(settings.textSizePreset == 2U, "text_size_preset clamps to the last preset");
    Check(settings.textSizePresets[0] == 4.0f && settings.textSizePresets[1] == 28.0f
            && settings.textSizePresets[2] == 36.0f,
        "text sizes clamp and keep defaults on bad input");
    Check(settings.wheelTools == std::vector<s::Tool>({ s::Tool::Draw, s::Tool::Zoom }),
        "wheel_tools dedupes known tools in order");

    // Serialize/parse round trip preserves every value.
    const std::string serialized = cnote::settings::SettingsStoreQt::Serialize(settings);
    Check(serialized.find("palette=FF112233,FF445566\n") != std::string::npos, "serializer writes the palette");
    Check(serialized.find("default_page_color=FFEEDDCC\n") != std::string::npos, "serializer writes hex colors uppercase");
    Check(serialized.find("default_page_size_preset=4\n") != std::string::npos, "serializer writes the page-size preset index");
    Check(serialized.find("shortcut_format=qt-portable-v1\n") != std::string::npos, "serializer writes the marker");
    Check(serialized.find("shortcut.Undo=Ctrl+Z\n") != std::string::npos, "serializer writes portable text");
    Check(serialized.find("malformed") == std::string::npos && serialized.find("comment") == std::string::npos,
        "serializer writes only known keys");
    CheckSameSettings(cnote::settings::SettingsStoreQt::ParseText(serialized), settings, "serialize/parse round trip");

    // Legacy default palette migrates to the current default.
    const cnote::settings::Settings migrated = cnote::settings::SettingsStoreQt::ParseText(
        "palette=FF1F2937,FF2563EB,FFDC2626,FF059669,FFF59E0B\n");
    Check(migrated.palette == s::DefaultPalette(), "legacy default palette migrates to the current default");

    // Missing and empty files keep the defaults.
    CheckSameSettings(cnote::settings::SettingsStoreQt::ParseText(""), cnote::settings::Settings {}, "empty text keeps defaults");

    // default_page_size_preset keeps the A4 default on missing or invalid values.
    Check(cnote::settings::SettingsStoreQt::ParseText("palette=FF112233\n").defaultPageSizePreset == 3U,
        "missing default_page_size_preset keeps A4");
    Check(cnote::settings::SettingsStoreQt::ParseText("palette=FF112233\ndefault_page_size_preset=99\n").defaultPageSizePreset == 3U,
        "out-of-range default_page_size_preset keeps A4");
    Check(cnote::settings::SettingsStoreQt::ParseText("palette=FF112233\ndefault_page_size_preset=abc\n").defaultPageSizePreset == 3U,
        "unparsable default_page_size_preset keeps A4");
    Check(cnote::settings::SettingsStoreQt::ParseText("palette=FF112233\ndefault_page_size_preset=0\n").defaultPageSizePreset == 0U,
        "in-range default_page_size_preset overrides the default");
}

// 5. Storage location rules.
void PathResolution(const std::filesystem::path& scratch) {
#ifdef _WIN32
    const QByteArray previous = qgetenv("APPDATA");
    qputenv("APPDATA", scratch.string().c_str());
    const std::filesystem::path windowsPath = cnote::settings::SettingsStoreQt::ResolveSettingsPath();
    Check(windowsPath == scratch / "cnote" / "settings.ini", "Windows APPDATA path is unchanged");
    qputenv("APPDATA", "");
    const std::filesystem::path fallback = cnote::settings::SettingsStoreQt::ResolveSettingsPath();
    Check(fallback == std::filesystem::current_path() / "cnote.settings.ini", "Windows fallback stays in the working directory");
    if (previous.isEmpty()) {
        qunsetenv("APPDATA");
    } else {
        qputenv("APPDATA", previous);
    }
    std::printf("windows settings path: %s\n", windowsPath.string().c_str());
#else
    qputenv("XDG_CONFIG_HOME", scratch.string().c_str());
    const std::filesystem::path resolved = cnote::settings::SettingsStoreQt::ResolveSettingsPath();
    const std::filesystem::path expected =
        std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString()) / "settings.ini";
    Check(resolved == expected, "settings path is AppConfigLocation/settings.ini");
    Check(QCoreApplication::applicationName() == QStringLiteral("cnote"), "application name is set to cnote");
    Check(QCoreApplication::organizationName().isEmpty(), "no organization component");
    const std::string text = resolved.string();
    Check(text.rfind(scratch.string(), 0) == 0, "settings path is under XDG_CONFIG_HOME");
    Check(text.ends_with("/cnote/settings.ini"), "settings path is <xdg>/cnote/settings.ini");
    std::printf("xdg settings path: %s\n", text.c_str());
#endif
}

// 6. Persistence: commit on success, leave the previous destination on failure.
void PersistenceKeepsPreviousFile(const std::filesystem::path& scratch) {
    const std::filesystem::path dest = scratch / "persist" / "settings.ini";
    cnote::settings::SettingsStoreQt store(dest);
    cnote::settings::Settings settings;
    settings.palette = { 0xFF010203U, 0xFF040506U };
    settings.autoHideToolbar = true;
    settings.zoomPresets[0] = 1.5f;
    settings.shortcuts[0].sequence = QKeySequence(Qt::CTRL | Qt::Key_K);
    Check(store.Save(settings), "save creates the destination directory and commits");

    const std::string previousBytes = ReadFile(dest);
    Check(!previousBytes.empty(), "saved file is not empty");
    CheckSameSettings(store.Load(), settings, "Load() returns what Save() wrote");
    Check(store.Load().shortcuts[0].sequence == QKeySequence(Qt::CTRL | Qt::Key_K),
        "rebound shortcuts survive a file round trip");

    cnote::settings::Settings replacement;
    replacement.palette = { 0xFFAABBCCU };
    replacement.dashGap = 1.25f;

#ifdef _WIN32
    std::error_code error;
    std::filesystem::permissions(dest, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace, error);
    Check(!error, "make the destination read-only");
#else
    std::error_code error;
    std::filesystem::permissions(dest.parent_path(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, error);
    Check(!error, "make the destination directory read-only");
#endif

    const bool saved = store.Save(replacement);
    Check(!saved, "save against a read-only destination reports failure");
    Check(ReadFile(dest) == previousBytes, "failed save leaves the previous file intact");

    // The settings service has no document access; a caller that only clears
    // dirty on a reported success must keep the document dirty.
    bool documentDirty = true;
    if (store.Save(replacement)) {
        documentDirty = false;
    }
    Check(documentDirty, "failed persist cannot clear document dirty state");

#ifdef _WIN32
    std::filesystem::permissions(dest, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
#else
    std::filesystem::permissions(dest.parent_path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
#endif

    const std::string beforeSuccess = ReadFile(dest);
    bool secondDirty = true;
    if (store.Save(replacement)) {
        secondDirty = false;
    }
    Check(!secondDirty, "successful persist reports success");
    Check(ReadFile(dest) != beforeSuccess, "successful persist replaces the destination");
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    const std::filesystem::path scratch = ScratchDirectory("tests");
    RoundTripActions();
    RoundTripVirtualKeyTable();
    MalformedRecordsKeepDefaults();
    ParsesRealPayload(scratch);
    PathResolution(scratch);
    PersistenceKeepsPreviousFile(scratch);

    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    std::printf(g_failures == 0 ? "ALL SETTINGS CHECKS PASSED\n" : "%d SETTINGS CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
