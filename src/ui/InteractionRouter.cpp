#include "ui/InteractionRouter.h"

#include <algorithm>
#include <cmath>

namespace cnote::ui {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSnapAngleRadians = kPi / 12.0f;      // App.cpp:2000
constexpr int kOvalSegmentCount = 72;                 // App.cpp:2014
constexpr int kMaxInterpolatedPoints = 64;            // App.cpp:1964
constexpr float kMinScreenSpacing = 1.75f;            // App.cpp:2051
constexpr float kMaxScreenSpacing = 3.0f;             // App.cpp:2052
constexpr int kWheelDeltaUnits = 120;                 // Win32 WHEEL_DELTA

float SquaredDistance(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

float DistanceBetween(Vec2 a, Vec2 b) {
    return std::sqrt(SquaredDistance(a, b));
}

Vec2 LerpPoint(Vec2 a, Vec2 b, float t) {
    return { a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t) };
}

float SquaredDistance(const QPointF& a, const QPointF& b) {
    const qreal dx = a.x() - b.x();
    const qreal dy = a.y() - b.y();
    return static_cast<float>((dx * dx) + (dy * dy));
}

}  // namespace

InteractionModifiers InteractionModifiers::FromQtModifiers(Qt::KeyboardModifiers modifiers) {
    InteractionModifiers result;
    result.snapLine = modifiers.testFlag(Qt::ShiftModifier);   // App.cpp:1986
    result.oval = modifiers.testFlag(Qt::ControlModifier);     // App.cpp:1993
    // 'A' (straight line) and Space (dotted) are keys, not modifiers: the shell
    // reports them through SetModifiers from its key events.
    return result;
}

Vec2 InteractionRouter::SnapLineEndpoint(Vec2 start, Vec2 end) {
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float distance = std::sqrt((dx * dx) + (dy * dy));
    if (distance <= 0.001f) {
        return end;
    }

    const float snappedAngle = std::round(std::atan2(dy, dx) / kSnapAngleRadians) * kSnapAngleRadians;
    return {
        start.x + (std::cos(snappedAngle) * distance),
        start.y + (std::sin(snappedAngle) * distance),
    };
}

std::vector<Vec2> InteractionRouter::BuildOvalStrokePoints(Vec2 start, Vec2 end, bool circle) {
    float endX = end.x;
    float endY = end.y;
    if (circle) {
        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float side = std::max(std::abs(dx), std::abs(dy));
        endX = start.x + (dx < 0.0f ? -side : side);
        endY = start.y + (dy < 0.0f ? -side : side);
    }

    const float left = std::min(start.x, endX);
    const float top = std::min(start.y, endY);
    const float right = std::max(start.x, endX);
    const float bottom = std::max(start.y, endY);
    const float radiusX = (right - left) * 0.5f;
    const float radiusY = (bottom - top) * 0.5f;
    if (radiusX <= 0.001f || radiusY <= 0.001f) {
        return { start, { endX, endY } };
    }

    const float centerX = left + radiusX;
    const float centerY = top + radiusY;
    std::vector<Vec2> points;
    points.reserve(kOvalSegmentCount + 1U);
    for (int index = 0; index <= kOvalSegmentCount; ++index) {
        const float angle = (static_cast<float>(index) / static_cast<float>(kOvalSegmentCount)) * 2.0f * kPi;
        points.push_back({
            centerX + (std::cos(angle) * radiusX),
            centerY + (std::sin(angle) * radiusY),
        });
    }
    return points;
}

float InteractionRouter::InputSpacingPageUnits(float pageSpacing, float pageScale) {
    const float scale = std::max(0.1f, pageScale);
    const float screenSpacing = std::clamp(std::max(0.05f, pageSpacing) * scale, kMinScreenSpacing, kMaxScreenSpacing);
    return screenSpacing / scale;
}

void InteractionRouter::AppendStrokePointWithResampling(std::vector<Vec2>& points, Vec2 point, float spacing) {
    if (points.empty()) {
        points.push_back(point);
        return;
    }

    const float minSpacing = std::max(0.05f, spacing);
    const Vec2 from = points.back();
    const float distance = DistanceBetween(from, point);
    if (distance < minSpacing) {
        return;
    }

    const int segmentCount = std::min(
        kMaxInterpolatedPoints + 1,
        std::max(1, static_cast<int>(std::ceil(distance / minSpacing))));
    for (int segment = 1; segment <= segmentCount; ++segment) {
        const float t = static_cast<float>(segment) / static_cast<float>(segmentCount);
        points.push_back(LerpPoint(from, point, t));
    }
}

