#include "ui/SettingsDialogQt.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/SettingsFormatQt.h"

namespace cnote::ui {

namespace {

QIcon ColorSwatchIcon(std::uint32_t color) {
    QPixmap pixmap(14, 14);
    pixmap.fill(QColor::fromRgb(color));
    QIcon icon(pixmap);
    return icon;
}

QString BindingLabel(const QKeySequence& sequence) {
    if (sequence.isEmpty()) {
        return QStringLiteral("Unbound");
    }
    return sequence.toString(QKeySequence::NativeText);
}

QWidget* MakeSliderRow(QSlider* slider, QLabel* label, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(slider, 1);
    label->setMinimumWidth(150);
    layout->addWidget(label);
    return row;
}

}  // namespace

SettingsDialogQt::SettingsDialogQt(
    settings::Settings& settings,
    settings::SettingsStoreQt store,
    QWidget* parent)
    : QDialog(parent),
      settings_(settings),
      store_(std::move(store)) {
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(QStringLiteral("Settings"));
    setModal(false);
    buildUi();
    refreshFromSettings();
}

SettingsDialogQt::~SettingsDialogQt() {
    if (capturingAction_.has_value()) {
        qApp->removeEventFilter(this);
    }
}

void SettingsDialogQt::buildUi() {
    auto* root = new QVBoxLayout(this);
    categoryTabs_ = new QTabWidget(this);
    categoryTabs_->setObjectName(QStringLiteral("categoryTabs"));
    root->addWidget(categoryTabs_);

    buildPalettePage();
    buildDrawingPage();
    buildShortcutsPage();
}

void SettingsDialogQt::buildPalettePage() {
    auto* page = new QWidget(categoryTabs_);
    auto* root = new QHBoxLayout(page);

    paletteList_ = new QListWidget(page);
    paletteList_->setObjectName(QStringLiteral("paletteList"));
    paletteList_->setMinimumWidth(220);
    root->addWidget(paletteList_, 1);

    auto* column = new QVBoxLayout();
    addColorButton_ = new QPushButton(QStringLiteral("Add Color"), page);
    addColorButton_->setObjectName(QStringLiteral("addColorButton"));
    removeColorButton_ = new QPushButton(QStringLiteral("Remove Color"), page);
    removeColorButton_->setObjectName(QStringLiteral("removeColorButton"));
    column->addWidget(addColorButton_);
    column->addWidget(removeColorButton_);

    hexColorEdit_ = new QLineEdit(page);
    hexColorEdit_->setObjectName(QStringLiteral("hexColorEdit"));
    hexColorEdit_->setMaxLength(10);
    column->addWidget(hexColorEdit_);

    hueSlider_ = new QSlider(Qt::Horizontal, page);
    hueSlider_->setObjectName(QStringLiteral("hueSlider"));
    hueSlider_->setRange(0, 360);
    hueLabel_ = new QLabel(page);
    hueLabel_->setObjectName(QStringLiteral("hueLabel"));
    column->addWidget(MakeSliderRow(hueSlider_, hueLabel_, page));

    saturationSlider_ = new QSlider(Qt::Horizontal, page);
    saturationSlider_->setObjectName(QStringLiteral("saturationSlider"));
    saturationSlider_->setRange(0, 100);
    saturationLabel_ = new QLabel(page);
    saturationLabel_->setObjectName(QStringLiteral("saturationLabel"));
    column->addWidget(MakeSliderRow(saturationSlider_, saturationLabel_, page));

    lightnessSlider_ = new QSlider(Qt::Horizontal, page);
    lightnessSlider_->setObjectName(QStringLiteral("lightnessSlider"));
    lightnessSlider_->setRange(0, 100);
    lightnessLabel_ = new QLabel(page);
    lightnessLabel_->setObjectName(QStringLiteral("lightnessLabel"));
    column->addWidget(MakeSliderRow(lightnessSlider_, lightnessLabel_, page));

    auto* savedLayout = new QHBoxLayout();
    savedPaletteNameEdit_ = new QLineEdit(page);
    savedPaletteNameEdit_->setObjectName(QStringLiteral("savedPaletteNameEdit"));
    savedPaletteNameEdit_->setMaxLength(48);
    savedPaletteNameEdit_->setPlaceholderText(QStringLiteral("Palette name"));
    savedLayout->addWidget(savedPaletteNameEdit_, 1);
    savePaletteButton_ = new QPushButton(QStringLiteral("Save"), page);
    savePaletteButton_->setObjectName(QStringLiteral("savePaletteButton"));
    applyPaletteButton_ = new QPushButton(QStringLiteral("Apply"), page);
    applyPaletteButton_->setObjectName(QStringLiteral("applyPaletteButton"));
    deletePaletteButton_ = new QPushButton(QStringLiteral("Delete"), page);
    deletePaletteButton_->setObjectName(QStringLiteral("deletePaletteButton"));
    savedLayout->addWidget(savePaletteButton_);
    savedLayout->addWidget(applyPaletteButton_);
    savedLayout->addWidget(deletePaletteButton_);
    column->addLayout(savedLayout);

    savedPaletteList_ = new QListWidget(page);
    savedPaletteList_->setObjectName(QStringLiteral("savedPaletteList"));
    savedPaletteList_->setMinimumHeight(120);
    column->addWidget(savedPaletteList_);
    root->addLayout(column, 1);

    categoryTabs_->addTab(page, QStringLiteral("Palette"));

    connect(paletteList_, &QListWidget::currentRowChanged, this, [this](int) {
        if (refreshing_) {
            return;
        }
        applyPaletteSelection();
    });
    connect(addColorButton_, &QPushButton::clicked, this, [this] { addPaletteColor(); });
    connect(removeColorButton_, &QPushButton::clicked, this, [this] { removePaletteColor(); });
    connect(hexColorEdit_, &QLineEdit::editingFinished, this, [this] { applyHexColorEdit(); });
    for (QSlider* slider : { hueSlider_, saturationSlider_, lightnessSlider_ }) {
        connect(slider, &QSlider::valueChanged, this, [this](int) {
            if (!refreshing_) {
                applyHueSaturationLightness();
            }
        });
    }
    connect(savedPaletteNameEdit_, &QLineEdit::textChanged, this, [this](const QString&) { refreshSavedPaletteButtons(); });
    connect(savePaletteButton_, &QPushButton::clicked, this, [this] { saveNamedPalette(); });
    connect(applyPaletteButton_, &QPushButton::clicked, this, [this] { applySelectedNamedPalette(); });
    connect(deletePaletteButton_, &QPushButton::clicked, this, [this] { deleteSelectedNamedPalette(); });
    connect(savedPaletteList_, &QListWidget::currentRowChanged, this, [this](int) { refreshSavedPaletteButtons(); });
    connect(savedPaletteList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { applySelectedNamedPalette(); });
}

void SettingsDialogQt::buildDrawingPage() {
    auto* page = new QWidget(categoryTabs_);
    auto* form = new QFormLayout(page);

    stabilizationSlider_ = new QSlider(Qt::Horizontal, page);
    stabilizationSlider_->setObjectName(QStringLiteral("stabilizationSlider"));
    stabilizationSlider_->setRange(5, 95);
    stabilizationLabel_ = new QLabel(page);
    stabilizationLabel_->setObjectName(QStringLiteral("stabilizationLabel"));
    form->addRow(QStringLiteral("Stabilization"), MakeSliderRow(stabilizationSlider_, stabilizationLabel_, page));

    spacingSlider_ = new QSlider(Qt::Horizontal, page);
    spacingSlider_->setObjectName(QStringLiteral("spacingSlider"));
    spacingSlider_->setRange(5, 405);
    spacingLabel_ = new QLabel(page);
    spacingLabel_->setObjectName(QStringLiteral("spacingLabel"));
    form->addRow(QStringLiteral("Spacing"), MakeSliderRow(spacingSlider_, spacingLabel_, page));

    strokeSmoothingSlider_ = new QSlider(Qt::Horizontal, page);
    strokeSmoothingSlider_->setObjectName(QStringLiteral("strokeSmoothingSlider"));
    strokeSmoothingSlider_->setRange(0, 100);
    strokeSmoothingLabel_ = new QLabel(page);
    strokeSmoothingLabel_->setObjectName(QStringLiteral("strokeSmoothingLabel"));
    form->addRow(QStringLiteral("Stroke Smoothing"), MakeSliderRow(strokeSmoothingSlider_, strokeSmoothingLabel_, page));

    drawCursorKindCombo_ = new QComboBox(page);
    drawCursorKindCombo_->setObjectName(QStringLiteral("drawCursorKindCombo"));
    drawCursorKindCombo_->addItem(QStringLiteral("Dot"));
    drawCursorKindCombo_->addItem(QStringLiteral("Precision Crosshair"));
    form->addRow(QStringLiteral("Draw Cursor"), drawCursorKindCombo_);

    drawCursorSizeSlider_ = new QSlider(Qt::Horizontal, page);
    drawCursorSizeSlider_->setObjectName(QStringLiteral("drawCursorSizeSlider"));
    drawCursorSizeSlider_->setRange(4, 24);
    drawCursorSizeLabel_ = new QLabel(page);
    drawCursorSizeLabel_->setObjectName(QStringLiteral("drawCursorSizeLabel"));
    form->addRow(QStringLiteral("Draw Cursor Size"), MakeSliderRow(drawCursorSizeSlider_, drawCursorSizeLabel_, page));

    dashLengthSlider_ = new QSlider(Qt::Horizontal, page);
    dashLengthSlider_->setObjectName(QStringLiteral("dashLengthSlider"));
    dashLengthSlider_->setRange(5, 80);
    dashLengthLabel_ = new QLabel(page);
    dashLengthLabel_->setObjectName(QStringLiteral("dashLengthLabel"));
    form->addRow(QStringLiteral("Dash Length"), MakeSliderRow(dashLengthSlider_, dashLengthLabel_, page));

    dashGapSlider_ = new QSlider(Qt::Horizontal, page);
    dashGapSlider_->setObjectName(QStringLiteral("dashGapSlider"));
    dashGapSlider_->setRange(5, 80);
    dashGapLabel_ = new QLabel(page);
    dashGapLabel_->setObjectName(QStringLiteral("dashGapLabel"));
    form->addRow(QStringLiteral("Dash Gap"), MakeSliderRow(dashGapSlider_, dashGapLabel_, page));

    autoSaveSlider_ = new QSlider(Qt::Horizontal, page);
    autoSaveSlider_->setObjectName(QStringLiteral("autoSaveSlider"));
    autoSaveSlider_->setRange(0, 300);
    autoSaveSlider_->setSingleStep(15);
    autoSaveSlider_->setPageStep(15);
    autoSaveLabel_ = new QLabel(page);
    autoSaveLabel_->setObjectName(QStringLiteral("autoSaveLabel"));
    form->addRow(QStringLiteral("Auto Save"), MakeSliderRow(autoSaveSlider_, autoSaveLabel_, page));

    autoHideToolbarCheck_ = new QCheckBox(QStringLiteral("Auto-hide toolbar"), page);
    autoHideToolbarCheck_->setObjectName(QStringLiteral("autoHideToolbarCheck"));
    form->addRow(QString(), autoHideToolbarCheck_);

    embedProjectInPdfCheck_ = new QCheckBox(QStringLiteral("Embed project in exported PDFs"), page);
    embedProjectInPdfCheck_->setObjectName(QStringLiteral("embedProjectInPdfCheck"));
    form->addRow(QString(), embedProjectInPdfCheck_);

    auto* zoomRow = new QWidget(page);
    auto* zoomLayout = new QHBoxLayout(zoomRow);
    zoomLayout->setContentsMargins(0, 0, 0, 0);
    zoomLayout->setSpacing(6);
    for (std::size_t index = 0; index < settings::kZoomPresetCount; ++index) {
        zoomPresetEdits_[index] = new QLineEdit(zoomRow);
        zoomPresetEdits_[index]->setObjectName(QStringLiteral("zoomPresetEdit%1").arg(index + 1));
        zoomPresetEdits_[index]->setMaxLength(8);
        zoomPresetEdits_[index]->setFixedWidth(72);
        zoomLayout->addWidget(zoomPresetEdits_[index]);
    }
    zoomLayout->addStretch(1);
    form->addRow(QStringLiteral("Zoom Presets (%)"), zoomRow);

    quickSizeSliderMaxEdit_ = new QLineEdit(page);
    quickSizeSliderMaxEdit_->setObjectName(QStringLiteral("quickSizeSliderMaxEdit"));
    quickSizeSliderMaxEdit_->setMaxLength(8);
    form->addRow(QStringLiteral("Quick Size Max"), quickSizeSliderMaxEdit_);

    brushSizePresetEdit_ = new QLineEdit(page);
    brushSizePresetEdit_->setObjectName(QStringLiteral("brushSizePresetEdit"));
    brushSizePresetEdit_->setMaxLength(96);
    form->addRow(QStringLiteral("Brush Size Presets"), brushSizePresetEdit_);
    auto* textSizeRow = new QWidget(page);
    auto* textSizeLayout = new QHBoxLayout(textSizeRow);
    textSizeLayout->setContentsMargins(0, 0, 0, 0);
    textSizeLayout->setSpacing(6);
    for (std::size_t index = 0; index < settings::kTextSizePresetCount; ++index) {
        textSizePresetEdits_[index] = new QLineEdit(textSizeRow);
        textSizePresetEdits_[index]->setObjectName(QStringLiteral("textSizePresetEdit%1").arg(index + 1));
        textSizePresetEdits_[index]->setMaxLength(8);
        textSizePresetEdits_[index]->setFixedWidth(72);
        textSizeLayout->addWidget(textSizePresetEdits_[index]);
    }
    textSizeLayout->addStretch(1);
    form->addRow(QStringLiteral("Text Sizes (S/M/L)"), textSizeRow);

    categoryTabs_->addTab(page, QStringLiteral("Drawing"));

    connect(stabilizationSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (refreshing_) return;
        settings_.inputSmoothing = static_cast<float>(value) / 100.0f;
        refreshDrawingControls();
        persist();
    });
    connect(spacingSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (refreshing_) return;
        settings_.inputSpacing = static_cast<float>(value) / 100.0f;
        refreshDrawingControls();
        persist();
    });
    connect(strokeSmoothingSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (refreshing_) return;
        settings_.strokeSmoothing = static_cast<float>(value) / 100.0f;
        refreshDrawingControls();
        persist();
    });
    connect(drawCursorSizeSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (refreshing_) return;
        settings_.drawCursorSize = static_cast<float>(value);
        refreshDrawingControls();
        persist();
    });
    connect(dashLengthSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (refreshing_) return;
        settings_.dashLength = static_cast<float>(value) / 10.0f;
        refreshDrawingControls();
        persist();
    });
    connect(dashGapSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (refreshing_) return;
        settings_.dashGap = static_cast<float>(value) / 10.0f;
        refreshDrawingControls();
        persist();
    });
    connect(autoSaveSlider_, &QSlider::valueChanged, this, [this](int) {
        if (refreshing_) return;
        applyAutoSaveSlider();
    });
    connect(autoHideToolbarCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (refreshing_) return;
        settings_.autoHideToolbar = checked;
        persist();
    });
    connect(embedProjectInPdfCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (refreshing_) return;
        settings_.embedProjectInExportedPdfs = checked;
        persist();
    });
    connect(drawCursorKindCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (refreshing_) return;
        settings_.drawCursorKind = index == 1 ? settings::DrawCursorKind::PrecisionCrosshair : settings::DrawCursorKind::Dot;
        refreshDrawingControls();
        persist();
    });
    for (std::size_t index = 0; index < settings::kZoomPresetCount; ++index) {
        connect(zoomPresetEdits_[index], &QLineEdit::editingFinished, this, [this, index] {
            applyZoomPresetEdit(zoomPresetEdits_[index], index, true);
        });
    }
    connect(quickSizeSliderMaxEdit_, &QLineEdit::editingFinished, this, [this] { applyQuickSizeSliderMaxEdit(true); });
    connect(brushSizePresetEdit_, &QLineEdit::editingFinished, this, [this] { applyBrushSizePresetEdit(true); });
    for (std::size_t index = 0; index < settings::kTextSizePresetCount; ++index) {
        connect(textSizePresetEdits_[index], &QLineEdit::editingFinished, this, [this, index] {
            applyTextSizePresetEdit(textSizePresetEdits_[index], index, true);
        });
    }
}

