// Offscreen Qt new-page-settings dialog test (migration plan step 6.2).
//
// Drives NewPageDialogQt the way a user would and asserts the legacy contract:
// the control inventory, draft dimension clamping and presets, default/recent
// colour swatches, hex and HSL colour edits, stroke colour profile selection,
// the remap toggle, the separate Apply to Current / Apply to All /
// Apply to Future Pages semantics (including recent-colour bookkeeping),
// Reset Defaults, refresh-on-open discarding the draft, modeless ownership,
// tab order and deterministic screenshots.  The pure page-application policy
// (clamping, raster replacement, palette remapping) is covered directly.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "Document.h"
#include "ProjectSession.h"
#include "qt/QtDocumentController.h"
#include "settings/SettingsStoreQt.h"
#include "shell/AppManager.h"
#include "ui/NewPageDialogQt.h"
#include "ui/PageDefaultsQt.h"
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

template <typename T>
T* Find(QWidget& dialog, const char* name) {
    T* widget = dialog.findChild<T*>(QString::fromUtf8(name));
    Check(widget != nullptr, std::string("control exists: ") + name);
    return widget;
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

cnote::Stroke MakeStroke(std::uint32_t color) {
    cnote::Stroke stroke;
    stroke.style.color = color;
    return stroke;
}

// 1. The legacy control inventory.
void ControlInventory() {
    namespace s = cnote::settings;
    s::Settings settings;
    settings.palette = { 0xFF000000U };
    cnote::ui::NewPageDialogQt dialog(settings, nullptr);

    Find<QLineEdit>(dialog, "widthEdit");
    Find<QLineEdit>(dialog, "heightEdit");
    auto* presets = Find<QComboBox>(dialog, "dimensionCombo");
    Check(presets != nullptr && presets->count() == static_cast<int>(cnote::ui::DimensionPresets().size()) + 1,
        "dimension combo lists Custom plus every preset");
    Check(presets != nullptr && presets->itemText(0) == QStringLiteral("Custom"), "dimension combo starts with Custom");
    Find<QLabel>(dialog, "colorPreview");
    Find<QLineEdit>(dialog, "hexColorEdit");
    auto* hue = Find<QSlider>(dialog, "hueSlider");
    auto* saturation = Find<QSlider>(dialog, "saturationSlider");
    auto* lightness = Find<QSlider>(dialog, "lightnessSlider");
    Find<QLabel>(dialog, "hueLabel");
    Find<QLabel>(dialog, "saturationLabel");
    Find<QLabel>(dialog, "lightnessLabel");
    Check(hue != nullptr && hue->minimum() == 0 && hue->maximum() == 360, "hue slider 0..360");
    Check(saturation != nullptr && saturation->minimum() == 0 && saturation->maximum() == 100, "saturation slider 0..100");
    Check(lightness != nullptr && lightness->minimum() == 0 && lightness->maximum() == 100, "lightness slider 0..100");
    for (std::size_t index = 0; index < s::kDefaultPageColors.size(); ++index) {
        Find<QPushButton>(dialog, ("defaultColorSwatch" + std::to_string(index)).c_str());
    }
    auto* recentEmpty = Find<QLabel>(dialog, "recentColorsEmptyLabel");
    Check(recentEmpty != nullptr && recentEmpty->isVisibleTo(&dialog), "empty recent colours show the legacy hint");
    Find<QComboBox>(dialog, "paletteProfileCombo");
    Find<QLabel>(dialog, "paletteProfileLabel");
    Find<QCheckBox>(dialog, "remapCheck");
    Find<QLabel>(dialog, "remapHintLabel");
    Find<QPushButton>(dialog, "appendPagesButton");
    Find<QPushButton>(dialog, "resetButton");
    Find<QPushButton>(dialog, "applyCurrentButton");
    Find<QPushButton>(dialog, "applyAllButton");
    Find<QPushButton>(dialog, "applyFutureButton");
    Check(dialog.draft().width == 595.0f && dialog.draft().height == 842.0f, "draft starts at the A4 default size");
}

// 2. Draft edits: dimensions, swatches, hex, HSL, profile, remap.
void DraftEdits() {
    namespace s = cnote::settings;
    s::Settings settings;
    settings.palette = { 0xFF000000U, 0xFF2563EBU };
    settings.defaultPageColor = 0xFFF4F0E6U;
    settings.savedPalettes.push_back({ u"Cool", { 0xFF111111U, 0xFF222222U } });
    cnote::ui::NewPageDialogQt dialog(settings, nullptr);

    auto* widthEdit = Find<QLineEdit>(dialog, "widthEdit");
    auto* heightEdit = Find<QLineEdit>(dialog, "heightEdit");
    widthEdit->setText(QStringLiteral("300"));
    Check(dialog.draft().width == 300.0f, "width edit updates the draft");
    heightEdit->setText(QStringLiteral("400"));
    Check(dialog.draft().height == 400.0f, "height edit updates the draft");
    widthEdit->setText(QStringLiteral("10"));
    Check(dialog.draft().width == cnote::ui::kMinPageSize, "width clamps to 72pt");
    widthEdit->setText(QStringLiteral("abc"));
    Check(dialog.draft().width == cnote::ui::kMinPageSize, "invalid width keeps the draft value");
    widthEdit->setText(QStringLiteral("842"));
    widthEdit->editingFinished();
    Check(widthEdit->text() == QStringLiteral("842"), "width edit normalizes on commit");

    auto* presets = Find<QComboBox>(dialog, "dimensionCombo");
    presets->activated(4);  // A4 Portrait
    Check(dialog.draft().width == 595.0f && dialog.draft().height == 842.0f, "dimension preset applies");
    Check(widthEdit->text() == QStringLiteral("595"), "dimension preset updates the width field");
    Check(presets->currentIndex() == 4, "dimension combo shows the matching preset");

    auto* swatch3 = Find<QPushButton>(dialog, "defaultColorSwatch3");
    swatch3->click();
    Check(dialog.draft().backgroundColor == s::kDefaultPageColors[3], "default swatch sets the background colour");

    auto* hexEdit = Find<QLineEdit>(dialog, "hexColorEdit");
    hexEdit->setText(QStringLiteral("#00FF00"));
    hexEdit->editingFinished();
    Check(dialog.draft().backgroundColor == 0xFF00FF00U, "hex edit sets the background colour");
    hexEdit->setText(QStringLiteral("#nope"));
    hexEdit->editingFinished();
    Check(dialog.draft().backgroundColor == 0xFF00FF00U, "invalid hex keeps the background colour");

    Find<QSlider>(dialog, "hueSlider")->setValue(0);
    Find<QSlider>(dialog, "saturationSlider")->setValue(100);
    Find<QSlider>(dialog, "lightnessSlider")->setValue(50);
    Check(dialog.draft().backgroundColor == cnote::ui::HslToColor(0.0f, 1.0f, 0.5f), "HSL sliders set the background colour");

    auto* profileCombo = Find<QComboBox>(dialog, "paletteProfileCombo");
    auto* profileLabel = Find<QLabel>(dialog, "paletteProfileLabel");
    Check(profileLabel->text() == QStringLiteral("Current Palette"), "draft profile starts as the current palette");
    profileCombo->activated(1);
    Check(dialog.draft().paletteProfile == settings.savedPalettes[0].colors, "saved palette profile applies");
    Check(profileLabel->text() == QStringLiteral("Cool"), "profile label shows the saved palette name");
    profileCombo->activated(0);
    Check(dialog.draft().paletteProfile == settings.palette, "current palette profile applies");

    auto* remap = Find<QCheckBox>(dialog, "remapCheck");
    remap->setChecked(true);
    Check(dialog.draft().applyPaletteProfileToExistingStrokes, "remap toggle updates the draft");
    remap->setChecked(false);
    Check(!dialog.draft().applyPaletteProfileToExistingStrokes, "remap toggle unchecks");

    // Recent swatches become clickable once defaults with history are set.
    dialog.setDefaults({ 612.0f, 792.0f, 0xFF010203U, settings.palette, false }, { 0xFF0A0B0CU, 0xFF0D0E0FU });
    dialog.syncDraftFromDefaults();
    auto* recent0 = Find<QPushButton>(dialog, "recentColorSwatch0");
    recent0->click();
    Check(dialog.draft().backgroundColor == 0xFF0A0B0CU, "recent swatch sets the background colour");
}

// 2b. Empty saved palettes are not offered and cannot shift the applied one.
void EmptySavedPalettesDoNotShiftProfiles() {
    namespace s = cnote::settings;
    s::Settings settings;
    settings.palette = { 0xFF000000U };
    settings.savedPalettes.push_back({ u"Empty", {} });
    settings.savedPalettes.push_back({ u"Cool", { 0xFF111111U, 0xFF222222U } });
    cnote::ui::NewPageDialogQt dialog(settings, nullptr);

    auto* combo = Find<QComboBox>(dialog, "paletteProfileCombo");
    auto* label = Find<QLabel>(dialog, "paletteProfileLabel");
    Check(combo->count() == 2, "the combo offers Current Palette plus only non-empty saved palettes");
    Check(combo->itemText(1) == QStringLiteral("Cool"), "the surviving palette keeps its name");

    combo->activated(1);
    Check(dialog.draft().paletteProfile == settings.savedPalettes[1].colors,
        "combo row N maps to the same filtered saved palette");
    Check(label->text() == QStringLiteral("Cool"), "the draft label names the filtered palette");

    cnote::ui::NewPageDraft matchedDraft = dialog.draft();
    matchedDraft.paletteProfile = settings.savedPalettes[1].colors;
    dialog.setDraft(matchedDraft);
    Check(combo->currentIndex() == 1, "a saved profile selects its filtered combo row");

    dialog.show();
    combo->activated(1);
    Check(dialog.draft().paletteProfile == settings.savedPalettes[1].colors,
        "the filtered mapping survives refresh-on-open");
    dialog.hide();
}

// 2c. An unchanged defaults/recent-colours model must not rebuild the swatches:
// every broad chrome refresh calls setDefaults, and destroying and recreating
// the buttons there was the hidden dialog's cost (PERF-039).  The swatch widgets
// still reflect the model right after the call, before any show.
void UnchangedDefaultsKeepSwatches() {
    namespace s = cnote::settings;
    s::Settings settings;
    settings.palette = { 0xFF000000U, 0xFF2563EBU };
    cnote::ui::NewPageDialogQt dialog(settings, nullptr);

    const cnote::ui::NewPageDefaults defaults { 612.0f, 792.0f, 0xFF010203U, settings.palette, false };
    const std::vector<std::uint32_t> recents { 0xFF0A0B0CU, 0xFF0D0E0FU };
    dialog.setDefaults(defaults, recents);
    auto* swatch0 = Find<QPushButton>(dialog, "recentColorSwatch0");
    Find<QPushButton>(dialog, "recentColorSwatch1");
    int destroyed = 0;
    QObject::connect(swatch0, &QObject::destroyed, [&destroyed]() { ++destroyed; });

    // What UpdateChrome does on every command (and used to do per viewport tick).
    dialog.setDefaults(defaults, recents);
    Check(destroyed == 0, "unchanged defaults and recent colours keep the swatch widgets");
    Check(dialog.findChild<QPushButton*>(QStringLiteral("recentColorSwatch0")) == swatch0,
        "the same recent swatch widget is still installed");
    std::printf("SWATCHES unchanged setDefaults: destroyed=%d\n", destroyed);

    dialog.setDefaults(defaults, { 0xFF111213U });
    Check(destroyed == 1, "changed recent colours rebuild the swatches");
    Check(dialog.findChild<QPushButton*>(QStringLiteral("recentColorSwatch0")) != nullptr,
        "the rebuilt swatch is installed");
    Check(dialog.findChild<QPushButton*>(QStringLiteral("recentColorSwatch1")) == nullptr,
        "the rebuilt swatch row matches the new history");
    Check(dialog.recentColors() == std::vector<std::uint32_t>({ 0xFF111213U }),
        "the dialog exposes the new recent colours");
}

// 3. Apply scopes, clamping into the defaults, recent-colour bookkeeping.
void ApplySemantics() {
    namespace s = cnote::settings;
    s::Settings settings;
    settings.palette = { 0xFF000000U, 0xFF2563EBU };
    settings.savedPalettes.push_back({ u"Cool", { 0xFF111111U, 0xFF222222U } });
    cnote::ui::NewPageDialogQt dialog(settings, nullptr);

    struct Request {
        cnote::ui::NewPageDraft draft;
        cnote::ui::NewPageApplyScope scope;
    };
    std::vector<Request> requests;
    QObject::connect(&dialog, &cnote::ui::NewPageDialogQt::applyRequested, [&](const cnote::ui::NewPageDraft& draft,
                                                                               cnote::ui::NewPageApplyScope scope) {
        requests.push_back({ draft, scope });
    });
    int futureSignals = 0;
    QObject::connect(&dialog, &cnote::ui::NewPageDialogQt::futureDefaultsChanged, [&](const cnote::ui::NewPageDefaults&, const std::vector<std::uint32_t>&) {
        ++futureSignals;
    });
    int appendSignals = 0;
    QObject::connect(&dialog, &cnote::ui::NewPageDialogQt::appendPagesRequested, [&] { ++appendSignals; });

    Find<QLineEdit>(dialog, "widthEdit")->setText(QStringLiteral("400"));
    Find<QLineEdit>(dialog, "heightEdit")->setText(QStringLiteral("500"));
    Find<QPushButton>(dialog, "defaultColorSwatch2")->click();
    Find<QComboBox>(dialog, "paletteProfileCombo")->activated(1);
    Find<QCheckBox>(dialog, "remapCheck")->setChecked(true);

    Find<QPushButton>(dialog, "applyCurrentButton")->click();
    Check(requests.size() == 1U && requests.back().scope == cnote::ui::NewPageApplyScope::CurrentPage,
        "Apply to Current Page requests the current-page scope");
    Check(requests.back().draft.width == 400.0f && requests.back().draft.height == 500.0f,
        "current-page request carries the draft dimensions");
    Check(requests.back().draft.paletteProfile == settings.savedPalettes[0].colors,
        "current-page request carries the draft profile");
    Check(requests.back().draft.applyPaletteProfileToExistingStrokes, "current-page request carries the remap flag");
    Check(dialog.defaults().backgroundColor == 0xFFFFFFFFU && dialog.recentColors().empty(),
        "Apply to Current Page does not touch the future defaults");

    Find<QPushButton>(dialog, "applyAllButton")->click();
    Check(requests.size() == 2U && requests.back().scope == cnote::ui::NewPageApplyScope::AllPages,
        "Apply to All requests the all-pages scope");
    Check(dialog.defaults().width == 595.0f, "Apply to All does not touch the future defaults");

    // Apply to Future Pages stores the clamped draft and records the previous colour.
    Find<QPushButton>(dialog, "applyFutureButton")->click();
    Check(requests.size() == 3U && requests.back().scope == cnote::ui::NewPageApplyScope::FuturePages,
        "Apply to Future Pages requests the future scope");
    Check(futureSignals == 1, "Apply to Future Pages reports the new defaults");
    Check(dialog.defaults().width == 400.0f && dialog.defaults().height == 500.0f, "future defaults take the draft size");
    Check(dialog.defaults().backgroundColor == s::kDefaultPageColors[2], "future defaults take the draft colour");
    Check(dialog.defaults().paletteProfile == settings.savedPalettes[0].colors, "future defaults resolve the draft profile");
    Check(dialog.defaults().applyPaletteProfileToExistingStrokes, "future defaults take the remap flag");
    Check(dialog.recentColors() == std::vector<std::uint32_t>({ 0xFFFFFFFFU }),
        "future apply records the previous background colour");

    // Re-applying the same colour does not add history; a new colour promotes the previous one.
    Find<QPushButton>(dialog, "applyFutureButton")->click();
    Check(dialog.recentColors() == std::vector<std::uint32_t>({ 0xFFFFFFFFU }), "unchanged colour adds no history");
    Find<QPushButton>(dialog, "defaultColorSwatch5")->click();
    Find<QPushButton>(dialog, "applyFutureButton")->click();
    Check(dialog.recentColors().size() == 2U && dialog.recentColors()[0] == s::kDefaultPageColors[2],
        "a changed colour promotes the previous colour");
    Check(dialog.recentColors()[1] == 0xFFFFFFFFU, "older colours stay behind the promoted one");
    Check(futureSignals == 3, "every future apply reports the defaults");

    // The recent list is capped and de-duplicated like the legacy bookkeeping.
    for (int index = 0; index < 12; ++index) {
        Find<QPushButton>(dialog, QStringLiteral("defaultColorSwatch%1").arg(index % 10).toUtf8().constData())->click();
        Find<QPushButton>(dialog, "applyFutureButton")->click();
    }
    Check(dialog.recentColors().size() <= cnote::ui::kMaxRecentPageColors, "recent colours are capped at 8");
    std::vector<std::uint32_t> unique = dialog.recentColors();
    std::sort(unique.begin(), unique.end());
    Check(std::unique(unique.begin(), unique.end()) == unique.end(), "recent colours are de-duplicated");

    Find<QPushButton>(dialog, "appendPagesButton")->click();
    Check(appendSignals == 1, "Append Pages reports a request");
}

// 4. Reset Defaults and refresh/cancel semantics.
void ResetAndRefresh() {
    namespace s = cnote::settings;
    s::Settings settings;
    settings.palette = { 0xFF000000U, 0xFF2563EBU };
    settings.defaultPageColor = 0xFFF4F0E6U;
    cnote::ui::NewPageDialogQt dialog(settings, nullptr);
    dialog.setDefaults({ 300.0f, 400.0f, 0xFF010101U, { 0xFF111111U }, true }, { 0xFFABC001U });
    dialog.syncDraftFromDefaults();

    settings.defaultPageSizePreset = 1U;  // Letter Landscape, not the A4 default.
    Find<QPushButton>(dialog, "resetButton")->click();
    Check(dialog.draft().width == 792.0f && dialog.draft().height == 612.0f,
        "reset uses the selected global page-size preset");
    Check(dialog.draft().backgroundColor == settings.defaultPageColor, "reset uses the settings default page colour");
    Check(dialog.draft().paletteProfile == settings.palette, "reset resolves the current palette");
    Check(!dialog.draft().applyPaletteProfileToExistingStrokes, "reset clears the remap flag");
    Check(dialog.defaults().width == 300.0f && dialog.recentColors().size() == 1U, "reset leaves the defaults untouched");

    int requests = 0;
    QObject::connect(&dialog, &cnote::ui::NewPageDialogQt::applyRequested, [&](const cnote::ui::NewPageDraft&, cnote::ui::NewPageApplyScope) {
        ++requests;
    });

    // Unapplied draft edits are discarded on close and re-synced on open.
    Find<QLineEdit>(dialog, "widthEdit")->setText(QStringLiteral("777"));
    Check(dialog.draft().width == 777.0f, "draft edit is live before closing");
    dialog.show();
    Find<QLineEdit>(dialog, "widthEdit")->setText(QStringLiteral("777"));
    Check(dialog.draft().width == 777.0f, "draft edit is live while open");
    dialog.hide();
    Check(dialog.draft().width == 300.0f, "hiding discards the live draft");
    dialog.reject();
    Check(requests == 0, "closing without applying does not apply anything");
    Check(dialog.defaults().width == 300.0f, "closing does not change the applied defaults");
    dialog.show();
    Check(dialog.draft().width == 300.0f, "reopening re-syncs the draft from the applied defaults");
    dialog.hide();

    // An external defaults change is picked up on the next open.
    dialog.setDefaults({ 595.0f, 842.0f, 0xFF202020U, { 0xFF010203U }, false }, {});
    dialog.show();
    Check(dialog.draft().width == 595.0f && dialog.draft().height == 842.0f, "reopening picks up new defaults");
    Check(dialog.draft().backgroundColor == 0xFF202020U, "reopening picks up the new default colour");
    Check(dialog.draft().paletteProfile == std::vector<std::uint32_t>({ 0xFF010203U }), "reopening picks up the profile");
    dialog.hide();
}

// 5. The portable page-application policy (clamping, raster, palette remap).
void PageApplicationPolicy() {
    namespace s = cnote::settings;
    const std::vector<std::uint32_t> sourceProfile { 0xFFAA0001U, 0xFFAA0002U, 0xFFAA0003U };
    const std::vector<std::uint32_t> targetProfile { 0xFFBB0001U, 0xFFBB0002U };

    cnote::Page page;
    page.width = 100.0f;
    page.height = 200.0f;
    page.paletteProfile = sourceProfile;
    page.sourcePdfSourceIndex = 3;
    page.sourcePdfPageIndex = 7;
    page.strokes.push_back(MakeStroke(0xFFAA0001U));
    page.strokes.push_back(MakeStroke(0xFFAA0002U));
    page.strokes.push_back(MakeStroke(0xFFAA0003U));
    page.strokes.push_back(MakeStroke(0xFF123456U));

    const bool changed = cnote::ui::ApplyConfiguredSettingsToPage(page, 612.0f, 792.0f, 0xFF112233U, targetProfile, true);
    Check(changed, "applying different settings reports a change");
    Check(page.width == 612.0f && page.height == 792.0f, "apply sets the page size");
    Check(page.sourcePdfSourceIndex == -1 && page.sourcePdfPageIndex == -1, "apply flattens the PDF source page");
    Check(page.raster.width > 0 && page.raster.height > 0, "apply rebuilds the page raster");
    Check(page.paletteProfile == targetProfile, "apply stores the target profile");
    Check(page.strokes[0].style.color == 0xFFBB0001U && page.strokes[1].style.color == 0xFFBB0002U,
        "apply remaps stroke colours by palette index");
    Check(page.strokes[2].style.color == 0xFFAA0003U, "colours past the target profile are left alone");
    Check(page.strokes[3].style.color == 0xFF123456U, "unrelated colours are left alone");

    const bool again = cnote::ui::ApplyConfiguredSettingsToPage(page, 612.0f, 792.0f, 0xFF112233U, targetProfile, true);
    Check(!again, "re-applying identical settings reports no change");
    Check(!cnote::ui::PageSettingsWouldChange(page, 612.0f, 792.0f, 0xFF112233U, targetProfile, true),
        "PERF-055: the predicate agrees with apply on an unchanged PDF-flattened page");

    // PERF-055: the raster check scans the stored image instead of building a
    // replacement, and the predicate the controller uses for history must
    // agree with the apply.
    const std::uint32_t white = 0xFFFFFFFFU;
    {
        cnote::Page solid;
        solid.width = 595.0f;
        solid.height = 842.0f;
        solid.raster = cnote::CreateSolidRaster(solid.width, solid.height, 1.5f, white);
        const std::uint64_t solidIdentity = solid.raster.pixels.Identity();
        const std::size_t solidBytes =
            static_cast<std::size_t>(solid.raster.stride) * static_cast<std::size_t>(solid.raster.height);

        cnote::ResetSolidRasterBuildCount();
        Check(!cnote::ui::PageSettingsWouldChange(solid, 595.0f, 842.0f, white, {}, false),
            "PERF-055: a solid page matching the settings needs no change");
        Check(!cnote::ui::ApplyConfiguredSettingsToPage(solid, 595.0f, 842.0f, white, {}, false),
            "PERF-055: applying matching settings reports no change");
        Check(cnote::SolidRasterBuildCount() == 0U, "PERF-055: an unchanged page builds no solid raster");
        Check(solid.raster.pixels.Identity() == solidIdentity, "PERF-055: an unchanged page keeps its raster");
        std::printf("PERF-055 matching solid page: %zu bytes retained, 0 built\n", solidBytes);

        std::vector<std::uint8_t> pixels = solid.raster.pixels.bytes();
        pixels[pixels.size() / 2U] ^= 0x01U;
        solid.raster.pixels = cnote::PixelBuffer(std::move(pixels));
        Check(!cnote::RasterImageMatchesSolidColor(solid.raster, 595.0f, 842.0f, 1.5f, white),
            "PERF-055: one changed pixel is not a solid fill");
        Check(cnote::ui::PageSettingsWouldChange(solid, 595.0f, 842.0f, white, {}, false),
            "PERF-055: the predicate sees the changed raster");
        cnote::ResetSolidRasterBuildCount();
        Check(cnote::ui::ApplyConfiguredSettingsToPage(solid, 595.0f, 842.0f, white, {}, false),
            "PERF-055: a changed raster is replaced");
        Check(cnote::SolidRasterBuildCount() == 1U, "PERF-055: a changed page builds one solid raster");
        Check(cnote::RasterImageMatchesSolidColor(solid.raster, 595.0f, 842.0f, 1.5f, white),
            "PERF-055: the replacement is the solid fill");

        cnote::Page wrongScale;
        wrongScale.width = 595.0f;
        wrongScale.height = 842.0f;
        wrongScale.raster = cnote::CreateSolidRaster(595.0f, 842.0f, 1.0f, white);
        Check(cnote::ui::PageSettingsWouldChange(wrongScale, 595.0f, 842.0f, white, {}, false),
            "PERF-055: a solid raster at another scale needs replacing");
        cnote::ResetSolidRasterBuildCount();
        Check(cnote::ui::ApplyConfiguredSettingsToPage(wrongScale, 595.0f, 842.0f, white, {}, false),
            "PERF-055: the scale-mismatched page applies");
        Check(cnote::SolidRasterBuildCount() == 1U, "PERF-055: the mismatched page builds one solid raster");
        Check(wrongScale.raster.scale == 1.5f, "PERF-055: the replacement uses the page-apply raster scale");

        cnote::Page noRaster;
        Check(!cnote::RasterImageMatchesSolidColor(noRaster.raster, 595.0f, 842.0f, 1.5f, white),
            "PERF-055: no raster is not a solid fill");
        Check(cnote::ui::PageSettingsWouldChange(noRaster, 595.0f, 842.0f, white, {}, false),
            "PERF-055: a page without a raster changes");

        // A palette-only change keeps the matching raster: the predicate and
        // apply both report it, but no replacement raster is built.
        const std::vector<std::uint32_t> oldProfile { 0xFFAA0001U };
        const std::vector<std::uint32_t> newProfile { 0xFFBB0001U };
        cnote::Page paletteOnly;
        paletteOnly.width = 595.0f;
        paletteOnly.height = 842.0f;
        paletteOnly.raster = cnote::CreateSolidRaster(595.0f, 842.0f, 1.5f, white);
        paletteOnly.paletteProfile = oldProfile;
        paletteOnly.strokes.push_back(MakeStroke(0xFFAA0001U));
        Check(cnote::ui::PageSettingsWouldChange(paletteOnly, 595.0f, 842.0f, white, newProfile, true),
            "PERF-055: a changed palette profile needs applying");
        Check(!cnote::ui::PageSettingsWouldChange(paletteOnly, 595.0f, 842.0f, white, newProfile, false),
            "PERF-055: the remap flag off leaves the palette alone");
        cnote::ResetSolidRasterBuildCount();
        Check(cnote::ui::ApplyConfiguredSettingsToPage(paletteOnly, 595.0f, 842.0f, white, newProfile, true),
            "PERF-055: a palette-only change applies");
        Check(cnote::SolidRasterBuildCount() == 0U, "PERF-055: a palette-only change builds no raster");
        Check(paletteOnly.paletteProfile == newProfile && paletteOnly.strokes[0].style.color == 0xFFBB0001U,
            "PERF-055: a palette-only change remaps the stored profile and strokes");
        Check(!cnote::ui::PageSettingsWouldChange(paletteOnly, 595.0f, 842.0f, white, newProfile, true),
            "PERF-055: the predicate agrees after the palette-only apply");
    }

    cnote::Page untouched;
    untouched.paletteProfile = sourceProfile;
    untouched.strokes.push_back(MakeStroke(0xFFAA0001U));
    const bool remappedWithoutFlag = cnote::ui::ApplyConfiguredSettingsToPage(untouched, 10.0f, 20.0f, 0xFF112233U, targetProfile, false);
    Check(remappedWithoutFlag, "clamping alone counts as a change");
    Check(untouched.width == cnote::ui::kMinPageSize && untouched.height == cnote::ui::kMinPageSize, "apply clamps to 72pt");
    Check(untouched.strokes[0].style.color == 0xFFAA0001U, "with the remap flag off stroke colours stay unchanged");
    Check(untouched.paletteProfile == sourceProfile, "with the remap flag off the page profile stays unchanged");

    std::vector<std::uint32_t> colors { 0xFF01U, 0xFF02U };
    cnote::ui::RecordPreviousPageColor(colors, 0xFF00U, 0xFF03U);
    Check(colors.size() == 3U && colors[0] == 0xFF00U, "previous colour is promoted to the front");
    Check(std::find(colors.begin(), colors.end(), 0xFF00U) != colors.end() && colors.size() == 3U, "no duplicate previous colour");
    std::vector<std::uint32_t> many;
    for (std::uint32_t index = 0; index < 12U; ++index) {
        cnote::ui::RecordPreviousPageColor(many, 0xFF000000U + index, 0xFF100000U + index);
    }
    Check(many.size() <= cnote::ui::kMaxRecentPageColors, "recent colours are capped");
}

cnote::Document::PageDefaults SolidPageDefaults() {
    cnote::Document::PageDefaults defaults;
    defaults.width = 595.0f;
    defaults.height = 842.0f;
    defaults.backgroundColor = 0xFFFFFFFFU;
    defaults.sizeOverridden = true;
    return defaults;
}

std::vector<cnote::Page> SolidPages(std::size_t count, std::uint32_t color) {
    std::vector<cnote::Page> pages;
    pages.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        cnote::Page page;
        page.width = 595.0f;
        page.height = 842.0f;
        page.raster = cnote::CreateSolidRaster(page.width, page.height, 1.5f, color);
        pages.push_back(std::move(page));
    }
    return pages;
}