InteractionRouter::InteractionRouter(InteractionHost& host, InteractionTuning tuning)
    : host_(host), tuning_(tuning) {}

void InteractionRouter::SetTool(Tool tool) {
    tool_ = tool;
}

CursorKind InteractionRouter::CursorForTool(Tool tool) {
    switch (tool) {
    case Tool::Draw:
        return CursorKind::Draw;
    case Tool::Erase:
        return CursorKind::Eraser;
    case Tool::Pan:
        return CursorKind::OpenHand;
    case Tool::Select:
    case Tool::Move:
    case Tool::Text:
    case Tool::Latex:
    case Tool::Zoom:
    default:
        return CursorKind::Arrow;
    }
}

void InteractionRouter::SetSelectionMode(SelectionMode mode) {
    selectionMode_ = mode;
}

void InteractionRouter::SetModifiers(InteractionModifiers modifiers) {
    modifiers_ = modifiers;
}

void InteractionRouter::SetTuning(InteractionTuning tuning) {
    tuning_ = tuning;
}

bool InteractionRouter::IsInteractionActive() const {
    // History.cpp:275 App::IsDocumentInteractionActive, plus the LaTeX transform
    // that Session.cpp:121 adds on top of it.
    return active_ != InteractionKind::None;
}

void InteractionRouter::SetActive(InteractionKind kind) {
    active_ = kind;
}

void InteractionRouter::UpdateCapture() {
    // App::UpdateCaptureState (App.cpp:5843-5861): the reference held capture
    // exactly while one of these interactions was live.
    const bool shouldCapture = active_ != InteractionKind::None;
    if (shouldCapture == pointerCaptured_) {
        return;
    }
    pointerCaptured_ = shouldCapture;
    host_.SetPointerCapture(shouldCapture);
}

void InteractionRouter::HandlePointerPress(const PointerSample& sample) {
    // MessageLoop.cpp:250-256: the middle button pans with any active tool and
    // takes precedence over the left-button routing.
    if (sample.button == Qt::MiddleButton) {
        BeginPan(sample.position, true);
        return;
    }
    if (sample.button != Qt::LeftButton) {
        // Right button: chrome context menus only (MessageLoop.cpp:257-266); the
        // widget handles that band before offering the event to the router.
        return;
    }
    if (active_ != InteractionKind::None) {
        // A second press cannot arrive for the same pointer while one is active;
        // ignore rather than stacking interactions.
        return;
    }
    BeginPressInteraction(sample);
}

void InteractionRouter::BeginPressInteraction(const PointerSample& sample) {
    // MessageLoop.cpp:209-243 tool routing.
    switch (tool_) {
    case Tool::Pan:
        BeginPan(sample.position, false);
        return;
    case Tool::Move:
        if (host_.PageWithPdfTextAt(sample.position).has_value()) {
            if (host_.HasSelection()) {
                host_.ClearSelection();
            }
            BeginSelection(sample.position, false);
            return;
        }
        BeginSelectOrMove(sample, true);
        return;
    case Tool::Select:
        if (host_.HasSelection() && host_.CursorInteractsWithSelection(sample.position)) {
            BeginMoveSelection(sample.position);
            return;
        }
        if (host_.HasSelection()) {
            host_.ClearSelection();
        }
        BeginSelection(sample.position, false);
        return;
    case Tool::Text:
        BeginTextTool(sample.position);
        return;
    case Tool::Latex:
        BeginLatexTool(sample.position);
        return;
    case Tool::Erase:
        BeginErase(sample.position);
        return;
    case Tool::Draw:
    case Tool::Zoom:
        // The reference press dispatch has no Zoom branch, so the zoom tool
        // falls through to the stroke path (MessageLoop.cpp:241-243).
        BeginStroke(sample.position);
        return;
    }
}

