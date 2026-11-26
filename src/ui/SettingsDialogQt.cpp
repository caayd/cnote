#include "ui/SettingsDialogQt.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/SettingsFormatQt.h"
#include "ui/WindowBackdropQt.h"

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

settings::ShortcutBinding* FindBindingIn(settings::Settings& values, settings::ShortcutAction action) {
    for (settings::ShortcutBinding& binding : values.shortcuts) {
        if (binding.action == action) {
            return &binding;
        }
    }
    return nullptr;
}

// Central validation for a staged edit, mirroring the ranges ParseText accepts.
// The dialog never invents a different policy for values the file format would
// clamp anyway.
void ClampSettings(settings::Settings& values) {
    if (values.palette.empty()) {
        values.palette = settings::DefaultPalette();
    }
    if (values.palette.size() > settings::kMaxPaletteColors) {
        values.palette.resize(settings::kMaxPaletteColors);
    }
    values.paletteIndex = std::min(values.paletteIndex, values.palette.size() - 1U);
    values.colorIndex = std::min(values.colorIndex, values.palette.size() - 1U);

    const auto presets = DimensionPresets();
    values.defaultPageSizePreset = std::min(values.defaultPageSizePreset, presets.size() - 1U);

    values.quickSizeSliderMax = std::clamp(values.quickSizeSliderMax, 1.0f, settings::kMaxBrushSize);
    values.drawBrushSize = std::clamp(values.drawBrushSize, 0.5f, values.quickSizeSliderMax);
    values.eraseBrushSize = std::clamp(values.eraseBrushSize, 0.5f, values.quickSizeSliderMax);
    values.inputSmoothing = std::clamp(values.inputSmoothing, 0.05f, 0.95f);
    values.inputSpacing = std::clamp(values.inputSpacing, 0.05f, 4.05f);
    values.strokeSmoothing = std::clamp(values.strokeSmoothing, 0.0f, 1.0f);
    values.dashLength = std::clamp(values.dashLength, 0.5f, 8.0f);
    values.dashGap = std::clamp(values.dashGap, 0.5f, 8.0f);
    values.drawCursorSize = std::clamp(values.drawCursorSize, 4.0f, 24.0f);
    values.autoSaveIntervalSeconds = std::min(values.autoSaveIntervalSeconds, 300U);
    for (float& zoom : values.zoomPresets) {
        const std::optional<float> decoded = DecodeZoomPreset(zoom);
        zoom = decoded.has_value()
            ? std::clamp(*decoded, settings::kMinZoom, settings::kMaxZoom)
            : EncodeZoomPreset(std::nullopt);
    }
    for (float& size : values.textSizePresets) {
        size = std::clamp(size, 4.0f, 512.0f);
    }
    values.textSizePreset = std::min(values.textSizePreset, settings::kTextSizePresetCount - 1U);
}

// Settings pages are a column of titled groups.  Every form label gets the same
// minimum width so the fields of all groups on a page line up.
constexpr int kFormLabelWidth = 140;
constexpr int kValueLabelWidth = 64;
constexpr int kNumberEditWidth = 72;

QWidget* MakeSliderRow(QSlider* slider, QLabel* label, int labelWidth, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    layout->addWidget(slider, 1);
    label->setMinimumWidth(labelWidth);
    layout->addWidget(label);
    return row;
}

QWidget* MakeNumberRow(QLineEdit* const* edits, std::size_t count, const char* namePrefix, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    for (std::size_t index = 0; index < count; ++index) {
        edits[index]->setParent(row);
        edits[index]->setObjectName(QStringLiteral("%1%2").arg(QString::fromUtf8(namePrefix)).arg(index + 1));
        edits[index]->setMaxLength(8);
        edits[index]->setFixedWidth(kNumberEditWidth);
        layout->addWidget(edits[index]);
    }
    layout->addStretch(1);
    return row;
}

QVBoxLayout* AddPage(QTabWidget* tabs, const QString& title, QWidget*& page) {
    page = new QWidget(tabs);
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(12);
    tabs->addTab(page, title);
    return layout;
}

QGroupBox* AddGroup(QVBoxLayout* pageLayout, const QString& title, QWidget* page) {
    auto* group = new QGroupBox(title, page);
    pageLayout->addWidget(group);
    return group;
}

QFormLayout* AddFormGroup(QVBoxLayout* pageLayout, const QString& title, QWidget* page) {
    auto* form = new QFormLayout(AddGroup(pageLayout, title, page));
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setHorizontalSpacing(16);
    return form;
}

void AddFormRow(QFormLayout* form, const QString& text, QWidget* field) {
    auto* label = new QLabel(text, form->parentWidget());
    label->setMinimumWidth(kFormLabelWidth);
    form->addRow(label, field);
}

