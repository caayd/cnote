#include "PrecisionTouchpad.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <interactioncontext.h>
#include <windowsx.h>

namespace cnote {
namespace {

constexpr UINT32 kInteractionMeasurementUnitsScreen = 1U;
constexpr UINT32 kInteractionUiFeedbackOff = 0U;
constexpr UINT32 kInteractionFilterPointersOff = 0U;
constexpr POINTER_INPUT_TYPE kPointerInputTypeTouchpad = static_cast<POINTER_INPUT_TYPE>(5);

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

template <typename T>
T ResolveProcAddress(HMODULE module, const char* name, WORD ordinal = 0) {
    if (module == nullptr) {
        return nullptr;
    }

    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr && ordinal != 0) {
        proc = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
    }
    return reinterpret_cast<T>(proc);
}

struct PrecisionTouchpadApi {
    using CreateInteractionContextFn = HRESULT(WINAPI*)(HINTERACTIONCONTEXT*);
    using DestroyInteractionContextFn = HRESULT(WINAPI*)(HINTERACTIONCONTEXT);
    using RegisterOutputCallbackInteractionContextFn = HRESULT(WINAPI*)(HINTERACTIONCONTEXT, INTERACTION_CONTEXT_OUTPUT_CALLBACK, void*);
    using SetInteractionConfigurationInteractionContextFn = HRESULT(WINAPI*)(HINTERACTIONCONTEXT, UINT32, const INTERACTION_CONTEXT_CONFIGURATION*);
    using SetPropertyInteractionContextFn = HRESULT(WINAPI*)(HINTERACTIONCONTEXT, INTERACTION_CONTEXT_PROPERTY, UINT32);
    using ProcessInertiaInteractionContextFn = HRESULT(WINAPI*)(HINTERACTIONCONTEXT);
    using RegisterTouchpadCapableWindowFn = BOOL(WINAPI*)(HWND, BOOL);
    using GetPointerFrameTouchpadInfoHistoryFn = BOOL(WINAPI*)(UINT32, UINT32*, UINT32*, POINTER_TOUCH_INFO*);
    using ReportWindowContentInertiaFn = BOOL(WINAPI*)(HWND, BOOL);
    // Microsoft documents this Windows 11 API as Ninput ordinal 2507.
    using ProcessPointerFramesInteractionContext2Fn = HRESULT(WINAPI*)(HINTERACTIONCONTEXT, UINT32, UINT32, const POINTER_TYPE_INFO*);

