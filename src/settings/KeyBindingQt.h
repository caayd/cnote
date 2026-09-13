#pragma once

#include <QKeySequence>
#include <QString>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace cnote::settings {

// Runtime shortcut actions, mirroring `App::ShortcutAction` and the legacy
// `src/ShortcutConfig.cpp` definitions one-for-one.  The Qt services never
// include the legacy Win32 `App.h`, so the enum is restated here; the Windows
// parity harness asserts both enumerations stay identical.
enum class ShortcutAction {
    ToolDraw,
    ToolSelect,
    ToolMove,
    ToolText,
    ToolLatex,
    ToolPan,
    ToolErase,
    New,
    Undo,
    Redo,
    Open,
    Save,
    ExportPdf,
    Copy,
    Cut,
    Paste,
    ResetView,
    ZoomPreset1,
    ZoomPreset2,
    ZoomPreset3,
    ZoomPreset4,
    ZoomPreset5,
    ToggleSettings,
    TogglePdfSidebar,
    ToggleQuickSize,
    ToggleWheel,
    PreviousPage,
    NextPage,
};

inline constexpr std::size_t kShortcutActionCount = 28;

// The stored legacy representation: a Windows virtual-key number plus three
// modifier flags (`shortcut.<ActionName>=<key>,<ctrl>,<shift>,<alt>`).
struct LegacyKeyRecord {
    unsigned int key = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

struct ShortcutDefinition {
    ShortcutAction action;
    const char* label;  // UTF-8, same display text as the legacy table
    LegacyKeyRecord defaultRecord;
};

std::span<const ShortcutDefinition> ShortcutDefinitions();
const ShortcutDefinition* FindShortcutDefinition(ShortcutAction action);
const char* ShortcutActionConfigName(ShortcutAction action);
std::optional<ShortcutAction> ParseShortcutActionConfigName(std::string_view name);

// Explicit Windows virtual-key -> Qt key table covering the keys the legacy
// settings dialog can capture: letters, digits, F1-F24, navigation/editing,
// OEM punctuation and the numpad.  Returns the Qt key value combined with
// Qt::KeypadModifier for numpad keys, or 0 when the virtual key has no Qt
// equivalent.  A VK number is never cast to Qt::Key.
int QtKeyFromVirtualKey(unsigned int virtualKey);

// Converts a stored legacy record into its runtime binding.  `key == 0` is the
// legacy "unbound" marker and yields an empty sequence; an unknown virtual key
// yields nullopt so the caller keeps the action's current default instead of
// binding a wrong key.
std::optional<QKeySequence> KeySequenceFromLegacyRecord(const LegacyKeyRecord& record);

QKeySequence DefaultKeySequence(ShortcutAction action);

// Exact marker written as `shortcut_format=<value>` for the new representation.
inline constexpr std::string_view kShortcutFormatValue = "qt-portable-v1";

// `QKeySequence::PortableText` conversion used for both directions.
QString ToPortableText(const QKeySequence& sequence);
std::optional<QKeySequence> FromPortableText(std::string_view text);

}  // namespace cnote::settings