QLabel* MakeValueLabel(const char* name, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setObjectName(QString::fromUtf8(name));
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

QSlider* MakeSlider(const char* name, int minimum, int maximum, QWidget* parent) {
    auto* slider = new QSlider(Qt::Horizontal, parent);
    slider->setObjectName(QString::fromUtf8(name));
    slider->setRange(minimum, maximum);
    return slider;
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
    captureState_ = {};
    if (QCoreApplication::instance() != nullptr) {
        qApp->removeEventFilter(this);
    }
}

void SettingsDialogQt::buildUi() {
    setMinimumWidth(620);
    auto* root = new QVBoxLayout(this);
    categoryTabs_ = new QTabWidget(this);
    categoryTabs_->setObjectName(QStringLiteral("categoryTabs"));
    root->addWidget(categoryTabs_);

    buildPalettePage();
    buildDrawingPage();
    buildSizesPage();
    buildViewPage();
    buildDocumentsPage();
    buildShortcutsPage();
}

void SettingsDialogQt::buildPalettePage() {
    QWidget* page = nullptr;
    QVBoxLayout* pageLayout = AddPage(categoryTabs_, QStringLiteral("Palette"), page);
    auto* columns = new QHBoxLayout();
    columns->setSpacing(12);
    pageLayout->addLayout(columns, 1);

    auto* colorsGroup = new QGroupBox(QStringLiteral("Colors"), page);
    auto* colorsLayout = new QVBoxLayout(colorsGroup);
    paletteList_ = new QListWidget(colorsGroup);
    paletteList_->setObjectName(QStringLiteral("paletteList"));
    paletteList_->setMinimumWidth(220);
    colorsLayout->addWidget(paletteList_, 1);
    auto* colorButtons = new QHBoxLayout();
    addColorButton_ = new QPushButton(QStringLiteral("Add Color"), colorsGroup);
    addColorButton_->setObjectName(QStringLiteral("addColorButton"));
    removeColorButton_ = new QPushButton(QStringLiteral("Remove Color"), colorsGroup);
    removeColorButton_->setObjectName(QStringLiteral("removeColorButton"));
    colorButtons->addWidget(addColorButton_);
    colorButtons->addWidget(removeColorButton_);
    colorButtons->addStretch(1);
    colorsLayout->addLayout(colorButtons);
    columns->addWidget(colorsGroup, 1);

    auto* rightColumn = new QVBoxLayout();
    rightColumn->setSpacing(12);
    columns->addLayout(rightColumn, 1);

    auto* editGroup = new QGroupBox(QStringLiteral("Edit Color"), page);
    auto* editLayout = new QVBoxLayout(editGroup);
    auto* hexRow = new QHBoxLayout();
    hexRow->addWidget(new QLabel(QStringLiteral("Hex"), editGroup));
    hexColorEdit_ = new QLineEdit(editGroup);
    hexColorEdit_->setObjectName(QStringLiteral("hexColorEdit"));
    hexColorEdit_->setMaxLength(10);
    hexRow->addWidget(hexColorEdit_, 1);
    editLayout->addLayout(hexRow);
    // These readouts name their slider ("Hue: 120 deg"), so they need the room.
    constexpr int kHslLabelWidth = 110;
    hueSlider_ = MakeSlider("hueSlider", 0, 360, editGroup);
    hueLabel_ = MakeValueLabel("hueLabel", editGroup);
    editLayout->addWidget(MakeSliderRow(hueSlider_, hueLabel_, kHslLabelWidth, editGroup));
    saturationSlider_ = MakeSlider("saturationSlider", 0, 100, editGroup);
    saturationLabel_ = MakeValueLabel("saturationLabel", editGroup);
    editLayout->addWidget(MakeSliderRow(saturationSlider_, saturationLabel_, kHslLabelWidth, editGroup));
    lightnessSlider_ = MakeSlider("lightnessSlider", 0, 100, editGroup);
    lightnessLabel_ = MakeValueLabel("lightnessLabel", editGroup);
    editLayout->addWidget(MakeSliderRow(lightnessSlider_, lightnessLabel_, kHslLabelWidth, editGroup));
    rightColumn->addWidget(editGroup);

    auto* savedGroup = new QGroupBox(QStringLiteral("Saved Palettes"), page);
    auto* savedLayout = new QVBoxLayout(savedGroup);
    auto* saveRow = new QHBoxLayout();
    savedPaletteNameEdit_ = new QLineEdit(savedGroup);
    savedPaletteNameEdit_->setObjectName(QStringLiteral("savedPaletteNameEdit"));
    savedPaletteNameEdit_->setMaxLength(48);
    savedPaletteNameEdit_->setPlaceholderText(QStringLiteral("Palette name"));
    saveRow->addWidget(savedPaletteNameEdit_, 1);
    savePaletteButton_ = new QPushButton(QStringLiteral("Save"), savedGroup);
    savePaletteButton_->setObjectName(QStringLiteral("savePaletteButton"));
    saveRow->addWidget(savePaletteButton_);
    savedLayout->addLayout(saveRow);
    savedPaletteList_ = new QListWidget(savedGroup);
    savedPaletteList_->setObjectName(QStringLiteral("savedPaletteList"));
    savedPaletteList_->setMinimumHeight(120);
    savedLayout->addWidget(savedPaletteList_, 1);
    auto* savedButtons = new QHBoxLayout();
    savedButtons->addStretch(1);
    applyPaletteButton_ = new QPushButton(QStringLiteral("Apply"), savedGroup);
    applyPaletteButton_->setObjectName(QStringLiteral("applyPaletteButton"));
    deletePaletteButton_ = new QPushButton(QStringLiteral("Delete"), savedGroup);
    deletePaletteButton_->setObjectName(QStringLiteral("deletePaletteButton"));
    savedButtons->addWidget(applyPaletteButton_);
    savedButtons->addWidget(deletePaletteButton_);
    savedLayout->addLayout(savedButtons);
    rightColumn->addWidget(savedGroup, 1);

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

// Connects a slider whose value is `field * scale` in the settings model.
void SettingsDialogQt::connectScaledSlider(QSlider* slider, float settings::Settings::*field, float scale) {
    connect(slider, &QSlider::valueChanged, this, [this, field, scale](int value) {
        if (refreshing_) return;
        settings::Settings candidate = settings_;
        candidate.*field = static_cast<float>(value) / scale;
        persist(std::move(candidate));
        refreshOptionControls();
    });
}

void SettingsDialogQt::connectCheck(QCheckBox* check, bool settings::Settings::*field) {
    connect(check, &QCheckBox::toggled, this, [this, field](bool checked) {
        if (refreshing_) return;
        settings::Settings candidate = settings_;
        candidate.*field = checked;
        persist(std::move(candidate));
        refreshOptionControls();
    });
}

void SettingsDialogQt::buildDrawingPage() {
    QWidget* page = nullptr;
    QVBoxLayout* pageLayout = AddPage(categoryTabs_, QStringLiteral("Drawing"), page);

    QFormLayout* stroke = AddFormGroup(pageLayout, QStringLiteral("Stroke"), page);
    stabilizationSlider_ = MakeSlider("stabilizationSlider", 5, 95, page);
    stabilizationLabel_ = MakeValueLabel("stabilizationLabel", page);
    AddFormRow(stroke, QStringLiteral("Stabilization"), MakeSliderRow(stabilizationSlider_, stabilizationLabel_, kValueLabelWidth, page));
    spacingSlider_ = MakeSlider("spacingSlider", 5, 405, page);
    spacingLabel_ = MakeValueLabel("spacingLabel", page);
    AddFormRow(stroke, QStringLiteral("Point Spacing"), MakeSliderRow(spacingSlider_, spacingLabel_, kValueLabelWidth, page));
    strokeSmoothingSlider_ = MakeSlider("strokeSmoothingSlider", 0, 100, page);
    strokeSmoothingLabel_ = MakeValueLabel("strokeSmoothingLabel", page);
    AddFormRow(stroke, QStringLiteral("Stroke Smoothing"), MakeSliderRow(strokeSmoothingSlider_, strokeSmoothingLabel_, kValueLabelWidth, page));

    QFormLayout* dashes = AddFormGroup(pageLayout, QStringLiteral("Dashed Lines"), page);
    dashLengthSlider_ = MakeSlider("dashLengthSlider", 5, 80, page);
    dashLengthLabel_ = MakeValueLabel("dashLengthLabel", page);
    AddFormRow(dashes, QStringLiteral("Dash Length"), MakeSliderRow(dashLengthSlider_, dashLengthLabel_, kValueLabelWidth, page));
    dashGapSlider_ = MakeSlider("dashGapSlider", 5, 80, page);
    dashGapLabel_ = MakeValueLabel("dashGapLabel", page);
    AddFormRow(dashes, QStringLiteral("Dash Gap"), MakeSliderRow(dashGapSlider_, dashGapLabel_, kValueLabelWidth, page));

    QFormLayout* gestures = AddFormGroup(pageLayout, QStringLiteral("Gestures"), page);
    scratchToEraseCheck_ = new QCheckBox(QStringLiteral("Scratch out ink to erase it"), page);
    scratchToEraseCheck_->setObjectName(QStringLiteral("scratchToEraseCheck"));
    gestures->addRow(scratchToEraseCheck_);
    pageLayout->addStretch(1);

    connectScaledSlider(stabilizationSlider_, &settings::Settings::inputSmoothing, 100.0f);
    connectScaledSlider(spacingSlider_, &settings::Settings::inputSpacing, 100.0f);
    connectScaledSlider(strokeSmoothingSlider_, &settings::Settings::strokeSmoothing, 100.0f);
    connectScaledSlider(dashLengthSlider_, &settings::Settings::dashLength, 10.0f);
    connectScaledSlider(dashGapSlider_, &settings::Settings::dashGap, 10.0f);
    connectCheck(scratchToEraseCheck_, &settings::Settings::scratchToErase);
}

void SettingsDialogQt::buildSizesPage() {
    QWidget* page = nullptr;
    QVBoxLayout* pageLayout = AddPage(categoryTabs_, QStringLiteral("Sizes"), page);

    QFormLayout* brush = AddFormGroup(pageLayout, QStringLiteral("Brush"), page);
    quickSizeSliderMaxEdit_ = new QLineEdit(page);
    quickSizeSliderMaxEdit_->setObjectName(QStringLiteral("quickSizeSliderMaxEdit"));
    quickSizeSliderMaxEdit_->setMaxLength(8);
    quickSizeSliderMaxEdit_->setFixedWidth(kNumberEditWidth);
    AddFormRow(brush, QStringLiteral("Quick Size Max"), quickSizeSliderMaxEdit_);
    brushSizePresetEdit_ = new QLineEdit(page);
    brushSizePresetEdit_->setObjectName(QStringLiteral("brushSizePresetEdit"));
    brushSizePresetEdit_->setMaxLength(96);
    AddFormRow(brush, QStringLiteral("Size Presets"), brushSizePresetEdit_);

    QFormLayout* text = AddFormGroup(pageLayout, QStringLiteral("Text"), page);
    for (QLineEdit*& edit : textSizePresetEdits_) {
        edit = new QLineEdit(page);
    }
    AddFormRow(text, QStringLiteral("Small / Medium / Large"),
        MakeNumberRow(textSizePresetEdits_, settings::kTextSizePresetCount, "textSizePresetEdit", page));
    pageLayout->addStretch(1);

    connect(quickSizeSliderMaxEdit_, &QLineEdit::editingFinished, this, [this] { applyQuickSizeSliderMaxEdit(true); });
    connect(brushSizePresetEdit_, &QLineEdit::editingFinished, this, [this] { applyBrushSizePresetEdit(true); });
    for (std::size_t index = 0; index < settings::kTextSizePresetCount; ++index) {
        connect(textSizePresetEdits_[index], &QLineEdit::editingFinished, this, [this, index] {
            applyTextSizePresetEdit(textSizePresetEdits_[index], index, true);
        });
    }
}

void SettingsDialogQt::buildViewPage() {
    QWidget* page = nullptr;
    QVBoxLayout* pageLayout = AddPage(categoryTabs_, QStringLiteral("View"), page);

    QFormLayout* cursor = AddFormGroup(pageLayout, QStringLiteral("Cursor"), page);
    drawCursorKindCombo_ = new QComboBox(page);
    drawCursorKindCombo_->setObjectName(QStringLiteral("drawCursorKindCombo"));
    drawCursorKindCombo_->addItem(QStringLiteral("Dot"));
    drawCursorKindCombo_->addItem(QStringLiteral("Precision Crosshair"));
    AddFormRow(cursor, QStringLiteral("Draw Cursor"), drawCursorKindCombo_);
    drawCursorSizeSlider_ = MakeSlider("drawCursorSizeSlider", 4, 24, page);
    drawCursorSizeLabel_ = MakeValueLabel("drawCursorSizeLabel", page);
    AddFormRow(cursor, QStringLiteral("Dot Size"), MakeSliderRow(drawCursorSizeSlider_, drawCursorSizeLabel_, kValueLabelWidth, page));

    QFormLayout* canvas = AddFormGroup(pageLayout, QStringLiteral("Canvas"), page);
    canvasBackdropCombo_ = new QComboBox(page);
    canvasBackdropCombo_->setObjectName(QStringLiteral("canvasBackdropCombo"));
    canvasBackdropCombo_->setToolTip(QStringLiteral(
        "What shows around the pages. Transparent needs Windows; Acrylic needs Windows 11 22H2 or later."));
    // Item index == settings::CanvasBackdrop value.
    const std::pair<settings::CanvasBackdrop, QString> backdrops[] {
        { settings::CanvasBackdrop::Solid, QStringLiteral("Solid") },
        { settings::CanvasBackdrop::Transparent, QStringLiteral("Transparent") },
        { settings::CanvasBackdrop::Acrylic, QStringLiteral("Acrylic (blurred)") },
    };
    auto* backdropModel = qobject_cast<QStandardItemModel*>(canvasBackdropCombo_->model());
    for (const auto& [backdrop, label] : backdrops) {
        canvasBackdropCombo_->addItem(label);
        backdropModel->item(canvasBackdropCombo_->count() - 1)->setEnabled(WindowBackdropSupported(backdrop));
    }
    AddFormRow(canvas, QStringLiteral("Background"), canvasBackdropCombo_);

    QFormLayout* toolbar = AddFormGroup(pageLayout, QStringLiteral("Toolbar"), page);
    autoHideToolbarCheck_ = new QCheckBox(QStringLiteral("Auto-hide toolbar"), page);
    autoHideToolbarCheck_->setObjectName(QStringLiteral("autoHideToolbarCheck"));
    toolbar->addRow(autoHideToolbarCheck_);

    QFormLayout* zoom = AddFormGroup(pageLayout, QStringLiteral("Zoom"), page);
    for (QLineEdit*& edit : zoomPresetEdits_) {
        edit = new QLineEdit(page);
    }
    AddFormRow(zoom, QStringLiteral("Presets (%)"),
        MakeNumberRow(zoomPresetEdits_, settings::kZoomPresetCount, "zoomPresetEdit", page));
    pageLayout->addStretch(1);

    connect(drawCursorKindCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (refreshing_) return;
        settings::Settings candidate = settings_;
        candidate.drawCursorKind = index == 1 ? settings::DrawCursorKind::PrecisionCrosshair : settings::DrawCursorKind::Dot;
        persist(std::move(candidate));
        refreshOptionControls();
    });
    connect(canvasBackdropCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (refreshing_ || index < 0) return;
        settings::Settings candidate = settings_;
        candidate.canvasBackdrop = static_cast<settings::CanvasBackdrop>(index);
        persist(std::move(candidate));
    });
    connectScaledSlider(drawCursorSizeSlider_, &settings::Settings::drawCursorSize, 1.0f);
    connectCheck(autoHideToolbarCheck_, &settings::Settings::autoHideToolbar);
    for (std::size_t index = 0; index < settings::kZoomPresetCount; ++index) {
        connect(zoomPresetEdits_[index], &QLineEdit::editingFinished, this, [this, index] {
            applyZoomPresetEdit(zoomPresetEdits_[index], index, true);
        });
    }
}

