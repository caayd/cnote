#pragma once

#include <cstdint>
#include <vector>

#include "Document.h"
#include "selection/InkOutline.h"

namespace cnote::ui {

// The eraser's fading trail, after Excalidraw's EraserTrail: a translucent
// comet in widget coordinates that follows an erase drag.  A point shrinks
// as it ages and is gone kDecayMs after it was sampled; the trail also tapers
// over its last kTaperPoints samples toward the tail, both eased out
// (1 - (1 - k)^4).  While the drag lasts its head keeps the full radius, so a
// resting eraser still shows its swept disc; after release the whole trail
// decays away.  Pure geometry over caller-supplied millisecond timestamps.
class EraserTrail {
public:
    static constexpr std::int64_t kDecayMs = 200;
    static constexpr int kTaperPoints = 10;
    // Samples follow the pointer by 1 - kStreamline (Excalidraw's 0.2).
    static constexpr float kStreamline = 0.2f;

    // Starts a trail at `point`; a trail still in progress decays on its own.
    void Begin(Vec2 point, std::int64_t nowMs, float radius);
    void Add(Vec2 point, std::int64_t nowMs);
    // Releases the head: the trail now only decays.
    void End();

    [[nodiscard]] bool Active() const { return active_; }
    // Whether anything is left to draw at `nowMs` (drives the animation).
    [[nodiscard]] bool Visible(std::int64_t nowMs) const;
    // Bounds of what AppendOutline draws at `nowMs`; false when nothing.
    [[nodiscard]] bool Bounds(std::int64_t nowMs, float* left, float* top, float* right, float* bottom) const;
    // Appends the trail at `nowMs` and forgets trails that have decayed.
    void AppendOutline(std::int64_t nowMs, float tolerance, InkOutline& outline);

private:
    struct Sample {
        Vec2 position {};
        std::int64_t time = 0;
    };
    struct Trail {
        std::vector<Sample> samples;
        float radius = 0.0f;
        bool head = false;   // the live trail: its newest sample keeps the full radius
    };

    [[nodiscard]] static float RadiusAt(const Trail& trail, std::size_t index, std::int64_t nowMs);
    [[nodiscard]] static bool TrailVisible(const Trail& trail, std::int64_t nowMs);

    std::vector<Trail> trails_;
    bool active_ = false;   // trails_.back() is the live trail
};

}  // namespace cnote::ui