void InteractionRouter::HandlePointerMove(const PointerSample& sample) {
    // MessageLoop.cpp:333-336: an in-flight pan (or a held middle button) wins
    // over every other active interaction.
    if (active_ == InteractionKind::Pan || sample.buttons.testFlag(Qt::MiddleButton)) {
        UpdatePan(sample.position);
        return;
    }

    switch (active_) {
    case InteractionKind::Select:
        ExtendSelection(sample);
        return;
    case InteractionKind::Transform:
        host_.UpdateTransform(sample.position, sample.modifiers);
        host_.InvalidateCanvas();
        return;
    case InteractionKind::MoveSelection:
    case InteractionKind::PendingTextClick:
        UpdateMoveSelection(sample);
        return;
    case InteractionKind::Stroke:
        if (ApplyStrokePoint(sample.position)) {
            host_.InvalidateCanvas();
        }
        return;
    case InteractionKind::Erase:
        ExtendErase(sample.position);
        return;
    case InteractionKind::Pan:
    case InteractionKind::ZoomWindow:
    case InteractionKind::None:
        return;
    }
}

void InteractionRouter::HandlePointerRelease(const PointerSample& sample) {
    // MessageLoop.cpp:487-490: the middle button ends only a middle-button pan.
    if (sample.button == Qt::MiddleButton) {
        if (active_ == InteractionKind::Pan && panFromMiddleButton_) {
            EndPan();
        }
        return;
    }

    // MessageLoop.cpp:398-485 release order.
    switch (active_) {
    case InteractionKind::Stroke: {
        // MessageLoop.cpp:451-458: releasing outside the canvas (or over another
        // page) still extends the active segment first.
        const std::optional<PageHit> hit = host_.HitTestPage(sample.position);
        if (!hit.has_value() || (strokePage_.has_value() && hit->pageIndex == strokePage_->pageIndex)) {
            static_cast<void>(ApplyStrokePoint(sample.position));
        }
        EndStroke();
        return;
    }
    case InteractionKind::Transform:
        host_.CommitTransform();
        SetActive(InteractionKind::None);
        UpdateCapture();
        host_.InvalidateCanvas();
        return;
    case InteractionKind::MoveSelection:
        EndMoveSelection();
        return;
    case InteractionKind::PendingTextClick: {
        // MessageLoop.cpp:467-478: the click finally opens the text editor.
        const std::optional<std::size_t> pageIndex = pendingTextClickPage_;
        const std::optional<std::size_t> textIndex = pendingTextClickIndex_;
        pendingTextClickPage_.reset();
        pendingTextClickIndex_.reset();
        SetActive(InteractionKind::None);
        UpdateCapture();
        if (pageIndex.has_value() && textIndex.has_value()) {
            host_.BeginTextEdit(*pageIndex, *textIndex);
        }
        host_.InvalidateCanvas();
        return;
    }
    case InteractionKind::Select:
        EndSelection();
        return;
    case InteractionKind::Erase:
        EndErase(true);
        return;
    case InteractionKind::Pan:
        if (!panFromMiddleButton_) {
            EndPan();
        }
        return;
    case InteractionKind::ZoomWindow:
    case InteractionKind::None:
        return;
    }
}

