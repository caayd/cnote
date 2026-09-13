#include "ui/NewPageDialogQt.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#include "settings/SettingsStoreQt.h"
#include "ui/SettingsFormatQt.h"

namespace cnote::ui {

namespace {

constexpr int kDefaultSwatchColumns = 5;
constexpr int kRecentSwatchColumns = 6;
constexpr int kSwatchSize = 24;

QString SwatchStyle(std::uint32_t color, bool selected) {
    return QStringLiteral("QPushButton { background-color: %1; border: %2px solid %3; }")
        .arg(FormatHexColor(color))
        .arg(selected ? 2 : 1)
        .arg(selected ? QStringLiteral("#5B8DEF") : QStringLiteral("#2E3A4A"));
}

}  // namespace

NewPageDialogQt::NewPageDialogQt(settings::Settings& settings, QWidget* parent)
    : QDialog(parent),
      settings_(settings) {
    setObjectName(QStringLiteral("newPageDialog"));
    setWindowTitle(QStringLiteral("New Page Settings"));
    setModal(false);
    buildUi();
    refreshFromSettings();
}

NewPageDialogQt::~NewPageDialogQt() = default;

void NewPageDialogQt::buildUi() {
    auto* root = new QVBoxLayout(this);

    auto* defaultsGroup = new QGroupBox(QStringLiteral("Defaults"), this);
    auto* form = new QFormLayout(defaultsGroup);

    auto* dimensionRow = new QWidget(defaultsGroup);
    auto* dimensionLayout = new QHBoxLayout(dimensionRow);
    dimensionLayout->setContentsMargins(0, 0, 0, 0);
    widthEdit_ = new QLineEdit(dimensionRow);
    widthEdit_->setObjectName(QStringLiteral("widthEdit"));
    widthEdit_->setFixedWidth(90);
    heightEdit_ = new QLineEdit(dimensionRow);
    heightEdit_->setObjectName(QStringLiteral("heightEdit"));
    heightEdit_->setFixedWidth(90);
    dimensionCombo_ = new QComboBox(dimensionRow);
    dimensionCombo_->setObjectName(QStringLiteral("dimensionCombo"));
    dimensionCombo_->addItem(QStringLiteral("Custom"));
    for (const DimensionPreset& preset : DimensionPresets()) {
        dimensionCombo_->addItem(QString::fromUtf8(preset.label));
    }
    dimensionLayout->addWidget(new QLabel(QStringLiteral("Width (pt)"), dimensionRow));
    dimensionLayout->addWidget(widthEdit_);
    dimensionLayout->addWidget(new QLabel(QStringLiteral("Height (pt)"), dimensionRow));
    dimensionLayout->addWidget(heightEdit_);
    dimensionLayout->addWidget(new QLabel(QStringLiteral("Common Sizes"), dimensionRow));
    dimensionLayout->addWidget(dimensionCombo_, 1);
    form->addRow(QStringLiteral("Page Size"), dimensionRow);

    auto* colorRow = new QWidget(defaultsGroup);
    auto* colorLayout = new QHBoxLayout(colorRow);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    colorPreview_ = new QLabel(colorRow);
    colorPreview_->setObjectName(QStringLiteral("colorPreview"));
    colorPreview_->setFixedSize(48, 24);
    hexColorEdit_ = new QLineEdit(colorRow);
    hexColorEdit_->setObjectName(QStringLiteral("hexColorEdit"));
    hexColorEdit_->setMaxLength(10);
    hexColorEdit_->setFixedWidth(110);
    colorLayout->addWidget(new QLabel(QStringLiteral("Background"), colorRow));
    colorLayout->addWidget(colorPreview_);
    colorLayout->addWidget(hexColorEdit_);
    colorLayout->addStretch(1);
    form->addRow(QStringLiteral("Color"), colorRow);

    hueSlider_ = new QSlider(Qt::Horizontal, defaultsGroup);
    hueSlider_->setObjectName(QStringLiteral("hueSlider"));
    hueSlider_->setRange(0, 360);
    hueLabel_ = new QLabel(defaultsGroup);
    hueLabel_->setObjectName(QStringLiteral("hueLabel"));
    hueLabel_->setMinimumWidth(110);
    auto* hueRow = new QWidget(defaultsGroup);
    auto* hueLayout = new QHBoxLayout(hueRow);
    hueLayout->setContentsMargins(0, 0, 0, 0);
    hueLayout->addWidget(hueSlider_, 1);
    hueLayout->addWidget(hueLabel_);
    form->addRow(QStringLiteral("Hue"), hueRow);

    saturationSlider_ = new QSlider(Qt::Horizontal, defaultsGroup);
    saturationSlider_->setObjectName(QStringLiteral("saturationSlider"));
    saturationSlider_->setRange(0, 100);
    saturationLabel_ = new QLabel(defaultsGroup);
    saturationLabel_->setObjectName(QStringLiteral("saturationLabel"));
    saturationLabel_->setMinimumWidth(110);
    auto* saturationRow = new QWidget(defaultsGroup);
    auto* saturationLayout = new QHBoxLayout(saturationRow);
    saturationLayout->setContentsMargins(0, 0, 0, 0);
    saturationLayout->addWidget(saturationSlider_, 1);
    saturationLayout->addWidget(saturationLabel_);
    form->addRow(QStringLiteral("Saturation"), saturationRow);

    lightnessSlider_ = new QSlider(Qt::Horizontal, defaultsGroup);
    lightnessSlider_->setObjectName(QStringLiteral("lightnessSlider"));
    lightnessSlider_->setRange(0, 100);
    lightnessLabel_ = new QLabel(defaultsGroup);
    lightnessLabel_->setObjectName(QStringLiteral("lightnessLabel"));
    lightnessLabel_->setMinimumWidth(110);
    auto* lightnessRow = new QWidget(defaultsGroup);
    auto* lightnessLayout = new QHBoxLayout(lightnessRow);
    lightnessLayout->setContentsMargins(0, 0, 0, 0);
    lightnessLayout->addWidget(lightnessSlider_, 1);
    lightnessLayout->addWidget(lightnessLabel_);
    form->addRow(QStringLiteral("Lightness"), lightnessRow);

    defaultSwatchContainer_ = new QWidget(defaultsGroup);
    form->addRow(QStringLiteral("Default Colors"), defaultSwatchContainer_);

    recentSwatchContainer_ = new QWidget(defaultsGroup);
    recentColorsEmptyLabel_ = new QLabel(QStringLiteral("No previous colors yet."), defaultsGroup);
    recentColorsEmptyLabel_->setObjectName(QStringLiteral("recentColorsEmptyLabel"));
    auto* recentRow = new QWidget(defaultsGroup);
    auto* recentLayout = new QVBoxLayout(recentRow);
    recentLayout->setContentsMargins(0, 0, 0, 0);
    recentLayout->addWidget(recentSwatchContainer_);
    recentLayout->addWidget(recentColorsEmptyLabel_);
    form->addRow(QStringLiteral("Previous Colors"), recentRow);

    paletteProfileCombo_ = new QComboBox(defaultsGroup);
    paletteProfileCombo_->setObjectName(QStringLiteral("paletteProfileCombo"));
    paletteProfileLabel_ = new QLabel(defaultsGroup);
    paletteProfileLabel_->setObjectName(QStringLiteral("paletteProfileLabel"));
    auto* profileRow = new QWidget(defaultsGroup);
    auto* profileLayout = new QHBoxLayout(profileRow);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    profileLayout->addWidget(paletteProfileCombo_, 1);
    profileLayout->addWidget(paletteProfileLabel_);
    form->addRow(QStringLiteral("Stroke Color Profile"), profileRow);

    remapCheck_ = new QCheckBox(QStringLiteral("Swap existing stroke colors on Current/All apply"), defaultsGroup);
    remapCheck_->setObjectName(QStringLiteral("remapCheck"));
    form->addRow(QString(), remapCheck_);

    root->addWidget(defaultsGroup);

    auto* hint = new QLabel(
        QStringLiteral("Current and All remap strokes from each page's stored profile. "
                       "Future Pages stores the selected profile for new pages."),
        this);
    hint->setObjectName(QStringLiteral("remapHintLabel"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* buttons = new QHBoxLayout();
    appendButton_ = new QPushButton(QStringLiteral("Append Pages..."), this);
    appendButton_->setObjectName(QStringLiteral("appendPagesButton"));
    resetButton_ = new QPushButton(QStringLiteral("Reset Defaults"), this);
    resetButton_->setObjectName(QStringLiteral("resetButton"));
    applyCurrentButton_ = new QPushButton(QStringLiteral("Apply to Current Page"), this);
    applyCurrentButton_->setObjectName(QStringLiteral("applyCurrentButton"));
    applyAllButton_ = new QPushButton(QStringLiteral("Apply to All"), this);
    applyAllButton_->setObjectName(QStringLiteral("applyAllButton"));
    applyFutureButton_ = new QPushButton(QStringLiteral("Apply to Future Pages"), this);
    applyFutureButton_->setObjectName(QStringLiteral("applyFutureButton"));
    buttons->addWidget(appendButton_);
    buttons->addWidget(resetButton_);
    buttons->addStretch(1);
    buttons->addWidget(applyCurrentButton_);
    buttons->addWidget(applyAllButton_);
    buttons->addWidget(applyFutureButton_);
    root->addLayout(buttons);

    connect(widthEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        applyDimensionText(widthEdit_, draft_.width, false);
    });
    connect(heightEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        applyDimensionText(heightEdit_, draft_.height, false);
    });
    connect(widthEdit_, &QLineEdit::editingFinished, this, [this] {
        applyDimensionText(widthEdit_, draft_.width, true);
    });
    connect(heightEdit_, &QLineEdit::editingFinished, this, [this] {
        applyDimensionText(heightEdit_, draft_.height, true);
    });
    connect(dimensionCombo_, &QComboBox::activated, this, [this](int index) {
        if (index <= 0) {
            return;
        }
        const auto presets = DimensionPresets();
        if (static_cast<std::size_t>(index - 1) >= presets.size()) {
            return;
        }
        draft_.width = presets[static_cast<std::size_t>(index - 1)].width;
        draft_.height = presets[static_cast<std::size_t>(index - 1)].height;
        refreshDraftControls();
    });
    connect(hexColorEdit_, &QLineEdit::editingFinished, this, [this] { applyHexColorEdit(); });
    for (QSlider* slider : { hueSlider_, saturationSlider_, lightnessSlider_ }) {
        connect(slider, &QSlider::valueChanged, this, [this](int) {
            applyHslSliders();
        });
    }
    connect(paletteProfileCombo_, &QComboBox::activated, this, [this](int index) {
        if (index == 0) {
            draft_.paletteProfile = ResolvedPaletteProfile({}, settings_.palette);
        } else if (index > 0 && static_cast<std::size_t>(index - 1) < settings_.savedPalettes.size()) {
            draft_.paletteProfile = settings_.savedPalettes[static_cast<std::size_t>(index - 1)].colors;
        } else {
            return;
        }
        refreshDraftControls();
    });
    connect(remapCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        draft_.applyPaletteProfileToExistingStrokes = checked;
    });
    connect(appendButton_, &QPushButton::clicked, this, [this] { emit appendPagesRequested(); });
    connect(resetButton_, &QPushButton::clicked, this, [this] { resetDraft(); });
    connect(applyCurrentButton_, &QPushButton::clicked, this, [this] { applyToCurrentPage(); });
    connect(applyAllButton_, &QPushButton::clicked, this, [this] { applyToAllPages(); });
    connect(applyFutureButton_, &QPushButton::clicked, this, [this] { applyToFuturePages(); });
}