void SettingsDialogQt::buildShortcutsPage() {
    auto* page = new QWidget(categoryTabs_);
    auto* root = new QVBoxLayout(page);

    shortcutList_ = new QListWidget(page);
    shortcutList_->setObjectName(QStringLiteral("shortcutList"));
    root->addWidget(shortcutList_, 1);

    auto* buttons = new QHBoxLayout();
    changeShortcutButton_ = new QPushButton(QStringLiteral("Change Shortcut"), page);
    changeShortcutButton_->setObjectName(QStringLiteral("changeShortcutButton"));
    clearShortcutButton_ = new QPushButton(QStringLiteral("Clear Shortcut"), page);
    clearShortcutButton_->setObjectName(QStringLiteral("clearShortcutButton"));
    buttons->addWidget(changeShortcutButton_);
    buttons->addWidget(clearShortcutButton_);
    buttons->addStretch(1);
    root->addLayout(buttons);

    shortcutHintLabel_ = new QLabel(QStringLiteral("Select a command to edit its binding."), page);
    shortcutHintLabel_->setObjectName(QStringLiteral("shortcutHintLabel"));
    root->addWidget(shortcutHintLabel_);

    categoryTabs_->addTab(page, QStringLiteral("Shortcuts"));

    connect(shortcutList_, &QListWidget::currentRowChanged, this, [this](int) {
        if (refreshing_) {
            return;
        }
        refreshShortcutButtons();
    });
    connect(shortcutList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (const auto action = selectedShortcutAction(); action.has_value()) {
            beginShortcutCapture(*action);
        }
    });
    connect(changeShortcutButton_, &QPushButton::clicked, this, [this] {
        if (const auto action = selectedShortcutAction(); action.has_value()) {
            beginShortcutCapture(*action);
        }
    });
    connect(clearShortcutButton_, &QPushButton::clicked, this, [this] { clearSelectedShortcut(); });
}

