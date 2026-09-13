#include "qt/QtDocumentController.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace cnote::qt {

namespace {

// Reference App helpers (src/App.cpp:1958-2055): pointer samples are filtered
// towards the raw pointer, deduplicated by a screen-relative spacing and long
// segments are resampled so rendering keeps the same curve.
constexpr float kMinScreenSpacing = 1.75f;
constexpr float kMaxScreenSpacing = 3.0f;
constexpr int kMaxInterpolatedPoints = 64;

float DistanceSquared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

float Distance(Vec2 a, Vec2 b) {
    return std::sqrt(DistanceSquared(a, b));
}

Vec2 Lerp(Vec2 from, Vec2 to, float amount) {
    return Vec2 {
        from.x + ((to.x - from.x) * amount),
        from.y + ((to.y - from.y) * amount),
    };
}

float InputSpacingPageUnits(float pageSpacing, float pageScale) {
    const float scale = std::max(0.1f, pageScale);
    const float screenSpacing = std::clamp(
        std::max(0.05f, pageSpacing) * scale, kMinScreenSpacing, kMaxScreenSpacing);
    return screenSpacing / scale;
}

void AppendStrokePointWithResampling(Stroke& stroke, Vec2 point, float spacing) {
    if (stroke.points.empty()) {
        stroke.points.push_back(point);
        return;
    }

    const float minSpacing = std::max(0.05f, spacing);
    const Vec2 from = stroke.points.back();
    const float distance = Distance(from, point);
    if (distance < minSpacing) {
        return;
    }

    const int segmentCount = std::min(
        kMaxInterpolatedPoints + 1,
        std::max(1, static_cast<int>(std::ceil(distance / minSpacing))));
    for (int segment = 1; segment <= segmentCount; ++segment) {
        const float t = static_cast<float>(segment) / static_cast<float>(segmentCount);
        stroke.points.push_back(Lerp(from, point, t));
    }
}

}  // namespace

QtDocumentController::QtDocumentController() = default;
QtDocumentController::~QtDocumentController() = default;

void QtDocumentController::ResetToBlank() {
    session_ = ProjectSession {};
    pendingStroke_.reset();
    pendingPage_.reset();
}

void QtDocumentController::AdoptDocument(Document document) {
    // A new document starts with empty history and no pending stroke; the
    // controller's stroke style and input settings belong to the view.
    session_ = ProjectSession {};
    session_.document = std::move(document);
    pendingStroke_.reset();
    pendingPage_.reset();
}

void QtDocumentController::BeginStroke(std::size_t pageIndex, Vec2 point, float pageScale) {
    (void)pageScale;
    pendingStroke_.reset();
    pendingPage_.reset();
    if (pageIndex >= session_.document.PageCount()) {
        return;
    }

    Stroke stroke;
    stroke.style = strokeStyle_;
    stroke.smoothing = input_.strokeSmoothing;
    stroke.points.push_back(point);
    pendingStroke_ = std::move(stroke);
    pendingPage_ = pageIndex;
}

void QtDocumentController::ExtendStroke(Vec2 point, float pageScale) {
    if (!pendingStroke_.has_value()) {
        return;
    }

    const float spacing = InputSpacingPageUnits(input_.inputSpacing, pageScale);
    Vec2 filtered = point;
    if (!pendingStroke_->points.empty()) {
        const float followAmount = std::clamp(1.0f - input_.inputSmoothing, 0.05f, 0.95f);
        filtered = Lerp(pendingStroke_->points.back(), point, followAmount);
        if (Distance(filtered, pendingStroke_->points.back()) < spacing) {
            return;
        }
    }
    AppendStrokePointWithResampling(*pendingStroke_, filtered, spacing);
}

void QtDocumentController::EndStroke() {
    if (!pendingStroke_.has_value() || !pendingPage_.has_value()) {
        CancelStroke();
        return;
    }

    Stroke finished = std::move(*pendingStroke_);
    const std::size_t pageIndex = *pendingPage_;
    pendingStroke_.reset();
    pendingPage_.reset();

    if (finished.points.empty() || pageIndex >= session_.document.PageCount()) {
        return;
    }

    // Same commit shape as the reference (App::EndStroke): append, push an
    // AddStroke action with its insert index, clear the redo stack, bump the
    // revision once.
    std::vector<Stroke>& strokes = session_.document.PageAt(pageIndex).strokes;
    HistoryAction action {};
    action.type = HistoryActionType::AddStroke;
    action.stroke = finished;
    action.index = strokes.size();
    action.pageIndex = pageIndex;
    strokes.push_back(std::move(finished));
    session_.undoActions.push_back(std::move(action));
    session_.redoActions.clear();
    session_.dirty = true;
    ++session_.revision;
}

void QtDocumentController::CancelStroke() {
    pendingStroke_.reset();
    pendingPage_.reset();
}

const Stroke* QtDocumentController::PendingStroke() const {
    return pendingStroke_.has_value() ? &*pendingStroke_ : nullptr;
}

std::size_t QtDocumentController::PendingStrokePage() const {
    return pendingPage_.value_or(0U);
}

bool QtDocumentController::ApplyStrokeHistoryAction(HistoryAction& action, bool undo) {
    if (action.pageIndex >= session_.document.PageCount()) {
        return false;
    }
    if (action.type != HistoryActionType::AddStroke && action.type != HistoryActionType::RemoveStroke) {
        return false;
    }

    std::vector<Stroke>& strokes = session_.document.PageAt(action.pageIndex).strokes;
    const bool inserting = (action.type == HistoryActionType::AddStroke) ? !undo : undo;
    if (inserting) {
        if (action.index > strokes.size()) {
            return false;
        }
        strokes.insert(strokes.begin() + static_cast<std::ptrdiff_t>(action.index), action.stroke);
    } else {
        if (action.index >= strokes.size()) {
            return false;
        }
        strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(action.index));
    }
    return true;
}

bool QtDocumentController::Undo() {
    if (session_.undoActions.empty()) {
        return false;
    }

    HistoryAction action = std::move(session_.undoActions.back());
    session_.undoActions.pop_back();
    if (!ApplyStrokeHistoryAction(action, true)) {
        session_.undoActions.push_back(std::move(action));
        return false;
    }

    session_.redoActions.push_back(std::move(action));
    session_.dirty = true;
    ++session_.revision;
    return true;
}

bool QtDocumentController::Redo() {
    if (session_.redoActions.empty()) {
        return false;
    }

    HistoryAction action = std::move(session_.redoActions.back());
    session_.redoActions.pop_back();
    if (!ApplyStrokeHistoryAction(action, false)) {
        session_.redoActions.push_back(std::move(action));
        return false;
    }

    session_.undoActions.push_back(std::move(action));
    session_.dirty = true;
    ++session_.revision;
    return true;
}

}  // namespace cnote::qt
