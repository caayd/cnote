#include "PrecisionTouchpad.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <interactioncontext.h>
#include <windowsx.h>

namespace cnote {
namespace {

constexpr UINT32 kInteractionMeasurementUnitsScreen = 1U;
constexpr UINT32 kInteractionUiFeedbackOff = 0U;
constexpr UINT32 kInteractionFilterPointersOff = 0U;
constexpr UINT kPrecisionTouchpadInertiaFrameMs = 15U;
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
        StopInertia(false);
        canvasGestureActive = false;
        manipulationActive = false;
        const PrecisionTouchpadApi& loadedApi = api;
        if (enabled && window != nullptr && loadedApi.registerTouchpadCapableWindow != nullptr) {
            static_cast<void>(loadedApi.registerTouchpadCapableWindow(window, FALSE));
        }
        enabled = false;
        if (interactionContext != nullptr && loadedApi.destroyInteractionContext != nullptr) {
            static_cast<void>(loadedApi.destroyInteractionContext(interactionContext));
            interactionContext = nullptr;
        }
        window = nullptr;
    }

    void StopInertia(bool reportToSystem) {
        if (!inertiaActive) {
            return;
        }
        inertiaActive = false;
        if (reportToSystem && api.reportWindowContentInertia != nullptr && window != nullptr) {
            static_cast<void>(api.reportWindowContentInertia(window, FALSE));
        }
    }

    void StopManipulation() {
        canvasGestureActive = false;
        manipulationActive = false;
    }

    void Emit(PrecisionTouchpadGesture gesture) {
        gesture.inertiaActive = inertiaActive;
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
        if (output.interactionId != INTERACTION_ID_MANIPULATION) {
            return;
        }

        if ((output.interactionFlags & INTERACTION_FLAG_BEGIN) != 0) {
            manipulationActive = true;
        }
        if ((output.interactionFlags & INTERACTION_FLAG_CANCEL) != 0) {
            StopManipulation();
            StopInertia(true);
            Emit(PrecisionTouchpadGesture { .phase = PrecisionTouchpadGesture::Phase::Cancel });
            return;
        }

        if ((output.interactionFlags & INTERACTION_FLAG_INERTIA) != 0) {
            if (!inertiaActive) {
                inertiaActive = true;
                if (api.reportWindowContentInertia != nullptr && window != nullptr) {
                    static_cast<void>(api.reportWindowContentInertia(window, TRUE));
                }
            }
        } else if (inertiaActive) {
            StopInertia(true);
        }

        const INTERACTION_ARGUMENTS_MANIPULATION& manipulation = output.arguments.manipulation;
        PrecisionTouchpadGesture gesture {
            .phase = (output.interactionFlags & INTERACTION_FLAG_END) != 0
                ? PrecisionTouchpadGesture::Phase::End
                : ((output.interactionFlags & INTERACTION_FLAG_BEGIN) != 0
                    ? PrecisionTouchpadGesture::Phase::Begin
                    : PrecisionTouchpadGesture::Phase::Update),
            .screenAnchor {
                static_cast<int>(std::lround(output.x)),
                static_cast<int>(std::lround(output.y)),
            },
            .cumulativeScale = manipulation.cumulative.scale,
            .translationX = manipulation.delta.translationX,
            .translationY = manipulation.delta.translationY,
        };

        if ((output.interactionFlags & INTERACTION_FLAG_END) != 0) {
            StopManipulation();
            if ((output.interactionFlags & INTERACTION_FLAG_INERTIA) == 0) {
                StopInertia(true);
            }
        }
        Emit(gesture);
    }

