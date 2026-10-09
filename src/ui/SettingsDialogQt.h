#pragma once

#include <QDialog>
#include <QString>

#include <optional>

#include "settings/KeyBindingQt.h"
#include "settings/SettingsStoreQt.h"

class QCheckBox;
class QComboBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QTabWidget;

namespace cnote::ui {

// Qt port of the legacy Win32 SettingsWindow (plan step 6.2).
//
// Same three categories, controls and policy as src/SettingsWindow.cpp: palette
// add/remove/rename/reset, HSL + hex colour editing, saved palettes, drawing
// spacing/smoothing/dash/quick-size/brush-preset values, zoom presets, auto-save
// interval, toggles and shortcut capture for every action in KeyBindingQt.
//
// The legacy dialog had no OK/Cancel: every edit is applied to the live model
// and persisted immediately (SaveAppSettings), and closing it changed nothing
// further.  This dialog keeps that behaviour: `settings` is the application's
// model, mutated in place, and `settingsChanged()` is emitted after every edit
// so the owning window can re-read palette/brush/chrome values.  A failed
// persist still applies the in-memory value (legacy semantics) but reports
// false through lastSaveSucceeded() and emits settingsSaveFailed().
class SettingsDialogQt : public QDialog {
    Q_OBJECT

public:
    SettingsDialogQt(settings::Settings& settings, settings::SettingsStoreQt store, QWidget* parent = nullptr);
    ~SettingsDialogQt() override;

    // Re-reads the whole model into the controls.  Called on every show(), so an
    // external settings change is picked up the next time the dialog opens.
    void refreshFromSettings();

    [[nodiscard]] settings::Settings& settings() { return settings_; }
    [[nodiscard]] const settings::Settings& settings() const { return settings_; }

    [[nodiscard]] bool lastSaveSucceeded() const { return lastSaveSucceeded_; }
    [[nodiscard]] QString lastSaveError() const { return lastSaveError_; }

    [[nodiscard]] bool capturingShortcut() const { return capturingAction_.has_value(); }
    [[nodiscard]] std::optional<settings::ShortcutAction> capturingAction() const { return capturingAction_; }

    // Selects the shortcut row for `action` and starts the legacy capture mode
    // ("Press Key..."); Esc cancels.  Used by the button and by the tests.
    void beginShortcutCapture(settings::ShortcutAction action);
    void clearSelectedShortcut();

signals:
    void settingsChanged();
    void settingsSaveFailed(const QString& reason);

protected:
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void buildPalettePage();
    void buildDrawingPage();
    void buildShortcutsPage();

    void persist();
    void notifyShortcutHint();

    void refreshPaletteControls(bool syncSliders);
    void refreshSavedPalettes();
    void refreshSavedPaletteButtons();
    void refreshDrawingControls();
    void refreshShortcutList();
    void refreshShortcutButtons();

    void applyPaletteSelection();
    void addPaletteColor();
    void removePaletteColor();
    void applyHexColorEdit();
    void applyHueSaturationLightness();
    void saveNamedPalette();
    void applySelectedNamedPalette();
    void deleteSelectedNamedPalette();
    void applyZoomPresetEdit(QLineEdit* edit, std::size_t index, bool normalizeText);
    void applyQuickSizeSliderMaxEdit(bool normalizeText);
    void applyBrushSizePresetEdit(bool normalizeText);
    void applyAutoSaveSlider();
    void applyTextSizePresetEdit(QLineEdit* edit, std::size_t index, bool normalizeText);
    void cancelShortcutCapture();

    [[nodiscard]] std::size_t selectedPaletteIndex() const;
    [[nodiscard]] std::optional<std::size_t> selectedSavedPaletteIndex() const;
    [[nodiscard]] std::optional<settings::ShortcutAction> selectedShortcutAction() const;
    [[nodiscard]] settings::ShortcutBinding* bindingFor(settings::ShortcutAction action);

    settings::Settings& settings_;
    settings::SettingsStoreQt store_;
    bool lastSaveSucceeded_ = true;
    QString lastSaveError_ {};
    bool refreshing_ = false;
    std::optional<settings::ShortcutAction> capturingAction_ {};

    QTabWidget* categoryTabs_ = nullptr;

    QListWidget* paletteList_ = nullptr;
    QPushButton* addColorButton_ = nullptr;
    QPushButton* removeColorButton_ = nullptr;
    QLineEdit* hexColorEdit_ = nullptr;
    QSlider* hueSlider_ = nullptr;
    QSlider* saturationSlider_ = nullptr;
    QSlider* lightnessSlider_ = nullptr;
    QLabel* hueLabel_ = nullptr;
    QLabel* saturationLabel_ = nullptr;
    QLabel* lightnessLabel_ = nullptr;
    QLineEdit* savedPaletteNameEdit_ = nullptr;
    QPushButton* savePaletteButton_ = nullptr;
    QPushButton* applyPaletteButton_ = nullptr;
    QPushButton* deletePaletteButton_ = nullptr;
    QListWidget* savedPaletteList_ = nullptr;

    QSlider* stabilizationSlider_ = nullptr;
    QLabel* stabilizationLabel_ = nullptr;
    QSlider* spacingSlider_ = nullptr;
    QLabel* spacingLabel_ = nullptr;
    QSlider* strokeSmoothingSlider_ = nullptr;
    QLabel* strokeSmoothingLabel_ = nullptr;
    QComboBox* drawCursorKindCombo_ = nullptr;
    QSlider* drawCursorSizeSlider_ = nullptr;
    QLabel* drawCursorSizeLabel_ = nullptr;
    QSlider* dashLengthSlider_ = nullptr;
    QLabel* dashLengthLabel_ = nullptr;
    QSlider* dashGapSlider_ = nullptr;
    QLabel* dashGapLabel_ = nullptr;
    QSlider* autoSaveSlider_ = nullptr;
    QLabel* autoSaveLabel_ = nullptr;
    QCheckBox* autoHideToolbarCheck_ = nullptr;
    QCheckBox* embedProjectInPdfCheck_ = nullptr;
    QLineEdit* zoomPresetEdits_[settings::kZoomPresetCount] {};
    QLineEdit* quickSizeSliderMaxEdit_ = nullptr;
    QLineEdit* brushSizePresetEdit_ = nullptr;
    QLineEdit* textSizePresetEdits_[settings::kTextSizePresetCount] {};

    QListWidget* shortcutList_ = nullptr;
    QPushButton* changeShortcutButton_ = nullptr;
    QPushButton* clearShortcutButton_ = nullptr;
    QLabel* shortcutHintLabel_ = nullptr;
};

}  // namespace cnote::ui