// --- model <-> controls ------------------------------------------------------

void SettingsDialogQt::refreshFromSettings() {
    refreshing_ = true;
    refreshPaletteControls(true);
    refreshSavedPalettes();
    refreshSavedPaletteButtons();
    refreshDrawingControls();
    refreshShortcutList();
    refreshShortcutButtons();
    refreshing_ = false;
}

void SettingsDialogQt::refreshPaletteControls(bool syncSliders) {
    const QSignalBlocker blockList(paletteList_);
    paletteList_->clear();
    for (std::size_t index = 0; index < settings_.palette.size(); ++index) {
        auto* item = new QListWidgetItem(
            ColorSwatchIcon(settings_.palette[index]),
            FormatPaletteEntry(index, settings_.palette[index]));
        paletteList_->addItem(item);
    }
    const std::size_t selection = selectedPaletteIndex();
    if (!settings_.palette.empty()) {
        paletteList_->setCurrentRow(static_cast<int>(selection));
    }
    addColorButton_->setEnabled(settings_.palette.size() < settings::kMaxPaletteColors);
    removeColorButton_->setEnabled(settings_.palette.size() > 1U);

    if (settings_.palette.empty()) {
        hexColorEdit_->clear();
        return;
    }

    const HslColor hsl = RgbToHsl(settings_.palette[selection]);
    const int hue = static_cast<int>(std::lround(hsl.hue));
    const int saturation = static_cast<int>(std::lround(hsl.saturation * 100.0f));
    const int lightness = static_cast<int>(std::lround(hsl.lightness * 100.0f));
    if (syncSliders) {
        const QSignalBlocker blockHue(hueSlider_);
        const QSignalBlocker blockSaturation(saturationSlider_);
        const QSignalBlocker blockLightness(lightnessSlider_);
        hueSlider_->setValue(hue);
        saturationSlider_->setValue(saturation);
        lightnessSlider_->setValue(lightness);
    }
    hueLabel_->setText(FormatHueLabel(hue));
    saturationLabel_->setText(FormatPercentLabel(QStringLiteral("Saturation"), saturation));
    lightnessLabel_->setText(FormatPercentLabel(QStringLiteral("Lightness"), lightness));
    if (!hexColorEdit_->hasFocus()) {
        hexColorEdit_->setText(FormatHexColor(settings_.palette[selection]));
    }
}

