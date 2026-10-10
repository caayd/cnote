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
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "Document.h"
#include "settings/SettingsStoreQt.h"
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

    Find<QPushButton>(dialog, "resetButton")->click();
    Check(dialog.draft().width == 595.0f && dialog.draft().height == 842.0f, "reset restores the A4 page size");
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
    dialog.hide();
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

// 6. Tab order, modeless ownership and screenshots.
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
    ApplySemantics();
    ResetAndRefresh();
    PageApplicationPolicy();
    TabOrderAndScreenshots();

    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    std::printf(g_failures == 0 ? "ALL NEW PAGE DIALOG CHECKS PASSED\n" : "%d NEW PAGE DIALOG CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