bool InteractionRouter::HandleWheel(const WheelSample& sample) {
    // MessageLoop.cpp:492-540 and Viewport.cpp:675 ZoomAt.
    if (sample.modifiers.testFlag(Qt::ControlModifier)) {
        if (sample.angleDeltaY != 0) {
            host_.ZoomByWheelSteps(sample.angleDeltaY / kWheelDeltaUnits, sample.position);
            return true;
        }
        return false;
    }

    const auto scrollPixels = [](int delta) {
        // MessageLoop.cpp:41-62 WheelScrollPixels with the default three scroll
        // lines and the 96-DPI 18-pixel unit.
        return (static_cast<qreal>(delta) / static_cast<qreal>(kWheelDeltaUnits))
            * static_cast<qreal>(kWheelScrollLines)
            * static_cast<qreal>(kWheelScrollUnitPixels);
    };

    if (!sample.pixelDelta.isNull()) {
        // Precision devices deliver pixel deltas that are already smooth and
        // carry the OS momentum: Qt's contract is "use the value directly to
        // scroll content on screen" (QWheelEvent::pixelDelta), so the canvas
        // pans 1:1 in this event and never re-animates or re-smooths it
        // (plan step 5.3).  The sign convention is pixelDelta's own, i.e. the
        // same one angleDelta uses: +y scrolls the content down, +x moves it
        // right, which is what the canvas pan does with the delta as-is.
        host_.PanBy(sample.pixelDelta);
        return true;
    }
    if (sample.angleDeltaY != 0) {
        // One notch is 54 logical pixels (3 lines x 18) and keeps the reference
        // 90 ms smooth scroll; the canvas animates this path only.
        host_.ScrollBy(QPointF(0.0, scrollPixels(sample.angleDeltaY)));
        return true;
    }
    if (sample.angleDeltaX != 0) {
        // Qt negates WM_MOUSEHWHEEL's delta (Qt 6.11
        // qwindowspointerhandler.cpp:748-752: "Qt horizontal wheel rotation
        // orientation is opposite to the one in WM_MOUSEHWHEEL"), while the
        // reference panned with the raw Win32 delta
        // (MessageLoop.cpp:539 SmoothPanBy(-WheelScrollPixels(wheelDelta), 0)).
        // Reproducing that motion in Qt's convention therefore negates the
        // negation: pan x by +scrollPixels(angleDelta().x()).
        host_.ScrollBy(QPointF(scrollPixels(sample.angleDeltaX), 0.0));
        return true;
    }
    return false;
}

void InteractionRouter::FinishInteraction() {
    // Session.cpp:95-108 App::FinishDocumentInteraction, in the same order.
    // Idempotent: every step below is a no-op once its interaction ended, so a
    // repeated lost-grab/focus-loss notification finishes exactly once.
    if (active_ == InteractionKind::Stroke) {
        EndStroke();
    }
    if (active_ == InteractionKind::Erase) {
        EndErase(true);
    }
    if (active_ == InteractionKind::MoveSelection) {
        EndMoveSelection();
    }
    if (active_ == InteractionKind::Transform) {
        host_.CommitTransform();
        SetActive(InteractionKind::None);
    }
    // The shell commits its text/LaTeX editors separately; the reference clears
    // the pending click before ending the selection.
    pendingTextClickPage_.reset();
    pendingTextClickIndex_.reset();
    if (active_ == InteractionKind::PendingTextClick) {
        SetActive(InteractionKind::None);
    }
    if (active_ == InteractionKind::Select) {
        EndSelection();
    }
    if (active_ == InteractionKind::Pan) {
        EndPan();
    }
    UpdateCapture();
}

// ---------------------------------------------------------------- pan -------

void InteractionRouter::BeginPan(const QPointF& logical, bool fromMiddleButton) {
    // App.cpp:5810-5823 BeginPan.
    if (active_ == InteractionKind::Stroke) {
        EndStroke();
    }
    if (!fromMiddleButton && host_.HasSelection()) {
        host_.ClearSelection();
    }
    SetActive(InteractionKind::Pan);
    panFromMiddleButton_ = fromMiddleButton;
    panLastPosition_ = logical;
    host_.SetCursor(CursorKind::ClosedHand);
    UpdateCapture();
    host_.InvalidateCanvas();
}

void InteractionRouter::UpdatePan(const QPointF& logical) {
    if (active_ != InteractionKind::Pan) {
        return;
    }
    const QPointF delta(logical.x() - panLastPosition_.x(), logical.y() - panLastPosition_.y());
    panLastPosition_ = logical;
    if (delta.isNull()) {
        return;
    }
    host_.PanBy(delta);
}

void InteractionRouter::EndPan() {
    SetActive(InteractionKind::None);
    panFromMiddleButton_ = false;
    // The pan's closed hand is replaced by whatever the active tool shows
    // (MessageLoop.cpp:643-647 puts the SizeAll cursor back only for the pan
    // states; every other tool falls through to its own cursor).
    host_.SetCursor(CursorForTool(tool_));
    UpdateCapture();
    host_.InvalidateCanvas();
}

// -------------------------------------------------------------- stroke -----

