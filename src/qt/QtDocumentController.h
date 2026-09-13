#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <QObject>
#include <QString>

#include "Document.h"
#include "ProjectSession.h"
#include "shell/AppManager.h"
#include "ui/CanvasController.h"
#include "ui/InteractionRouter.h"

namespace cnote {
class PdfiumBackend;
}

namespace cnote::render {
class PdfBackgroundProvider;
}

namespace cnote::qt {

// Per-view Qt editor controller (plan steps 4.1-4.4, 5.5-5.6).  It owns the
// view's transient state - pending stroke, active text/LaTeX edit, current page
// - and operates on the shared ProjectSession the AppManager registers it with,
// so document, history, dirty state and autosave are shared between views while
// viewport and interaction state stay per view.
//
// History goes through the portable cnote_model reducer
// (cnote::ApplyHistoryAction); this class only moves the stacks, bumps the
// revision once per commit and broadcasts to the session's views.
class QtDocumentController final : public QObject, public ui::CanvasController, public SessionView {
    Q_OBJECT

public:
    explicit QtDocumentController(AppManager& manager, QObject* parent = nullptr);
    ~QtDocumentController() override;

    // --- session ----------------------------------------------------------
    [[nodiscard]] const std::shared_ptr<ProjectSession>& Session() const { return session_; }
    // Joins an existing session or adopts a new one, registering this view.
    void AttachSession(std::shared_ptr<ProjectSession> session);
    [[nodiscard]] bool OpenFile(const std::filesystem::path& path, std::string* error);
    void NewBlankDocument();
    [[nodiscard]] bool SaveTo(const std::filesystem::path& path, std::string* error);
    [[nodiscard]] bool ExportPdf(const std::filesystem::path& path, bool embedProject, std::string* error);

    [[nodiscard]] render::PdfBackgroundProvider* PdfBackground() const { return pdfBackground_.get(); }
    [[nodiscard]] bool Dirty() const { return session_ != nullptr && session_->dirty; }
    // Diagnostics: Saved changes broadcast by this view's controller (plan 6.6);
    // a successful save is exactly one, a failed save none.
    [[nodiscard]] std::uint64_t SavedChangeCount() const { return savedChangeCount_; }
    [[nodiscard]] bool HasSavedPath() const;
    [[nodiscard]] const std::filesystem::path& FilePath() const;

    // The canvas supplies the overlay content when an edit is finished from
    // outside the canvas (AppManager::FinishInteraction).
    void SetEditContentSource(ui::EditContentSource* source) { editContent_ = source; }

    // --- selection and object-gesture surface (ui::CanvasController) --------
    [[nodiscard]] bool HasSelection() const override;
    [[nodiscard]] const ui::PageSelection& SelectionFor(std::size_t pageIndex) const override;
    void ClearSelection() override;
    void AddObjectToSelection(const ui::ObjectRef& object) override;
    void SelectObjects(std::size_t pageIndex, ui::PageSelection selection) override;
    void SetCurrentPageIndex(std::size_t index) override;

    // One transaction per gesture: the page is snapshotted when the gesture
    // starts and a single ReplacePages action records before/after on commit.
    void BeginPageGesture(std::size_t pageIndex) override;
    void CommitPageGesture() override;
    void CancelPageGesture() override;
    [[nodiscard]] std::optional<std::size_t> GesturePageIndex() const override { return gesturePage_; }
    [[nodiscard]] Page* GesturePage();
    void MoveSelectionBy(std::size_t pageIndex, Vec2 delta) override;
    void CommitErase(std::size_t pageIndex, const std::vector<ui::ObjectRef>& targets) override;

    // Stroke effects the interaction router drives (line/oval modes replace the
    // whole pending point list).
    void SetPendingStrokeDotted(bool dotted) override;
    void ReplacePendingStrokePoints(const std::vector<Vec2>& points, bool dotted) override;

    void SetActiveStrokeStyle(StrokeStyle style) { strokeStyle_ = style; }
    [[nodiscard]] StrokeStyle ActiveStrokeStyle() const { return strokeStyle_; }

    // --- ui::CanvasController --------------------------------------------
    [[nodiscard]] const Document& DocumentRef() const override;
    [[nodiscard]] std::uint64_t Revision() const override;
    [[nodiscard]] ui::CanvasInputSettings InputSettings() const override { return input_; }

