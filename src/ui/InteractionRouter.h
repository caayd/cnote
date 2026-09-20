#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <QPointF>
#include <Qt>

#include "Document.h"

namespace cnote::ui {

// Widget-free pointer interaction router (migration plan steps 5.1-5.3).
//
// The legacy frontend implements input in two places: src/shell/MessageLoop.cpp
// decides *what starts and continues* (tool routing, drag state, capture) and
// the App methods it calls implement the operations.  This class ports the
// decision layer only: it is Qt Gui plus the model's Vec2, never a widget, App,
// HWND or HDC, and it reports every effect through InteractionHost so the
// canvas owns geometry, caches, model mutation and painting.
//
// Everything the router does is traceable to a legacy line; the important ones
// are cited at each decision below.  Chrome (toolbar/sidebar/popup clicks) is
// deliberately not routed here: the widget must offer those bands first, exactly
// as MessageLoop.cpp:189-211 does before tool routing.
//
// Not ported here (still in the frontend):
//   * Precision-touchpad/inertia routing (plan 5.4, src/platform/windows) - the
//     router consumes plain pointer samples; wheel intents are covered.
//   * RichEdit/QTextEdit editor internals (plan 5.5) and the LaTeX suggestion
//     popup (plan 5.6); the router only starts/ends edit sessions.
//   * Model-side selection containment/translation and PDF text selection
//     internals: the router emits the request, the host resolves objects.
//   * Pointer pressure/tilt: the legacy code ignores both.

enum class Tool {
    Select,
    Move,
    Text,
    Latex,
    Draw,
    Pan,
    Erase,
    Zoom,
};

// App::SelectionMode (App.h:765 defaults to Lasso; AppSettings.cpp:1219 restores
// it).  The box override used by click-selection is not a mode: see
// SelectionRequest::boxOverride.
enum class SelectionMode {
    Box,
    Lasso,
};

enum class CursorKind {
    Arrow,
    Cross,
    OpenHand,
    ClosedHand,
    Text,
    Eraser,
    Move,
    // The configured drawing cursor: the canvas builds it from the application
    // settings (dot diameter / precision crosshair), the reference
    // App::DrawCursorForCurrentSize (App.cpp:7465-7485).  It is not a system
    // shape, so it carries no Qt::CursorShape equivalent.
    Draw,
};

// App::ImageTransformHandle order (App.h).
enum class TransformHandle {
    Move,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Left,
    Right,
    Top,
    Bottom,
    Rotate,
};

enum class TransformTarget {
    Stroke,
    Text,
    Latex,
    Image,
};

enum class ObjectKind {
    Stroke,
    Image,
    Text,
    Latex,
};

struct ObjectRef {
    std::size_t pageIndex = 0;
    ObjectKind kind = ObjectKind::Stroke;
    std::size_t objectIndex = 0;

    friend bool operator==(const ObjectRef& a, const ObjectRef& b) {
        return a.pageIndex == b.pageIndex && a.kind == b.kind && a.objectIndex == b.objectIndex;
    }
};

enum class InteractionKind {
    None,
    Pan,
    Stroke,
    Erase,
    Select,
    MoveSelection,
    Transform,
    PendingTextClick,
    ZoomWindow,
};

enum class CancelReason {
    LostGrab,
    FocusLost,
    Escape,
};

// Plan step 5.1: the pointer sample the controller accepts.  No pressure/tilt.
struct PointerSample {
    QPointF position;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    quint64 timestamp = 0;
};

// The four stroke modes the legacy read straight from key state
// (App.cpp:1981-1995).  Shift and Ctrl also arrive in
// PointerSample::modifiers; 'A' and Space are keys, so the shell reports the
// full state here (from its key events) and calls SetModifiers() when any of
// them changes - that is the message loop's ExtendStroke-on-key path
// (MessageLoop.cpp:915-925 and 996-1006).
struct InteractionModifiers {
    bool straightLine = false;  // 'A' held         (App.cpp:1981)
    bool snapLine = false;      // Shift held       (App.cpp:1986)
    bool dotted = false;        // Space held       (App.cpp:1990)
    bool oval = false;          // Ctrl held        (App.cpp:1993)