    CreateInteractionContextFn createInteractionContext = nullptr;
    DestroyInteractionContextFn destroyInteractionContext = nullptr;
    RegisterOutputCallbackInteractionContextFn registerOutputCallbackInteractionContext = nullptr;
    SetInteractionConfigurationInteractionContextFn setInteractionConfigurationInteractionContext = nullptr;
    SetPropertyInteractionContextFn setPropertyInteractionContext = nullptr;
    ProcessInertiaInteractionContextFn processInertiaInteractionContext = nullptr;
    RegisterTouchpadCapableWindowFn registerTouchpadCapableWindow = nullptr;
    GetPointerFrameTouchpadInfoHistoryFn getPointerFrameTouchpadInfoHistory = nullptr;
    ReportWindowContentInertiaFn reportWindowContentInertia = nullptr;
    ProcessPointerFramesInteractionContext2Fn processPointerFramesInteractionContext2 = nullptr;
};

PrecisionTouchpadApi LoadPrecisionTouchpadApi(HMODULE user32, HMODULE ninput) {
    PrecisionTouchpadApi api {};
    api.createInteractionContext = ResolveProcAddress<PrecisionTouchpadApi::CreateInteractionContextFn>(ninput, "CreateInteractionContext");
    api.destroyInteractionContext = ResolveProcAddress<PrecisionTouchpadApi::DestroyInteractionContextFn>(ninput, "DestroyInteractionContext");
    api.registerOutputCallbackInteractionContext = ResolveProcAddress<PrecisionTouchpadApi::RegisterOutputCallbackInteractionContextFn>(ninput, "RegisterOutputCallbackInteractionContext");
    api.setInteractionConfigurationInteractionContext = ResolveProcAddress<PrecisionTouchpadApi::SetInteractionConfigurationInteractionContextFn>(ninput, "SetInteractionConfigurationInteractionContext");
    api.setPropertyInteractionContext = ResolveProcAddress<PrecisionTouchpadApi::SetPropertyInteractionContextFn>(ninput, "SetPropertyInteractionContext");
    api.processInertiaInteractionContext = ResolveProcAddress<PrecisionTouchpadApi::ProcessInertiaInteractionContextFn>(ninput, "ProcessInertiaInteractionContext");
    api.registerTouchpadCapableWindow = ResolveProcAddress<PrecisionTouchpadApi::RegisterTouchpadCapableWindowFn>(user32, "RegisterTouchpadCapableWindow");
    api.getPointerFrameTouchpadInfoHistory = ResolveProcAddress<PrecisionTouchpadApi::GetPointerFrameTouchpadInfoHistoryFn>(user32, "GetPointerFrameTouchpadInfoHistory");
    api.reportWindowContentInertia = ResolveProcAddress<PrecisionTouchpadApi::ReportWindowContentInertiaFn>(user32, "ReportWindowContentInertia", 2695);
    api.processPointerFramesInteractionContext2 = ResolveProcAddress<PrecisionTouchpadApi::ProcessPointerFramesInteractionContext2Fn>(ninput, "ProcessPointerFramesInteractionContext2", 2507);
    return api;
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace

struct PrecisionTouchpad::State {
    enum class Phase {
        Disabled,
        Idle,
        Routing,
        Manipulating,
        Inertia,
    };

    State(PrecisionTouchpadGestureCallback gestureCallbackIn, PrecisionTouchpadRouteCallback routeCallbackIn, void* clientDataIn)
        : gestureCallback(gestureCallbackIn), routeCallback(routeCallbackIn), clientData(clientDataIn) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr) {
            user32 = LoadLibraryW(L"user32.dll");
        }
        ninput = LoadLibraryW(L"ninput.dll");
        api = LoadPrecisionTouchpadApi(user32, ninput);
    }

    ~State() {
        Shutdown();
        if (ninput != nullptr) {
            FreeLibrary(ninput);
            ninput = nullptr;
        }
    }

    void Shutdown() {
        const bool wasEnabled = phase != Phase::Disabled;
        // TransitionTo reports the inertia stop to the system before the
        // window is forgotten, so Shutdown never leaves the OS believing the
        // window is still coasting.
        TransitionTo(Phase::Disabled);
        const PrecisionTouchpadApi& loadedApi = api;
        if (wasEnabled && window != nullptr && loadedApi.registerTouchpadCapableWindow != nullptr) {
            static_cast<void>(loadedApi.registerTouchpadCapableWindow(window, FALSE));
        }
        if (interactionContext != nullptr && loadedApi.destroyInteractionContext != nullptr) {
            static_cast<void>(loadedApi.destroyInteractionContext(interactionContext));
            interactionContext = nullptr;
        }
        window = nullptr;
    }

    // Single owner of every phase change, including the system-visible content
    // inertia report.  Cancel is only ever emitted from Manipulating (see
    // HandleOutput), so the flag soup cannot desynchronise from the real state.
    void TransitionTo(Phase next) {
        if (phase == next) {
            return;
        }
        if (phase == Phase::Inertia && api.reportWindowContentInertia != nullptr && window != nullptr) {
            static_cast<void>(api.reportWindowContentInertia(window, FALSE));
        }
        if (next == Phase::Inertia && api.reportWindowContentInertia != nullptr && window != nullptr) {
            static_cast<void>(api.reportWindowContentInertia(window, TRUE));
        }
        phase = next;
    }

    void Emit(PrecisionTouchpadGesture gesture) {
        gesture.inertiaActive = phase == Phase::Inertia;
        if (gestureCallback != nullptr) {
            gestureCallback(clientData, gesture);
        }
    }

    static void CALLBACK OutputCallback(void* callbackData, const INTERACTION_CONTEXT_OUTPUT* output) {
        if (callbackData == nullptr || output == nullptr) {
            return;
        }
        static_cast<State*>(callbackData)->HandleOutput(*output);
    }

