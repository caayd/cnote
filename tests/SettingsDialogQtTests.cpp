// Offscreen Qt settings-dialog test (migration plan step 6.2).
//
// Drives the real SettingsDialogQt the way a user would (buttons, sliders, line
// edits, key events) and asserts the observable contract: the legacy control
// inventory, palette add/remove/hex/HSL/named-palette semantics, drawing
// clamping, zoom/quick-size/brush-preset parsing, shortcut capture and duplicate
// clearing, immediate apply plus persistence, refresh-on-open, modeless
// ownership, tab order and deterministic screenshots.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QString>
#include <QTabWidget>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "settings/SettingsStoreQt.h"
#include "ui/SettingsDialogQt.h"
#include "ui/SettingsFormatQt.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

std::filesystem::path ScratchDirectory(const char* name) {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    const std::filesystem::path scratch = base / (std::string("cnote-dialog-") + name);
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

template <typename T>
T* Find(QWidget& dialog, const char* name) {
    T* widget = dialog.findChild<T*>(QString::fromUtf8(name));
    Check(widget != nullptr, std::string("control exists: ") + name);
    return widget;
}

void UseTempSettingsHome(const std::filesystem::path& scratch) {
#ifdef _WIN32
    qputenv("APPDATA", scratch.string().c_str());
#else
    qputenv("XDG_CONFIG_HOME", scratch.string().c_str());
#endif
}

std::string ScreenshotHash(QWidget& widget, const char* label) {
    const QImage image = widget.grab().toImage();
    Check(!image.isNull(), std::string("screenshot rendered: ") + label);
    const QByteArray bytes(
        reinterpret_cast<const char*>(image.constBits()),
        static_cast<qsizetype>(image.sizeInBytes()));
    const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    const std::string hex = hash.toHex().toStdString();
    std::printf("screenshot %-10s %dx%d sha256=%s\n", label, image.width(), image.height(), hex.c_str());
    return hex;
}

cnote::settings::Settings MakeSettings(std::vector<std::uint32_t> palette) {
    cnote::settings::Settings settings;
    settings.palette = std::move(palette);
    return settings;
}

// 1. The legacy control inventory, with the legacy ranges.
void ControlInventory(const std::filesystem::path& scratch) {
    namespace s = cnote::settings;
    s::Settings settings = MakeSettings({ 0xFF000000U, 0xFF2563EBU, 0xFFDC2626U });
    cnote::ui::SettingsDialogQt dialog(settings, s::SettingsStoreQt(scratch / "settings.ini"), nullptr);

    auto* tabs = Find<QTabWidget>(dialog, "categoryTabs");
    Check(tabs != nullptr && tabs->count() == 3, "three settings categories");
    if (tabs != nullptr && tabs->count() == 3) {
        Check(tabs->tabText(0) == QStringLiteral("Palette"), "category 1 is Palette");
        Check(tabs->tabText(1) == QStringLiteral("Drawing"), "category 2 is Drawing");
        Check(tabs->tabText(2) == QStringLiteral("Shortcuts"), "category 3 is Shortcuts");
    }

    auto* paletteList = Find<QListWidget>(dialog, "paletteList");
    Find<QPushButton>(dialog, "addColorButton");
    Find<QPushButton>(dialog, "removeColorButton");
    Find<QLineEdit>(dialog, "hexColorEdit");
    auto* hue = Find<QSlider>(dialog, "hueSlider");
    auto* saturation = Find<QSlider>(dialog, "saturationSlider");
    auto* lightness = Find<QSlider>(dialog, "lightnessSlider");
    Find<QLabel>(dialog, "hueLabel");
    Find<QLabel>(dialog, "saturationLabel");
    Find<QLabel>(dialog, "lightnessLabel");
    Find<QLineEdit>(dialog, "savedPaletteNameEdit");
    Find<QPushButton>(dialog, "savePaletteButton");
    Find<QPushButton>(dialog, "applyPaletteButton");
    Find<QPushButton>(dialog, "deletePaletteButton");
    Find<QListWidget>(dialog, "savedPaletteList");
    Check(paletteList != nullptr && paletteList->count() == 3, "palette list shows every colour");

    auto* stabilization = Find<QSlider>(dialog, "stabilizationSlider");
    auto* spacing = Find<QSlider>(dialog, "spacingSlider");
    auto* strokeSmoothing = Find<QSlider>(dialog, "strokeSmoothingSlider");
    auto* cursorKind = Find<QComboBox>(dialog, "drawCursorKindCombo");
    auto* cursorSize = Find<QSlider>(dialog, "drawCursorSizeSlider");
    auto* dashLength = Find<QSlider>(dialog, "dashLengthSlider");
    auto* dashGap = Find<QSlider>(dialog, "dashGapSlider");
    auto* autoSave = Find<QSlider>(dialog, "autoSaveSlider");
    Find<QCheckBox>(dialog, "autoHideToolbarCheck");
    Find<QCheckBox>(dialog, "embedProjectInPdfCheck");
    Check(hue != nullptr && hue->minimum() == 0 && hue->maximum() == 360, "hue slider 0..360");
    Check(saturation != nullptr && saturation->minimum() == 0 && saturation->maximum() == 100, "saturation slider 0..100");
    Check(lightness != nullptr && lightness->minimum() == 0 && lightness->maximum() == 100, "lightness slider 0..100");
    Check(stabilization != nullptr && stabilization->minimum() == 5 && stabilization->maximum() == 95, "stabilization slider 5..95");
    Check(spacing != nullptr && spacing->minimum() == 5 && spacing->maximum() == 405, "spacing slider 5..405");
    Check(strokeSmoothing != nullptr && strokeSmoothing->minimum() == 0 && strokeSmoothing->maximum() == 100, "stroke smoothing slider 0..100");
    Check(cursorSize != nullptr && cursorSize->minimum() == 4 && cursorSize->maximum() == 24, "draw cursor size slider 4..24");
    Check(dashLength != nullptr && dashLength->minimum() == 5 && dashLength->maximum() == 80, "dash length slider 5..80");
    Check(dashGap != nullptr && dashGap->minimum() == 5 && dashGap->maximum() == 80, "dash gap slider 5..80");
    Check(autoSave != nullptr && autoSave->minimum() == 0 && autoSave->maximum() == 300, "auto save slider 0..300");
    Check(cursorKind != nullptr && cursorKind->count() == 2 && cursorKind->itemText(0) == QStringLiteral("Dot")
            && cursorKind->itemText(1) == QStringLiteral("Precision Crosshair"),
        "draw cursor kind choices");
    for (int index = 1; index <= 5; ++index) {
        Find<QLineEdit>(dialog, ("zoomPresetEdit" + std::to_string(index)).c_str());
    }
    Find<QLineEdit>(dialog, "quickSizeSliderMaxEdit");
    Find<QLineEdit>(dialog, "brushSizePresetEdit");

    auto* shortcutList = Find<QListWidget>(dialog, "shortcutList");
    Find<QPushButton>(dialog, "changeShortcutButton");
    Find<QPushButton>(dialog, "clearShortcutButton");
    Find<QLabel>(dialog, "shortcutHintLabel");
    Check(shortcutList != nullptr && shortcutList->count() == static_cast<int>(s::ShortcutDefinitions().size()),
        "shortcut list shows every action");
}

// 2. Palette add/remove/hex/HSL/named-palette semantics.
void PaletteOperations(const std::filesystem::path& scratch) {
    namespace s = cnote::settings;
    const std::filesystem::path path = scratch / "palette-settings.ini";
    s::Settings settings = MakeSettings({ 0xFF000000U, 0xFF2563EBU, 0xFFDC2626U });
    cnote::ui::SettingsDialogQt dialog(settings, s::SettingsStoreQt(path), nullptr);

    auto* paletteList = Find<QListWidget>(dialog, "paletteList");
    auto* addButton = Find<QPushButton>(dialog, "addColorButton");
    auto* removeButton = Find<QPushButton>(dialog, "removeColorButton");
    auto* hexEdit = Find<QLineEdit>(dialog, "hexColorEdit");
    auto* hue = Find<QSlider>(dialog, "hueSlider");
    auto* saturation = Find<QSlider>(dialog, "saturationSlider");
    auto* lightness = Find<QSlider>(dialog, "lightnessSlider");

    paletteList->setCurrentRow(1);
    Check(settings.paletteIndex == 1U, "selecting a palette row updates settings_palette_index");
    Check(hue->value() == static_cast<int>(std::lround(cnote::ui::RgbToHsl(settings.palette[1]).hue)),
        "hue slider follows the selected colour");

    // Add appends a copy of the selected colour (legacy AddPaletteColor).
    addButton->click();
    Check(settings.palette.size() == 4U, "add colour grows the palette");
    Check(settings.palette.back() == 0xFF2563EBU, "add colour copies the selected colour");
    Check(settings.paletteIndex == 3U, "add colour selects the new entry");
    for (int index = 0; index < 8; ++index) {
        addButton->click();
    }
    Check(settings.palette.size() == s::kMaxPaletteColors, "palette caps at 12 colours");
    Check(!addButton->isEnabled(), "add colour disables at the cap");

    // Remove deletes the selected entry and clamps the indices.
    paletteList->setCurrentRow(11);
    Check(settings.paletteIndex == 11U, "selected the last palette entry");
    removeButton->click();
    Check(settings.palette.size() == 11U, "remove colour shrinks the palette");
    Check(settings.paletteIndex == 10U, "remove colour clamps the palette index");
    while (settings.palette.size() > 1U) {
        paletteList->setCurrentRow(0);
        removeButton->click();
    }
    Check(settings.palette.size() == 1U, "palette keeps at least one colour");
    Check(!removeButton->isEnabled(), "remove colour disables at one colour");
    const std::size_t singleSize = settings.palette.size();
    removeButton->click();
    Check(settings.palette.size() == singleSize, "remove colour is a no-op at one colour");

    // Hex edit applies on commit and rejects invalid text.
    hexEdit->setText(QStringLiteral("#123456"));
    hexEdit->editingFinished();
    Check(settings.palette[settings.paletteIndex] == 0xFF123456U, "hex edit applies a valid colour");
    hexEdit->setText(QStringLiteral("zzz"));
    hexEdit->editingFinished();
    Check(settings.palette[settings.paletteIndex] == 0xFF123456U, "invalid hex leaves the colour unchanged");

    // HSL sliders write HslToColor (legacy UpdatePaletteFromHslSliders).
    hue->setValue(0);
    saturation->setValue(100);
    lightness->setValue(50);
    Check(settings.palette[settings.paletteIndex] == cnote::ui::HslToColor(0.0f, 1.0f, 0.5f),
        "HSL sliders produce the legacy HSL colour");

    // Named palettes: save, case-insensitive replace, apply, delete.
    auto* nameEdit = Find<QLineEdit>(dialog, "savedPaletteNameEdit");
    auto* saveButton = Find<QPushButton>(dialog, "savePaletteButton");
    auto* applyButton = Find<QPushButton>(dialog, "applyPaletteButton");
    auto* deleteButton = Find<QPushButton>(dialog, "deletePaletteButton");
    auto* savedList = Find<QListWidget>(dialog, "savedPaletteList");

    nameEdit->setText(QStringLiteral("Warm"));
    Check(saveButton->isEnabled(), "save enables once a name is typed");
    saveButton->click();
    Check(settings.savedPalettes.size() == 1U, "named palette is saved");
    Check(settings.savedPalettes[0].name == u"Warm", "named palette keeps its name");
    Check(settings.savedPalettes[0].colors == settings.palette, "named palette stores the current colours");

    hue->setValue(120);
    saturation->setValue(100);
    lightness->setValue(50);
    nameEdit->setText(QStringLiteral("warm"));
    saveButton->click();
    Check(settings.savedPalettes.size() == 1U, "a case-insensitive duplicate replaces instead of appending");
    Check(settings.savedPalettes[0].colors == settings.palette, "replaced palette keeps the new colours");

    settings.palette = { 0xFF010203U, 0xFF040506U };
    settings.paletteIndex = 1U;
    savedList->setCurrentRow(0);
    applyButton->click();
    Check(settings.palette == settings.savedPalettes[0].colors, "apply replaces the palette with the saved colours");
    Check(settings.paletteIndex <= settings.palette.size() - 1U, "apply clamps the palette index");

    savedList->setCurrentRow(0);
    deleteButton->click();
    Check(settings.savedPalettes.empty(), "delete removes the saved palette");

    // Every applied palette change was persisted.
    const s::Settings reloaded = s::SettingsStoreQt(path).Load();
    Check(reloaded.palette == settings.palette, "palette edits persisted");
    Check(reloaded.savedPalettes.empty(), "deleted named palette persisted");
}

// 3. Drawing clamping, parsing and toggles.
void DrawingOperations(const std::filesystem::path& scratch) {
    namespace s = cnote::settings;
    const std::filesystem::path path = scratch / "drawing-settings.ini";
    s::Settings settings = MakeSettings({ 0xFF000000U });
    cnote::ui::SettingsDialogQt dialog(settings, s::SettingsStoreQt(path), nullptr);

    Find<QSlider>(dialog, "stabilizationSlider")->setValue(5);
    Check(settings.inputSmoothing == 0.05f, "stabilization 5 maps to 0.05");
    Find<QSlider>(dialog, "stabilizationSlider")->setValue(95);
    Check(settings.inputSmoothing == 0.95f, "stabilization 95 maps to 0.95");
    Find<QSlider>(dialog, "spacingSlider")->setValue(405);
    Check(settings.inputSpacing == 4.05f, "spacing 405 maps to 4.05");
    Find<QSlider>(dialog, "strokeSmoothingSlider")->setValue(55);
    Check(settings.strokeSmoothing == 0.55f, "stroke smoothing 55 maps to 0.55");
    Find<QSlider>(dialog, "dashLengthSlider")->setValue(5);
    Check(settings.dashLength == 0.5f, "dash length 5 maps to 0.5");
    Find<QSlider>(dialog, "dashGapSlider")->setValue(80);
    Check(settings.dashGap == 8.0f, "dash gap 80 maps to 8.0");
    Find<QSlider>(dialog, "drawCursorSizeSlider")->setValue(24);
    Check(settings.drawCursorSize == 24.0f, "draw cursor size maps directly");

    auto* autoSave = Find<QSlider>(dialog, "autoSaveSlider");
    autoSave->setValue(37);
    Check(settings.autoSaveIntervalSeconds == 30U, "auto save snaps to the nearest 15 seconds");
    Check(autoSave->value() == 30, "auto save slider shows the snapped value");
    autoSave->setValue(290);
    Check(settings.autoSaveIntervalSeconds == 285U, "auto save snaps near the cap");
    autoSave->setValue(0);
    Check(settings.autoSaveIntervalSeconds == 0U, "auto save can be off");

    auto* cursorKind = Find<QComboBox>(dialog, "drawCursorKindCombo");
    auto* cursorSize = Find<QSlider>(dialog, "drawCursorSizeSlider");
    cursorKind->setCurrentIndex(1);
    Check(settings.drawCursorKind == s::DrawCursorKind::PrecisionCrosshair, "cursor kind combo selects the crosshair");
    Check(!cursorSize->isEnabled(), "cursor size disables for the crosshair");
    cursorKind->setCurrentIndex(0);
    Check(settings.drawCursorKind == s::DrawCursorKind::Dot, "cursor kind combo restores the dot");
    Check(cursorSize->isEnabled(), "cursor size re-enables for the dot");

    auto* pageSize = Find<QComboBox>(dialog, "defaultPageSizeCombo");
    pageSize->setCurrentIndex(5);
    Check(settings.defaultPageSizePreset == 5U, "default page size selection applies");
    pageSize->setCurrentIndex(1);
    Check(settings.defaultPageSizePreset == 1U, "default page size selection changes");

    auto* quickSize = Find<QLineEdit>(dialog, "quickSizeSliderMaxEdit");
    settings.drawBrushSize = 15.0f;
    settings.eraseBrushSize = 18.0f;
    quickSize->setText(QStringLiteral("5"));
    quickSize->editingFinished();
    Check(settings.quickSizeSliderMax == 5.0f, "quick size max accepts an in-range value");
    Check(settings.drawBrushSize == 5.0f && settings.eraseBrushSize == 5.0f, "quick size max shrinks brush sizes");
    quickSize->setText(QStringLiteral("99"));
    quickSize->editingFinished();
    Check(settings.quickSizeSliderMax == s::kMaxBrushSize, "quick size max clamps to 20");
    quickSize->setText(QStringLiteral("abc"));
    quickSize->editingFinished();
    Check(settings.quickSizeSliderMax == s::kMaxBrushSize, "invalid quick size max keeps the value");

    auto* presets = Find<QLineEdit>(dialog, "brushSizePresetEdit");
    presets->setText(QStringLiteral(" 3 , 1.5 ,1.5, 2 "));
    presets->editingFinished();
    Check(settings.brushSizePresets == std::vector<float>({ 1.5f, 2.0f, 3.0f }), "brush presets snap, sort and dedupe");
    presets->setText(QStringLiteral("abc"));
    presets->editingFinished();
    Check(settings.brushSizePresets == std::vector<float>({ 1.5f, 2.0f, 3.0f }), "invalid brush presets keep the value");

    auto* zoom1 = Find<QLineEdit>(dialog, "zoomPresetEdit1");
    zoom1->setText(QStringLiteral("150"));
    zoom1->editingFinished();
    Check(settings.zoomPresets[0] == 1.5f, "zoom preset accepts a percentage");
    Check(zoom1->text() == QStringLiteral("150"), "zoom preset field normalizes to the percentage");
    zoom1->setText(QStringLiteral("1000"));
    zoom1->editingFinished();
    Check(settings.zoomPresets[0] == s::kMaxZoom, "zoom preset clamps to 4.0");
    zoom1->setText(QString());
    zoom1->editingFinished();
    Check(settings.zoomPresets[0] == 0.0f, "empty zoom preset unsets");
    zoom1->setText(QStringLiteral("abc"));
    zoom1->editingFinished();
    Check(settings.zoomPresets[0] == 0.0f, "invalid zoom preset keeps the value");

    auto* autoHide = Find<QCheckBox>(dialog, "autoHideToolbarCheck");
    auto* embed = Find<QCheckBox>(dialog, "embedProjectInPdfCheck");
    autoHide->setChecked(true);
    Check(settings.autoHideToolbar, "auto-hide toggle persists");
    embed->setChecked(false);
    Check(!settings.embedProjectInExportedPdfs, "embed toggle persists");

    const s::Settings reloaded = s::SettingsStoreQt(path).Load();
    Check(reloaded.inputSmoothing == settings.inputSmoothing, "drawing values persisted");
    Check(reloaded.autoSaveIntervalSeconds == settings.autoSaveIntervalSeconds, "auto save persisted");
    Check(reloaded.brushSizePresets == settings.brushSizePresets, "brush presets persisted");
    Check(reloaded.zoomPresets == settings.zoomPresets, "zoom presets persisted");
    Check(reloaded.defaultPageSizePreset == settings.defaultPageSizePreset, "default page size preset persisted");
    Check(reloaded.autoHideToolbar && !reloaded.embedProjectInExportedPdfs, "toggles persisted");
}

// 4. Shortcut capture, duplicate clearing, Escape.
void ShortcutCaptureOperations(const std::filesystem::path& scratch) {
    namespace s = cnote::settings;
    s::Settings settings = MakeSettings({ 0xFF000000U });
    const s::SettingsStoreQt store(scratch / "shortcut-settings.ini");
    cnote::ui::SettingsDialogQt dialog(settings, store, nullptr);

    const auto binding = [&](s::ShortcutAction action) -> QKeySequence {
        for (const s::ShortcutBinding& entry : settings.shortcuts) {
            if (entry.action == action) {
                return entry.sequence;
            }
        }
        return {};
    };

    dialog.clearSelectedShortcut();
    Check(binding(s::ShortcutAction::ToolDraw).isEmpty(), "clear shortcut unbinds the selected action");

    dialog.beginShortcutCapture(s::ShortcutAction::ToolDraw);
    Check(dialog.capturingShortcut(), "capture mode is active");
    QKeyEvent modifierOnly(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    QApplication::sendEvent(&dialog, &modifierOnly);
    Check(dialog.capturingShortcut(), "modifier-only key does not finish capture");
    Check(binding(s::ShortcutAction::ToolDraw).isEmpty(), "modifier-only key does not bind");
    QKeyEvent newKey(QEvent::KeyPress, Qt::Key_K, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &newKey);
    Check(!dialog.capturingShortcut(), "key press finishes capture");
    Check(binding(s::ShortcutAction::ToolDraw) == QKeySequence(Qt::Key_K), "captured key becomes the binding");

    // Escape cancels.
    const QKeySequence beforeEscape = binding(s::ShortcutAction::ToolSelect);
    dialog.beginShortcutCapture(s::ShortcutAction::ToolSelect);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &escape);
    Check(!dialog.capturingShortcut(), "Escape leaves capture mode");
    Check(binding(s::ShortcutAction::ToolSelect) == beforeEscape, "Escape leaves the binding unchanged");

    // A duplicate chord clears the other command (legacy HandleShortcutCapture).
    const QKeySequence saveDefault = binding(s::ShortcutAction::Save);
    Check(!saveDefault.isEmpty(), "Save starts bound");
    dialog.beginShortcutCapture(s::ShortcutAction::ToolDraw);
    QKeyEvent duplicate(QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier);
    QApplication::sendEvent(&dialog, &duplicate);
    Check(binding(s::ShortcutAction::ToolDraw) == saveDefault, "captured chord binds the target action");
    Check(binding(s::ShortcutAction::Save).isEmpty(), "duplicate chord clears the other action");

    const s::Settings reloaded = store.Load();
    Check(reloaded.shortcuts[0].sequence == settings.shortcuts[0].sequence, "captured bindings persisted");
}

// 5. Refresh-on-open (external changes are re-read) and modeless ownership.
void RefreshAndOwnership(const std::filesystem::path& scratch) {
    namespace s = cnote::settings;
    s::Settings settings = MakeSettings({ 0xFF000000U, 0xFF111111U });
    QWidget owner;
    const s::SettingsStoreQt store(scratch / "refresh-settings.ini");
    cnote::ui::SettingsDialogQt dialog(settings, store, &owner);

    dialog.show();
    Check(!dialog.isModal(), "settings dialog is modeless");
    Check(dialog.parentWidget() == &owner, "settings dialog keeps its owner");
    Check(owner.isEnabled(), "owner stays interactive while the dialog is open");

    auto* spacing = Find<QSlider>(dialog, "spacingSlider");
    auto* zoom1 = Find<QLineEdit>(dialog, "zoomPresetEdit1");
    auto* paletteList = Find<QListWidget>(dialog, "paletteList");
    auto* pageSize = Find<QComboBox>(dialog, "defaultPageSizeCombo");

    settings.zoomPresets[0] = 2.0f;
    settings.inputSpacing = 2.5f;
    settings.palette.push_back(0xFF222222U);
    settings.defaultPageSizePreset = 5U;
    dialog.hide();
    dialog.show();
    Check(zoom1->text() == QStringLiteral("200"), "zoom preset control re-read the model on open");
    Check(spacing->value() == 250, "spacing slider re-read the model on open");
    Check(paletteList->count() == 3, "palette list re-read the model on open");
    Check(pageSize->currentIndex() == 5, "default page size combo re-read the model on open");

    dialog.refreshFromSettings();
    settings.inputSpacing = 1.25f;
    dialog.refreshFromSettings();
    Check(spacing->value() == 125, "refreshFromSettings re-reads the model");

    // Refreshing syncs the combo from the model without re-applying it.
    pageSize->setCurrentIndex(2);
    settings.defaultPageSizePreset = 4U;
    int changedDuringRefresh = 0;
    QObject::connect(&dialog, &cnote::ui::SettingsDialogQt::settingsChanged, [&] { ++changedDuringRefresh; });
    dialog.refreshFromSettings();
    Check(changedDuringRefresh == 0, "refreshFromSettings does not re-apply the page-size selection");
    Check(pageSize->currentIndex() == 4, "refreshFromSettings re-reads the default page size");
    Check(settings.defaultPageSizePreset == 4U, "refreshFromSettings keeps the model page-size value");

    // The legacy dialog had no OK/Cancel: closing does not revert applied edits.
    spacing->setValue(275);
    const float appliedSpacing = settings.inputSpacing;
    Check(appliedSpacing == 2.75f, "spacing slider applied before closing");
    dialog.reject();
    Check(settings.inputSpacing == appliedSpacing, "closing the dialog keeps applied edits");
    Check(store.Load().inputSpacing == appliedSpacing, "closing keeps the persisted value");
}

// 6. A failed persist reports failure, keeps the file and keeps the applied value.
void FailedPersist(const std::filesystem::path& scratch) {
    namespace s = cnote::settings;
    const std::filesystem::path dest = scratch / "readonly" / "settings.ini";
    s::Settings settings = MakeSettings({ 0xFF000000U });
    s::SettingsStoreQt store(dest);
    cnote::ui::SettingsDialogQt dialog(settings, store, nullptr);
    Check(store.Save(settings), "initial save succeeds");
    const std::string previous = ReadFile(dest);

    int saveFailures = 0;
    QObject::connect(&dialog, &cnote::ui::SettingsDialogQt::settingsSaveFailed, [&](const QString&) { ++saveFailures; });

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

    Find<QSlider>(dialog, "spacingSlider")->setValue(300);
    Check(settings.inputSpacing == 3.0f, "failed persist still applies the value in memory");
    Check(!dialog.lastSaveSucceeded(), "failed persist reports failure");
    Check(saveFailures == 1, "failed persist emits settingsSaveFailed once");
    Check(!dialog.lastSaveError().isEmpty(), "failed persist reports a reason");
    Check(ReadFile(dest) == previous, "failed persist leaves the previous file intact");

#ifdef _WIN32
    std::filesystem::permissions(dest, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
#else
    std::filesystem::permissions(dest.parent_path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
#endif
}

// 7. Keyboard focus/tab order and deterministic screenshots.
void TabOrderAndScreenshots(const std::filesystem::path& scratch) {
    namespace s = cnote::settings;
    s::Settings settings = MakeSettings({ 0xFF000000U, 0xFF2563EBU });
    cnote::ui::SettingsDialogQt dialog(settings, s::SettingsStoreQt(scratch / "screenshot-settings.ini"), nullptr);
    dialog.resize(900, 700);
    dialog.show();
    QApplication::processEvents();

    const QStringList expected {
        QStringLiteral("paletteList"),
        QStringLiteral("addColorButton"),
        QStringLiteral("removeColorButton"),
        QStringLiteral("hexColorEdit"),
        QStringLiteral("hueSlider"),
        QStringLiteral("saturationSlider"),
        QStringLiteral("lightnessSlider"),
        QStringLiteral("savedPaletteNameEdit"),
        QStringLiteral("savePaletteButton"),
        QStringLiteral("applyPaletteButton"),
        QStringLiteral("deletePaletteButton"),
        QStringLiteral("savedPaletteList"),
        QStringLiteral("stabilizationSlider"),
        QStringLiteral("spacingSlider"),
        QStringLiteral("strokeSmoothingSlider"),
        QStringLiteral("drawCursorKindCombo"),
        QStringLiteral("drawCursorSizeSlider"),
        QStringLiteral("dashLengthSlider"),
        QStringLiteral("dashGapSlider"),
        QStringLiteral("autoSaveSlider"),
        QStringLiteral("autoHideToolbarCheck"),
        QStringLiteral("embedProjectInPdfCheck"),
        QStringLiteral("zoomPresetEdit1"),
        QStringLiteral("zoomPresetEdit5"),
        QStringLiteral("quickSizeSliderMaxEdit"),
        QStringLiteral("brushSizePresetEdit"),
        QStringLiteral("shortcutList"),
        QStringLiteral("changeShortcutButton"),
        QStringLiteral("clearShortcutButton"),
    };

    QStringList focusOrder;
    QWidget* widget = dialog.focusWidget() != nullptr ? dialog.focusWidget() : &dialog;
    QWidget* start = widget;
    do {
        if (widget->focusPolicy() != Qt::NoFocus && !widget->objectName().isEmpty()
            && widget->objectName() != QStringLiteral("settingsDialog")) {
            focusOrder.append(widget->objectName());
        }
        widget = widget->nextInFocusChain();
    } while (widget != nullptr && widget != start);

    for (const QString& name : expected) {
        Check(focusOrder.contains(name), "tab order reaches " + name.toStdString());
    }
    const int paletteListIndex = focusOrder.indexOf(QStringLiteral("paletteList"));
    const int shortcutListIndex = focusOrder.indexOf(QStringLiteral("shortcutList"));
    Check(paletteListIndex >= 0 && shortcutListIndex > paletteListIndex, "palette controls precede shortcut controls in the tab order");

    auto* tabs = Find<QTabWidget>(dialog, "categoryTabs");
    const std::string paletteHash = ScreenshotHash(dialog, "palette");
    tabs->setCurrentIndex(1);
    QApplication::processEvents();
    const std::string drawingHash = ScreenshotHash(dialog, "drawing");
    tabs->setCurrentIndex(2);
    QApplication::processEvents();
    const std::string shortcutsHash = ScreenshotHash(dialog, "shortcuts");
    Check(!paletteHash.empty() && paletteHash != drawingHash && drawingHash != shortcutsHash,
        "each category renders a distinct screenshot");

    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    Check(ScreenshotHash(dialog, "palette2") == paletteHash, "screenshots are deterministic");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const std::filesystem::path scratch = ScratchDirectory("settings");
    UseTempSettingsHome(scratch);

    ControlInventory(scratch);
    PaletteOperations(scratch);
    DrawingOperations(scratch);
    ShortcutCaptureOperations(scratch);
    RefreshAndOwnership(scratch);
    FailedPersist(scratch);
    TabOrderAndScreenshots(scratch);

    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    std::printf(g_failures == 0 ? "ALL SETTINGS DIALOG CHECKS PASSED\n" : "%d SETTINGS DIALOG CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