// --- model <-> controls ------------------------------------------------------

std::vector<std::uint32_t> NewPageDialogQt::currentSettingsProfile() const {
    return settings_.palette;
}

std::vector<std::uint32_t> NewPageDialogQt::resolvedDraftProfile() const {
    return ResolvedPaletteProfile(draft_.paletteProfile, settings_.palette);
}

QString NewPageDialogQt::draftProfileLabel() const {
    const std::vector<std::uint32_t> resolved = resolvedDraftProfile();
    if (resolved == settings_.palette) {
        return QStringLiteral("Current Palette");
    }
    for (const settings::NamedPalette& palette : settings_.savedPalettes) {
        if (palette.colors == resolved && !palette.name.empty()) {
            return QString::fromUtf16(palette.name.data(), static_cast<qsizetype>(palette.name.size()));
        }
    }
    return QStringLiteral("Custom Profile");
}

void NewPageDialogQt::refreshFromSettings() {
    refreshPaletteProfileCombo();
    refreshDefaultSwatches();
    refreshRecentColors();
    syncDraftFromDefaults();
}

void NewPageDialogQt::syncDraftFromDefaults() {
    draft_.width = defaults_.width;
    draft_.height = defaults_.height;
    draft_.backgroundColor = defaults_.backgroundColor;
    draft_.paletteProfile = ResolvedPaletteProfile(defaults_.paletteProfile, settings_.palette);
    draft_.applyPaletteProfileToExistingStrokes = defaults_.applyPaletteProfileToExistingStrokes;
    refreshDraftControls();
}

