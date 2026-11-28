#include "selection/StrokeSmoothing.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

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

float Lerp(float from, float to, float amount) {
    return from + ((to - from) * amount);
}

// Upper bound on the steps one long pull is resampled into (App.cpp:1964).
constexpr int kMaxInterpolatedPoints = 64;

// The sampling normalization both the incremental stabilizer and the cache key
// use; the batch stage applies the same rules through Reset.  Two raw samplings
// that normalize alike produce identical stabilized points, including the
// zero-sampling identity: BuildStabilizedPath's fast path and a slow replay of
// the normalized zero sampling emit the same points and pressures.
StrokeSampling NormalizedSampling(StrokeSampling sampling) {
    return StrokeSampling {
        std::isfinite(sampling.streamline) ? std::clamp(sampling.streamline, 0.0f, 0.95f) : 0.0f,
        std::isfinite(sampling.spacing) ? std::max(0.0f, sampling.spacing) : 0.0f,
    };
}

InkPath ComputeStrokeRenderPath(const Stroke& stroke, float fallbackSmoothing, bool withPressure) {
    static const std::vector<float> kNoPressure;
    const bool pressure = withPressure && StrokeHasPressure(stroke);
    return BuildSmoothedPath(
        BuildStabilizedPath(stroke.points, pressure ? stroke.pressures : kNoPressure, stroke.sampling),
        StrokeRenderSmoothing(stroke, fallbackSmoothing));
}

std::uint64_t Mix64(std::uint64_t value) {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value;
}

// Two rolling streams over the raw point/pressure bit patterns.  A cache key
// may borrow the vectors (no copy); a collision would only be a wrong path, so
// the fingerprint is 128 bits rather than one 64-bit hash.
struct FingerprintAccumulator {
    std::uint64_t first = 0xcbf29ce484222325ULL;
    std::uint64_t second = 0x9e3779b97f4a7c15ULL;

    void Add(std::uint64_t value) {
        first = (first ^ value) * 0x100000001b3ULL;
        second ^= value * 0x9e3779b97f4a7c15ULL + 0x100000001b3ULL;
        second = (second << 27U) | (second >> 37U);
    }

    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> Finish(std::size_t count) const {
        const auto length = static_cast<std::uint64_t>(count);
        return { Mix64(first ^ length), Mix64(second + length) };
    }
};

}  // namespace

void IncrementalStrokeStabilizer::Reset(StrokeSampling sampling, bool pressure) {
    // Non-finite values (a damaged file) fall back to the identity.
    sampling_ = NormalizedSampling(sampling);
    pressure_ = pressure;
    hasLast_ = false;
    last_ = {};
    lastPressure_ = 0.0f;
}

void IncrementalStrokeStabilizer::Feed(Vec2 raw, float pressure, InkPath& out) {
    const auto push = [this, &out](Vec2 point, float value) {
        out.points.push_back(point);
        if (pressure_) {
            out.pressures.push_back(value);
        }
    };
    if (!hasLast_) {
        push(raw, pressure);
        last_ = raw;
        lastPressure_ = pressure;
        hasLast_ = true;
        return;
    }

    // The reference's follow factor, clamp(1 - smoothing, 0.05, 0.95); a zero
    // streamline keeps the sample bit-exact instead of lerping by 1.
    const float follow = std::max(0.05f, 1.0f - sampling_.streamline);
    const bool pulls = sampling_.streamline > 0.0f;
    const Vec2 filtered = pulls ? Lerp(last_, raw, follow) : raw;
    const float filteredPressure = pulls ? Lerp(lastPressure_, pressure, follow) : pressure;
    if (sampling_.spacing <= 0.0f) {
        push(filtered, filteredPressure);
        last_ = filtered;
        lastPressure_ = filteredPressure;
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
        const float t = static_cast<float>(segment) / static_cast<float>(segmentCount);
        push(Lerp(last_, filtered, t), Lerp(lastPressure_, filteredPressure, t));
    }
    last_ = filtered;
    lastPressure_ = filteredPressure;
}