void InteractionRouter::BeginStroke(const QPointF& logical) {
    // App.cpp:4453-4466 BeginStroke.
    if (active_ == InteractionKind::Pan || active_ == InteractionKind::Erase) {
        return;
    }
    const std::optional<PageHit> hit = host_.HitTestPage(logical);
    if (!hit.has_value()) {
        return;
    }
    if (host_.HasSelection()) {
        host_.ClearSelection();
    }
    host_.SetViewPage(hit->pageIndex);

    strokePage_ = hit;
    strokeDotted_ = modifiers_.dotted;
    strokePoints_.clear();
    strokePoints_.push_back(host_.ScreenToPage(logical, true));
    freehandPoints_ = strokePoints_;
    SetActive(InteractionKind::Stroke);
    UpdateCapture();
    host_.BeginStroke(hit->pageIndex, strokePoints_.front(), strokeDotted_);
    host_.InvalidateCanvas();
}

bool InteractionRouter::ApplyStrokePoint(const QPointF& logical) {
    // App.cpp:4476-4560 ApplyStrokePoint.
    if (active_ != InteractionKind::Stroke || !strokePage_.has_value()) {
        return false;
    }
    const float pageScale = strokePage_->scale;
    if (pageScale <= 0.0f) {
        return false;
    }

    // Page-boundary segmentation: finish the current page's stroke and start a
    // new segment on the page under the pointer (App.cpp:4490-4496).
    if (const std::optional<PageHit> hit = host_.HitTestPage(logical); hit.has_value() && hit->pageIndex != strokePage_->pageIndex) {
        EndStroke();
        host_.SetViewPage(hit->pageIndex);
        strokePage_ = hit;
        strokePoints_.clear();
        strokePoints_.push_back(host_.ScreenToPage(logical, true));
        freehandPoints_ = strokePoints_;
        SetActive(InteractionKind::Stroke);
        UpdateCapture();
        host_.BeginStroke(hit->pageIndex, strokePoints_.front(), strokeDotted_);
        host_.InvalidateCanvas();
        return true;
    }

    const Vec2 rawPoint = host_.ScreenToPage(logical, true);
    const float inputSpacing = InputSpacingPageUnits(tuning_.inputSpacing, pageScale);
    const bool straightLineMode = modifiers_.straightLine;

    if (freehandPoints_.empty()) {
        freehandPoints_ = strokePoints_;
    }

    const bool dottedMode = modifiers_.dotted;
    const bool styleChanged = strokeDotted_ != dottedMode;
    strokeDotted_ = dottedMode;
    if (styleChanged) {
        host_.SetStrokeDotted(dottedMode);
    }

    bool pointAppended = true;
    Vec2 filteredPoint = rawPoint;
    if (!freehandPoints_.empty()) {
        const float followAmount = std::clamp(1.0f - tuning_.inputSmoothing, 0.05f, 0.95f);
        filteredPoint = LerpPoint(freehandPoints_.back(), rawPoint, followAmount);
        if (SquaredDistance(filteredPoint, freehandPoints_.back()) < (inputSpacing * inputSpacing)) {
            pointAppended = false;
        }
    }

    if (pointAppended) {
        const std::size_t previousCount = freehandPoints_.size();
        AppendStrokePointWithResampling(freehandPoints_, filteredPoint, inputSpacing);
        for (std::size_t index = previousCount; index < freehandPoints_.size(); ++index) {
            host_.AppendStrokePoint(freehandPoints_[index], dottedMode);
        }
    }

    if (straightLineMode && !freehandPoints_.empty()) {
        const Vec2 startPoint = freehandPoints_.front();
        if (modifiers_.oval) {
            strokePoints_ = BuildOvalStrokePoints(startPoint, rawPoint, modifiers_.snapLine);
        } else {
            const Vec2 endPoint = modifiers_.snapLine ? SnapLineEndpoint(startPoint, rawPoint) : rawPoint;
            strokePoints_.clear();
            strokePoints_.push_back(startPoint);
            strokePoints_.push_back(endPoint);
        }
        host_.ReplaceStrokePoints(strokePoints_, dottedMode);
        return true;
    }

    if (!pointAppended && !styleChanged) {
        return false;
    }
    strokePoints_ = freehandPoints_;
    return true;
}

