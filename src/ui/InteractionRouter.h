#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <QPointF>
#include <Qt>

#include "Document.h"
#include "selection/ScratchOut.h"

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
//   * Pointer tilt: the legacy code ignores it.  Pen pressure (which the legacy
//     ignored too) rides PointerSample into strokes.

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
    // Resize arrows along an edge or a corner diagonal: Vertical (top/bottom
    // edges), Horizontal (left/right), Diagonal (top-left/bottom-right) and
    // AntiDiagonal (top-right/bottom-left).
    ResizeVertical,
    ResizeHorizontal,
    ResizeDiagonal,
    ResizeAntiDiagonal,
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
    TextCreate,
};

enum class CancelReason {
    LostGrab,
    FocusLost,
    Escape,
};

// Plan step 5.1: the pointer sample the controller accepts.  No tilt.
struct PointerSample {
    QPointF position;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    quint64 timestamp = 0;
    // Pen pressure 0..1 from a tablet; empty for mouse input.
    std::optional<float> pressure;
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

// The fixed-width text box a text-tool drag would create, in page units.
struct TextBoxPreview {
    std::size_t pageIndex = 0;
    Vec2 topLeft {};
    float width = 0.0f;
};

struct TransformRequest {
    TransformTarget target = TransformTarget::Stroke;
    std::size_t pageIndex = 0;
    std::size_t objectIndex = 0;
    TransformHandle handle = TransformHandle::Move;
    // The press point in widget coordinates: rotation measures the pointer's
    // start angle from it.  Carried in the request so BeginTransform needs no
    // probe-time side channel.
    QPointF pressLogical {};
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
// Excalidraw TEXT_AUTOWRAP_THRESHOLD: how far a text-tool press must travel
// sideways before it sizes a wrapping text instead of creating a growing one
// (wider than a click's jitter, so fast clicks don't make narrow texts).
inline constexpr double kTextAutowrapThreshold = 36.0;
inline constexpr float kPendingEraseOpacity = 0.5f;     // App.cpp:172
inline constexpr int kWheelScrollLines = 3;             // MessageLoop.cpp:54 fallback
inline constexpr int kWheelScrollUnitPixels = 18;       // MessageLoop.cpp:57

// Stroke tuning (settings input_spacing / input_smoothing / scratch_to_erase).
// The router is the only place pointer samples are stabilized and resampled;
// the controller stores the points it emits verbatim.
struct InteractionTuning {
    float inputSpacing = 0.85f;
    float inputSmoothing = 0.3f;
    // A freehand zigzag over objects erases them (RecognizeScratchOut).
    bool scratchToErase = true;
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
    // Maps a widget point through a specific page's view; used mid-stroke
    // when the pointer sits between pages and HitTestPage/ScreenToPage have
    // no page to convert through (there it would return {0,0} and spike the
    // stroke to the page origin).
    [[nodiscard]] virtual Vec2 ScreenToPageOnPage(std::size_t pageIndex, const QPointF& logical, bool clampToPage) const = 0;
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
    // App::EraseAt (App.cpp:5713-5799) object collection for one sample: every
    // object on `pageIndex` the swept circle along `start`->`end` (page units)
    // touches.  `start` equals `end` for the first sample of a drag or after a
    // page change.
    [[nodiscard]] virtual std::vector<ObjectRef> EraseTargetsAt(std::size_t pageIndex, Vec2 start, Vec2 end) const = 0;

    // --- viewport ---------------------------------------------------------
    virtual void PanBy(const QPointF& logicalDelta) = 0;
    virtual void ZoomByWheelSteps(int steps, const QPointF& anchor) = 0;
    virtual void ScrollBy(const QPointF& logicalDelta) = 0;
    virtual void SetCursor(CursorKind cursor) = 0;
    virtual void InvalidateCanvas() = 0;
    // App::UpdateCaptureState (App.cpp:5843-5861): the reference held capture
    // while an interaction was live and released it once finished.
    virtual void SetPointerCapture(bool captured) = 0;