void SettingsDialogQt::buildDocumentsPage() {
    QWidget* page = nullptr;
    QVBoxLayout* pageLayout = AddPage(categoryTabs_, QStringLiteral("Documents"), page);

    QFormLayout* newPages = AddFormGroup(pageLayout, QStringLiteral("New Pages"), page);
    defaultPageSizeCombo_ = new QComboBox(page);
    defaultPageSizeCombo_->setObjectName(QStringLiteral("defaultPageSizeCombo"));
    for (const DimensionPreset& preset : DimensionPresets()) {
        defaultPageSizeCombo_->addItem(QString::fromUtf8(preset.label));
    }
    AddFormRow(newPages, QStringLiteral("Default Page Size"), defaultPageSizeCombo_);

    QFormLayout* saving = AddFormGroup(pageLayout, QStringLiteral("Saving"), page);
    autoSaveSlider_ = MakeSlider("autoSaveSlider", 0, 300, page);
    autoSaveSlider_->setSingleStep(15);
    autoSaveSlider_->setPageStep(15);
    autoSaveLabel_ = MakeValueLabel("autoSaveLabel", page);
    AddFormRow(saving, QStringLiteral("Auto Save"), MakeSliderRow(autoSaveSlider_, autoSaveLabel_, kValueLabelWidth, page));

    QFormLayout* pdf = AddFormGroup(pageLayout, QStringLiteral("PDF Export"), page);
    embedProjectInPdfCheck_ = new QCheckBox(QStringLiteral("Embed project in exported PDFs"), page);
    embedProjectInPdfCheck_->setObjectName(QStringLiteral("embedProjectInPdfCheck"));
    pdf->addRow(embedProjectInPdfCheck_);
    pageLayout->addStretch(1);

    connect(defaultPageSizeCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (refreshing_) return;
        if (index < 0 || static_cast<std::size_t>(index) >= DimensionPresets().size()) {
            refreshOptionControls();
            return;
        }
        settings::Settings candidate = settings_;
        candidate.defaultPageSizePreset = static_cast<std::size_t>(index);
        persist(std::move(candidate));
        refreshOptionControls();
    });
    connect(autoSaveSlider_, &QSlider::valueChanged, this, [this](int) {
        if (refreshing_) return;
        applyAutoSaveSlider();
    });
    connectCheck(embedProjectInPdfCheck_, &settings::Settings::embedProjectInExportedPdfs);
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
    refreshOptionControls();
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
    const std::size_t selection =
        std::min(settings_.paletteIndex, settings_.palette.empty() ? 0U : settings_.palette.size() - 1U);
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