void InteractionRouter::EndStroke() {
    // App.cpp:4794-4870: one committed stroke segment, unless nothing was drawn.
    if (active_ == InteractionKind::Stroke) {
        if (strokePoints_.empty()) {
            host_.DiscardStroke();
        } else {
            host_.CommitStroke();
        }
    }
    SetActive(InteractionKind::None);
    strokePage_.reset();
    strokePoints_.clear();
    freehandPoints_.clear();
    strokeDotted_ = false;
    UpdateCapture();
    host_.InvalidateCanvas();
}

// -------------------------------------------------------------- erase ------

void InteractionRouter::BeginErase(const QPointF& logical) {
    // App.cpp:5539-5552 BeginErase.
    if (active_ == InteractionKind::Pan) {
        return;
    }
    if (!host_.HitTestPage(logical).has_value()) {
        return;
    }
    if (host_.HasSelection()) {
        host_.ClearSelection();
    }
    pendingErase_.clear();
    SetActive(InteractionKind::Erase);
    UpdateCapture();
    ExtendErase(logical);
}

void InteractionRouter::ExtendErase(const QPointF& logical) {
    // App.cpp:5558-5562 / EraseAt: accumulate the objects under the pointer; the
    // pending set renders at kPendingEraseOpacity until one commit.
    if (active_ != InteractionKind::Erase) {
        return;
    }
    bool changed = false;
    for (const ObjectRef& target : host_.EraseTargetsAt(logical)) {
        if (std::find(pendingErase_.begin(), pendingErase_.end(), target) == pendingErase_.end()) {
            pendingErase_.push_back(target);
            changed = true;
        }
    }
    if (changed) {
        host_.UpdatePendingErase(pendingErase_);
        host_.InvalidateCanvas();
    }
}

void InteractionRouter::EndErase(bool commit) {
    // App.cpp:5639-5720 EndErase: one history action for the whole drag.
    if (active_ != InteractionKind::Erase) {
        return;
    }
    if (commit && !pendingErase_.empty()) {
        host_.CommitErase(pendingErase_);
    } else if (!pendingErase_.empty()) {
        host_.UpdatePendingErase({});
    }
    pendingErase_.clear();
    SetActive(InteractionKind::None);
    UpdateCapture();
    host_.InvalidateCanvas();
}

// ----------------------------------------------------------- selection -----

void InteractionRouter::BeginSelection(const QPointF& logical, bool boxOverride) {
    // Selection.cpp:1221-1246 BeginSelection.
    if (active_ == InteractionKind::Pan) {
        return;
    }
    if (!host_.IsPointInCanvas(logical)) {
        host_.ClearSelection();
        selectionBoxOverride_ = false;
        host_.InvalidateCanvas();
        return;
    }

    selectionAnchor_ = logical;
    selectionCurrent_ = logical;
    selectionLassoPoints_.clear();
    selectionLassoPoints_.push_back(logical);
    selectionBoxOverride_ = boxOverride;
    SetActive(InteractionKind::Select);
    if (const std::optional<PageHit> hit = host_.HitTestPage(logical); hit.has_value()) {
        host_.SetViewPage(hit->pageIndex);
    }
    UpdateCapture();
    host_.UpdateSelectionOverlay(selectionAnchor_, selectionCurrent_, !boxOverride && selectionMode_ == SelectionMode::Lasso, selectionLassoPoints_);
    host_.InvalidateCanvas();
}

void InteractionRouter::ExtendSelection(const PointerSample& sample) {
    // Selection.cpp:1250-1262 ExtendSelection.
    if (active_ != InteractionKind::Select) {
        return;
    }
    selectionCurrent_ = sample.position;
    if (selectionMode_ == SelectionMode::Lasso) {
        if (selectionLassoPoints_.empty()
            || SquaredDistance(selectionCurrent_, selectionLassoPoints_.back())
                >= (kLassoPointSpacing * kLassoPointSpacing)) {
            selectionLassoPoints_.push_back(selectionCurrent_);
        }
    }
    host_.UpdateSelectionOverlay(selectionAnchor_, selectionCurrent_, !selectionBoxOverride_ && selectionMode_ == SelectionMode::Lasso, selectionLassoPoints_);
    host_.InvalidateCanvas();
}