    // --- stroke -----------------------------------------------------------
    // The host applies the current brush colour/width; `dotted` is the mode the
    // router tracked (App.cpp:1990) and `sampling` the stabilization the stroke
    // replays at render time (the tuning at the page's current scale).  A
    // stroke begun with a `pressure` records one per point (pressure width).
    virtual void BeginStroke(std::size_t pageIndex, Vec2 pagePoint, bool dotted, StrokeSampling sampling,
        std::optional<float> pressure) = 0;
    // App::ApplyStrokePoint writes activeStroke_->style.dotted directly
    // (App.cpp:4515-4517) when only the Space mode changed.
    virtual void SetStrokeDotted(bool dotted) = 0;
    // One raw pointer sample, in page units, with its pen pressure if any.
    virtual void AppendStrokePoint(Vec2 pagePoint, bool dotted, std::optional<float> pressure) = 0;
    // Straight-line/oval modes replace the whole point list (App.cpp:4532-4544).
    virtual void ReplaceStrokePoints(const std::vector<Vec2>& points, bool dotted) = 0;
    virtual void CommitStroke() = 0;
    virtual void DiscardStroke() = 0;

    // --- selection --------------------------------------------------------
    virtual void ClearSelection() = 0;
    virtual void UpdateSelectionOverlay(const QPointF& anchor, const QPointF& current, bool lasso, const std::vector<QPointF>& lassoPoints) = 0;
    // Drops the live marquee/lasso/pdf-text overlay without committing it.
    virtual void CancelSelectionOverlay() = 0;
    // One call per completed selection gesture (App::EndSelection,
    // Selection.cpp:1268); `boxOverride` marks the click-selection path
    // (Selection.cpp:1581) and `click` the 18-px click radius (Selection.cpp:1269).
    // `additive` (Shift held at the press) adds the result to the selection
    // instead of replacing it.
    virtual void CommitSelection(const QPointF& anchor, const QPointF& current, bool lasso, bool boxOverride, bool click,
        bool additive) = 0;
    virtual void AddObjectToSelection(ObjectRef object) = 0;
    // Shift-click on an object: select it, or deselect it when selected.
    virtual void ToggleObjectInSelection(ObjectRef object) = 0;
    virtual void BeginMoveSelection(std::size_t pageIndex) = 0;
    // Alt-press variant: the host duplicates the selection inside the move
    // gesture so the drag carries the clones, not the originals.
    virtual void BeginCloneMoveSelection(std::size_t pageIndex) = 0;
    virtual void UpdateMoveSelection(const QPointF& logicalDelta) = 0;
    virtual void CommitMoveSelection() = 0;
    // Abandons an in-flight move: restores the page snapshot the gesture
    // began with and drops the drag overlay.
    virtual void CancelMoveSelection() = 0;

    // --- transforms -------------------------------------------------------
    // Starts the one-transaction gesture and stores the transform session.
    // Returns false when no session could start (no selection bounds), in
    // which case the router does not enter Transform.
    virtual bool BeginTransform(const TransformRequest& request) = 0;
    virtual void UpdateTransform(const QPointF& logical, Qt::KeyboardModifiers modifiers) = 0;
    virtual void CommitTransform() = 0;
    // Abandons an in-flight transform, restoring the page snapshot.
    virtual void CancelTransform() = 0;

    // --- erase ------------------------------------------------------------
    // Pending marks are shown at kPendingEraseOpacity; the whole drag becomes
    // one CommitErase call, i.e. one transaction (App::EndErase, App.cpp:5639).
    virtual void UpdatePendingErase(const std::vector<ObjectRef>& pending) = 0;
    virtual void CommitErase(const std::vector<ObjectRef>& pending) = 0;