InkPath BuildStabilizedPath(
    const std::vector<Vec2>& points, const std::vector<float>& pressures, StrokeSampling sampling) {
    const bool pressure = !pressures.empty() && pressures.size() == points.size();
    // Both the fast path and the replay see the same normalized sampling the
    // render-path cache keys on, so the key covers every input this function
    // reads.
    const StrokeSampling normalized = NormalizedSampling(sampling);
    if (normalized.streamline == 0.0f && normalized.spacing == 0.0f) {
        return InkPath { points, pressure ? pressures : std::vector<float> {} };
    }
    IncrementalStrokeStabilizer stabilizer;
    stabilizer.Reset(normalized, pressure);
    InkPath stabilized;
    stabilized.points.reserve(points.size());
    if (pressure) {
        stabilized.pressures.reserve(points.size());
    }
    for (std::size_t index = 0; index < points.size(); ++index) {
        stabilizer.Feed(points[index], pressure ? pressures[index] : 0.0f, stabilized);
    }
    return stabilized;
}

InkPath BuildSmoothedPath(InkPath path, float strength) {
    const std::vector<Vec2>& points = path.points;
    if (points.size() <= 2U || strength <= 0.001f) {
        return path;
    }
    const bool pressure = path.pressures.size() == points.size();
    const std::vector<float>& pressures = path.pressures;

    InkPath smoothed;
    smoothed.points.reserve(points.size());
    if (pressure) {
        smoothed.pressures.reserve(points.size());
    }
    const auto push = [&smoothed, pressure](Vec2 point, float value) {
        smoothed.points.push_back(point);
        if (pressure) {
            smoothed.pressures.push_back(value);
        }
    };
    push(points.front(), pressure ? pressures.front() : 0.0f);

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

        if (DistanceSquared(filtered, smoothed.points.back()) >= 0.05f) {
            push(filtered, pressure
                    ? (pressures[index - 1] * sideWeight) + (pressures[index] * centerWeight) + (pressures[index + 1] * sideWeight)
                    : 0.0f);
        }
    }

    if (DistanceSquared(points.back(), smoothed.points.back()) >= 0.01f) {
        push(points.back(), pressure ? pressures.back() : 0.0f);
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
    prevPressure_ = 0.0f;
    currPressure_ = 0.0f;
}

bool IncrementalStrokeSmoother::Feed(Vec2 point, float pressure, Vec2* emitted, float* emittedPressure) {
    bool has = false;
    if (fed_ >= 2U) {
        // s_{fed-1} is now stable: it is the last point the 3-tap filter can
        // finalize.  Emits the *filtered candidate*; the caller applies
        // BuildSmoothedPath's 0.05 squared-distance drop against its own
        // emission history.
        if (emitted != nullptr) {
            *emitted = raw_ ? curr_
                : Vec2 {
                    (prev_.x * sideWeight_) + (curr_.x * centerWeight_) + (point.x * sideWeight_),
                    (prev_.y * sideWeight_) + (curr_.y * centerWeight_) + (point.y * sideWeight_),
                };
        }
        if (emittedPressure != nullptr) {
            *emittedPressure = raw_ ? currPressure_
                : (prevPressure_ * sideWeight_) + (currPressure_ * centerWeight_) + (pressure * sideWeight_);
        }
        has = true;
    }
    prev_ = curr_;
    curr_ = point;
    prevPressure_ = currPressure_;
    currPressure_ = pressure;
    ++fed_;
    return has;
}