void SettingsDialogQt::refreshOptionControls() {
    const QSignalBlocker blockStabilization(stabilizationSlider_);
    const QSignalBlocker blockSpacing(spacingSlider_);
    const QSignalBlocker blockStrokeSmoothing(strokeSmoothingSlider_);
    const QSignalBlocker blockCursorSize(drawCursorSizeSlider_);
    const QSignalBlocker blockDashLength(dashLengthSlider_);
    const QSignalBlocker blockDashGap(dashGapSlider_);
    const QSignalBlocker blockAutoSave(autoSaveSlider_);
    const QSignalBlocker blockCursorKind(drawCursorKindCombo_);
    const QSignalBlocker blockCanvasBackdrop(canvasBackdropCombo_);
    const QSignalBlocker blockDefaultPageSize(defaultPageSizeCombo_);
    const QSignalBlocker blockAutoHide(autoHideToolbarCheck_);
    const QSignalBlocker blockEmbed(embedProjectInPdfCheck_);
    const QSignalBlocker blockScratchToErase(scratchToEraseCheck_);

    stabilizationSlider_->setValue(static_cast<int>(std::lround(settings_.inputSmoothing * 100.0f)));
    spacingSlider_->setValue(static_cast<int>(std::lround(settings_.inputSpacing * 100.0f)));
    strokeSmoothingSlider_->setValue(static_cast<int>(std::lround(settings_.strokeSmoothing * 100.0f)));
    drawCursorSizeSlider_->setValue(static_cast<int>(std::lround(settings_.drawCursorSize)));
    dashLengthSlider_->setValue(static_cast<int>(std::lround(settings_.dashLength * 10.0f)));
    dashGapSlider_->setValue(static_cast<int>(std::lround(settings_.dashGap * 10.0f)));
    autoSaveSlider_->setValue(static_cast<int>(settings_.autoSaveIntervalSeconds));
    drawCursorKindCombo_->setCurrentIndex(settings_.drawCursorKind == settings::DrawCursorKind::PrecisionCrosshair ? 1 : 0);
    canvasBackdropCombo_->setCurrentIndex(static_cast<int>(settings_.canvasBackdrop));
    defaultPageSizeCombo_->setCurrentIndex(static_cast<int>(
        std::min(settings_.defaultPageSizePreset, DimensionPresets().size() - 1U)));
    autoHideToolbarCheck_->setChecked(settings_.autoHideToolbar);
    embedProjectInPdfCheck_->setChecked(settings_.embedProjectInExportedPdfs);
    scratchToEraseCheck_->setChecked(settings_.scratchToErase);

    stabilizationLabel_->setText(FormatFloatSliderLabel(settings_.inputSmoothing, 100));
    spacingLabel_->setText(FormatSpacingLabel(settings_.inputSpacing));
    strokeSmoothingLabel_->setText(FormatFloatSliderLabel(settings_.strokeSmoothing, 100));
    drawCursorSizeLabel_->setText(FormatPixelLabel(settings_.drawCursorSize));
    dashLengthLabel_->setText(FormatDashLabel(settings_.dashLength));
    dashGapLabel_->setText(FormatDashLabel(settings_.dashGap));
    autoSaveLabel_->setText(FormatAutoSaveLabel(DecodeAutoSaveInterval(settings_.autoSaveIntervalSeconds)));

    const bool dotCursor = settings_.drawCursorKind == settings::DrawCursorKind::Dot;
    drawCursorSizeSlider_->setEnabled(dotCursor);
    drawCursorSizeLabel_->setEnabled(dotCursor);

    for (std::size_t index = 0; index < settings::kZoomPresetCount; ++index) {
        const QSignalBlocker blockEdit(zoomPresetEdits_[index]);
        zoomPresetEdits_[index]->setText(FormatZoomPresetValue(DecodeZoomPreset(settings_.zoomPresets[index])));
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
    if (captureState_.kind == CaptureState::Kind::Armed) {
        changeShortcutButton_->setText(QStringLiteral("Press Key..."));
        shortcutHintLabel_->setText(QStringLiteral("Press the new shortcut. Esc cancels."));
    } else {
        changeShortcutButton_->setText(QStringLiteral("Change Shortcut"));
        shortcutHintLabel_->setText(QStringLiteral("Select a command to edit its binding."));
    }
}

// --- edits -------------------------------------------------------------------

bool SettingsDialogQt::persist(settings::Settings candidate) {
    ClampSettings(candidate);
    lastSaveSucceeded_ = store_.Save(candidate);
    if (lastSaveSucceeded_) {
        settings_ = std::move(candidate);
        lastSaveError_.clear();
        emit settingsChanged();
    } else {
        lastSaveError_ = QStringLiteral("Could not write %1")
                             .arg(QString::fromStdString(store_.path().string()));
        emit settingsSaveFailed(lastSaveError_);
    }
    return lastSaveSucceeded_;
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
    settings::Settings candidate = settings_;
    candidate.paletteIndex = static_cast<std::size_t>(row);
    persist(std::move(candidate));
    refreshPaletteControls(true);
}

void SettingsDialogQt::addPaletteColor() {
    if (settings_.palette.empty() || settings_.palette.size() >= settings::kMaxPaletteColors) {
        return;
    }
    settings::Settings candidate = settings_;
    const std::size_t source = std::min(candidate.paletteIndex, candidate.palette.size() - 1U);
    candidate.palette.push_back(candidate.palette[source]);
    candidate.paletteIndex = candidate.palette.size() - 1U;
    candidate.colorIndex = std::min(candidate.colorIndex, candidate.palette.size() - 1U);
    persist(std::move(candidate));
    refreshPaletteControls(true);
}

void SettingsDialogQt::removePaletteColor() {
    if (settings_.palette.size() <= 1U || settings_.paletteIndex >= settings_.palette.size()) {
        return;
    }
    settings::Settings candidate = settings_;
    candidate.palette.erase(candidate.palette.begin() + static_cast<std::ptrdiff_t>(candidate.paletteIndex));
    if (candidate.paletteIndex >= candidate.palette.size()) {
        candidate.paletteIndex = candidate.palette.size() - 1U;
    }
    candidate.colorIndex = std::min(candidate.colorIndex, candidate.palette.size() - 1U);
    persist(std::move(candidate));
    refreshPaletteControls(true);
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
        refreshPaletteControls(true);
        return;
    }
    settings::Settings candidate = settings_;
    candidate.palette[candidate.paletteIndex] = *parsed;
    persist(std::move(candidate));
    refreshPaletteControls(true);
}

