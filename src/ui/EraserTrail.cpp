#include "ui/EraserTrail.h"

#include <algorithm>
#include <cmath>

namespace cnote::ui {

namespace {

// Excalidraw's easeOut (packages/common/src/utils.ts).
float EaseOut(float k) {
    const float rest = 1.0f - std::clamp(k, 0.0f, 1.0f);
    return 1.0f - (rest * rest * rest * rest);
}

// Radii below this draw nothing; the visible trail starts at the first larger one.
constexpr float kMinVisibleRadius = 0.01f;

}  // namespace

void EraserTrail::Begin(Vec2 point, std::int64_t nowMs, float radius) {
    End();
    Trail trail;
    trail.samples.push_back(Sample { point, nowMs });
    trail.radius = radius;
    trail.head = true;
    trails_.push_back(std::move(trail));
    active_ = true;
}

void EraserTrail::Add(Vec2 point, std::int64_t nowMs) {
    if (!active_) {
        return;
    }
    std::vector<Sample>& samples = trails_.back().samples;
    // Points reported within kSampleIntervalMs of the newest sample replace
    // it, so the trail's length does not depend on the device's report rate.
    std::int64_t time = nowMs;
    if (samples.size() > 1U && nowMs - samples.back().time < kSampleIntervalMs) {
        time = samples.back().time;
        samples.pop_back();
    }
    const Vec2 last = samples.back().position;
    const float follow = 1.0f - kStreamline;
    const Vec2 filtered { last.x + ((point.x - last.x) * follow), last.y + ((point.y - last.y) * follow) };
    if (Vec2Equal(filtered, last)) {
        return;
    }
    samples.push_back(Sample { filtered, time });
    // Samples further back than the taper are drawn with no radius forever.
    if (samples.size() > static_cast<std::size_t>(kTaperPoints)) {
        samples.erase(samples.begin());
    }
}

void EraserTrail::End() {
    if (active_) {
        trails_.back().head = false;
        active_ = false;
    }
}

float EraserTrail::RadiusAt(const Trail& trail, std::size_t index, std::int64_t nowMs) {
    const std::size_t count = trail.samples.size();
    if (trail.head && index + 1U == count) {
        return trail.radius;
    }
    const float age = static_cast<float>(nowMs - trail.samples[index].time);
    const float time = 1.0f - (age / static_cast<float>(kDecayMs));
    const float fromHead = static_cast<float>(std::min<std::size_t>(static_cast<std::size_t>(kTaperPoints), count - index));
    const float length = (static_cast<float>(kTaperPoints) - fromHead) / static_cast<float>(kTaperPoints);
    return trail.radius * std::min(EaseOut(length), EaseOut(time));
}

bool EraserTrail::TrailVisible(const Trail& trail, std::int64_t nowMs) {
    return !trail.samples.empty() && RadiusAt(trail, trail.samples.size() - 1U, nowMs) > kMinVisibleRadius;
}

bool EraserTrail::Visible(std::int64_t nowMs) const {
    return std::any_of(trails_.begin(), trails_.end(),
        [nowMs](const Trail& trail) { return TrailVisible(trail, nowMs); });
}

bool EraserTrail::Bounds(std::int64_t nowMs, float* left, float* top, float* right, float* bottom) const {
    bool any = false;
    for (const Trail& trail : trails_) {
        for (std::size_t index = 0; index < trail.samples.size(); ++index) {
            const float radius = RadiusAt(trail, index, nowMs);
            if (radius <= kMinVisibleRadius) {
                continue;
            }
            const Vec2 p = trail.samples[index].position;
            if (!any) {
                *left = p.x - radius;
                *top = p.y - radius;
                *right = p.x + radius;
                *bottom = p.y + radius;
                any = true;
                continue;
            }
            *left = std::min(*left, p.x - radius);
            *top = std::min(*top, p.y - radius);
            *right = std::max(*right, p.x + radius);
            *bottom = std::max(*bottom, p.y + radius);
        }
    }
    return any;
}

void EraserTrail::AppendOutline(std::int64_t nowMs, float tolerance, InkOutline& outline) {
    // The live trail stays even when only its head shows; a released one is
    // forgotten once its newest sample has decayed.
    trails_.erase(std::remove_if(trails_.begin(), trails_.end(),
                      [nowMs](const Trail& trail) { return !trail.head && !TrailVisible(trail, nowMs); }),
        trails_.end());
    for (const Trail& trail : trails_) {
        // Radii grow toward the head (newer and further from the tail), so
        // the visible part is a suffix.
        InkCenterline centerline;
        for (std::size_t index = 0; index < trail.samples.size(); ++index) {
            const float radius = RadiusAt(trail, index, nowMs);
            if (radius > kMinVisibleRadius || !centerline.points.empty()) {
                centerline.Add(trail.samples[index].position, std::max(radius, 0.0f));
            }
        }
        AppendInkOutline(outline, centerline, tolerance);
    }
}

}  // namespace cnote::ui
