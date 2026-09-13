#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

#include "Document.h"
#include "ProjectSession.h"
#include "ui/CanvasController.h"

namespace cnote::qt {

// Minimal editor controller for the Qt migration slice (plan step 3.9; App
// becomes this per-view controller in step 4.1).  It owns the shared
// ProjectSession - document plus undo/redo stacks - the pending freehand stroke
// and the file-loading operation, and exposes them through the ui
// CanvasController seam so CanvasWidget needs no knowledge of App or Win32.
//
// History: this slice creates AddStroke/RemoveStroke actions with the same
// structure and stack discipline as the legacy frontend (App::Undo/Redo).  The
// portable reducer extraction (plan step 4.3) will replace the two stroke
// cases below; until then only stroke actions are applied here.
class QtDocumentController final : public ui::CanvasController {
public:
    QtDocumentController();
    ~QtDocumentController() override;

    // Reads a PDF (preferring a valid embedded project) or a cnote project.
    // Returns false and fills `error` when the file cannot be loaded, leaving
    // the current document untouched.
    [[nodiscard]] bool OpenFile(const std::filesystem::path& path, std::string* error);
    void ResetToBlank();

    // Replaces the session content (open/new): clears history and dirty state,
    // drops any pending stroke, keeps the controller's stroke style settings.
    void AdoptDocument(Document document);

    [[nodiscard]] const std::filesystem::path& FilePath() const { return session_.document.FilePath(); }
    [[nodiscard]] bool Dirty() const { return session_.dirty; }

    void SetActiveStrokeStyle(StrokeStyle style) { strokeStyle_ = style; }
    [[nodiscard]] StrokeStyle ActiveStrokeStyle() const { return strokeStyle_; }

    // ui::CanvasController
    [[nodiscard]] const Document& DocumentRef() const override { return session_.document; }
    [[nodiscard]] std::uint64_t Revision() const override { return session_.revision; }
    [[nodiscard]] ui::CanvasInputSettings InputSettings() const override { return input_; }

    void BeginStroke(std::size_t pageIndex, Vec2 point, float pageScale) override;
    void ExtendStroke(Vec2 point, float pageScale) override;
    void EndStroke() override;
    void CancelStroke() override;
    [[nodiscard]] const Stroke* PendingStroke() const override;
    [[nodiscard]] std::size_t PendingStrokePage() const override;

    [[nodiscard]] bool CanUndo() const override { return !session_.undoActions.empty(); }
    [[nodiscard]] bool CanRedo() const override { return !session_.redoActions.empty(); }
    [[nodiscard]] bool Undo() override;
    [[nodiscard]] bool Redo() override;

private:
    // Applies the stroke subset of the history reducer; returns false when the
    // action is not a stroke action or its indices no longer fit the page.
    bool ApplyStrokeHistoryAction(HistoryAction& action, bool undo);

    ProjectSession session_ {};
    ui::CanvasInputSettings input_ {};
    StrokeStyle strokeStyle_ {};
    std::optional<std::size_t> pendingPage_ {};
    std::optional<Stroke> pendingStroke_ {};
};

}  // namespace cnote::qt
