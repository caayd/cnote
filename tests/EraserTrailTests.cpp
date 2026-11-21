// Eraser trail geometry (ui::EraserTrail): the live head, the comet taper and
// the decay after release, over explicit millisecond timestamps.

#include <cmath>
#include <cstdio>
#include <string>

#include "ui/EraserTrail.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) {
        ++g_failures;
    }
}

bool Near(float a, float b, float tolerance = 0.01f) {
    return std::abs(a - b) <= tolerance;
}

using cnote::ui::EraserTrail;

}  // namespace

int main() {
    constexpr float kRadius = 10.0f;

    // A resting eraser keeps its full disc at the head however long it rests.
    {
        EraserTrail trail;
        trail.Begin({ 100.0f, 50.0f }, 0, kRadius);
        float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
        Check(trail.Visible(5000) && trail.Bounds(5000, &left, &top, &right, &bottom)
                && Near(left, 90.0f) && Near(top, 40.0f) && Near(right, 110.0f) && Near(bottom, 60.0f),
            "the live head shows the eraser's full disc while resting");
        cnote::InkOutline outline;
        trail.AppendOutline(5000, 0.25f, outline);
        Check(!outline.polygonEnds.empty(), "the resting head draws");
    }

    // A fast drag shows a short comet: only the newest samples have width,
    // so the trail ends well before where the drag began.
    {
        EraserTrail trail;
        trail.Begin({ 0.0f, 0.0f }, 0, kRadius);
        for (int i = 1; i <= 40; ++i) {
            trail.Add({ 10.0f * static_cast<float>(i), 0.0f }, 4 * i);
        }
        float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
        Check(trail.Bounds(160, &left, &top, &right, &bottom), "the dragging trail has bounds");
        Check(left > 250.0f && right > 380.0f && Near(bottom - top, 2.0f * kRadius),
            "the trail tapers to nothing within its last samples (left=" + std::to_string(left)
                + " right=" + std::to_string(right) + ")");
    }

    // After release the trail decays within kDecayMs of its newest sample and
    // is forgotten, so the canvas stops animating.
    {
        EraserTrail trail;
        trail.Begin({ 0.0f, 0.0f }, 1000, kRadius);
        trail.Add({ 20.0f, 0.0f }, 1010);
        trail.End();
        Check(!trail.Active(), "release ends the live trail");
        Check(trail.Visible(1010 + EraserTrail::kDecayMs - 20), "the released trail is still fading just before the decay time");
        Check(!trail.Visible(1010 + EraserTrail::kDecayMs), "the released trail is gone at the decay time");
        cnote::InkOutline outline;
        trail.AppendOutline(1010 + EraserTrail::kDecayMs + 50, 0.25f, outline);
        Check(outline.polygonEnds.empty(), "a decayed trail draws nothing");
    }

    std::printf("SUMMARY failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