void InteractionRouter::EndSelection() {
    // Selection.cpp:1268-1275 and the click-versus-marquee threshold at 1269.
    if (active_ != InteractionKind::Select) {
        return;
    }
    const bool lasso = !selectionBoxOverride_ && selectionMode_ == SelectionMode::Lasso;
    bool click = SquaredDistance(selectionAnchor_, selectionCurrent_)
        <= (kSelectionClickRadius * kSelectionClickRadius);
    if (lasso) {
        // Selection.cpp:1269: a lasso is a click only while its whole path stays
        // inside the click radius.  The anchor-to-release distance alone reads a
        // closed loop that travels away and returns as a click, and a click
        // commits a rectangle around the anchor instead of the polygon.
        click = true;
        for (const QPointF& point : selectionLassoPoints_) {
            if (SquaredDistance(selectionAnchor_, point) > (kSelectionClickRadius * kSelectionClickRadius)) {
                click = false;
                break;
            }
        }
        // The release can sit up to 6 px past the last sampled point
        // (Selection.cpp:1252), so keep it in the polygon.  Publishing the
        // overlay before the commit (the EndErase ordering) hands the appended
        // point to the canvas, which builds the polygon from its overlay copy.
        if (!selectionLassoPoints_.empty()
            && SquaredDistance(selectionCurrent_, selectionLassoPoints_.back()) > 1.0f) {
            selectionLassoPoints_.push_back(selectionCurrent_);
            host_.UpdateSelectionOverlay(selectionAnchor_, selectionCurrent_, true, selectionLassoPoints_);
        }
    }
    host_.CommitSelection(selectionAnchor_, selectionCurrent_, lasso, selectionBoxOverride_, click);
    selectionLassoPoints_.clear();
    selectionBoxOverride_ = false;
    SetActive(InteractionKind::None);
    UpdateCapture();
    host_.InvalidateCanvas();
}

void InteractionRouter::BeginSelectOrMove(const PointerSample& sample, bool moveTool) {
    static_cast<void>(moveTool);
    // MessageLoop.cpp:228 routes both Move and Select interactions that start
    // inside a selection through App::BeginMoveSelection.
    BeginMoveSelection(sample.position);
}

void InteractionRouter::BeginMoveSelection(const QPointF& logical) {
    // Selection.cpp:1453-1620 BeginMoveSelection.
    const std::optional<PageHit> hit = host_.HitTestPage(logical);
    if (!hit.has_value()) {
        if (host_.HasSelection()) {
            host_.ClearSelection();
            host_.InvalidateCanvas();
        }
        return;
    }
    const std::size_t pageIndex = hit->pageIndex;

    if (const std::optional<TransformRequest> handle = host_.HitTestSelectionHandle(logical);
        handle.has_value() && handle->pageIndex == pageIndex) {
        SetActive(InteractionKind::Transform);
        host_.BeginTransform(*handle);
        UpdateCapture();
        host_.InvalidateCanvas();
        return;
    }

    const std::optional<SelectionHit> objectHit = host_.HitTestSelectionObject(logical, pageIndex);
    if (!objectHit.has_value()) {
        if (host_.HasSelection() && host_.CursorInteractsWithSelection(logical)) {
            // No object under the cursor but the press is inside the selection
            // frame: the reference moves the whole selection instead of
            // starting a new one (Selection.cpp:1553-1582).
            SetActive(InteractionKind::MoveSelection);
            movePageIndex_ = pageIndex;
            moveLastPosition_ = logical;
            UpdateCapture();
            host_.BeginMoveSelection(pageIndex);
            host_.InvalidateCanvas();
            return;
        }
        // Nothing under the cursor: the reference falls back to a box selection
        // (Selection.cpp:1578-1582).
        host_.ClearSelection();
        BeginSelection(logical, true);
        return;
    }

    ObjectRef object { pageIndex, objectHit->kind, objectHit->objectIndex };
    if (!objectHit->selected) {
        host_.ClearSelection();
        host_.AddObjectToSelection(object);
    }

    if (!objectHit->moveGrip && tool_ == Tool::Move && objectHit->kind == ObjectKind::Text && objectHit->selected) {
        // An already-selected text under the Move tool is a click-to-edit until
        // the pointer travels past kTextEditDragThreshold (Selection.cpp:1605-1613
        // and 1650-1687).
        pendingTextClickPage_ = pageIndex;
        pendingTextClickIndex_ = objectHit->objectIndex;
        pendingTextClickPosition_ = logical;
        SetActive(InteractionKind::PendingTextClick);
        UpdateCapture();
        return;
    }

    SetActive(InteractionKind::MoveSelection);
    movePageIndex_ = pageIndex;
    moveLastPosition_ = logical;
    UpdateCapture();
    host_.BeginMoveSelection(pageIndex);
    host_.InvalidateCanvas();
}