void SettingsDialogQt::refreshSavedPalettes() {
    const QSignalBlocker block(savedPaletteList_);
    const int previousSelection = savedPaletteList_->currentRow();
    savedPaletteList_->clear();
    for (const settings::NamedPalette& palette : settings_.savedPalettes) {
        savedPaletteList_->addItem(QString::fromUtf16(
            palette.name.data(),
            static_cast<qsizetype>(palette.name.size())));
    }
    if (!settings_.savedPalettes.empty()) {
        const int clamped = previousSelection >= 0
            ? std::clamp(previousSelection, 0, static_cast<int>(settings_.savedPalettes.size()) - 1)
            : 0;
        savedPaletteList_->setCurrentRow(clamped);
    }
}

void SettingsDialogQt::refreshSavedPaletteButtons() {
    const bool hasName = !savedPaletteNameEdit_->text().trimmed().isEmpty();
    const bool hasSelection = selectedSavedPaletteIndex().has_value();
    savePaletteButton_->setEnabled(hasName && !settings_.palette.empty());
    applyPaletteButton_->setEnabled(hasSelection);
    deletePaletteButton_->setEnabled(hasSelection);
}

void SettingsDialogQt::refreshDrawingControls() {
    const QSignalBlocker blockStabilization(stabilizationSlider_);
    const QSignalBlocker blockSpacing(spacingSlider_);
    const QSignalBlocker blockStrokeSmoothing(strokeSmoothingSlider_);
    const QSignalBlocker blockCursorSize(drawCursorSizeSlider_);
    const QSignalBlocker blockDashLength(dashLengthSlider_);
    const QSignalBlocker blockDashGap(dashGapSlider_);
    const QSignalBlocker blockAutoSave(autoSaveSlider_);
    const QSignalBlocker blockCursorKind(drawCursorKindCombo_);
    const QSignalBlocker blockAutoHide(autoHideToolbarCheck_);
    const QSignalBlocker blockEmbed(embedProjectInPdfCheck_);

    stabilizationSlider_->setValue(static_cast<int>(std::lround(settings_.inputSmoothing * 100.0f)));
    spacingSlider_->setValue(static_cast<int>(std::lround(settings_.inputSpacing * 100.0f)));
    strokeSmoothingSlider_->setValue(static_cast<int>(std::lround(settings_.strokeSmoothing * 100.0f)));
    drawCursorSizeSlider_->setValue(static_cast<int>(std::lround(settings_.drawCursorSize)));
    dashLengthSlider_->setValue(static_cast<int>(std::lround(settings_.dashLength * 10.0f)));
    dashGapSlider_->setValue(static_cast<int>(std::lround(settings_.dashGap * 10.0f)));
    autoSaveSlider_->setValue(static_cast<int>(settings_.autoSaveIntervalSeconds));
    drawCursorKindCombo_->setCurrentIndex(settings_.drawCursorKind == settings::DrawCursorKind::PrecisionCrosshair ? 1 : 0);
    autoHideToolbarCheck_->setChecked(settings_.autoHideToolbar);
    embedProjectInPdfCheck_->setChecked(settings_.embedProjectInExportedPdfs);

    stabilizationLabel_->setText(FormatFloatSliderLabel(QStringLiteral("Stabilization"), settings_.inputSmoothing, 100));
    spacingLabel_->setText(FormatSpacingLabel(settings_.inputSpacing));
    strokeSmoothingLabel_->setText(FormatFloatSliderLabel(QStringLiteral("Stroke Smoothing"), settings_.strokeSmoothing, 100));
    drawCursorSizeLabel_->setText(FormatPixelLabel(QStringLiteral("Draw Cursor Size"), settings_.drawCursorSize));
    dashLengthLabel_->setText(FormatDashLabel(QStringLiteral("Dash Length"), settings_.dashLength));
    dashGapLabel_->setText(FormatDashLabel(QStringLiteral("Dash Gap"), settings_.dashGap));
    autoSaveLabel_->setText(FormatAutoSaveLabel(settings_.autoSaveIntervalSeconds));

    const bool dotCursor = settings_.drawCursorKind == settings::DrawCursorKind::Dot;
    drawCursorSizeSlider_->setEnabled(dotCursor);
    drawCursorSizeLabel_->setEnabled(dotCursor);

    for (std::size_t index = 0; index < settings::kZoomPresetCount; ++index) {
        const QSignalBlocker blockEdit(zoomPresetEdits_[index]);
        zoomPresetEdits_[index]->setText(FormatZoomPresetValue(settings_.zoomPresets[index]));
    }
    if (!quickSizeSliderMaxEdit_->hasFocus()) {
        quickSizeSliderMaxEdit_->setText(QString::fromStdString(settings::FormatBrushSizeValue(settings_.quickSizeSliderMax)));
    }
    if (!brushSizePresetEdit_->hasFocus()) {
        brushSizePresetEdit_->setText(QString::fromStdString(settings::FormatBrushSizePresets(settings_.brushSizePresets)));
    }
    for (std::size_t index = 0; index < settings::kTextSizePresetCount; ++index) {
        const QSignalBlocker blockEdit(textSizePresetEdits_[index]);
        textSizePresetEdits_[index]->setText(
            QString::fromStdString(settings::FormatBrushSizeValue(settings_.textSizePresets[index])));
    }
}

