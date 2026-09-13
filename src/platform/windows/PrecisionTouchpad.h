#pragma once

#include <memory>

#include <windows.h>

namespace cnote {

struct PrecisionTouchpadGesture {
    enum class Phase {
        Begin,
        Update,
        End,
        Cancel,
    };
    Phase phase = Phase::Update;
    POINT screenAnchor {};
    float cumulativeScale = 1.0f;
    float translationX = 0.0f;
    float translationY = 0.0f;
    bool inertiaActive = false;
};

using PrecisionTouchpadGestureCallback = void (*)(void* clientData, const PrecisionTouchpadGesture& gesture);
using PrecisionTouchpadRouteCallback = bool (*)(void* clientData, POINT clientCursor);

class PrecisionTouchpad final {
public:
    PrecisionTouchpad(
        PrecisionTouchpadGestureCallback gestureCallback,
        PrecisionTouchpadRouteCallback routeCallback,
        void* clientData);
    ~PrecisionTouchpad();

    PrecisionTouchpad(const PrecisionTouchpad&) = delete;
    PrecisionTouchpad& operator=(const PrecisionTouchpad&) = delete;

    void Initialize(HWND window);
    void Shutdown();
    [[nodiscard]] bool HandlePointerMessage(UINT message, WPARAM wParam);
    [[nodiscard]] bool ProcessInertia();

private:
    struct State;
    struct StateDeleter {
        void operator()(State* state) const;
    };

    std::unique_ptr<State, StateDeleter> state_;
};

}  // namespace cnote
