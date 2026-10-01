#include "selection/StrokeSmoothing.h"

#include <algorithm>
#include <cmath>

namespace cnote {

namespace {

float DistanceSquared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

}  // namespace

std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength) {
    if (points.size() <= 2U || strength <= 0.001f) {
        return points;
    }

    std::vector<Vec2> smoothed;
    smoothed.reserve(points.size());
    smoothed.push_back(points.front());

    const float sideWeight = 0.25f * std::clamp(strength, 0.0f, 1.0f);
    const float centerWeight = 1.0f - (sideWeight * 2.0f);

    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const Vec2 previous = points[index - 1];
        const Vec2 current = points[index];
        const Vec2 next = points[index + 1];
        const Vec2 filtered {
            (previous.x * sideWeight) + (current.x * centerWeight) + (next.x * sideWeight),
            (previous.y * sideWeight) + (current.y * centerWeight) + (next.y * sideWeight),
        };

        if (DistanceSquared(filtered, smoothed.back()) >= 0.05f) {
            smoothed.push_back(filtered);
        }
    }

    if (DistanceSquared(points.back(), smoothed.back()) >= 0.01f) {
        smoothed.push_back(points.back());
    }

    return smoothed;
}

void IncrementalStrokeSmoother::Reset(float strength) {
    sideWeight_ = 0.25f * std::clamp(strength, 0.0f, 1.0f);
    centerWeight_ = 1.0f - (sideWeight_ * 2.0f);
    raw_ = strength <= 0.001f;
    fed_ = 0;
    prev_ = {};
    curr_ = {};
}

bool IncrementalStrokeSmoother::Feed(Vec2 point, Vec2* emitted) {
    bool has = false;
    if (fed_ >= 2U) {
        // s_{fed-1} is now stable: it is the last point the 3-tap filter can
        // finalize.  Emits the *filtered candidate*; the caller applies
        // BuildSmoothedPoints' 0.05 squared-distance drop against its own
        // emission history.
        if (emitted != nullptr) {
            *emitted = raw_ ? curr_
                : Vec2 {
                    (prev_.x * sideWeight_) + (curr_.x * centerWeight_) + (point.x * sideWeight_),
                    (prev_.y * sideWeight_) + (curr_.y * centerWeight_) + (point.y * sideWeight_),
                };
        }
        has = true;
    }
    prev_ = curr_;
    curr_ = point;
    ++fed_;
    return has;
}

bool IncrementalStrokeSmoother::TailPoint(Vec2 rawLast, Vec2 lastSmoothed, Vec2* tail) const {
    // BuildSmoothedPoints appends the raw last point verbatim when it is far
    // enough from the last emitted point.
    if (DistanceSquared(rawLast, lastSmoothed) < 0.01f) {
        return false;
    }
    if (tail != nullptr) {
        *tail = rawLast;
    }
    return true;
}


float StrokeRenderSmoothing(const Stroke& stroke, float fallbackSmoothing) {
    const float smoothing = std::isfinite(stroke.smoothing) ? stroke.smoothing : fallbackSmoothing;
    return std::clamp(smoothing, 0.0f, 1.0f);
}

}  // namespace cnote