void SettingsDialogQt::refreshShortcutList() {
    const QSignalBlocker block(shortcutList_);
    const int previousSelection = shortcutList_->currentRow();
    shortcutList_->clear();
    for (const settings::ShortcutDefinition& definition : settings::ShortcutDefinitions()) {
        const settings::ShortcutBinding* binding = bindingFor(definition.action);
        const QString bindingText = binding != nullptr ? BindingLabel(binding->sequence) : QStringLiteral("Unbound");
        shortcutList_->addItem(QStringLiteral("%1  -  %2")
                                   .arg(QString::fromUtf8(definition.label), bindingText));
    }
    if (shortcutList_->count() > 0) {
        shortcutList_->setCurrentRow(previousSelection >= 0 ? previousSelection : 0);
    }
}

void SettingsDialogQt::refreshShortcutButtons() {
    const bool hasSelection = selectedShortcutAction().has_value();
    changeShortcutButton_->setEnabled(hasSelection);
    clearShortcutButton_->setEnabled(hasSelection);
    notifyShortcutHint();
}

void SettingsDialogQt::notifyShortcutHint() {
    if (capturingAction_.has_value()) {
        changeShortcutButton_->setText(QStringLiteral("Press Key..."));
        shortcutHintLabel_->setText(QStringLiteral("Press the new shortcut. Esc cancels."));
    } else {
        changeShortcutButton_->setText(QStringLiteral("Change Shortcut"));
        shortcutHintLabel_->setText(QStringLiteral("Select a command to edit its binding."));
    }
}