    void BeginStroke(std::size_t pageIndex, Vec2 point, float pageScale) override;
    void ExtendStroke(Vec2 point, float pageScale) override;
    void EndStroke() override;
    void CancelStroke() override;
    [[nodiscard]] const Stroke* PendingStroke() const override;
    [[nodiscard]] std::size_t PendingStrokePage() const override;

    [[nodiscard]] bool CanUndo() const override;
    [[nodiscard]] bool CanRedo() const override;
    [[nodiscard]] bool Undo() override;
    [[nodiscard]] bool Redo() override;

    // --- SessionView ------------------------------------------------------
    [[nodiscard]] bool IsInteractionActive() const override;
    void FinishInteraction() override;
    void OnSessionChanged(const ProjectChange& change, bool originating) override;
    [[nodiscard]] std::size_t CurrentPageIndex() const override { return currentPage_; }

    // --- object creation and editing -------------------------------------
    // Inserts an empty object at the given page position and starts editing it;
    // an empty commit removes it again, exactly like the reference frontend.
    std::size_t AddTextObject(std::size_t pageIndex, Vec2 topLeft, float fontSize) override;
    std::size_t AddLatexObject(std::size_t pageIndex, Vec2 topLeft, float fontSize) override;

    [[nodiscard]] bool BeginTextEdit(std::size_t pageIndex, std::size_t textIndex);
    [[nodiscard]] bool BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex);

    [[nodiscard]] bool EditingText() const { return editingTextIndex_.has_value(); }
    [[nodiscard]] bool EditingLatex() const { return editingLatexIndex_.has_value(); }
    // Object snapshot taken when the edit began; the overlay fills itself from
    // it and converts back through cnote::text helpers on commit.
    [[nodiscard]] const PageText* EditingTextObject() const;
    [[nodiscard]] const PageLatex* EditingLatexObject() const;
    [[nodiscard]] std::size_t EditingPageIndex() const;
    [[nodiscard]] std::optional<std::size_t> EditingTextIndex() const { return editingTextIndex_; }
    [[nodiscard]] std::optional<std::size_t> EditingLatexIndex() const { return editingLatexIndex_; }

    // Commits the edited object; the caller passes the overlay's converted
    // content and spans. Nothing is recorded when the object is unchanged.
    void CommitTextEdit(PageText edited);
    void CancelTextEdit();
    void CommitLatexEdit(const QString& source);
    void CancelLatexEdit();

signals:
    // A commit changed the shared document (the canvas drops its caches).
    void SessionChanged();
    // The active edit started or ended (overlays show/hide, chrome updates).
    void EditStateChanged();

private:
    void NotifySessionChanged(bool originating);
    // Committed vs Saved change broadcast (plan 6.6: commit before broadcasting
    // Saved); a failed save broadcasts nothing.
    void BroadcastChange(ProjectChange::Kind kind);
    void RebuildPdfBackground();
    [[nodiscard]] std::uint64_t NextObjectId() const;
    [[nodiscard]] Page* PageAt(std::size_t pageIndex);
    [[nodiscard]] const Page* PageAt(std::size_t pageIndex) const;

    AppManager& manager_;
    std::shared_ptr<ProjectSession> session_;
    std::unique_ptr<PdfiumBackend> pdfBackend_;
    std::unique_ptr<render::PdfBackgroundProvider> pdfBackground_;

    ui::CanvasInputSettings input_ {};
    StrokeStyle strokeStyle_ {};
    std::size_t currentPage_ = 0;

    ui::EditContentSource* editContent_ = nullptr;
    std::optional<std::size_t> pendingStrokePage_ {};
    std::optional<Stroke> pendingStroke_ {};

    std::map<std::size_t, ui::PageSelection> selection_;
    std::optional<std::size_t> gesturePage_ {};
    Page gestureBefore_ {};
    bool gestureChanged_ = false;
    std::uint64_t savedChangeCount_ = 0;

    // Text edit state: the object snapshot the overlay was filled from.
    std::optional<std::size_t> editingTextPage_ {};
    std::optional<std::size_t> editingTextIndex_ {};
    PageText editingTextBefore_ {};
    bool editingTextCreates_ = false;

    std::optional<std::size_t> editingLatexPage_ {};
    std::optional<std::size_t> editingLatexIndex_ {};
    PageLatex editingLatexBefore_ {};
    bool editingLatexCreates_ = false;
};

}  // namespace cnote::qt
