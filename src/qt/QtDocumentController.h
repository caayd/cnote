#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <QObject>
#include <QString>

#include "cnote_ui_export.h"
#include "Document.h"
#include "document/SelectionClipboardCodec.h"
#include "ui/PageDefaultsQt.h"
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
class CNOTE_UI_EXPORT QtDocumentController final : public QObject, public ui::CanvasController, public SessionView {
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
    // App-level fallback; does not resize existing pages or override project settings.
    void SetDefaultPageSize(float width, float height);

    // What one save produced.  `path`/`revision` are the request's, so a
    // completion can tell which revision of the document reached the disk.
    struct SaveOutcome {
        bool success = false;
        std::string error;
        std::filesystem::path path;
        std::uint64_t revision = 0;
    };
    using SaveCompletion = std::function<void(const SaveOutcome&)>;

    // Starts a save and returns without waiting: the immutable document
    // snapshot is taken here (cheap; rasters and PDF bytes stay shared) and
    // serialization, compression and the atomic replacement run on the
    // background writer.  `onDone` runs on the GUI thread, after the session's
    // dirty/path bookkeeping, never on the worker.  Returns false - with
    // *error set - only when the request is refused up front (PDF destination).
    [[nodiscard]] bool RequestSave(const std::filesystem::path& path, std::string* error,
        SaveCompletion onDone = {});
    // Saves and waits for the write to commit: the last-view close and tests
    // use it where continuing before the destination exists would lose data.
    [[nodiscard]] bool SaveTo(const std::filesystem::path& path, std::string* error);
    [[nodiscard]] bool ExportPdf(const std::filesystem::path& path, bool embedProject, std::string* error);

    [[nodiscard]] render::PdfBackgroundProvider* PdfBackground() const { return pdfBackground_.get(); }
    // Closing a window detaches its view from the shared session so the close
    // of the remaining view is treated as the last view; showing the window
    // again re-registers it (RegisterView is idempotent).
    void DetachForClose();
    void ReattachForShow();

    [[nodiscard]] bool Dirty() const { return session_ != nullptr && session_->dirty; }
    // Diagnostics: Saved changes broadcast by this view's controller (plan 6.6);
    // a successful save is exactly one, a failed save none.
    [[nodiscard]] std::uint64_t SavedChangeCount() const { return savedChangeCount_; }
    [[nodiscard]] bool HasSavedPath() const;
    [[nodiscard]] const std::filesystem::path& FilePath() const;
    // True when the document's current path is a real project destination.  A
    // PDF container (or a project recovered from one) has the PDF as its path,
    // so an in-place save would overwrite it and must go through Save As.
    [[nodiscard]] bool CanSaveInPlace() const;

    // The canvas supplies the overlay content.  When an edit is ended from
    // inside this controller - undo/redo, a session replacement or close, or a
    // remote commit (which cancels instead) - the live text/LaTeX is captured
    // through this source; without one the edit is cancelled, never committed
    // with an empty value.
    void SetEditContentSource(ui::EditContentSource* source) { editContent_ = source; }

    // --- selection and object-gesture surface (ui::CanvasController) --------
    [[nodiscard]] bool HasSelection() const override;
    [[nodiscard]] const ui::PageSelection& SelectionFor(std::size_t pageIndex) const override;
    void ClearSelection() override;
    void AddObjectToSelection(const ui::ObjectRef& object) override;
    void SelectObjects(std::size_t pageIndex, ui::PageSelection selection) override;
    void ToggleObjectInSelection(const ui::ObjectRef& object) override;
    void AddToSelection(std::size_t pageIndex, ui::PageSelection selection) override;
    void SetCurrentPageIndex(std::size_t index) override;

    // One transaction per gesture: only the objects the gesture touches are
    // snapshotted and recorded (a ReplaceObjects history action), so a
    // one-object move costs that object's state instead of the whole page.
    void BeginPageGesture(std::size_t pageIndex) override;
    void CommitPageGesture() override;
    void CancelPageGesture() override;
    [[nodiscard]] std::optional<std::size_t> GesturePageIndex() const override {
        return gesture_.has_value() ? std::optional<std::size_t>(gesture_->pageIndex) : std::nullopt;
    }
    [[nodiscard]] Page* GesturePage();
    void MoveSelectionBy(std::size_t pageIndex, Vec2 delta) override;
    // Alt-drag move: duplicates the selected objects on `pageIndex` in place
    // inside the open gesture (new ids for texts/LaTeX), repoints the
    // selection at the clones and records the clones as insertions, so the
    // commit keeps them even without a drag. False when nothing was selected.
    bool CloneSelection(std::size_t pageIndex) override;
    // Whole-selection affine transform for a resize/rotate gesture: restores
    // the gesture snapshot first, so `transform` is absolute, not cumulative.
    void TransformSelection(std::size_t pageIndex, const ui::SelectionTransform& transform) override;
    void CommitErase(std::size_t pageIndex, const std::vector<ui::ObjectRef>& targets) override;
    bool RecolorSelection(std::uint32_t color) override;