    void HandleOutput(const INTERACTION_CONTEXT_OUTPUT& output) {
        if (output.interactionId != INTERACTION_ID_MANIPULATION || phase == Phase::Disabled) {
            return;
        }

        const bool begin = (output.interactionFlags & INTERACTION_FLAG_BEGIN) != 0;
        const bool end = (output.interactionFlags & INTERACTION_FLAG_END) != 0;
        const bool cancel = (output.interactionFlags & INTERACTION_FLAG_CANCEL) != 0;
        const bool inertia = (output.interactionFlags & INTERACTION_FLAG_INERTIA) != 0;

        if (cancel) {
            // Cancel is only meaningful for a manipulation that actually
            // began; a stray context cancel must not surface as a sink Cancel
            // without a matching Begin.  Transitioning away from Inertia still
            // reports the inertia stop to the system.
            const bool wasManipulating = phase == Phase::Manipulating;
            TransitionTo(Phase::Idle);
            if (wasManipulating) {
                Emit(PrecisionTouchpadGesture {
                    .phase = PrecisionTouchpadGesture::Phase::Cancel,
                    .cancel = true,
                });
            }
            return;
        }

        if (begin && phase != Phase::Manipulating) {
            TransitionTo(Phase::Manipulating);
        }
        if (inertia) {
            if (phase != Phase::Inertia) {
                TransitionTo(Phase::Inertia);
            }
        } else if (phase == Phase::Inertia) {
            TransitionTo(Phase::Idle);
        }

        const INTERACTION_ARGUMENTS_MANIPULATION& manipulation = output.arguments.manipulation;
        PrecisionTouchpadGesture gesture {
            .phase = end
                ? PrecisionTouchpadGesture::Phase::End
                : (begin ? PrecisionTouchpadGesture::Phase::Begin : PrecisionTouchpadGesture::Phase::Update),
            .screenAnchor {
                static_cast<int>(std::lround(output.x)),
                static_cast<int>(std::lround(output.y)),
            },
            .cumulativeScale = manipulation.cumulative.scale,
            .translationX = manipulation.delta.translationX,
            .translationY = manipulation.delta.translationY,
            .begin = begin,
            .end = end,
        };

        if (end) {
            // Finish the manipulation, but keep coasting in Inertia when the
            // same frame carries inertia frames.
            TransitionTo(inertia ? Phase::Inertia : Phase::Idle);
        }
        Emit(gesture);
    }

    bool HandlePointerMessage(UINT message, WPARAM wParam) {
        if (phase == Phase::Disabled || interactionContext == nullptr) {
            return false;
        }

        const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        POINTER_INFO pointerInfo {};
        if (GetPointerInfo(pointerId, &pointerInfo) == FALSE || pointerInfo.pointerType != kPointerInputTypeTouchpad) {
            return false;
        }

        POINT cursor = pointerInfo.ptPixelLocation;
        ScreenToClient(window, &cursor);
        if (message == WM_POINTERDOWN || (pointerInfo.pointerFlags & POINTER_FLAG_NEW) != 0 || phase == Phase::Idle) {
            const bool routed = routeCallback != nullptr && routeCallback(clientData, cursor);
            if (!routed) {
                if (phase == Phase::Routing) {
                    TransitionTo(Phase::Idle);
                }
                return false;
            }
            if (phase == Phase::Idle) {
                TransitionTo(Phase::Routing);
            }
        }

        UINT32 entriesCount = 0;
        UINT32 pointerCount = 0;
        if (!QueryTouchFrame(pointerId, entriesCount, pointerCount)
            || api.processPointerFramesInteractionContext2 == nullptr
            || FAILED(api.processPointerFramesInteractionContext2(
                interactionContext,
                entriesCount,
                pointerCount,
                typeInfo.data()))) {
            if ((pointerInfo.pointerFlags & POINTER_FLAG_UP) != 0 || message == WM_POINTERUP) {
                if (phase == Phase::Routing || phase == Phase::Manipulating) {
                    TransitionTo(Phase::Idle);
                }
            }
            return false;
        }

        static_cast<void>(SkipPointerFrameMessages(pointerId));
        if ((pointerInfo.pointerFlags & POINTER_FLAG_UP) != 0 && phase == Phase::Routing) {
            TransitionTo(Phase::Idle);
        }
        return true;
    }

