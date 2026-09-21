#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <QString>

#include "Document.h"
#include "ui/InteractionRouter.h"
#include "ui/SelectionMath.h"

namespace cnote::ui {

// Selected object indices for one page, exactly like the reference's
// selected*Indices_ maps.  Selection is per view and never part of history.
struct PageSelection {
    std::vector<std::size_t> strokes;
    std::vector<std::size_t> images;
    std::vector<std::size_t> texts;
    std::vector<std::size_t> latexes;

    [[nodiscard]] bool Empty() const {
        return strokes.empty() && images.empty() && texts.empty() && latexes.empty();
    }
};

// Implemented by the canvas: the controller needs the overlay's current content
// when an edit is finished from outside the canvas (e.g. the session manager
// finishes interactions before another view edits).
class EditContentSource {
public:
    virtual ~EditContentSource() = default;
    [[nodiscard]] virtual PageText CaptureEditedText() const = 0;
    [[nodiscard]] virtual QString CaptureEditedLatexSource() const = 0;
};

// Tuning values the legacy App read from its settings; the canvas needs them to
// sample and preview strokes exactly like the reference frontend.
struct CanvasInputSettings {
    float inputSpacing = 0.85f;     // page units at scale 1 (App::Settings default)
    float inputSmoothing = 0.3f;    // 0..1 pointer follow factor
    float strokeSmoothing = 0.55f;  // render-time smoothing strength
    std::uint32_t objectColor = 0xFF111827;  // active palette slot; seeds new text/LaTeX objects
};

// One resize/rotate step of the whole selection on a page: the selection bounds
// captured at BeginTransform (`from`), the dragged bounds (`to`) and the
// additional rotation about the `from` centre.  `rotateDegrees` is 0 for pure
// resizes.
struct SelectionTransform {
    SelectionBounds from;
    SelectionBounds to;
    float rotateDegrees = 0.0f;
    // Shift-resize: texts scale their font (and box) like the reference's
    // UpdateTextTransform instead of reflowing into the resized box.
    bool scaleTextFont = false;
};

// Per-view seam between CanvasWidget (viewport, transient interaction state,
// caches) and the editor controller that owns the shared ProjectSession
// (plan steps 3.1 and 4.1).  CanvasWidget never touches App, HWND/HDC or the
// document directly: it reads the model through DocumentRef() and mutates only
// through these calls, so the same widget will work with the Qt App controller
// without a second rendering path.
class CanvasController {
public:
    virtual ~CanvasController() = default;

    // Shared model. Valid until the controller is destroyed; the widget treats
    // the returned reference as read-only.
    [[nodiscard]] virtual const Document& DocumentRef() const = 0;

    // Bumped once per committed document change; viewport-only operations do
    // not bump it. Used to drop stale page caches.
    [[nodiscard]] virtual std::uint64_t Revision() const = 0;

    [[nodiscard]] virtual CanvasInputSettings InputSettings() const = 0;

    // Freehand stroke lifecycle. CanvasWidget owns the per-view page scale and
    // segments strokes at page boundaries; it always calls
    // Begin -> Extend* -> End|Cancel. pageScale lets the controller keep the
    // reference's screen-relative sample spacing.
    virtual void BeginStroke(std::size_t pageIndex, Vec2 point, float pageScale) = 0;
    virtual void ExtendStroke(Vec2 point, float pageScale) = 0;
    virtual void EndStroke() = 0;
    virtual void CancelStroke() = 0;
    // Live stroke modes the interaction router tracks (line/oval replace the
    // whole pending point list; Space toggles dotted).
    virtual void SetPendingStrokeDotted(bool dotted) = 0;
    virtual void ReplacePendingStrokePoints(const std::vector<Vec2>& points, bool dotted) = 0;

    // In-progress stroke, for the live preview. Null when no stroke is active.
    [[nodiscard]] virtual const Stroke* PendingStroke() const = 0;
    [[nodiscard]] virtual std::size_t PendingStrokePage() const = 0;

    // --- editing (plan steps 5.5/5.6) -------------------------------------
    // The overlay editors live in the canvas; the controller owns the edit
    // session (object snapshot, history action) and the renderer exclusion.
    [[nodiscard]] virtual bool BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) = 0;
    [[nodiscard]] virtual bool BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex) = 0;
    [[nodiscard]] virtual const PageText* EditingTextObject() const = 0;
    [[nodiscard]] virtual const PageLatex* EditingLatexObject() const = 0;
    [[nodiscard]] virtual std::size_t EditingPageIndex() const = 0;
    [[nodiscard]] virtual std::optional<std::size_t> EditingTextIndex() const = 0;
    [[nodiscard]] virtual std::optional<std::size_t> EditingLatexIndex() const = 0;
    virtual void CommitTextEdit(PageText edited) = 0;
    virtual void CancelTextEdit() = 0;
    virtual void CommitLatexEdit(const QString& source) = 0;
    virtual void CancelLatexEdit() = 0;
    // Object creation from the text/LaTeX tools (the host positions the object).
    virtual std::size_t AddTextObject(std::size_t pageIndex, Vec2 topLeft, float fontSize) = 0;
    virtual std::size_t AddLatexObject(std::size_t pageIndex, Vec2 topLeft, float fontSize) = 0;

    // --- selection, gestures and erase (ui::InteractionHost effects) --------
    [[nodiscard]] virtual bool HasSelection() const = 0;
    [[nodiscard]] virtual const PageSelection& SelectionFor(std::size_t pageIndex) const = 0;
    virtual void ClearSelection() = 0;
    virtual void AddObjectToSelection(const ObjectRef& object) = 0;
    virtual void SelectObjects(std::size_t pageIndex, PageSelection selection) = 0;
    // Removes every selected object as one undo step and clears the selection
    // (the Delete key path); false when nothing was selected.
    virtual bool RemoveSelection() = 0;
    virtual void SetCurrentPageIndex(std::size_t index) = 0;

    // One transaction per gesture: snapshot on begin, one ReplacePages action on
    // commit, restore on cancel.
    virtual void BeginPageGesture(std::size_t pageIndex) = 0;
    virtual void CommitPageGesture() = 0;
    virtual void CancelPageGesture() = 0;
    [[nodiscard]] virtual std::optional<std::size_t> GesturePageIndex() const = 0;
    virtual void MoveSelectionBy(std::size_t pageIndex, Vec2 delta) = 0;
    // One affine transform of the whole selection on `pageIndex`, applied on top
    // of the gesture's begin snapshot (implementations restore the snapshot before
    // applying, so calls are absolute, not cumulative). `from` is the selection
    // bounds captured at BeginTransform; `to` is the dragged bounds; `rotateDegrees`
    // is the additional rotation about the `from` center (0 for pure resizes).
    virtual void TransformSelection(std::size_t pageIndex, const SelectionTransform& transform) = 0;
    virtual void CommitErase(std::size_t pageIndex, const std::vector<ObjectRef>& targets) = 0;
    // Recolors every selected stroke/text/LaTeX object to `color` as one undo
    // step (the palette-click path); false when nothing was recolored.
    virtual bool RecolorSelection(std::uint32_t color) = 0;

    [[nodiscard]] virtual bool CanUndo() const = 0;
    [[nodiscard]] virtual bool CanRedo() const = 0;
    [[nodiscard]] virtual bool Undo() = 0;
    [[nodiscard]] virtual bool Redo() = 0;
};

}  // namespace cnote::ui