// --- edits -------------------------------------------------------------------

void SettingsDialogQt::persist() {
    lastSaveSucceeded_ = store_.Save(settings_);
    if (lastSaveSucceeded_) {
        lastSaveError_.clear();
    } else {
        lastSaveError_ = QStringLiteral("Could not write %1")
                             .arg(QString::fromStdString(store_.path().string()));
        emit settingsSaveFailed(lastSaveError_);
    }
    emit settingsChanged();
}

std::size_t SettingsDialogQt::selectedPaletteIndex() const {
    const int row = paletteList_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= settings_.palette.size()) {
        return std::min(settings_.paletteIndex, settings_.palette.empty() ? 0U : settings_.palette.size() - 1U);
    }
    return static_cast<std::size_t>(row);
}

std::optional<std::size_t> SettingsDialogQt::selectedSavedPaletteIndex() const {
    const int row = savedPaletteList_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= settings_.savedPalettes.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(row);
}

std::optional<settings::ShortcutAction> SettingsDialogQt::selectedShortcutAction() const {
    const int row = shortcutList_->currentRow();
    const auto definitions = settings::ShortcutDefinitions();
    if (row < 0 || static_cast<std::size_t>(row) >= definitions.size()) {
        return std::nullopt;
    }
    return definitions[static_cast<std::size_t>(row)].action;
}

settings::ShortcutBinding* SettingsDialogQt::bindingFor(settings::ShortcutAction action) {
    for (settings::ShortcutBinding& binding : settings_.shortcuts) {
        if (binding.action == action) {
            return &binding;
        }
    }
    return nullptr;
}

void SettingsDialogQt::applyPaletteSelection() {
    const int row = paletteList_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= settings_.palette.size()) {
        return;
    }
    if (settings_.paletteIndex == static_cast<std::size_t>(row)) {
        refreshPaletteControls(true);
        return;
    }
    settings_.paletteIndex = static_cast<std::size_t>(row);
    refreshPaletteControls(true);
    persist();
}

void SettingsDialogQt::addPaletteColor() {
    if (settings_.palette.empty() || settings_.palette.size() >= settings::kMaxPaletteColors) {
        return;
    }
    const std::size_t source = std::min(settings_.paletteIndex, settings_.palette.size() - 1U);
    settings_.palette.push_back(settings_.palette[source]);
    settings_.paletteIndex = settings_.palette.size() - 1U;
    settings_.colorIndex = std::min(settings_.colorIndex, settings_.palette.size() - 1U);
    refreshPaletteControls(true);
    persist();
}

void SettingsDialogQt::removePaletteColor() {
    if (settings_.palette.size() <= 1U || settings_.paletteIndex >= settings_.palette.size()) {
        return;
    }
    settings_.palette.erase(settings_.palette.begin() + static_cast<std::ptrdiff_t>(settings_.paletteIndex));
    if (settings_.paletteIndex >= settings_.palette.size()) {
        settings_.paletteIndex = settings_.palette.size() - 1U;
    }
    settings_.colorIndex = std::min(settings_.colorIndex, settings_.palette.size() - 1U);
    refreshPaletteControls(true);
    persist();
}

void SettingsDialogQt::applyHexColorEdit() {
    if (settings_.palette.empty() || settings_.paletteIndex >= settings_.palette.size()) {
        return;
    }
    const auto parsed = ParseHexColor(hexColorEdit_->text());
    if (!parsed.has_value()) {
        refreshPaletteControls(true);
        return;
    }
    if (settings_.palette[settings_.paletteIndex] == *parsed) {
        return;
    }
    settings_.palette[settings_.paletteIndex] = *parsed;
    refreshPaletteControls(true);
    persist();
}