void NewPageDialogQt::setDefaults(NewPageDefaults defaults, std::vector<std::uint32_t> recentColors) {
    defaults_ = std::move(defaults);
    recentColors_ = std::move(recentColors);
    if (recentColors_.size() > kMaxRecentPageColors) {
        recentColors_.resize(kMaxRecentPageColors);
    }
    refreshRecentColors();
}

void NewPageDialogQt::setDraft(const NewPageDraft& draft) {
    draft_ = draft;
    refreshDraftControls();
}

void NewPageDialogQt::refreshDraftControls() {
    const QSignalBlocker blockWidth(widthEdit_);
    const QSignalBlocker blockHeight(heightEdit_);
    const QSignalBlocker blockDimension(dimensionCombo_);
    const QSignalBlocker blockHex(hexColorEdit_);
    const QSignalBlocker blockHue(hueSlider_);
    const QSignalBlocker blockSaturation(saturationSlider_);
    const QSignalBlocker blockLightness(lightnessSlider_);
    const QSignalBlocker blockRemap(remapCheck_);
    const QSignalBlocker blockProfile(paletteProfileCombo_);

    if (!widthEdit_->hasFocus()) {
        widthEdit_->setText(FormatDimensionValue(draft_.width));
    }
    if (!heightEdit_->hasFocus()) {
        heightEdit_->setText(FormatDimensionValue(draft_.height));
    }
    dimensionCombo_->setCurrentIndex(0);
    const QString presetLabel = DimensionPresetLabelFor(draft_.width, draft_.height);
    for (int index = 1; index < dimensionCombo_->count(); ++index) {
        if (dimensionCombo_->itemText(index) == presetLabel) {
            dimensionCombo_->setCurrentIndex(index);
            break;
        }
    }

    colorPreview_->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #2E3A4A;").arg(FormatHexColor(draft_.backgroundColor)));
    if (!hexColorEdit_->hasFocus()) {
        hexColorEdit_->setText(FormatHexColor(draft_.backgroundColor));
    }

    const HslColor hsl = RgbToHsl(draft_.backgroundColor);
    const int hue = static_cast<int>(std::lround(hsl.hue));
    const int saturation = static_cast<int>(std::lround(hsl.saturation * 100.0f));
    const int lightness = static_cast<int>(std::lround(hsl.lightness * 100.0f));
    hueSlider_->setValue(hue);
    saturationSlider_->setValue(saturation);
    lightnessSlider_->setValue(lightness);
    hueLabel_->setText(FormatHueLabel(hue));
    saturationLabel_->setText(FormatPercentLabel(QStringLiteral("Saturation"), saturation));
    lightnessLabel_->setText(FormatPercentLabel(QStringLiteral("Lightness"), lightness));

    remapCheck_->setChecked(draft_.applyPaletteProfileToExistingStrokes);

    // Palette profile combo: 0 = current palette, 1..n = saved palettes; a
    // profile matching neither shows a disabled legacy "Custom Profile" entry.
    paletteProfileCombo_->setCurrentIndex(0);
    const std::vector<std::uint32_t> resolved = resolvedDraftProfile();
    bool matchedProfile = resolved == settings_.palette;
    if (!matchedProfile) {
        for (std::size_t index = 0; index < settings_.savedPalettes.size(); ++index) {
            if (settings_.savedPalettes[index].colors == resolved) {
                const int comboIndex = static_cast<int>(index) + 1;
                if (comboIndex < paletteProfileCombo_->count()) {
                    paletteProfileCombo_->setCurrentIndex(comboIndex);
                }
                matchedProfile = true;
                break;
            }
        }
    }
    ensureCustomProfileEntry(!matchedProfile);
    if (!matchedProfile) {
        paletteProfileCombo_->setCurrentIndex(paletteProfileCombo_->count() - 1);
    }
    paletteProfileLabel_->setText(draftProfileLabel());
}