    [[nodiscard]] static InteractionModifiers FromQtModifiers(Qt::KeyboardModifiers modifiers);
};

struct WheelSample {
    QPointF position;
    int angleDeltaY = 0;
    int angleDeltaX = 0;
    QPointF pixelDelta;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
};

struct PageHit {
    std::size_t pageIndex = 0;
    float scale = 1.0f;
};

struct TransformRequest {
    TransformTarget target = TransformTarget::Stroke;
    std::size_t pageIndex = 0;
    std::size_t objectIndex = 0;
    TransformHandle handle = TransformHandle::Move;
};

// App::BeginMoveSelection object hit test outcome (Selection.cpp:1453-1620):
// the host reports what sits under the cursor and whether it is selected.
struct SelectionHit {
    ObjectKind kind = ObjectKind::Stroke;
    std::size_t objectIndex = 0;
    bool selected = false;
    bool moveGrip = false;  // App::CursorHitsSelectionMoveGrip (Selection.cpp:711)
};

// Reference constants, all 96-DPI logical values.
inline constexpr double kSelectionClickRadius = 18.0;   // Selection.cpp:1269
inline constexpr double kLassoPointSpacing = 6.0;       // Selection.cpp:1252 (36.0 squared)
inline constexpr double kTextEditDragThreshold = 5.0;   // Selection.cpp:1652 (25.0 squared)
inline constexpr float kPendingEraseOpacity = 0.5f;     // App.cpp:172
inline constexpr int kWheelScrollLines = 3;             // MessageLoop.cpp:54 fallback
inline constexpr int kWheelScrollUnitPixels = 18;       // MessageLoop.cpp:57

// Stroke sampling tuning, read from the application settings in the legacy app
// (CanvasController.h CanvasInputSettings defaults).
struct InteractionTuning {
    float inputSpacing = 0.85f;
    float inputSmoothing = 0.3f;
};

// Effects the canvas/controller owns.  Every call is safe to ignore for a
// renderer-less test double, and the router never assumes ordering beyond the
// documented begin/update/commit triples.
class InteractionHost {
public:
    virtual ~InteractionHost() = default;

    // --- geometry ---------------------------------------------------------
    [[nodiscard]] virtual std::optional<PageHit> HitTestPage(const QPointF& logical) const = 0;
    [[nodiscard]] virtual Vec2 ScreenToPage(const QPointF& logical, bool clampToPage) const = 0;
    // App::CanvasRect containment (Selection.cpp:1227).
    [[nodiscard]] virtual bool IsPointInCanvas(const QPointF& logical) const = 0;

    // --- object/selection queries ----------------------------------------
    [[nodiscard]] virtual bool HasSelection() const = 0;
    // App::CursorInteractsWithSelection (Selection.cpp:823).
    [[nodiscard]] virtual bool CursorInteractsWithSelection(const QPointF& logical) const = 0;
    // App::HitTestText / HitTestLatex (Selection.cpp:397/414).
    [[nodiscard]] virtual std::optional<std::pair<std::size_t, std::size_t>> HitTestText(const QPointF& logical) const = 0;
    [[nodiscard]] virtual std::optional<std::pair<std::size_t, std::size_t>> HitTestLatex(const QPointF& logical) const = 0;
    // App::CursorHitsPdfText (Selection.cpp:680).
    [[nodiscard]] virtual std::optional<std::size_t> PageWithPdfTextAt(const QPointF& logical) const = 0;
    // App::CursorHitsSelectionMoveGrip + selected-object hit tests
    // (Selection.cpp:1446-1576).
    [[nodiscard]] virtual std::optional<SelectionHit> HitTestSelectionObject(const QPointF& logical, std::size_t pageIndex) const = 0;
    // App::BeginMoveSelection transform-handle probes, in the legacy order
    // stroke, image, text, latex (Selection.cpp:1468-1501).
    [[nodiscard]] virtual std::optional<TransformRequest> HitTestSelectionHandle(const QPointF& logical) const = 0;
    // App::EraseAt (App.cpp:5558) object collection for one sample.
    [[nodiscard]] virtual std::vector<ObjectRef> EraseTargetsAt(const QPointF& logical) const = 0;