    // Fills touchInfo/typeInfo for this frame.  The history size is stable
    // across the messages of one gesture, so the buffers kept between messages
    // usually let a single query replace the old size-probe-plus-fill pair;
    // only the first frame, a frame that outgrows them, and the undocumented
    // column case below ask for the size.
    bool QueryTouchFrame(UINT32 pointerId, UINT32& entriesCount, UINT32& pointerCount) {
        if (api.getPointerFrameTouchpadInfoHistory == nullptr) {
            return false;
        }

        if (frameEntries == 0U || framePointers == 0U) {
            // No cached size yet: ask for the required dimensions, then fill.
            if (!ProbeAndFillTouchFrame(pointerId, entriesCount, pointerCount)) {
                return false;
            }
        } else if (!FillTouchFrame(pointerId, entriesCount, pointerCount)) {
            if (GetLastError() == ERROR_NO_DATA) {
                // The frame is gone: do not trust the cached size for the next one.
                ResetTouchFrame();
                return false;
            }
            // Too few columns is undocumented: a frame with more pointers than
            // the cached buffer has columns can fail instead of reporting the
            // needed totals through the counts.  That frame may carry a new
            // contact's down, so probe the size and fill again rather than
            // dropping it.
            if (!ProbeAndFillTouchFrame(pointerId, entriesCount, pointerCount)) {
                ResetTouchFrame();
                return false;
            }
        }

        if (entriesCount == 0U || pointerCount == 0U) {
            ResetTouchFrame();
            return false;
        }

        if (entriesCount > frameEntries || pointerCount != framePointers) {
            // The API reports the totals it needs when the buffer was too
            // small; size it exactly and query the full history once more.
            if (!ResizeTouchFrame(entriesCount, pointerCount)) {
                ResetTouchFrame();
                return false;
            }
            if (!FillTouchFrame(pointerId, entriesCount, pointerCount)
                || entriesCount == 0U
                || pointerCount != framePointers
                || entriesCount > frameEntries) {
                ResetTouchFrame();
                return false;
            }
        }

        const std::size_t sampleCount =
            static_cast<std::size_t>(entriesCount) * static_cast<std::size_t>(pointerCount);
        typeInfo.resize(sampleCount);
        for (std::size_t index = 0; index < sampleCount; ++index) {
            typeInfo[index].type = kPointerInputTypeTouchpad;
            typeInfo[index].touchInfo = touchInfo[index];
        }
        return true;
    }

    // One fill query against the cached buffers at their current size.
    bool FillTouchFrame(UINT32 pointerId, UINT32& entriesCount, UINT32& pointerCount) {
        entriesCount = frameEntries;
        pointerCount = framePointers;
        return api.getPointerFrameTouchpadInfoHistory(
                   pointerId, &entriesCount, &pointerCount, touchInfo.data())
            != FALSE;
    }

    // Sizes the buffers from the API's required totals, then fills once.
    bool ProbeAndFillTouchFrame(UINT32 pointerId, UINT32& entriesCount, UINT32& pointerCount) {
        entriesCount = 0U;
        pointerCount = 0U;
        if (api.getPointerFrameTouchpadInfoHistory(pointerId, &entriesCount, &pointerCount, nullptr) == FALSE
            || !ResizeTouchFrame(entriesCount, pointerCount)) {
            return false;
        }
        return FillTouchFrame(pointerId, entriesCount, pointerCount);
    }

    bool ResizeTouchFrame(UINT32 entriesCount, UINT32 pointerCount) {
        if (entriesCount == 0U
            || pointerCount == 0U
            || static_cast<std::size_t>(entriesCount) > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(pointerCount)) {
            return false;
        }
        frameEntries = entriesCount;
        framePointers = pointerCount;
        touchInfo.resize(static_cast<std::size_t>(entriesCount) * static_cast<std::size_t>(pointerCount));
        return true;
    }

    void ResetTouchFrame() {
        frameEntries = 0U;
        framePointers = 0U;
        touchInfo.clear();
        typeInfo.clear();
    }

    PrecisionTouchpadGestureCallback gestureCallback = nullptr;
    PrecisionTouchpadRouteCallback routeCallback = nullptr;
    void* clientData = nullptr;
    HMODULE ninput = nullptr;
    PrecisionTouchpadApi api {};
    HWND window = nullptr;
    HINTERACTIONCONTEXT interactionContext = nullptr;
    std::vector<POINTER_TOUCH_INFO> touchInfo {};
    std::vector<POINTER_TYPE_INFO> typeInfo {};
    UINT32 frameEntries = 0U;
    UINT32 framePointers = 0U;
    Phase phase = Phase::Disabled;
};