std::vector<std::uint64_t> RasterIdentities(const cnote::Document& document) {
    std::vector<std::uint64_t> identities;
    identities.reserve(document.PageCount());
    for (std::size_t index = 0; index < document.PageCount(); ++index) {
        identities.push_back(document.PageAt(index).raster.pixels.Identity());
    }
    return identities;
}

std::vector<cnote::Page> SnapshotPages(const cnote::Document& document) {
    std::vector<cnote::Page> pages;
    pages.reserve(document.PageCount());
    for (std::size_t index = 0; index < document.PageCount(); ++index) {
        pages.push_back(document.PageAt(index));
    }
    return pages;
}

bool PagesMatch(const cnote::Document& document, const std::vector<cnote::Page>& expected) {
    if (document.PageCount() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (!cnote::PageEqual(document.PageAt(index), expected[index])) {
            return false;
        }
    }
    return true;
}

// 6. PERF-055: Apply to All Pages builds no replacement raster for an
// unchanged page and records one ReplacePages action covering only the pages
// that changed.  The 50-page fixture is the one the PR measures.
void ApplyAllPagesRecordsOnlyChangedPages() {
    constexpr std::size_t kPageCount = 50;
    const std::uint32_t white = 0xFFFFFFFFU;
    const std::uint32_t blue = 0xFF2F6FEDU;
    const cnote::Document::PageDefaults defaults = SolidPageDefaults();

    cnote::ui::NewPageDraft whiteDraft;
    whiteDraft.width = 595.0f;
    whiteDraft.height = 842.0f;
    whiteDraft.backgroundColor = white;
    cnote::ui::NewPageDraft blueDraft = whiteDraft;
    blueDraft.backgroundColor = blue;

    // 1. Every page already matches: no history entry, no raster built, no
    // commit and every stored raster is kept.
    {
        cnote::AppManager manager;
        cnote::qt::QtDocumentController controller(manager);
        auto session = std::make_shared<cnote::ProjectSession>();
        session->document = cnote::Document::CreateEditable({}, SolidPages(kPageCount, white), 0, defaults);
        controller.AttachSession(session);

        const std::vector<std::uint64_t> identities = RasterIdentities(session->document);
        const std::size_t undoBefore = session->undoActions.size();
        const std::uint64_t revisionBefore = session->revision;
        cnote::ResetSolidRasterBuildCount();
        const auto started = std::chrono::steady_clock::now();
        controller.ApplyNewPageDraft(whiteDraft, cnote::ui::NewPageApplyScope::AllPages);
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

        Check(session->undoActions.size() == undoBefore, "PERF-055: an unchanged Apply to All records no history");
        Check(cnote::SolidRasterBuildCount() == 0U, "PERF-055: an unchanged Apply to All builds no solid raster");
        Check(RasterIdentities(session->document) == identities, "PERF-055: unchanged pages keep their rasters");
        Check(session->revision == revisionBefore && !controller.Dirty(),
            "PERF-055: an unchanged Apply to All commits nothing");
        std::printf("PERF-055 unchanged 50-page Apply to All: %.3f ms, 0 raster bytes built\n", elapsedMs);
    }

    // 2. Mixed document: matching solid pages, solid pages of the old colour,
    // and PDF-backed pages (which are flattened even when their pixels match).
    {
        std::vector<cnote::Page> pages = SolidPages(kPageCount, white);
        for (std::size_t index = 25; index < 35; ++index) {
            pages[index].raster = cnote::CreateSolidRaster(pages[index].width, pages[index].height, 1.5f, blue);
        }
        for (std::size_t index = 35; index < kPageCount; ++index) {
            pages[index].sourcePdfSourceIndex = 0;
            pages[index].sourcePdfPageIndex = static_cast<std::int32_t>(index - 35U);
            if (index >= 45) {
                pages[index].raster = cnote::CreateSolidRaster(pages[index].width, pages[index].height, 1.5f, blue);
            }
        }
        const std::size_t rasterBytes =
            static_cast<std::size_t>(pages.front().raster.stride) * static_cast<std::size_t>(pages.front().raster.height);
        const std::vector<cnote::Page> original = pages;

        cnote::AppManager manager;
        cnote::qt::QtDocumentController controller(manager);
        auto session = std::make_shared<cnote::ProjectSession>();
        session->document = cnote::Document::CreateEditable({}, std::move(pages), 0, defaults);
        controller.AttachSession(session);
        const std::vector<std::uint64_t> identities = RasterIdentities(session->document);

        const std::uint64_t revisionBefore = session->revision;
        cnote::ResetSolidRasterBuildCount();
        const auto started = std::chrono::steady_clock::now();
        controller.ApplyNewPageDraft(whiteDraft, cnote::ui::NewPageApplyScope::AllPages);
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

        std::vector<std::size_t> expectedChanged;
        for (std::size_t index = 25; index < kPageCount; ++index) {
            expectedChanged.push_back(index);
        }
        Check(cnote::SolidRasterBuildCount() == expectedChanged.size(),
            "PERF-055: a mixed Apply to All builds one raster per changed page only");
        Check(session->revision == revisionBefore + 1U, "PERF-055: a mixed Apply to All is one commit");
        Check(session->undoActions.size() == 1U
                && session->undoActions.back().type == cnote::HistoryActionType::ReplacePages,
            "PERF-055: a mixed Apply to All records one ReplacePages action");
        const cnote::HistoryAction action = session->undoActions.back();
        Check(action.pageIndices == expectedChanged, "PERF-055: the action names only the changed pages");
        Check(action.pages.size() == expectedChanged.size()
                && action.replacementPages.size() == expectedChanged.size(),
            "PERF-055: before and after snapshots cover only the changed pages");
        bool snapshotsMatch = action.pages.size() == expectedChanged.size();
        for (std::size_t index = 0; snapshotsMatch && index < expectedChanged.size(); ++index) {
            snapshotsMatch = cnote::PageEqual(action.pages[index], original[expectedChanged[index]]);
        }
        Check(snapshotsMatch, "PERF-055: the before snapshots are the original changed pages");

        bool changedPagesCorrect = true;
        for (std::size_t index = 25; index < kPageCount; ++index) {
            const cnote::Page& page = session->document.PageAt(index);
            changedPagesCorrect = changedPagesCorrect
                && page.sourcePdfSourceIndex == -1
                && page.sourcePdfPageIndex == -1
                && cnote::RasterImageMatchesSolidColor(page.raster, 595.0f, 842.0f, 1.5f, white);
        }
        Check(changedPagesCorrect, "PERF-055: every changed page is the flattened solid fill");
        const std::vector<std::uint64_t> appliedIdentities = RasterIdentities(session->document);
        bool unchangedRastersKept = true;
        for (std::size_t index = 0; index < 25; ++index) {
            unchangedRastersKept = unchangedRastersKept && appliedIdentities[index] == identities[index];
        }
        Check(unchangedRastersKept, "PERF-055: unchanged pages keep their raster buffers");
        Check(controller.Dirty(), "PERF-055: a mixed Apply to All dirties the session");
        std::printf("PERF-055 mixed 50-page Apply to All: %.3f ms, %zu bytes built (%zu pages)\n",
            elapsedMs, expectedChanged.size() * rasterBytes, expectedChanged.size());

        const std::vector<cnote::Page> applied = SnapshotPages(session->document);
        const std::uint64_t buildsAfterApply = cnote::SolidRasterBuildCount();
        Check(controller.Undo(), "PERF-055: one undo reverts the whole mixed Apply to All");
        Check(cnote::SolidRasterBuildCount() == buildsAfterApply, "PERF-055: undo builds no raster");
        Check(PagesMatch(session->document, original), "PERF-055: undo restores every page exactly");
        Check(RasterIdentities(session->document) == identities, "PERF-055: undo restores the shared rasters");
        Check(controller.Redo(), "PERF-055: the mixed Apply to All redoes");
        Check(cnote::SolidRasterBuildCount() == buildsAfterApply, "PERF-055: redo builds no raster");
        Check(PagesMatch(session->document, applied), "PERF-055: redo restores every applied page exactly");
    }

    // 3. Every page changes: the same one-action/undo/redo shape, with one
    // raster built per page (not one per page plus a comparison raster).
    {
        std::vector<cnote::Page> pages = SolidPages(kPageCount, white);
        const std::size_t rasterBytes =
            static_cast<std::size_t>(pages.front().raster.stride) * static_cast<std::size_t>(pages.front().raster.height);
        const std::vector<cnote::Page> original = pages;

        cnote::AppManager manager;
        cnote::qt::QtDocumentController controller(manager);
        auto session = std::make_shared<cnote::ProjectSession>();
        session->document = cnote::Document::CreateEditable({}, std::move(pages), 0, defaults);
        controller.AttachSession(session);
        const std::vector<std::uint64_t> identities = RasterIdentities(session->document);

        cnote::ResetSolidRasterBuildCount();
        const auto started = std::chrono::steady_clock::now();
        controller.ApplyNewPageDraft(blueDraft, cnote::ui::NewPageApplyScope::AllPages);
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

        Check(cnote::SolidRasterBuildCount() == kPageCount,
            "PERF-055: an all-changed Apply to All builds exactly one raster per page");
        std::vector<std::size_t> expectedChanged;
        for (std::size_t index = 0; index < kPageCount; ++index) {
            expectedChanged.push_back(index);
        }
        Check(session->undoActions.size() == 1U
                && session->undoActions.back().type == cnote::HistoryActionType::ReplacePages
                && session->undoActions.back().pageIndices == expectedChanged,
            "PERF-055: an all-changed Apply to All records all pages in one action");
        bool allChanged = true;
        for (std::size_t index = 0; index < kPageCount; ++index) {
            allChanged = allChanged
                && cnote::RasterImageMatchesSolidColor(session->document.PageAt(index).raster, 595.0f, 842.0f, 1.5f, blue);
        }
        Check(allChanged, "PERF-055: every page is the new solid fill");
        std::printf("PERF-055 all-changed 50-page Apply to All: %.3f ms, %zu bytes built (%zu pages)\n",
            elapsedMs, kPageCount * rasterBytes, kPageCount);

        const std::vector<cnote::Page> applied = SnapshotPages(session->document);
        Check(controller.Undo(), "PERF-055: the all-changed Apply to All undoes in one step");
        Check(PagesMatch(session->document, original), "PERF-055: undo restores every all-changed page exactly");
        Check(RasterIdentities(session->document) == identities, "PERF-055: undo restores every original raster");
        Check(controller.Redo(), "PERF-055: the all-changed Apply to All redoes");
        Check(PagesMatch(session->document, applied), "PERF-055: redo restores every applied page exactly");
    }
}