    // --- scratch-out ------------------------------------------------------
    // The objects on `pageIndex` the scratch covers (ScratchOut::CoversPath /
    // CoversArea); the live stroke itself is not on the page yet.  While the
    // stroke is live the router shows them through UpdatePendingErase, like
    // the eraser's pending marks.
    [[nodiscard]] virtual std::vector<ObjectRef> ScratchOutTargets(std::size_t pageIndex, const ScratchOut& scratch) const = 0;
    // Drops the live stroke and erases `targets` as one history action.
    virtual void CommitScratchOut(const std::vector<ObjectRef>& targets) = 0;

    // --- editing sessions -------------------------------------------------
    virtual void BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) = 0;
    // `width` 0 creates a text that grows with its content; anything else a
    // text that wraps at that width.
    virtual void CreateTextAt(std::size_t pageIndex, Vec2 pagePoint, float width) = 0;
    // The box a text-tool drag is sizing; nullopt drops it.
    virtual void UpdateTextCreatePreview(std::optional<TextBoxPreview> preview) = 0;
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
    // The cursor over a selection transform handle, and while dragging it
    // (Excalidraw's getCursorForResizingElement): resize arrows along the
    // handle's axis, a hand for the rotate handle that closes while rotating.
    [[nodiscard]] static CursorKind CursorForHandle(TransformHandle handle, bool dragging);
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
    // Abandons the live interaction without committing it: a stroke is
    // discarded, pending erase marks are cleared, page gestures are rolled
    // back and marquee/lasso overlays are hidden.  Idempotent.
    void CancelInteraction();
    // Hold-to-snap: replaces the live freehand stroke with the shape
    // RecognizeShape fits to it, perfecting circles and triangles while Shift
    // (snapLine) is held.  The canvas calls this once the pointer has rested mid-stroke;
    // returns false (stroke unchanged) when no stroke is live, the
    // straight-line/oval mode owns it, it already snapped, or no shape fits.
    // A snapped stroke ignores further movement until release; toggling
    // Shift before then re-snaps it.
    bool SnapStrokeToShape();

    [[nodiscard]] bool IsInteractionActive() const;
    [[nodiscard]] InteractionKind ActiveInteraction() const { return state_.kind; }
    [[nodiscard]] bool PanFromMiddleButton() const { return state_.pan.fromMiddleButton; }
    [[nodiscard]] const std::vector<ObjectRef>& PendingErase() const { return state_.erase.pending; }

    // Pure ports of the legacy stroke helpers, exposed for the tests and for
    // the canvas preview: App.cpp:1997 (SnapLineEndpoint), App.cpp:2013
    // (BuildOvalStrokePoints), App.cpp:2050 (InputSpacingPageUnits).  The
    // App.cpp:1958 filter/resample now replays at render time
    // (BuildStabilizedPath).
    [[nodiscard]] static Vec2 SnapLineEndpoint(Vec2 start, Vec2 end);
    [[nodiscard]] static std::vector<Vec2> BuildOvalStrokePoints(Vec2 start, Vec2 end, bool circle);
    [[nodiscard]] static float InputSpacingPageUnits(float pageSpacing, float pageScale);

private:
    // MessageLoop.cpp:189-246 (left) and 250-256 (middle) press routing.
    void BeginPressInteraction(const PointerSample& sample);
    void BeginSelectOrMove(const PointerSample& sample, bool moveTool);
    // Shift press under Select/Move: toggles the object under the pointer or
    // starts an additive marquee/lasso; never moves or replaces the selection.
    void BeginShiftSelection(const PointerSample& sample, bool moveTool);
    // Starts a transform when `logical` is on a selection handle, on or off
    // the page; false when there is none or the host refused it.
    bool TryBeginTransform(const QPointF& logical);
    void BeginSelection(const QPointF& logical, bool boxOverride, bool additive = false);
    void ExtendSelection(const PointerSample& sample);
    void EndSelection();
    void BeginMoveSelection(const PointerSample& sample);
    void UpdateMoveSelection(const PointerSample& sample);
    void EndMoveSelection();
    void BeginErase(const QPointF& logical);
    void ExtendErase(const QPointF& logical);
    void EndErase(bool commit);
    void BeginStroke(const QPointF& logical);
    bool ApplyStrokePoint(const QPointF& logical);
    void EndStroke();
    // Scratch-out preview: recognizes the live freehand stroke and, when it
    // is a zigzag over objects, shows them as pending erase marks (cleared
    // when it stops being one).  EndStroke erases the marked objects instead
    // of committing the stroke.
    void RefreshScratchOutPreview();
    // Recognizes the live stroke's freehand points under the current Shift
    // state and replaces the stroke with the shape; false when none fits.
    bool ApplyShapeSnap();
    // Starts one stroke segment on `hit`, resetting the host preview.  The
    // interaction stays Stroke; a page crossing commits the previous segment
    // and starts this one without releasing capture.
    void StartStrokeSegment(const PageHit& hit, Vec2 pagePoint, bool dotted);
    void BeginPan(const QPointF& logical, bool fromMiddleButton);
    void UpdatePan(const QPointF& logical);
    void EndPan();
    void BeginTextTool(const QPointF& logical);
    void UpdateTextCreate(const QPointF& logical);
    void EndTextCreate(bool create);
    void BeginLatexTool(const QPointF& logical);
    void UpdateCapture();
    // The only transition back to None: forgets every payload, so no ending
    // path can leave another kind's data behind.
    void ResetInteraction();

