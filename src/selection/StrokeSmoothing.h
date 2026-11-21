#pragma once

#include <vector>

#include "Document.h"

namespace cnote {

// Pure stroke geometry shared by the renderers and the PDF exporters. Kept in
// a portable TU so the model-side services do not pull in the Qt canvas.
//
// A stroke renders from its raw points in two stages: BuildStabilizedPath
// replays the pointer stabilization the stroke recorded (Stroke::sampling),
// then BuildSmoothedPath applies the 3-tap render smoothing.  The canvas
// preview runs the same stages incrementally (IncrementalStrokeStabilizer,
// IncrementalStrokeSmoother) and must stay point-for-point identical.
// Pressure, when the stroke has it, rides along every stage as a third
// coordinate.

// A centerline as it renders: points plus, for strokes drawn with pressure,
// the pressure at each point (parallel to `points`; empty otherwise).
struct InkPath {
    std::vector<Vec2> points;
    std::vector<float> pressures;
};

// Incremental form of BuildStabilizedPath: feeding the raw samples in order
// appends exactly the points (and pressures) the batch form returns.
class IncrementalStrokeStabilizer {
public:
    // `pressure`: whether fed pressures are carried into the output path.
    void Reset(StrokeSampling sampling, bool pressure);
    // Feeds one raw sample and appends what it stabilizes to `out`.
    void Feed(Vec2 raw, float pressure, InkPath& out);

private:
    StrokeSampling sampling_ {};
    bool pressure_ = false;
    bool hasLast_ = false;
    Vec2 last_ {};
    float lastPressure_ = 0.0f;
};

// `pressures` is parallel to `points` or empty (no pressure).
[[nodiscard]] InkPath BuildStabilizedPath(
    const std::vector<Vec2>& points, const std::vector<float>& pressures, StrokeSampling sampling);

[[nodiscard]] InkPath BuildSmoothedPath(InkPath path, float strength);

// Incremental form of BuildSmoothedPath for a stroke that grows one sample
// at a time (the live-drawing preview).  A smoothed point s_i = w*p_{i-1} +
// c*p_i + w*p_{i+1} is fixed once the next raw point exists, so feeding raw
// points in order emits each stable smoothed point exactly once; the
// last-but-one raw point and the raw tail stay provisional (see TailPoint,
// which mirrors BuildSmoothedPath's final clamped push).  Pressures are
// filtered with the same weights.
class IncrementalStrokeSmoother {
public:
    void Reset(float strength);
    // Feeds one point; returns the newly stabilized smoothed point (and its
    // pressure) or false.  Emits nothing for the first two feeds.
    bool Feed(Vec2 point, float pressure, Vec2* emitted, float* emittedPressure);
    // BuildSmoothedPath's tail rule applied to the pending suffix: the point
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
    float prevPressure_ = 0.0f;
    float currPressure_ = 0.0f;
};

[[nodiscard]] float StrokeRenderSmoothing(const Stroke& stroke, float fallbackSmoothing);

// Whether a stroke renders as a pressure-width outline: it has a pressure per
// point and is solid (a dash pattern follows a constant-width pen).
[[nodiscard]] bool StrokeHasPressure(const Stroke& stroke);

// The centerline a stroke renders and hit-tests along: stabilized, then
// smoothed; pressures are filled in only when StrokeHasPressure.
[[nodiscard]] InkPath BuildStrokeRenderPath(const Stroke& stroke, float fallbackSmoothing);

}  // namespace cnote