// 7. Tab order, modeless ownership and screenshots.
void TabOrderAndScreenshots() {
    namespace s = cnote::settings;
    s::Settings settings;
    settings.palette = { 0xFF000000U, 0xFF2563EBU };
    settings.savedPalettes.push_back({ u"Cool", { 0xFF111111U, 0xFF222222U } });
    QWidget owner;
    cnote::ui::NewPageDialogQt dialog(settings, &owner);
    dialog.resize(820, 640);
    dialog.show();
    QApplication::processEvents();

    Check(!dialog.isModal(), "new-page dialog is modeless");
    Check(dialog.parentWidget() == &owner, "new-page dialog keeps its owner");
    Check(owner.isEnabled(), "owner stays interactive while the new-page dialog is open");

    const QStringList expected {
        QStringLiteral("widthEdit"),
        QStringLiteral("heightEdit"),
        QStringLiteral("dimensionCombo"),
        QStringLiteral("hexColorEdit"),
        QStringLiteral("hueSlider"),
        QStringLiteral("saturationSlider"),
        QStringLiteral("lightnessSlider"),
        QStringLiteral("defaultColorSwatch0"),
        QStringLiteral("paletteProfileCombo"),
        QStringLiteral("remapCheck"),
        QStringLiteral("appendPagesButton"),
        QStringLiteral("resetButton"),
        QStringLiteral("applyCurrentButton"),
        QStringLiteral("applyAllButton"),
        QStringLiteral("applyFutureButton"),
    };

    QStringList focusOrder;
    QWidget* widget = dialog.focusWidget() != nullptr ? dialog.focusWidget() : &dialog;
    QWidget* start = widget;
    do {
        if (widget->focusPolicy() != Qt::NoFocus && !widget->objectName().isEmpty()
            && widget->objectName() != QStringLiteral("newPageDialog")) {
            focusOrder.append(widget->objectName());
        }
        widget = widget->nextInFocusChain();
    } while (widget != nullptr && widget != start);

    for (const QString& name : expected) {
        Check(focusOrder.contains(name), "tab order reaches " + name.toStdString());
    }
    const int sizeIndex = focusOrder.indexOf(QStringLiteral("widthEdit"));
    const int applyIndex = focusOrder.indexOf(QStringLiteral("applyFutureButton"));
    Check(sizeIndex >= 0 && applyIndex > sizeIndex, "size controls precede the apply buttons in the tab order");

    const std::string currentHash = ScreenshotHash(dialog, "current");
    dialog.setDefaults({ 595.0f, 842.0f, 0xFF202020U, settings.savedPalettes[0].colors, true }, { 0xFF0A0B0CU, 0xFF050607U });
    dialog.syncDraftFromDefaults();
    QApplication::processEvents();
    const std::string changedHash = ScreenshotHash(dialog, "changed");
    Check(!currentHash.empty() && currentHash != changedHash, "draft changes change the rendering");

    dialog.syncDraftFromDefaults();
    QApplication::processEvents();
    Check(ScreenshotHash(dialog, "changed2") == changedHash, "screenshots are deterministic");
    dialog.hide();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const std::filesystem::path scratch = ScratchDirectory("newpage");
#ifdef _WIN32
    qputenv("APPDATA", scratch.string().c_str());
#else
    qputenv("XDG_CONFIG_HOME", scratch.string().c_str());
#endif

    ControlInventory();
    DraftEdits();
    UnchangedDefaultsKeepSwatches();
    EmptySavedPalettesDoNotShiftProfiles();
    ApplySemantics();
    ResetAndRefresh();
    PageApplicationPolicy();
    ApplyAllPagesRecordsOnlyChangedPages();
    TabOrderAndScreenshots();

    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    std::printf(g_failures == 0 ? "ALL NEW PAGE DIALOG CHECKS PASSED\n" : "%d NEW PAGE DIALOG CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
