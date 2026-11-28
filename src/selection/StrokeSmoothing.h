#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
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
// smoothed; pressures are filled in only when StrokeHasPressure.  This is the
// uncached computation; every consumer goes through CachedStrokeRenderPath.
[[nodiscard]] InkPath BuildStrokeRenderPath(const Stroke& stroke, float fallbackSmoothing);

// Bounded, content-addressed cache of rendered stroke centerlines (PERF-023).
//
// BuildStrokeRenderPath is a pure function of the stroke content and the
// fallback smoothing: raw points and pressures, StrokeHasPressure's dotted/
// pressure rule, StrokeSampling after the same clamping IncrementalStroke-
// Stabilizer::Reset applies, and the smoothed strength.  The cache keys on
// exactly those inputs, so it needs no epoch or call-site invalidation:
//  * an in-place move/transform rewrites the points, changing the key;
//  * undo/redo and clipboard copies present whichever content is current;
//  * views sharing one session share an entry only when the contents agree.
// A miss computes with BuildStrokeRenderPath (one code path); a hit returns the
// stored centerline, so consumers cannot observe a difference.
class StrokePathCache {
public:
    static constexpr std::size_t kDefaultMaxEntries = 512U;
    static constexpr std::size_t kDefaultMaxBytes = 8U * 1024U * 1024U;

    explicit StrokePathCache(
        std::size_t maxEntries = kDefaultMaxEntries,
        std::size_t maxBytes = kDefaultMaxBytes);

    // `withPressure` false skips the pressure channel entirely (geometry-only
    // consumers); the points are byte-identical to the full path's, which is
    // all a hit test reads.  Renderers and exporters pass true.
    [[nodiscard]] std::shared_ptr<const InkPath> Path(
        const Stroke& stroke, float fallbackSmoothing, bool withPressure);

    // Drops every retained centerline; counters and the peak-byte watermark
    // stay for diagnostics.
    void Clear() noexcept;

    // Diagnostics: computations, hits, and current/peak retained centerline
    // bytes (vector storage plus the InkPath header, not list-node overhead).
    [[nodiscard]] std::size_t RecomputeCount() const noexcept { return recomputeCount_; }
    [[nodiscard]] std::size_t HitCount() const noexcept { return hitCount_; }
    [[nodiscard]] std::size_t EntryCount() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t CachedBytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t PeakCachedBytes() const noexcept { return peakBytes_; }

private:
    struct Fingerprint {
        std::uint64_t first = 0;
        std::uint64_t second = 0;
        friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
    };
    struct Key {
        Fingerprint points {};
        Fingerprint pressures {};
        std::uint64_t pointCount = 0;
        std::uint64_t pressureCount = 0;
        std::uint32_t smoothingBits = 0;
        std::uint32_t streamlineBits = 0;
        std::uint32_t spacingBits = 0;
        bool carriesPressure = false;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
    };
    struct Entry {
        Key key {};
        std::shared_ptr<const InkPath> path;
        std::size_t bytes = 0;
    };

    [[nodiscard]] static Fingerprint FingerprintPoints(const std::vector<Vec2>& points);
    [[nodiscard]] static Fingerprint FingerprintPressures(const std::vector<float>& pressures);
    [[nodiscard]] static Key MakeKey(
        const Stroke& stroke, float fallbackSmoothing, bool withPressure);
    [[nodiscard]] static std::size_t PathBytes(const InkPath& path);

    std::size_t maxEntries_ = kDefaultMaxEntries;
    std::size_t maxBytes_ = kDefaultMaxBytes;
    // Most-recently-used first; the tail is evicted.
    std::list<Entry> entries_;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
    std::size_t bytes_ = 0;
    std::size_t peakBytes_ = 0;
    std::size_t recomputeCount_ = 0;
    std::size_t hitCount_ = 0;
};

// The process-wide cache used by the render and hit-test call sites.  It is
// thread-local so an off-thread renderer gets its own bounded instance; the
// key is content, so correctness never depends on which instance serves a call.
[[nodiscard]] StrokePathCache& SharedStrokePathCache();

// Cached counterpart of BuildStrokeRenderPath.
[[nodiscard]] std::shared_ptr<const InkPath> CachedStrokeRenderPath(
    const Stroke& stroke, float fallbackSmoothing, bool withPressure);

}  // namespace cnote
