#pragma once

#include <QDialog>
#include <QString>

#include <optional>

#include "cnote_ui_export.h"
#include "settings/KeyBindingQt.h"
#include "settings/SettingsStoreQt.h"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QHideEvent;
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
// model.  An edit is staged into a copy, clamped, saved, and only then assigned
// to `settings`; `settingsChanged()` is emitted after a successful edit.  A
// failed persist therefore leaves the live model and the controls unchanged and
// reports the failure through lastSaveSucceeded() and settingsSaveFailed().
class CNOTE_UI_EXPORT SettingsDialogQt : public QDialog {
    Q_OBJECT

private:
    // Shortcut capture owns an application-wide key filter while armed; it must
    // never outlive visibility.  `Armed` is the only phase in which key events
    // are intercepted, and hide/close/destruction return to `Idle`.
    struct CaptureState {
        enum class Kind {
            Idle,
            Armed,
        };

        Kind kind = Kind::Idle;
        settings::ShortcutAction action = settings::ShortcutAction::ToolDraw;  // valid only when Armed
    };

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

    [[nodiscard]] bool capturingShortcut() const { return captureState_.kind == CaptureState::Kind::Armed; }
    [[nodiscard]] std::optional<settings::ShortcutAction> capturingAction() const {
        if (captureState_.kind != CaptureState::Kind::Armed) {
            return std::nullopt;
        }
        return captureState_.action;
    }

    // Selects the shortcut row for `action` and starts the legacy capture mode
    // ("Press Key..."); Esc cancels.  Used by the button and by the tests.
    void beginShortcutCapture(settings::ShortcutAction action);
    void clearSelectedShortcut();

signals:
    void settingsChanged();
    void settingsSaveFailed(const QString& reason);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void buildPalettePage();
    void buildDrawingPage();
    void buildShortcutsPage();

    // Stages `candidate`, clamps it, saves it, and only applies it to the live
    // model on success.  Returns whether the settings were applied.
    bool persist(settings::Settings candidate);
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

    [[nodiscard]] std::optional<std::size_t> selectedSavedPaletteIndex() const;
    [[nodiscard]] std::optional<settings::ShortcutAction> selectedShortcutAction() const;
    [[nodiscard]] settings::ShortcutBinding* bindingFor(settings::ShortcutAction action);

    settings::Settings& settings_;
    settings::SettingsStoreQt store_;
    bool lastSaveSucceeded_ = true;
    QString lastSaveError_ {};
    bool refreshing_ = false;
    CaptureState captureState_ {};

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
    QComboBox* defaultPageSizeCombo_ = nullptr;
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
    QCheckBox* scratchToEraseCheck_ = nullptr;
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