    // Stroke effects the interaction router drives (line/oval modes replace the
    // whole pending point list).
    void SetPendingStrokeDotted(bool dotted) override;
    void ReplacePendingStrokePoints(const std::vector<Vec2>& points, bool dotted) override;

    void SetActiveStrokeStyle(StrokeStyle style) { strokeStyle_ = style; }
    [[nodiscard]] StrokeStyle ActiveStrokeStyle() const { return strokeStyle_; }
    // Input tuning (spacing/smoothing/render smoothing) from the settings model.
    void SetInputSettings(ui::CanvasInputSettings settings) { input_ = settings; }

    // --- page operations (plan 6.2: Add Page / Append Pages / page settings) --
    // Inserts `count` pages built from `draft` at `index` (clamped to the page
    // count); one AddPages undo step. Returns the first inserted index.
    std::size_t InsertPages(std::size_t index, std::size_t count, const ui::NewPageDraft& draft);
    // Legacy NewPageSettingsWindow actions: apply the draft to the current page,
    // to every page, or store it for pages created later.
    void ApplyNewPageDraft(const ui::NewPageDraft& draft, ui::NewPageApplyScope scope);
    void SetNewPageDefaults(const ui::NewPageDefaults& defaults);
    [[nodiscard]] ui::NewPageDefaults NewPageDefaults() const;

    // --- page clipboard (sidebar page management) ---------------------------
    // Page operations take `pages` sorted and unique, every index in range.
    // Copies `pages` into the process-global page clipboard
    // (AppManager::SetClipboardPages); false for an invalid list.
    bool CopyPages(const std::vector<std::size_t>& pages);
    // Copy + remove as one undo step; false (clipboard untouched) when the
    // pages cannot be removed: an invalid list or every page of the document.
    bool CutPages(const std::vector<std::size_t>& pages);
    // Removes `pages` as one RemovePages undo step; false for an invalid list
    // or when it would leave the document without pages.
    bool RemovePages(const std::vector<std::size_t>& pages);
    // Inserts the clipboard pages after `index` (clamped to the page count) as
    // one AddPages undo step.  Returns the first inserted index, or nullopt
    // when the page clipboard is empty.
    std::optional<std::size_t> PastePages(std::size_t index);
    // Moves `pages` (sorted, unique) as one block, in their current order, to
    // gap `slot` (0..PageCount, counted before the move; sidebar drag reorder)
    // as one ReorderPages undo step; per-page selections move with their
    // pages.  Returns the block's new first index, or nullopt when an index is
    // out of range or the move would leave every page where it is.
    std::optional<std::size_t> MovePages(const std::vector<std::size_t>& pages, std::size_t slot);
    // Rotates page `index` clockwise by quarter turns (1 = 90°, 2 = 180°,
    // 3 = 90° counter-clockwise): every object coordinate and the stored
    // raster transform with it, the page frame swaps dims for odd turns and
    // Page::rotationDegrees accumulates for PDF backgrounds.  One ReplacePages
    // undo step; false when out of range or the turn count is 0.
    bool RotatePage(std::size_t pageIndex, int quarterTurns);

    // --- selection clipboard, document side (the shell owns QClipboard) ------
    // Collects the selected objects of every page the view has a selection on,
    // with the bounds the paste centers on. False when nothing is selected.
    [[nodiscard]] bool BuildSelectionPayload(cnote::SelectionClipboardData& data) const;
    // PDF text selection (the reference's selectedPdfTextBoxes_): true when the
    // view has selected PDF text, and the selected text assembled in document
    // order with the reference's line-break heuristic. The shell publishes it
    // as plain text.
    [[nodiscard]] bool HasSelectedPdfText() const;
    [[nodiscard]] QString SelectedPdfText() const;
    // Removes the selected objects as one undo step; false when nothing was
    // selected. A pdf-text-only selection is cleared, not removed.
    bool RemoveSelection() override;
    [[nodiscard]] const std::vector<PdfTextCharBox>* PdfTextCharBoxesFor(std::size_t pageIndex) const override;
    // Inserts `data` into `pageIndex` with the payload's bounds centered on
    // `target` (clamped to stay inside the page), selects the pasted objects and
    // records one undo step.
    bool PastePayload(std::size_t pageIndex, Vec2 target, const cnote::SelectionClipboardData& data);

    // --- ui::CanvasController --------------------------------------------
    [[nodiscard]] const Document& DocumentRef() const override;
    [[nodiscard]] ui::CanvasInputSettings InputSettings() const override { return input_; }
    [[nodiscard]] std::uint64_t TransientGeneration() const override {
        return session_ != nullptr ? session_->transientGeneration : 0U;
    }