bool IncrementalStrokeSmoother::TailPoint(Vec2 rawLast, Vec2 lastSmoothed, Vec2* tail) const {
    // BuildSmoothedPath appends the raw last point verbatim when it is far
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

bool StrokeHasPressure(const Stroke& stroke) {
    return !stroke.style.dotted && !stroke.pressures.empty() && stroke.pressures.size() == stroke.points.size();
}

InkPath BuildStrokeRenderPath(const Stroke& stroke, float fallbackSmoothing) {
    return ComputeStrokeRenderPath(stroke, fallbackSmoothing, true);
}

StrokePathCache::StrokePathCache(std::size_t maxEntries, std::size_t maxBytes)
    : maxEntries_(maxEntries)
    , maxBytes_(maxBytes) {
}

StrokePathCache::Fingerprint StrokePathCache::FingerprintPoints(const std::vector<Vec2>& points) {
    FingerprintAccumulator accumulator;
    for (const Vec2& point : points) {
        const std::uint64_t x = std::bit_cast<std::uint32_t>(point.x);
        const std::uint64_t y = std::bit_cast<std::uint32_t>(point.y);
        accumulator.Add((x << 32U) | y);
    }
    const auto [first, second] = accumulator.Finish(points.size());
    return Fingerprint { first, second };
}

StrokePathCache::Fingerprint StrokePathCache::FingerprintPressures(const std::vector<float>& pressures) {
    FingerprintAccumulator accumulator;
    for (const float pressure : pressures) {
        accumulator.Add(std::bit_cast<std::uint32_t>(pressure));
    }
    const auto [first, second] = accumulator.Finish(pressures.size());
    return Fingerprint { first, second };
}

StrokePathCache::Key StrokePathCache::MakeKey(
    const Stroke& stroke, float fallbackSmoothing, bool withPressure) {
    Key key;
    key.points = FingerprintPoints(stroke.points);
    key.pointCount = stroke.points.size();
    // StrokeHasPressure carries the dotted exclusion: a dotted stroke never
    // renders a pressure outline, so its path has no pressure channel.
    key.carriesPressure = withPressure && StrokeHasPressure(stroke);
    if (key.carriesPressure) {
        key.pressures = FingerprintPressures(stroke.pressures);
        key.pressureCount = stroke.pressures.size();
    }
    key.smoothingBits = std::bit_cast<std::uint32_t>(StrokeRenderSmoothing(stroke, fallbackSmoothing));
    const StrokeSampling sampling = NormalizedSampling(stroke.sampling);
    key.streamlineBits = std::bit_cast<std::uint32_t>(sampling.streamline);
    key.spacingBits = std::bit_cast<std::uint32_t>(sampling.spacing);
    return key;
}

std::size_t StrokePathCache::PathBytes(const InkPath& path) {
    return sizeof(InkPath)
        + (path.points.capacity() * sizeof(Vec2))
        + (path.pressures.capacity() * sizeof(float));
}

std::size_t StrokePathCache::KeyHash::operator()(const Key& key) const noexcept {
    std::uint64_t hash = 0x9e3779b97f4a7c15ULL;
    const auto add = [&hash](std::uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
    };
    add(key.points.first);
    add(key.points.second);
    add(key.pressures.first);
    add(key.pressures.second);
    add(key.pointCount);
    add(key.pressureCount);
    add(key.smoothingBits);
    add(key.streamlineBits);
    add(key.spacingBits);
    add(key.carriesPressure ? 1U : 0U);
    return static_cast<std::size_t>(Mix64(hash));
}

std::shared_ptr<const InkPath> StrokePathCache::Path(
    const Stroke& stroke, float fallbackSmoothing, bool withPressure) {
    const Key key = MakeKey(stroke, fallbackSmoothing, withPressure);
    const auto found = index_.find(key);
    if (found != index_.end()) {
        entries_.splice(entries_.begin(), entries_, found->second);
        ++hitCount_;
        return found->second->path;
    }

    auto path = std::make_shared<InkPath>(ComputeStrokeRenderPath(stroke, fallbackSmoothing, withPressure));
    ++recomputeCount_;

    const std::size_t bytes = PathBytes(*path);
    if (maxEntries_ == 0U || bytes > maxBytes_) {
        return path;
    }
    while (entries_.size() >= maxEntries_ || bytes_ + bytes > maxBytes_) {
        bytes_ -= entries_.back().bytes;
        index_.erase(entries_.back().key);
        entries_.pop_back();
    }
    bytes_ += bytes;
    peakBytes_ = std::max(peakBytes_, bytes_);
    entries_.push_front(Entry { key, path, bytes });
    index_.emplace(key, entries_.begin());
    return path;
}

void StrokePathCache::Clear() noexcept {
    entries_.clear();
    index_.clear();
    bytes_ = 0;
}

StrokePathCache& SharedStrokePathCache() {
    thread_local StrokePathCache cache;
    return cache;
}

std::shared_ptr<const InkPath> CachedStrokeRenderPath(
    const Stroke& stroke, float fallbackSmoothing, bool withPressure) {
    return SharedStrokePathCache().Path(stroke, fallbackSmoothing, withPressure);
}

}  // namespace cnote
