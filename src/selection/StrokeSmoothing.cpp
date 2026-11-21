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

Vec2 Lerp(Vec2 from, Vec2 to, float amount) {
    return Vec2 {
        from.x + ((to.x - from.x) * amount),
        from.y + ((to.y - from.y) * amount),
    };
}

// Upper bound on the steps one long pull is resampled into (App.cpp:1964).
constexpr int kMaxInterpolatedPoints = 64;

}  // namespace

void IncrementalStrokeStabilizer::Reset(StrokeSampling sampling) {
    // Non-finite values (a damaged file) fall back to the identity.
    sampling_.streamline = std::isfinite(sampling.streamline) ? std::clamp(sampling.streamline, 0.0f, 0.95f) : 0.0f;
    sampling_.spacing = std::isfinite(sampling.spacing) ? std::max(0.0f, sampling.spacing) : 0.0f;
    hasLast_ = false;
    last_ = {};
}

void IncrementalStrokeStabilizer::Feed(Vec2 raw, std::vector<Vec2>& out) {
    if (!hasLast_) {
        out.push_back(raw);
        last_ = raw;
        hasLast_ = true;
        return;
    }

    // The reference's follow factor, clamp(1 - smoothing, 0.05, 0.95); a zero
    // streamline keeps the sample bit-exact instead of lerping by 1.
    const Vec2 filtered = sampling_.streamline > 0.0f
        ? Lerp(last_, raw, std::max(0.05f, 1.0f - sampling_.streamline))
        : raw;
    if (sampling_.spacing <= 0.0f) {
        out.push_back(filtered);
        last_ = filtered;
        return;
    }

    // App.cpp:1958-1979: drop a pull below the spacing, resample a longer one.
    const float spacing = std::max(0.05f, sampling_.spacing);
    const float distance = std::sqrt(DistanceSquared(filtered, last_));
    if (distance < spacing) {
        return;
    }
    const int segmentCount = std::min(
        kMaxInterpolatedPoints + 1,
        std::max(1, static_cast<int>(std::ceil(distance / spacing))));
    for (int segment = 1; segment <= segmentCount; ++segment) {
        out.push_back(Lerp(last_, filtered, static_cast<float>(segment) / static_cast<float>(segmentCount)));
    }
    last_ = filtered;
}

std::vector<Vec2> BuildStabilizedPoints(const std::vector<Vec2>& points, StrokeSampling sampling) {
    if (sampling.streamline == 0.0f && sampling.spacing == 0.0f) {
        return points;
    }
    IncrementalStrokeStabilizer stabilizer;
    stabilizer.Reset(sampling);
    std::vector<Vec2> stabilized;
    stabilized.reserve(points.size());
    for (const Vec2& point : points) {
        stabilizer.Feed(point, stabilized);
    }
    return stabilized;
}

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

std::vector<Vec2> BuildStrokeRenderPoints(const Stroke& stroke, float fallbackSmoothing) {
    return BuildSmoothedPoints(
        BuildStabilizedPoints(stroke.points, stroke.sampling),
        StrokeRenderSmoothing(stroke, fallbackSmoothing));
}

}  // namespace cnote