    bool HandlePointerMessage(UINT message, WPARAM wParam) {
        if (!enabled || interactionContext == nullptr) {
            return false;
        }

        const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        POINTER_INFO pointerInfo {};
        if (GetPointerInfo(pointerId, &pointerInfo) == FALSE || pointerInfo.pointerType != kPointerInputTypeTouchpad) {
            return false;
        }

        POINT cursor = pointerInfo.ptPixelLocation;
        ScreenToClient(window, &cursor);
        if (message == WM_POINTERDOWN || (pointerInfo.pointerFlags & POINTER_FLAG_NEW) != 0 || !canvasGestureActive) {
            canvasGestureActive = routeCallback != nullptr && routeCallback(clientData, cursor);
        }
        if (!canvasGestureActive) {
            return false;
        }

        UINT32 entriesCount = 0;
        UINT32 pointerCount = 0;
        if (api.getPointerFrameTouchpadInfoHistory == nullptr
            || api.getPointerFrameTouchpadInfoHistory(pointerId, &entriesCount, &pointerCount, nullptr) == FALSE
            || entriesCount == 0U
            || pointerCount == 0U) {
            return false;
        }

        std::vector<POINTER_TOUCH_INFO> touchInfo(entriesCount * pointerCount);
        if (api.getPointerFrameTouchpadInfoHistory(pointerId, &entriesCount, &pointerCount, touchInfo.data()) == FALSE
            || entriesCount == 0U
            || pointerCount == 0U) {
            return false;
        }

        touchInfo.resize(entriesCount * pointerCount);
        std::vector<POINTER_TYPE_INFO> typeInfo(touchInfo.size());
        for (std::size_t index = 0; index < touchInfo.size(); ++index) {
            typeInfo[index].type = kPointerInputTypeTouchpad;
            typeInfo[index].touchInfo = touchInfo[index];
        }

        if (api.processPointerFramesInteractionContext2 == nullptr
            || FAILED(api.processPointerFramesInteractionContext2(
                interactionContext,
                entriesCount,
                pointerCount,
                typeInfo.data()))) {
            if ((pointerInfo.pointerFlags & POINTER_FLAG_UP) != 0 || message == WM_POINTERUP) {
                StopManipulation();
            }
            return false;
        }

        static_cast<void>(SkipPointerFrameMessages(pointerId));
        if ((pointerInfo.pointerFlags & POINTER_FLAG_UP) != 0 && !manipulationActive && !inertiaActive) {
            canvasGestureActive = false;
        }
        return true;
    }

    PrecisionTouchpadGestureCallback gestureCallback = nullptr;
    PrecisionTouchpadRouteCallback routeCallback = nullptr;
    void* clientData = nullptr;
    HMODULE ninput = nullptr;
    PrecisionTouchpadApi api {};
    HWND window = nullptr;
    HINTERACTIONCONTEXT interactionContext = nullptr;
    bool enabled = false;
    bool canvasGestureActive = false;
    bool manipulationActive = false;
    bool inertiaActive = false;
};

void PrecisionTouchpad::StateDeleter::operator()(State* state) const {
    delete state;
}

PrecisionTouchpad::PrecisionTouchpad(
    PrecisionTouchpadGestureCallback gestureCallback,
    PrecisionTouchpadRouteCallback routeCallback,
    void* clientData)
    : state_(new State(gestureCallback, routeCallback, clientData)) {
}

PrecisionTouchpad::~PrecisionTouchpad() = default;

void PrecisionTouchpad::Initialize(HWND window) {
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
        return;
    }

    if (FAILED(state.api.createInteractionContext(&state.interactionContext)) || state.interactionContext == nullptr) {
        state.interactionContext = nullptr;
        return;
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
        return;
    }

    state.enabled = true;
}

void PrecisionTouchpad::Shutdown() {
    state_->Shutdown();
}

bool PrecisionTouchpad::HandlePointerMessage(UINT message, WPARAM wParam) {
    return state_->HandlePointerMessage(message, wParam);
}

bool PrecisionTouchpad::ProcessInertia() {
    State& state = *state_;
    if (!state.inertiaActive || state.interactionContext == nullptr || state.api.processInertiaInteractionContext == nullptr) {
        state.StopInertia(false);
        return false;
    }
    if (FAILED(state.api.processInertiaInteractionContext(state.interactionContext))) {
        state.StopInertia(true);
        return false;
    }
    return true;
}


}  // namespace cnote