void SettingsDialogQt::applyHueSaturationLightness() {
    if (settings_.palette.empty() || settings_.paletteIndex >= settings_.palette.size()) {
        return;
    }
    const float hue = static_cast<float>(hueSlider_->value());
    const float saturation = static_cast<float>(saturationSlider_->value()) / 100.0f;
    const float lightness = static_cast<float>(lightnessSlider_->value()) / 100.0f;
    const std::uint32_t color = HslToColor(hue, saturation, lightness);
    settings::Settings candidate = settings_;
    candidate.palette[candidate.paletteIndex] = color;
    if (!persist(std::move(candidate))) {
        refreshPaletteControls(true);
        return;
    }

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
}

void SettingsDialogQt::saveNamedPalette() {
    const QString trimmedName = savedPaletteNameEdit_->text().trimmed();
    if (trimmedName.isEmpty() || settings_.palette.empty()) {
        return;
    }

    const std::u16string name = trimmedName.toStdU16String();
    settings::Settings candidate = settings_;
    auto existing = std::find_if(
        candidate.savedPalettes.begin(),
        candidate.savedPalettes.end(),
        [&](const settings::NamedPalette& palette) {
            return QString::fromUtf16(palette.name.data(), static_cast<qsizetype>(palette.name.size()))
                       .compare(trimmedName, Qt::CaseInsensitive)
                == 0;
        });
    if (existing != candidate.savedPalettes.end()) {
        existing->name = name;
        existing->colors = candidate.palette;
    } else {
        candidate.savedPalettes.push_back({ name, candidate.palette });
    }
    persist(std::move(candidate));
    refreshSavedPalettes();
    refreshSavedPaletteButtons();
}