    // --- viewport ---------------------------------------------------------
    virtual void PanBy(const QPointF& logicalDelta) = 0;
    virtual void ZoomByWheelSteps(int steps, const QPointF& anchor) = 0;
    virtual void ScrollBy(const QPointF& logicalDelta) = 0;
    virtual void SetViewPage(std::size_t pageIndex) = 0;
    virtual void SetCursor(CursorKind cursor) = 0;
    virtual void InvalidateCanvas() = 0;
    // App::UpdateCaptureState (App.cpp:5843-5861): the reference held capture
    // while an interaction was live and released it once finished.
    virtual void SetPointerCapture(bool captured) = 0;

    // --- stroke -----------------------------------------------------------
    // The host applies the current brush colour/width; `dotted` is the mode the
    // router tracked (App.cpp:1990).
    virtual void BeginStroke(std::size_t pageIndex, Vec2 pagePoint, bool dotted) = 0;
    // App::ApplyStrokePoint writes activeStroke_->style.dotted directly
    // (App.cpp:4515-4517) when only the Space mode changed.
    virtual void SetStrokeDotted(bool dotted) = 0;
    virtual void AppendStrokePoint(Vec2 pagePoint, bool dotted) = 0;
    // Straight-line/oval modes replace the whole point list (App.cpp:4532-4544).
    virtual void ReplaceStrokePoints(const std::vector<Vec2>& points, bool dotted) = 0;
    virtual void CommitStroke() = 0;
    virtual void DiscardStroke() = 0;

    // --- selection --------------------------------------------------------
    virtual void ClearSelection() = 0;
    virtual void UpdateSelectionOverlay(const QPointF& anchor, const QPointF& current, bool lasso, const std::vector<QPointF>& lassoPoints) = 0;
    // One call per completed selection gesture (App::EndSelection,
    // Selection.cpp:1268); `boxOverride` marks the click-selection path
    // (Selection.cpp:1581) and `click` the 18-px click radius (Selection.cpp:1269).
    virtual void CommitSelection(const QPointF& anchor, const QPointF& current, bool lasso, bool boxOverride, bool click) = 0;
    virtual void AddObjectToSelection(ObjectRef object) = 0;
    virtual void BeginMoveSelection(std::size_t pageIndex) = 0;
    virtual void UpdateMoveSelection(const QPointF& logicalDelta) = 0;
    virtual void CommitMoveSelection() = 0;

    // --- transforms -------------------------------------------------------
    virtual void BeginTransform(const TransformRequest& request) = 0;
    virtual void UpdateTransform(const QPointF& logical, Qt::KeyboardModifiers modifiers) = 0;
    virtual void CommitTransform() = 0;

    // --- erase ------------------------------------------------------------
    // Pending marks are shown at kPendingEraseOpacity; the whole drag becomes
    // one CommitErase call, i.e. one transaction (App::EndErase, App.cpp:5639).
    virtual void UpdatePendingErase(const std::vector<ObjectRef>& pending) = 0;
    virtual void CommitErase(const std::vector<ObjectRef>& pending) = 0;

    // --- editing sessions -------------------------------------------------
    virtual void BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) = 0;
    virtual void CreateTextAt(std::size_t pageIndex, Vec2 pagePoint) = 0;
    virtual void BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex) = 0;
    virtual void CreateLatexAt(std::size_t pageIndex, Vec2 pagePoint) = 0;
};

class InteractionRouter {
public:
    explicit InteractionRouter(InteractionHost& host, InteractionTuning tuning = {});

    void SetTool(Tool tool);
    [[nodiscard]] Tool ActiveTool() const { return tool_; }
    // The cursor a tool shows when no interaction overrides it: the reference
    // App::HandleMessage WM_SETCURSOR branches (MessageLoop.cpp:669-678: the
    // drawing tool gets App::DrawCursorForCurrentSize, the eraser its square
    // brush cursor, Pan SizeAll, everything else the arrow).  The canvas maps
    // the kind to a real cursor, because only it knows its device pixel ratio
    // and the configured cursor settings.
    [[nodiscard]] static CursorKind CursorForTool(Tool tool);
    void SetSelectionMode(SelectionMode mode);
    [[nodiscard]] SelectionMode ActiveSelectionMode() const { return selectionMode_; }
    void SetModifiers(InteractionModifiers modifiers);
    [[nodiscard]] InteractionModifiers Modifiers() const { return modifiers_; }
    void SetTuning(InteractionTuning tuning);