PrecisionTouchpad::PrecisionTouchpad(
    PrecisionTouchpadGestureCallback gestureCallback,
    PrecisionTouchpadRouteCallback routeCallback,
    void* clientData)
    : state_(new State(gestureCallback, routeCallback, clientData)) {
}

PrecisionTouchpad::~PrecisionTouchpad() = default;

[[nodiscard]] bool PrecisionTouchpad::Initialize(HWND window) {
    State& state = *state_;
    state.Shutdown();
    state.window = window;
    if (window == nullptr
        || state.api.createInteractionContext == nullptr
        || state.api.destroyInteractionContext == nullptr
        || state.api.registerOutputCallbackInteractionContext == nullptr
        || state.api.setInteractionConfigurationInteractionContext == nullptr
        || state.api.setPropertyInteractionContext == nullptr
        || state.api.processInertiaInteractionContext == nullptr
        || state.api.registerTouchpadCapableWindow == nullptr
        || state.api.getPointerFrameTouchpadInfoHistory == nullptr
        || state.api.processPointerFramesInteractionContext2 == nullptr) {
        return false;
    }

    if (FAILED(state.api.createInteractionContext(&state.interactionContext)) || state.interactionContext == nullptr) {
        state.interactionContext = nullptr;
        return false;
    }

    const HRESULT callbackHr = state.api.registerOutputCallbackInteractionContext(
        state.interactionContext,
        &State::OutputCallback,
        &state);
    const INTERACTION_CONTEXT_CONFIGURATION configuration[] {
        {
            INTERACTION_ID_MANIPULATION,
            INTERACTION_CONFIGURATION_FLAG_MANIPULATION
                | INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_X
                | INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_Y
                | INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_INERTIA
                | INTERACTION_CONFIGURATION_FLAG_MANIPULATION_RAILS_X
                | INTERACTION_CONFIGURATION_FLAG_MANIPULATION_RAILS_Y
                | INTERACTION_CONFIGURATION_FLAG_MANIPULATION_SCALING
        },
    };
    const HRESULT configurationHr = SUCCEEDED(callbackHr)
        ? state.api.setInteractionConfigurationInteractionContext(
            state.interactionContext,
            static_cast<UINT32>(std::size(configuration)),
            configuration)
        : E_FAIL;
    const HRESULT measurementHr = SUCCEEDED(configurationHr)
        ? state.api.setPropertyInteractionContext(
            state.interactionContext,
            INTERACTION_CONTEXT_PROPERTY_MEASUREMENT_UNITS,
            kInteractionMeasurementUnitsScreen)
        : E_FAIL;
    const HRESULT feedbackHr = SUCCEEDED(measurementHr)
        ? state.api.setPropertyInteractionContext(
            state.interactionContext,
            INTERACTION_CONTEXT_PROPERTY_INTERACTION_UI_FEEDBACK,
            kInteractionUiFeedbackOff)
        : E_FAIL;
    const HRESULT filterHr = SUCCEEDED(feedbackHr)
        ? state.api.setPropertyInteractionContext(
            state.interactionContext,
            INTERACTION_CONTEXT_PROPERTY_FILTER_POINTERS,
            kInteractionFilterPointersOff)
        : E_FAIL;
    if (FAILED(filterHr) || state.api.registerTouchpadCapableWindow(window, TRUE) == FALSE) {
        state.Shutdown();
        return false;
    }

    state.TransitionTo(State::Phase::Idle);
    return true;

}
void PrecisionTouchpad::Shutdown() {
    state_->Shutdown();
}

bool PrecisionTouchpad::HandlePointerMessage(UINT message, WPARAM wParam) {
    return state_->HandlePointerMessage(message, wParam);
}

bool PrecisionTouchpad::ProcessInertia() {
    State& state = *state_;
    if (state.phase != State::Phase::Inertia
        || state.interactionContext == nullptr
        || state.api.processInertiaInteractionContext == nullptr) {
        if (state.phase == State::Phase::Inertia) {
            state.TransitionTo(State::Phase::Idle);
        }
        return false;
    }
    if (FAILED(state.api.processInertiaInteractionContext(state.interactionContext))) {
        state.TransitionTo(State::Phase::Idle);
        return false;
    }
    return true;
}


}  // namespace cnote