    void BeginStroke(std::size_t pageIndex, Vec2 point, StrokeSampling sampling, std::optional<float> pressure) override;
    void ExtendStroke(Vec2 point, std::optional<float> pressure) override;
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
    // A text `width` of 0 grows with its content, anything else wraps there.
    std::size_t AddTextObject(std::size_t pageIndex, Vec2 topLeft, float fontSize, float width) override;
    std::size_t AddLatexObject(std::size_t pageIndex, Vec2 topLeft, float fontSize) override;

    [[nodiscard]] bool BeginTextEdit(std::size_t pageIndex, std::size_t textIndex);
    [[nodiscard]] bool BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex);

    [[nodiscard]] bool EditingText() const {
        return edit_.has_value() && edit_->kind == EditSession::Kind::Text;
    }
    [[nodiscard]] bool EditingLatex() const {
        return edit_.has_value() && edit_->kind == EditSession::Kind::LaTeX;
    }
    // The object under edit: the snapshot taken when the edit began, with the
    // box FitEditingText has fitted to the content since.  The overlay fills
    // itself from it and converts back through cnote::text helpers on commit.
    // The object itself is always located by id (see EditSession), never by a
    // stored index: a page insert or another view's commit can move it.
    [[nodiscard]] const PageText* EditingTextObject() const;
    [[nodiscard]] const PageLatex* EditingLatexObject() const;
    [[nodiscard]] std::size_t EditingPageIndex() const;
    [[nodiscard]] std::optional<std::size_t> EditingTextIndex() const;
    [[nodiscard]] std::optional<std::size_t> EditingLatexIndex() const;

    // Re-fits the edited text's box to `edited` (the overlay's content and
    // spans; see text::FitTextBox): an auto-width text keeps its alignment's
    // anchor and starts wrapping once it outgrows `maxAutoWidth`.
    void FitEditingText(const PageText& edited, float maxAutoWidth);
    // Alignment is the whole object's (PageText::alignment), so the edit
    // changes the live object; the commit records it with the content.
    void SetEditingTextAlignment(TextAlignment alignment);
    // Commits the edited object; the caller passes the overlay's converted
    // content and spans. Nothing is recorded when the object is unchanged.
    void CommitTextEdit(PageText edited);
    void CancelTextEdit();
    void CommitLatexEdit(const QString& source);
    void CancelLatexEdit();

signals:
    // A commit changed the shared document (repaint; per-page caches
    // invalidate through Document::PageEpoch).
    void SessionChanged();
    // The active edit started or ended (overlays show/hide, chrome updates).
    void EditStateChanged();
    // The object selection changed, whatever the origin: a canvas gesture
    // (ClearSelection/AddObjectToSelection/SelectObjects), a cut
    // (RemoveSelection) or a paste that selects what it inserted.  The shell
    // refreshes its selection-gated actions (copy/cut) on this.
    void SelectionChanged();

