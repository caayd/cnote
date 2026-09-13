#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <QString>

#include "Document.h"

namespace cnote::ui {

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

    [[nodiscard]] virtual bool CanUndo() const = 0;
    [[nodiscard]] virtual bool CanRedo() const = 0;
    [[nodiscard]] virtual bool Undo() = 0;
    [[nodiscard]] virtual bool Redo() = 0;
};

}  // namespace cnote::ui
