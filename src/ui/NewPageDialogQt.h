#pragma once

#include <QDialog>

#include <cstdint>
#include <vector>

#include "settings/SettingsStoreQt.h"
#include "ui/PageDefaultsQt.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;

namespace cnote::ui {

// Qt port of the legacy Win32 NewPageSettingsWindow (plan step 6.2).
//
// Same draft model and policy as src/NewPageSettingsWindow.cpp: width/height,
// background colour (default swatches, previous colours, hex, HSL), stroke
// colour profile, the remap toggle and the Apply to Current / Apply to All /
// Apply to Future Pages / Reset Defaults / Append Pages actions.  Nothing is
// applied until an apply button is pressed; the draft is live but local, and
// closing the dialog discards it (re-synced from the applied defaults on the
// next open, which is the legacy refresh behaviour).
//
// The dialog owns the applied "future pages" state and the recent-colour list
// (legacy App::newPageSettings_) and emits applyRequested() with the draft and
// scope.  Document mutation, history and cache invalidation belong to the
// owning controller, which also pushes futureDefaultsChanged() into
// Document::SetNewPageDefaults.
class NewPageDialogQt : public QDialog {
    Q_OBJECT

public:
    // `settings` is the application's live settings model (read-only here: the
    // palette, saved palettes and default page colour feed the draft).
    explicit NewPageDialogQt(settings::Settings& settings, QWidget* parent = nullptr);
    ~NewPageDialogQt() override;

    [[nodiscard]] settings::Settings& settings() { return settings_; }

    [[nodiscard]] const NewPageDefaults& defaults() const { return defaults_; }
    void setDefaults(NewPageDefaults defaults, std::vector<std::uint32_t> recentColors = {});

    [[nodiscard]] const std::vector<std::uint32_t>& recentColors() const { return recentColors_; }
    [[nodiscard]] const NewPageDraft& draft() const { return draft_; }
    void setDraft(const NewPageDraft& draft);

    // Legacy SyncDraftFromApp: draft := defaults (profile resolved so it is
    // never empty) + control refresh.  Lines up the next show().
    void syncDraftFromDefaults();
    void refreshFromSettings();

public slots:
    void applyToCurrentPage();
    void applyToAllPages();
    void applyToFuturePages();
    void resetDraft();

signals:
    void applyRequested(const cnote::ui::NewPageDraft& draft, cnote::ui::NewPageApplyScope scope);
    void futureDefaultsChanged(const cnote::ui::NewPageDefaults& defaults, const std::vector<std::uint32_t>& recentColors);
    void appendPagesRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void refreshDraftControls();
    void refreshDefaultSwatches();
    void refreshRecentColors();
    void refreshPaletteProfileCombo();
    void ensureCustomProfileEntry(bool needed);
    void commitDimensionEdits();

    void applyDimensionText(QLineEdit* edit, float& value, bool normalizeText);
    void applyHexColorEdit();
    void applyHslSliders();

    [[nodiscard]] std::vector<std::uint32_t> currentSettingsProfile() const;
    [[nodiscard]] std::vector<std::uint32_t> resolvedDraftProfile() const;
    [[nodiscard]] QString draftProfileLabel() const;

    settings::Settings& settings_;
    NewPageDefaults defaults_ {};
    std::vector<std::uint32_t> recentColors_ {};
    NewPageDraft draft_ {};

    QLineEdit* widthEdit_ = nullptr;
    QLineEdit* heightEdit_ = nullptr;
    QComboBox* dimensionCombo_ = nullptr;
    QWidget* defaultSwatchContainer_ = nullptr;
    QWidget* recentSwatchContainer_ = nullptr;
    QLabel* recentColorsEmptyLabel_ = nullptr;
    QLabel* colorPreview_ = nullptr;
    QLineEdit* hexColorEdit_ = nullptr;
    QSlider* hueSlider_ = nullptr;
    QSlider* saturationSlider_ = nullptr;
    QSlider* lightnessSlider_ = nullptr;
    QLabel* hueLabel_ = nullptr;
    QLabel* saturationLabel_ = nullptr;
    QLabel* lightnessLabel_ = nullptr;
    QComboBox* paletteProfileCombo_ = nullptr;
    QLabel* paletteProfileLabel_ = nullptr;
    QCheckBox* remapCheck_ = nullptr;
    QPushButton* applyCurrentButton_ = nullptr;
    QPushButton* applyAllButton_ = nullptr;
    QPushButton* applyFutureButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* appendButton_ = nullptr;
};

}  // namespace cnote::ui