private:
    // One live text/LaTeX edit.  The object is addressed by id - the same rule
    // cnote::ApplyHistoryAction uses - never by a stored index: a page insert,
    // a page rotation or a remote commit can move it, and committing through a
    // stale index would rename or erase the wrong object.  The `*Before`
    // snapshot is the undo base; `text` is the live object, whose box follows
    // the content as it is typed.  `creates` marks the placeholder
    // AddTextObject/AddLatexObject pushed before editing, which an empty
    // commit removes without history.
    struct EditSession {
        enum class Kind { Text, LaTeX };
        Kind kind = Kind::Text;
        std::size_t pageIndex = 0;
        std::uint64_t objectId = 0;
        bool creates = false;
        PageText textBefore {};
        PageText text {};
        PageLatex latexBefore {};
    };

    // One in-progress page gesture.  Only the objects the gesture touches are
    // snapshotted, and CommitPageGesture records just those as one
    // ReplaceObjects action, so retained history is O(affected objects) rather
    // than O(page).  The four `before` maps hold the pre-gesture state of
    // objects mutated in place; `removed` holds objects an eraser deletes (the
    // pre-removal index is the key) and `inserted` the indices an Alt-drag
    // clone appends.  The move bounds cache lets MoveSelectionBy measure each
    // selected object once and translate by the applied delta, so a move
    // sample costs O(1) per object instead of re-measuring strokes/text/LaTeX.
    struct PageGesture {
        struct MoveBounds {
            bool built = false;
            std::vector<ui::SelectionBounds> strokes;
            std::vector<ui::SelectionBounds> images;
            std::vector<ui::SelectionBounds> texts;
            std::vector<ui::SelectionBounds> latexes;
        };
        std::size_t pageIndex = 0;
        std::map<std::size_t, Stroke> beforeStrokes {};
        std::map<std::size_t, PageImage> beforeImages {};
        std::map<std::size_t, PageText> beforeTexts {};
        std::map<std::size_t, PageLatex> beforeLatexes {};
        std::map<std::size_t, Stroke> removedStrokes {};
        std::map<std::size_t, PageImage> removedImages {};
        std::map<std::size_t, PageText> removedTexts {};
        std::map<std::size_t, PageLatex> removedLatexes {};
        std::set<std::size_t> insertedStrokes {};
        std::set<std::size_t> insertedImages {};
        std::set<std::size_t> insertedTexts {};
        std::set<std::size_t> insertedLatexes {};
        MoveBounds moveBounds {};
    };

    // The stroke being drawn and the page it belongs to, as one value so the
    // page can never outlive (or diverge from) the stroke.  The name is not
    // `PendingStroke`: that is the CanvasController accessor.
    struct PendingStrokeState {
        std::size_t pageIndex = 0;
        Stroke stroke {};
    };

    void NotifySessionChanged(bool originating);
    void RebuildPdfBackground();
    [[nodiscard]] std::uint64_t NextObjectId() const;
    [[nodiscard]] Page* PageAt(std::size_t pageIndex);
    [[nodiscard]] const Page* PageAt(std::size_t pageIndex) const;
    // Ends every transient interaction before the shared document changes under
    // it.  `commitOpenEdit` finishes a live edit with the overlay's captured
    // content (a remote commit passes false: its snapshot and object may
    // already be stale, so it must be dropped, never written back).
    // `restoreGesture` puts an in-progress gesture's mutated objects back (local
    // abort); a remote commit drops it without restoring, because its captured
    // objects predate the other view's committed change.
    void AbortTransientState(bool commitOpenEdit = true, bool restoreGesture = true);
    // Commits (or, without an edit-content source, cancels) the live edit.
    void FinishOpenEdit();
    // Shared commit tail: dirty, one revision bump, the viewed page resynced to
    // the document's and one broadcast.
    void NoteCommitted();
    // Pushes `action`, drops the redo stack and runs the shared tail.
    void RecordCommit(HistoryAction action);
    // Captures the pre-gesture state of every selected object the gesture may
    // mutate.  Idempotent: an object already captured (or appended by this
    // gesture's clone) keeps its first snapshot.
    void CaptureGestureSelection(const Page& page, const ui::PageSelection& selection);
    // Builds the delta of everything this gesture changed from the current
    // page, or nullopt when nothing changed (or the page is gone).  Compares
    // only captured/inserted/removed objects, never the whole page.
    [[nodiscard]] std::optional<HistoryAction> BuildPageGestureAction() const;
    // In-place gesture edits do not bump Document::PageEpoch until commit, so
    // every view's content-derived caches key on this session counter instead.
    void TouchTransient() {
        if (session_ != nullptr) {
            ++session_->transientGeneration;
        }
    }

    AppManager& manager_;
    std::shared_ptr<ProjectSession> session_;
    std::unique_ptr<PdfiumBackend> pdfBackend_;
    std::unique_ptr<render::PdfBackgroundProvider> pdfBackground_;

    ui::CanvasInputSettings input_ {};
    StrokeStyle strokeStyle_ {};
    float defaultPageWidth_ = 595.0f;
    float defaultPageHeight_ = 842.0f;
    std::size_t currentPage_ = 0;

    ui::EditContentSource* editContent_ = nullptr;
    std::optional<PendingStrokeState> pendingStroke_ {};
    std::optional<EditSession> edit_;
    std::optional<PageGesture> gesture_;

    std::map<std::size_t, ui::PageSelection> selection_;

    // Extracted PDF character boxes per source page, keyed by (source bytes,
    // source page index). Extraction loads the whole PDF text page through
    // PDFium, so it is cached: the key pins the byte buffer (shared_ptr is the
    // key and the pin, so a replaced source can never collide).
    struct PdfTextCacheKey {
        std::shared_ptr<const std::vector<std::uint8_t>> pdfData;
        std::size_t sourcePage = 0;
        // Page rotation bakes into the cached boxes, so it keys them too.
        int quarterTurns = 0;
        [[nodiscard]] bool operator<(const PdfTextCacheKey& other) const {
            if (pdfData != other.pdfData) {
                return std::owner_less<std::shared_ptr<const std::vector<std::uint8_t>>> {}(pdfData, other.pdfData);
            }
            if (sourcePage != other.sourcePage) {
                return sourcePage < other.sourcePage;
            }
            return quarterTurns < other.quarterTurns;
        }
    };
    mutable std::map<PdfTextCacheKey, std::vector<PdfTextCharBox>> pdfTextCache_;
    std::uint64_t savedChangeCount_ = 0;
};

}  // namespace cnote::qt