void InteractionRouter::UpdateMoveSelection(const PointerSample& sample) {
    // Selection.cpp:1650-1700 UpdateMoveSelection, including the pending
    // text-click drag threshold that promotes the click to a move.
    if (active_ == InteractionKind::PendingTextClick) {
        if (SquaredDistance(sample.position, pendingTextClickPosition_)
                < (kTextEditDragThreshold * kTextEditDragThreshold)
            || !pendingTextClickPage_.has_value()) {
            return;
        }
        const std::size_t pageIndex = *pendingTextClickPage_;
        const QPointF startPosition = pendingTextClickPosition_;
        pendingTextClickPage_.reset();
        pendingTextClickIndex_.reset();

        const std::optional<PageHit> hit = host_.HitTestPage(startPosition);
        if (!hit.has_value()) {
            SetActive(InteractionKind::None);
            UpdateCapture();
            return;
        }

        SetActive(InteractionKind::MoveSelection);
        movePageIndex_ = pageIndex;
        moveLastPosition_ = startPosition;
        UpdateCapture();
        host_.BeginMoveSelection(pageIndex);
        const QPointF delta(sample.position.x() - startPosition.x(), sample.position.y() - startPosition.y());
        host_.UpdateMoveSelection(delta);
        host_.InvalidateCanvas();
        return;
    }

    if (active_ != InteractionKind::MoveSelection) {
        return;
    }
    if (sample.position == moveLastPosition_) {
        return;
    }
    const QPointF delta(sample.position.x() - moveLastPosition_.x(), sample.position.y() - moveLastPosition_.y());
    moveLastPosition_ = sample.position;
    host_.UpdateMoveSelection(delta);
    host_.InvalidateCanvas();
}

void InteractionRouter::EndMoveSelection() {
    // Selection.cpp:1706 EndMoveSelection.
    if (active_ != InteractionKind::MoveSelection) {
        return;
    }
    host_.CommitMoveSelection();
    movePageIndex_.reset();
    SetActive(InteractionKind::None);
    UpdateCapture();
    host_.InvalidateCanvas();
}

// ------------------------------------------------------- text / latex ------

void InteractionRouter::BeginTextTool(const QPointF& logical) {
    // TextEditing.cpp:1111-1156 BeginTextToolInteraction.
    if (const std::optional<std::pair<std::size_t, std::size_t>> hitText = host_.HitTestText(logical); hitText.has_value()) {
        host_.SetViewPage(hitText->first);
        SetActive(InteractionKind::None);
        host_.BeginTextEdit(hitText->first, hitText->second);
        return;
    }

    const std::optional<PageHit> hit = host_.HitTestPage(logical);
    if (!hit.has_value()) {
        host_.ClearSelection();
        host_.InvalidateCanvas();
        return;
    }
    host_.SetViewPage(hit->pageIndex);
    SetActive(InteractionKind::None);
    host_.CreateTextAt(hit->pageIndex, host_.ScreenToPage(logical, true));
}

void InteractionRouter::BeginLatexTool(const QPointF& logical) {
    // LatexEditing.cpp:806-843 BeginLatexToolInteraction.
    if (const std::optional<std::pair<std::size_t, std::size_t>> hitLatex = host_.HitTestLatex(logical); hitLatex.has_value()) {
        host_.SetViewPage(hitLatex->first);
        SetActive(InteractionKind::None);
        host_.BeginLatexEdit(hitLatex->first, hitLatex->second);
        return;
    }

    const std::optional<PageHit> hit = host_.HitTestPage(logical);
    if (!hit.has_value()) {
        host_.ClearSelection();
        host_.InvalidateCanvas();
        return;
    }
    host_.SetViewPage(hit->pageIndex);
    SetActive(InteractionKind::None);
    host_.CreateLatexAt(hit->pageIndex, host_.ScreenToPage(logical, true));
}

}  // namespace cnote::ui