void SettingsDialogQt::applyHueSaturationLightness() {
    if (settings_.palette.empty() || settings_.paletteIndex >= settings_.palette.size()) {
        return;
    }
    const float hue = static_cast<float>(hueSlider_->value());
    const float saturation = static_cast<float>(saturationSlider_->value()) / 100.0f;
    const float lightness = static_cast<float>(lightnessSlider_->value()) / 100.0f;
    const std::uint32_t color = HslToColor(hue, saturation, lightness);
    settings_.palette[settings_.paletteIndex] = color;

    if (QListWidgetItem* item = paletteList_->item(static_cast<int>(settings_.paletteIndex)); item != nullptr) {
        item->setIcon(ColorSwatchIcon(color));
        item->setText(FormatPaletteEntry(settings_.paletteIndex, color));
    }
    const HslColor hsl = RgbToHsl(color);
    const int hslHue = static_cast<int>(std::lround(hsl.hue));
    const int hslSaturation = static_cast<int>(std::lround(hsl.saturation * 100.0f));
    const int hslLightness = static_cast<int>(std::lround(hsl.lightness * 100.0f));
    hueLabel_->setText(FormatHueLabel(hslHue));
    saturationLabel_->setText(FormatPercentLabel(QStringLiteral("Saturation"), hslSaturation));
    lightnessLabel_->setText(FormatPercentLabel(QStringLiteral("Lightness"), hslLightness));
    if (!hexColorEdit_->hasFocus()) {
        hexColorEdit_->setText(FormatHexColor(color));
    }
    persist();
}

void SettingsDialogQt::saveNamedPalette() {
    const QString trimmedName = savedPaletteNameEdit_->text().trimmed();
    if (trimmedName.isEmpty() || settings_.palette.empty()) {
        return;
    }

    const std::u16string name = trimmedName.toStdU16String();
    auto existing = std::find_if(
        settings_.savedPalettes.begin(),
        settings_.savedPalettes.end(),
        [&](const settings::NamedPalette& palette) {
            return QString::fromUtf16(palette.name.data(), static_cast<qsizetype>(palette.name.size()))
                       .compare(trimmedName, Qt::CaseInsensitive)
                == 0;
        });
    if (existing != settings_.savedPalettes.end()) {
        existing->name = name;
        existing->colors = settings_.palette;
    } else {
        settings_.savedPalettes.push_back({ name, settings_.palette });
    }
    refreshSavedPalettes();
    refreshSavedPaletteButtons();
    persist();
}

void SettingsDialogQt::applySelectedNamedPalette() {
    const auto selected = selectedSavedPaletteIndex();
    if (!selected.has_value() || settings_.savedPalettes[*selected].colors.empty()) {
        return;
    }
    settings_.palette = settings_.savedPalettes[*selected].colors;
    settings_.paletteIndex = std::min(settings_.paletteIndex, settings_.palette.size() - 1U);
    settings_.colorIndex = std::min(settings_.colorIndex, settings_.palette.size() - 1U);
    refreshPaletteControls(true);
    refreshSavedPaletteButtons();
    persist();
}

void SettingsDialogQt::deleteSelectedNamedPalette() {
    const auto selected = selectedSavedPaletteIndex();
    if (!selected.has_value()) {
        return;
    }
    settings_.savedPalettes.erase(settings_.savedPalettes.begin() + static_cast<std::ptrdiff_t>(*selected));
    refreshSavedPalettes();
    refreshSavedPaletteButtons();
    persist();
}

void SettingsDialogQt::applyZoomPresetEdit(QLineEdit* edit, std::size_t index, bool normalizeText) {
    if (edit == nullptr || index >= settings_.zoomPresets.size()) {
        return;
    }
    const QString text = edit->text();
    if (text.trimmed().isEmpty()) {
        settings_.zoomPresets[index] = 0.0f;
        persist();
        if (normalizeText) {
            const QSignalBlocker block(edit);
            edit->setText(FormatZoomPresetValue(0.0f));
        }
        return;
    }

    const auto parsedPercent = ParseZoomPresetPercent(text);
    if (!parsedPercent.has_value()) {
        if (normalizeText) {
            const QSignalBlocker block(edit);
            edit->setText(FormatZoomPresetValue(settings_.zoomPresets[index]));
        }
        return;
    }

    settings_.zoomPresets[index] = std::clamp(*parsedPercent / 100.0f, settings::kMinZoom, settings::kMaxZoom);
    persist();
    if (normalizeText) {
        const QSignalBlocker block(edit);
        edit->setText(FormatZoomPresetValue(settings_.zoomPresets[index]));
    }
}

void SettingsDialogQt::applyQuickSizeSliderMaxEdit(bool normalizeText) {
    const auto parsed = ParseStrictFloat(quickSizeSliderMaxEdit_->text().trimmed());
    if (!parsed.has_value()) {
        if (normalizeText) {
            const QSignalBlocker block(quickSizeSliderMaxEdit_);
            quickSizeSliderMaxEdit_->setText(QString::fromStdString(settings::FormatBrushSizeValue(settings_.quickSizeSliderMax)));
        }
        return;
    }

    settings_.quickSizeSliderMax = std::clamp(*parsed, 1.0f, settings::kMaxBrushSize);
    settings_.drawBrushSize = std::min(settings_.drawBrushSize, settings_.quickSizeSliderMax);
    settings_.eraseBrushSize = std::min(settings_.eraseBrushSize, settings_.quickSizeSliderMax);
    persist();
    if (normalizeText) {
        const QSignalBlocker block(quickSizeSliderMaxEdit_);
        quickSizeSliderMaxEdit_->setText(QString::fromStdString(settings::FormatBrushSizeValue(settings_.quickSizeSliderMax)));
    }
}