void NewPageDialogQt::ensureCustomProfileEntry(bool needed) {
    const int last = paletteProfileCombo_->count() - 1;
    const bool hasCustom = last >= 0 && paletteProfileCombo_->itemText(last) == QStringLiteral("Custom Profile");
    if (needed && !hasCustom) {
        paletteProfileCombo_->addItem(QStringLiteral("Custom Profile"));
        paletteProfileCombo_->setItemData(paletteProfileCombo_->count() - 1, QVariant(0), Qt::UserRole - 1);
    } else if (!needed && hasCustom) {
        paletteProfileCombo_->removeItem(last);
    }
}

void NewPageDialogQt::refreshDefaultSwatches() {
    if (auto* old = defaultSwatchContainer_->layout(); old != nullptr) {
        QLayoutItem* item = nullptr;
        while ((item = old->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        delete old;
    }

    auto* grid = new QGridLayout(defaultSwatchContainer_);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);
    const auto colors = settings::kDefaultPageColors;
    for (std::size_t index = 0; index < colors.size(); ++index) {
        auto* button = new QPushButton(defaultSwatchContainer_);
        button->setObjectName(QStringLiteral("defaultColorSwatch%1").arg(index));
        button->setFixedSize(kSwatchSize, kSwatchSize);
        button->setStyleSheet(SwatchStyle(colors[index], colors[index] == draft_.backgroundColor));
        connect(button, &QPushButton::clicked, this, [this, color = colors[index]] {
            draft_.backgroundColor = color;
            refreshDraftControls();
        });
        grid->addWidget(
            button,
            static_cast<int>(index) / kDefaultSwatchColumns,
            static_cast<int>(index) % kDefaultSwatchColumns);
    }
    grid->setColumnStretch(kDefaultSwatchColumns, 1);
}

void NewPageDialogQt::refreshRecentColors() {
    if (auto* old = recentSwatchContainer_->layout(); old != nullptr) {
        QLayoutItem* item = nullptr;
        while ((item = old->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        delete old;
    }

    auto* grid = new QGridLayout(recentSwatchContainer_);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);
    for (std::size_t index = 0; index < recentColors_.size(); ++index) {
        auto* button = new QPushButton(recentSwatchContainer_);
        button->setObjectName(QStringLiteral("recentColorSwatch%1").arg(index));
        button->setFixedSize(kSwatchSize, kSwatchSize);
        button->setStyleSheet(SwatchStyle(recentColors_[index], recentColors_[index] == draft_.backgroundColor));
        connect(button, &QPushButton::clicked, this, [this, color = recentColors_[index]] {
            draft_.backgroundColor = color;
            refreshDraftControls();
        });
        grid->addWidget(
            button,
            static_cast<int>(index) / kRecentSwatchColumns,
            static_cast<int>(index) % kRecentSwatchColumns);
    }
    grid->setColumnStretch(kRecentSwatchColumns, 1);
    recentColorsEmptyLabel_->setVisible(recentColors_.empty());
}

void NewPageDialogQt::refreshPaletteProfileCombo() {
    const QSignalBlocker block(paletteProfileCombo_);
    paletteProfileCombo_->clear();
    paletteProfileCombo_->addItem(QStringLiteral("Current Palette"));
    for (const settings::NamedPalette& palette : settings_.savedPalettes) {
        if (palette.colors.empty()) {
            continue;
        }
        paletteProfileCombo_->addItem(QString::fromUtf16(palette.name.data(), static_cast<qsizetype>(palette.name.size())));
    }
}

// --- edits -------------------------------------------------------------------

void NewPageDialogQt::applyDimensionText(QLineEdit* edit, float& value, bool normalizeText) {
    if (edit == nullptr) {
        return;
    }
    if (const auto parsed = ParsePositiveFloat(edit->text()); parsed.has_value()) {
        value = std::max(kMinPageSize, *parsed);
    }
    if (normalizeText) {
        const QSignalBlocker block(edit);
        edit->setText(FormatDimensionValue(value));
    }
}

void NewPageDialogQt::commitDimensionEdits() {
    applyDimensionText(widthEdit_, draft_.width, true);
    applyDimensionText(heightEdit_, draft_.height, true);
    applyHexColorEdit();
}

void NewPageDialogQt::applyHexColorEdit() {
    const auto parsed = ParseHexColor(hexColorEdit_->text());
    if (!parsed.has_value()) {
        refreshDraftControls();
        return;
    }
    draft_.backgroundColor = *parsed;
    refreshDraftControls();
}

void NewPageDialogQt::applyHslSliders() {
    const float hue = static_cast<float>(hueSlider_->value());
    const float saturation = static_cast<float>(saturationSlider_->value()) / 100.0f;
    const float lightness = static_cast<float>(lightnessSlider_->value()) / 100.0f;
    draft_.backgroundColor = HslToColor(hue, saturation, lightness);

    colorPreview_->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #2E3A4A;").arg(FormatHexColor(draft_.backgroundColor)));
    if (!hexColorEdit_->hasFocus()) {
        const QSignalBlocker block(hexColorEdit_);
        hexColorEdit_->setText(FormatHexColor(draft_.backgroundColor));
    }
    const HslColor hsl = RgbToHsl(draft_.backgroundColor);
    hueLabel_->setText(FormatHueLabel(static_cast<int>(std::lround(hsl.hue))));
    saturationLabel_->setText(FormatPercentLabel(QStringLiteral("Saturation"), static_cast<int>(std::lround(hsl.saturation * 100.0f))));
    lightnessLabel_->setText(FormatPercentLabel(QStringLiteral("Lightness"), static_cast<int>(std::lround(hsl.lightness * 100.0f))));
}

void NewPageDialogQt::resetDraft() {
    draft_.width = 612.0f;
    draft_.height = 792.0f;
    draft_.backgroundColor = settings_.defaultPageColor;
    draft_.paletteProfile = ResolvedPaletteProfile({}, settings_.palette);
    draft_.applyPaletteProfileToExistingStrokes = false;
    refreshDraftControls();
}

void NewPageDialogQt::applyToCurrentPage() {
    commitDimensionEdits();
    refreshDraftControls();
    emit applyRequested(draft_, NewPageApplyScope::CurrentPage);
}

void NewPageDialogQt::applyToAllPages() {
    commitDimensionEdits();
    refreshDraftControls();
    emit applyRequested(draft_, NewPageApplyScope::AllPages);
}

void NewPageDialogQt::applyToFuturePages() {
    commitDimensionEdits();

    const std::uint32_t previousColor = defaults_.backgroundColor;
    defaults_.width = std::max(kMinPageSize, draft_.width);
    defaults_.height = std::max(kMinPageSize, draft_.height);
    defaults_.backgroundColor = draft_.backgroundColor;
    defaults_.paletteProfile = resolvedDraftProfile();
    defaults_.applyPaletteProfileToExistingStrokes = draft_.applyPaletteProfileToExistingStrokes;

    RecordPreviousPageColor(recentColors_, previousColor, draft_.backgroundColor);
    refreshRecentColors();
    refreshDraftControls();

    emit futureDefaultsChanged(defaults_, recentColors_);
    emit applyRequested(draft_, NewPageApplyScope::FuturePages);
}

void NewPageDialogQt::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    refreshFromSettings();
}

}  // namespace cnote::ui