    void HandlePointerPress(const PointerSample& sample);
    void HandlePointerMove(const PointerSample& sample);
    void HandlePointerRelease(const PointerSample& sample);
    // Returns true when the event was consumed (ctrl-zoom or scrolling).
    bool HandleWheel(const WheelSample& sample);
    // Legacy focus-loss / lost-grab finishing (Session.cpp:95
    // FinishDocumentInteraction): commits the partial interaction and releases
    // capture.  Idempotent, so repeated focus-loss/deactivation/cancellation
    // notifications finish exactly once.
    void FinishInteraction();

    [[nodiscard]] bool IsInteractionActive() const;
    [[nodiscard]] InteractionKind ActiveInteraction() const { return active_; }
    [[nodiscard]] bool PanFromMiddleButton() const { return panFromMiddleButton_; }
    [[nodiscard]] const std::vector<ObjectRef>& PendingErase() const { return pendingErase_; }

    // Pure ports of the legacy stroke helpers, exposed for the tests and for
    // the canvas preview: App.cpp:1997 (SnapLineEndpoint), App.cpp:2013
    // (BuildOvalStrokePoints), App.cpp:2050 (InputSpacingPageUnits),
    // App.cpp:1958 (AppendStrokePointWithResampling).
    [[nodiscard]] static Vec2 SnapLineEndpoint(Vec2 start, Vec2 end);
    [[nodiscard]] static std::vector<Vec2> BuildOvalStrokePoints(Vec2 start, Vec2 end, bool circle);
    [[nodiscard]] static float InputSpacingPageUnits(float pageSpacing, float pageScale);
    static void AppendStrokePointWithResampling(std::vector<Vec2>& points, Vec2 point, float spacing);

private:
    // MessageLoop.cpp:189-246 (left) and 250-256 (middle) press routing.
    void BeginPressInteraction(const PointerSample& sample);
    void BeginSelectOrMove(const PointerSample& sample, bool moveTool);
    void BeginSelection(const QPointF& logical, bool boxOverride);
    void ExtendSelection(const PointerSample& sample);
    void EndSelection();
    void BeginMoveSelection(const QPointF& logical);
    void UpdateMoveSelection(const PointerSample& sample);
    void EndMoveSelection();
    void BeginErase(const QPointF& logical);
    void ExtendErase(const QPointF& logical);
    void EndErase(bool commit);
    void BeginStroke(const QPointF& logical);
    bool ApplyStrokePoint(const QPointF& logical);
    void EndStroke();
    void BeginPan(const QPointF& logical, bool fromMiddleButton);
    void UpdatePan(const QPointF& logical);
    void EndPan();
    void BeginTextTool(const QPointF& logical);
    void BeginLatexTool(const QPointF& logical);
    void UpdateCapture();
    void SetActive(InteractionKind kind);

    InteractionHost& host_;
    InteractionTuning tuning_;
    Tool tool_ = Tool::Draw;
    SelectionMode selectionMode_ = SelectionMode::Lasso;
    InteractionModifiers modifiers_;

    InteractionKind active_ = InteractionKind::None;

    bool panFromMiddleButton_ = false;
    QPointF panLastPosition_;

    std::optional<PageHit> strokePage_;
    std::vector<Vec2> strokePoints_;    // active points (line/oval modes replace them)
    std::vector<Vec2> freehandPoints_;  // unfiltered freehand accumulation (App.cpp:4497)
    bool strokeDotted_ = false;

    std::vector<ObjectRef> pendingErase_;

    QPointF selectionAnchor_;
    QPointF selectionCurrent_;
    std::vector<QPointF> selectionLassoPoints_;
    bool selectionBoxOverride_ = false;

    std::optional<std::size_t> movePageIndex_;
    QPointF moveLastPosition_;

    std::optional<std::size_t> pendingTextClickPage_;
    std::optional<std::size_t> pendingTextClickIndex_;
    QPointF pendingTextClickPosition_;

    bool pointerCaptured_ = false;
};

}  // namespace cnote::ui