void SettingsDialogQt::applyBrushSizePresetEdit(bool normalizeText) {
    std::vector<float> presets;
    if (!settings::TryParseBrushSizePresets(brushSizePresetEdit_->text().toStdString(), presets)) {
        if (normalizeText) {
            const QSignalBlocker block(brushSizePresetEdit_);
            brushSizePresetEdit_->setText(QString::fromStdString(settings::FormatBrushSizePresets(settings_.brushSizePresets)));
        }
        return;
    }

    settings_.brushSizePresets = std::move(presets);
    persist();
    if (normalizeText) {
        const QSignalBlocker block(brushSizePresetEdit_);
        brushSizePresetEdit_->setText(QString::fromStdString(settings::FormatBrushSizePresets(settings_.brushSizePresets)));
    }
}

void SettingsDialogQt::applyTextSizePresetEdit(QLineEdit* edit, std::size_t index, bool normalizeText) {
    if (edit == nullptr || index >= settings_.textSizePresets.size()) {
        return;
    }
    const auto parsed = ParseStrictFloat(edit->text().trimmed());
    if (!parsed.has_value()) {
        if (normalizeText) {
            const QSignalBlocker block(edit);
            edit->setText(QString::fromStdString(settings::FormatBrushSizeValue(settings_.textSizePresets[index])));
        }
        return;
    }

    settings_.textSizePresets[index] = std::clamp(*parsed, 4.0f, 512.0f);
    persist();
    if (normalizeText) {
        const QSignalBlocker block(edit);
        edit->setText(QString::fromStdString(settings::FormatBrushSizeValue(settings_.textSizePresets[index])));
    }
}


void SettingsDialogQt::applyAutoSaveSlider() {
    const int value = autoSaveSlider_->value();
    const unsigned int seconds = static_cast<unsigned int>(
        std::clamp(std::lround(static_cast<float>(value) / 15.0f) * 15, 0L, 300L));
    settings_.autoSaveIntervalSeconds = seconds;
    if (autoSaveSlider_->value() != static_cast<int>(seconds)) {
        const QSignalBlocker block(autoSaveSlider_);
        autoSaveSlider_->setValue(static_cast<int>(seconds));
    }
    autoSaveLabel_->setText(FormatAutoSaveLabel(seconds));
    persist();
}

// --- shortcuts ---------------------------------------------------------------

void SettingsDialogQt::beginShortcutCapture(settings::ShortcutAction action) {
    capturingAction_ = action;
    qApp->installEventFilter(this);
    notifyShortcutHint();
    setFocus(Qt::OtherFocusReason);
}

void SettingsDialogQt::cancelShortcutCapture() {
    if (!capturingAction_.has_value()) {
        return;
    }
    capturingAction_.reset();
    qApp->removeEventFilter(this);
    notifyShortcutHint();
}

void SettingsDialogQt::clearSelectedShortcut() {
    const auto action = selectedShortcutAction();
    if (!action.has_value()) {
        return;
    }
    if (settings::ShortcutBinding* binding = bindingFor(*action); binding != nullptr) {
        binding->sequence = QKeySequence {};
    }
    refreshShortcutList();
    refreshShortcutButtons();
    persist();
}

void SettingsDialogQt::keyPressEvent(QKeyEvent* event) {
    if (!capturingAction_.has_value()) {
        QDialog::keyPressEvent(event);
        return;
    }

    if (event->isAutoRepeat()) {
        return;
    }

    const int key = event->key();
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
        || key == Qt::Key_unknown) {
        return;
    }
    if (key == Qt::Key_Escape) {
        cancelShortcutCapture();
        return;
    }

    const Qt::KeyboardModifiers modifiers = event->modifiers()
        & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    const QKeySequence sequence(key | static_cast<int>(modifiers));
    if (sequence.isEmpty()) {
        return;
    }

    // The legacy capture clears any other command already using the same chord.
    for (settings::ShortcutBinding& binding : settings_.shortcuts) {
        if (binding.action != *capturingAction_ && binding.sequence == sequence) {
            binding.sequence = QKeySequence {};
        }
    }
    if (settings::ShortcutBinding* binding = bindingFor(*capturingAction_); binding != nullptr) {
        binding->sequence = sequence;
    }

    capturingAction_.reset();
    qApp->removeEventFilter(this);
    refreshShortcutList();
    refreshShortcutButtons();
    persist();
}

bool SettingsDialogQt::eventFilter(QObject* watched, QEvent* event) {
    if (capturingAction_.has_value() && event->type() == QEvent::KeyPress) {
        keyPressEvent(static_cast<QKeyEvent*>(event));
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsDialogQt::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    refreshFromSettings();
}

}  // namespace cnote::ui