    // The router owns exactly one interaction value.  Only the payload for
    // state_.kind is meaningful; ResetInteraction clears them all.
    struct InteractionState {
        struct PanData {
            bool fromMiddleButton = false;
            QPointF lastPosition {};
        };
        struct StrokeData {
            std::optional<PageHit> page;
            // Replacement geometry for line/oval/snap modes; empty for plain
            // freehand, whose authoritative raw samples live in `scratch`.
            std::vector<Vec2> points;
            // Raw freehand samples (App.cpp:4497) with incremental scratch-out
            // recognition state.
            ScratchOutTracker scratch;
            bool dotted = false;
            bool snapped = false;        // SnapStrokeToShape replaced the points
            // Objects the stroke would scratch out if it ended now; shown as
            // pending erase marks.
            std::vector<ObjectRef> scratchTargets;
        };
        struct EraseData {
            std::vector<ObjectRef> pending;
            // App.cpp:5713-5799 EraseAt's lastErasePageIndex_/lastErasePagePoint_:
            // the segment start for the next sample, reset when the cursor
            // leaves the page.
            std::optional<std::size_t> lastPageIndex;
            Vec2 lastPagePoint {};
        };
        struct SelectData {
            QPointF anchor {};
            QPointF current {};
            std::vector<QPointF> lassoPoints;
            bool boxOverride = false;
            bool additive = false;
        };
        struct MoveData {
            QPointF lastPosition {};
        };
        struct PendingTextClickData {
            std::optional<std::size_t> pageIndex;
            std::optional<std::size_t> textIndex;
            QPointF position {};
        };
        // A text-tool press on the page, decided on release: a click creates a
        // growing text at `anchor`; a drag past kTextAutowrapThreshold sizes a
        // wrapping one from `anchor` towards the pointer (Excalidraw's
        // dragNewTextElement, which never turns back into a growing one).
        struct TextCreateData {
            std::size_t pageIndex = 0;
            QPointF origin {};
            Vec2 anchor {};
            bool sized = false;
            Vec2 topLeft {};
            float width = 0.0f;
        };

        InteractionKind kind = InteractionKind::None;
        PanData pan;
        StrokeData stroke;
        EraseData erase;
        SelectData select;
        MoveData move;
        PendingTextClickData pendingTextClick;
        TextCreateData textCreate;
    };

    InteractionHost& host_;
    InteractionTuning tuning_;
    // Pen pressure of the sample being handled (empty for mouse input).
    std::optional<float> samplePressure_;
    Tool tool_ = Tool::Draw;
    SelectionMode selectionMode_ = SelectionMode::Lasso;
    InteractionModifiers modifiers_;

    InteractionState state_;
    bool pointerCaptured_ = false;
};

}  // namespace cnote::ui
