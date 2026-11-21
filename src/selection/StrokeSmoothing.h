#pragma once

#include <vector>

#include "Document.h"

namespace cnote {

// Pure stroke geometry shared by the renderers and the PDF exporters. Kept in
// a portable TU so the model-side services do not pull in the Qt canvas.
//
// A stroke renders from its raw points in two stages: BuildStabilizedPoints
// replays the pointer stabilization the stroke recorded (Stroke::sampling),
// then BuildSmoothedPoints applies the 3-tap render smoothing.  The canvas
// preview runs the same stages incrementally (IncrementalStrokeStabilizer,
// IncrementalStrokeSmoother) and must stay point-for-point identical.

// Incremental form of BuildStabilizedPoints: feeding the raw points in order
// appends exactly the points the batch form returns.
class IncrementalStrokeStabilizer {
public:
    void Reset(StrokeSampling sampling);
    // Feeds one raw point and appends the points it stabilizes to `out`.
    void Feed(Vec2 raw, std::vector<Vec2>& out);

private:
    StrokeSampling sampling_ {};
    bool hasLast_ = false;
    Vec2 last_ {};
};

[[nodiscard]] std::vector<Vec2> BuildStabilizedPoints(const std::vector<Vec2>& points, StrokeSampling sampling);

[[nodiscard]] std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength);

// Incremental form of BuildSmoothedPoints for a stroke that grows one sample
// at a time (the live-drawing preview).  A smoothed point s_i = w*p_{i-1} +
// c*p_i + w*p_{i+1} is fixed once the next raw point exists, so feeding raw
// points in order emits each stable smoothed point exactly once; the
// last-but-one raw point and the raw tail stay provisional (see TailPoint,
// which mirrors BuildSmoothedPoints' final clamped push).
class IncrementalStrokeSmoother {
public:
    void Reset(float strength);
    // Feeds one raw point; returns the newly stabilized smoothed point or
    // false.  Emits nothing for the first two feeds.
    bool Feed(Vec2 point, Vec2* emitted);
    // BuildSmoothedPoints' tail rule applied to the pending suffix: the point
    // the live tail should end at.
    [[nodiscard]] bool TailPoint(Vec2 rawLast, Vec2 lastSmoothed, Vec2* tail) const;
    [[nodiscard]] bool EmitsRaw() const { return raw_; }

private:
    float sideWeight_ = 0.0f;
    float centerWeight_ = 0.0f;
    bool raw_ = false;            // strength <= 0.001: emit raw points verbatim
    std::size_t fed_ = 0;
    Vec2 prev_ {};
    Vec2 curr_ {};
};

[[nodiscard]] float StrokeRenderSmoothing(const Stroke& stroke, float fallbackSmoothing);

// The geometry a stroke renders and hit-tests as: stabilized, then smoothed.
[[nodiscard]] std::vector<Vec2> BuildStrokeRenderPoints(const Stroke& stroke, float fallbackSmoothing);

}  // namespace cnote