void SettingsDialogQt::applySelectedNamedPalette() {
    const auto selected = selectedSavedPaletteIndex();
    if (!selected.has_value() || settings_.savedPalettes[*selected].colors.empty()) {
        return;
    }
    settings::Settings candidate = settings_;
    candidate.palette = candidate.savedPalettes[*selected].colors;
    candidate.paletteIndex = std::min(candidate.paletteIndex, candidate.palette.size() - 1U);
    candidate.colorIndex = std::min(candidate.colorIndex, candidate.palette.size() - 1U);
    persist(std::move(candidate));
    refreshPaletteControls(true);
    refreshSavedPaletteButtons();
}

void SettingsDialogQt::deleteSelectedNamedPalette() {
    const auto selected = selectedSavedPaletteIndex();
    if (!selected.has_value()) {
        return;
    }
    settings::Settings candidate = settings_;
    candidate.savedPalettes.erase(candidate.savedPalettes.begin() + static_cast<std::ptrdiff_t>(*selected));
    persist(std::move(candidate));
    refreshSavedPalettes();
    refreshSavedPaletteButtons();
}

void SettingsDialogQt::applyZoomPresetEdit(QLineEdit* edit, std::size_t index, bool normalizeText) {
    if (edit == nullptr || index >= settings_.zoomPresets.size()) {
        return;
    }
    const QString text = edit->text();
    if (text.trimmed().isEmpty()) {
        settings::Settings candidate = settings_;
        candidate.zoomPresets[index] = EncodeZoomPreset(std::nullopt);
        persist(std::move(candidate));
        if (normalizeText) {
            const QSignalBlocker block(edit);
            edit->setText(FormatZoomPresetValue(DecodeZoomPreset(settings_.zoomPresets[index])));
        }
        return;
    }

    const auto parsedPercent = ParseZoomPresetPercent(text);
    if (!parsedPercent.has_value()) {
        if (normalizeText) {
            const QSignalBlocker block(edit);
            edit->setText(FormatZoomPresetValue(DecodeZoomPreset(settings_.zoomPresets[index])));
        }
        return;
    }

    settings::Settings candidate = settings_;
    candidate.zoomPresets[index] = std::clamp(*parsedPercent / 100.0f, settings::kMinZoom, settings::kMaxZoom);
    persist(std::move(candidate));
    if (normalizeText) {
        const QSignalBlocker block(edit);
        edit->setText(FormatZoomPresetValue(DecodeZoomPreset(settings_.zoomPresets[index])));
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

    settings::Settings candidate = settings_;
    candidate.quickSizeSliderMax = std::clamp(*parsed, 1.0f, settings::kMaxBrushSize);
    candidate.drawBrushSize = std::min(candidate.drawBrushSize, candidate.quickSizeSliderMax);
    candidate.eraseBrushSize = std::min(candidate.eraseBrushSize, candidate.quickSizeSliderMax);
    persist(std::move(candidate));
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

    settings::Settings candidate = settings_;
    candidate.brushSizePresets = std::move(presets);
    persist(std::move(candidate));
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

    settings::Settings candidate = settings_;
    candidate.textSizePresets[index] = std::clamp(*parsed, 4.0f, 512.0f);
    persist(std::move(candidate));
    if (normalizeText) {
        const QSignalBlocker block(edit);
        edit->setText(QString::fromStdString(settings::FormatBrushSizeValue(settings_.textSizePresets[index])));
    }
}


void SettingsDialogQt::applyAutoSaveSlider() {
    const int value = autoSaveSlider_->value();
    const unsigned int seconds = static_cast<unsigned int>(
        std::clamp(std::lround(static_cast<float>(value) / 15.0f) * 15, 0L, 300L));
    settings::Settings candidate = settings_;
    candidate.autoSaveIntervalSeconds = seconds;
    persist(std::move(candidate));
    if (autoSaveSlider_->value() != static_cast<int>(settings_.autoSaveIntervalSeconds)) {
        const QSignalBlocker block(autoSaveSlider_);
        autoSaveSlider_->setValue(static_cast<int>(settings_.autoSaveIntervalSeconds));
    }
    autoSaveLabel_->setText(FormatAutoSaveLabel(DecodeAutoSaveInterval(settings_.autoSaveIntervalSeconds)));
}

// --- shortcuts ---------------------------------------------------------------

void SettingsDialogQt::beginShortcutCapture(settings::ShortcutAction action) {
    const bool wasArmed = captureState_.kind == CaptureState::Kind::Armed;
    captureState_ = CaptureState { CaptureState::Kind::Armed, action };
    if (!wasArmed && QCoreApplication::instance() != nullptr) {
        qApp->installEventFilter(this);
    }
    notifyShortcutHint();
    setFocus(Qt::OtherFocusReason);
}

void SettingsDialogQt::cancelShortcutCapture() {
    if (captureState_.kind != CaptureState::Kind::Armed) {
        return;
    }
    captureState_ = {};
    if (QCoreApplication::instance() != nullptr) {
        qApp->removeEventFilter(this);
    }
    notifyShortcutHint();
}

void SettingsDialogQt::clearSelectedShortcut() {
    const auto action = selectedShortcutAction();
    if (!action.has_value()) {
        return;
    }
    settings::Settings candidate = settings_;
    if (settings::ShortcutBinding* binding = FindBindingIn(candidate, *action); binding != nullptr) {
        binding->sequence = QKeySequence {};
    }
    persist(std::move(candidate));
    refreshShortcutList();
    refreshShortcutButtons();
}

void SettingsDialogQt::keyPressEvent(QKeyEvent* event) {
    if (captureState_.kind != CaptureState::Kind::Armed) {
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

    const settings::ShortcutAction action = captureState_.action;
    settings::Settings candidate = settings_;
    // The legacy capture clears any other command already using the same chord.
    for (settings::ShortcutBinding& binding : candidate.shortcuts) {
        if (binding.action != action && binding.sequence == sequence) {
            binding.sequence = QKeySequence {};
        }
    }
    if (settings::ShortcutBinding* binding = FindBindingIn(candidate, action); binding != nullptr) {
        binding->sequence = sequence;
    }

    cancelShortcutCapture();
    persist(std::move(candidate));
    refreshShortcutList();
    refreshShortcutButtons();
}

bool SettingsDialogQt::eventFilter(QObject* watched, QEvent* event) {
    if (captureState_.kind == CaptureState::Kind::Armed && isVisible() && event->type() == QEvent::KeyPress) {
        keyPressEvent(static_cast<QKeyEvent*>(event));
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsDialogQt::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    refreshFromSettings();
}

void SettingsDialogQt::hideEvent(QHideEvent* event) {
    cancelShortcutCapture();
    QDialog::hideEvent(event);
}

void SettingsDialogQt::closeEvent(QCloseEvent* event) {
    cancelShortcutCapture();
    QDialog::closeEvent(event);
}

}  // namespace cnote::ui
