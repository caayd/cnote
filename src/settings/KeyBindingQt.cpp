#include "settings/KeyBindingQt.h"

#include <array>

namespace cnote::settings {

namespace {

// Windows virtual-key codes, declared locally: this portable service never
// includes <windows.h> (plan step 6.5).
constexpr unsigned int kVkBack = 0x08;
constexpr unsigned int kVkTab = 0x09;
constexpr unsigned int kVkClear = 0x0C;
constexpr unsigned int kVkReturn = 0x0D;
constexpr unsigned int kVkShift = 0x10;
constexpr unsigned int kVkControl = 0x11;
constexpr unsigned int kVkMenu = 0x12;
constexpr unsigned int kVkPause = 0x13;
constexpr unsigned int kVkCapital = 0x14;
constexpr unsigned int kVkEscape = 0x1B;
constexpr unsigned int kVkSpace = 0x20;
constexpr unsigned int kVkPrior = 0x21;
constexpr unsigned int kVkNext = 0x22;
constexpr unsigned int kVkEnd = 0x23;
constexpr unsigned int kVkHome = 0x24;
constexpr unsigned int kVkLeft = 0x25;
constexpr unsigned int kVkUp = 0x26;
constexpr unsigned int kVkRight = 0x27;
constexpr unsigned int kVkDown = 0x28;
constexpr unsigned int kVkSnapshot = 0x2C;
constexpr unsigned int kVkInsert = 0x2D;
constexpr unsigned int kVkDelete = 0x2E;
constexpr unsigned int kVkLWin = 0x5B;
constexpr unsigned int kVkRWin = 0x5C;
constexpr unsigned int kVkApps = 0x5D;
constexpr unsigned int kVkNumpad0 = 0x60;
constexpr unsigned int kVkNumpad9 = 0x69;
constexpr unsigned int kVkMultiply = 0x6A;
constexpr unsigned int kVkAdd = 0x6B;
constexpr unsigned int kVkSeparator = 0x6C;
constexpr unsigned int kVkSubtract = 0x6D;
constexpr unsigned int kVkDecimal = 0x6E;
constexpr unsigned int kVkDivide = 0x6F;
constexpr unsigned int kVkF1 = 0x70;
constexpr unsigned int kVkF24 = 0x87;
constexpr unsigned int kVkNumLock = 0x90;
constexpr unsigned int kVkScroll = 0x91;
constexpr unsigned int kVkOem1 = 0xBA;       // ;:
constexpr unsigned int kVkOemPlus = 0xBB;    // =+
constexpr unsigned int kVkOemComma = 0xBC;   // ,<
constexpr unsigned int kVkOemMinus = 0xBD;   // -_
constexpr unsigned int kVkOemPeriod = 0xBE;  // .>
constexpr unsigned int kVkOem2 = 0xBF;       // /?
constexpr unsigned int kVkOem3 = 0xC0;       // `~
constexpr unsigned int kVkOem4 = 0xDB;       // [{
constexpr unsigned int kVkOem5 = 0xDC;       // \|
constexpr unsigned int kVkOem6 = 0xDD;       // ]}
constexpr unsigned int kVkOem7 = 0xDE;       // '"

// Same order, labels and default bindings as src/ShortcutConfig.cpp.
constexpr std::array<ShortcutDefinition, kShortcutActionCount> kDefinitions {{
    { ShortcutAction::ToolDraw, "Draw Tool", { 'D', false, false, false } },
    { ShortcutAction::ToolSelect, "Select Tool", { 'S', false, false, false } },
    { ShortcutAction::ToolMove, "Move Tool", { 'M', false, false, false } },
    { ShortcutAction::ToolText, "Text Tool", { 'T', false, false, false } },
    { ShortcutAction::ToolLatex, "Latex Tool", { 'L', false, false, false } },
    { ShortcutAction::ToolPan, "Pan Tool", { 'H', false, false, false } },
    { ShortcutAction::ToolErase, "Erase Tool", { 'E', false, false, false } },
    { ShortcutAction::New, "New", { 'N', true, false, false } },
    { ShortcutAction::Undo, "Undo", { 'Z', true, false, false } },
    { ShortcutAction::Redo, "Redo", { 'Y', true, false, false } },
    { ShortcutAction::Open, "Open", { 'O', true, false, false } },
    { ShortcutAction::Save, "Save Cnote", { 'S', true, false, false } },
    { ShortcutAction::ExportPdf, "Export PDF", { 0, false, false, false } },
    { ShortcutAction::Copy, "Copy", { 'C', true, false, false } },
    { ShortcutAction::Cut, "Cut", { 'X', true, false, false } },
    { ShortcutAction::Paste, "Paste", { 'V', true, false, false } },
    { ShortcutAction::ResetView, "Reset View", { '0', false, false, false } },
    { ShortcutAction::ZoomPreset1, "Zoom Preset 1", { 0, false, false, false } },
    { ShortcutAction::ZoomPreset2, "Zoom Preset 2", { 0, false, false, false } },
    { ShortcutAction::ZoomPreset3, "Zoom Preset 3", { 0, false, false, false } },
    { ShortcutAction::ZoomPreset4, "Zoom Preset 4", { 0, false, false, false } },
    { ShortcutAction::ZoomPreset5, "Zoom Preset 5", { 0, false, false, false } },
    { ShortcutAction::ToggleSettings, "Settings", { 0, false, false, false } },
    { ShortcutAction::TogglePdfSidebar, "PDF Sidebar", { 'B', true, false, false } },
    { ShortcutAction::ToggleQuickSize, "Quick Popup", { 'W', true, false, false } },
    { ShortcutAction::ToggleWheel, "Wheel", { 'Q', true, false, false } },
    { ShortcutAction::PreviousPage, "Prev Page", { kVkPrior, false, false, false } },
    { ShortcutAction::NextPage, "Next Page", { kVkNext, false, false, false } },
}};

int NumpadKey(int key) {
    return static_cast<int>(Qt::KeypadModifier) | key;
}

}  // namespace

std::span<const ShortcutDefinition> ShortcutDefinitions() {
    return std::span<const ShortcutDefinition>(kDefinitions);
}

const ShortcutDefinition* FindShortcutDefinition(ShortcutAction action) {
    for (const ShortcutDefinition& definition : kDefinitions) {
        if (definition.action == action) {
            return &definition;
        }
    }
    return nullptr;
}

const char* ShortcutActionConfigName(ShortcutAction action) {
    switch (action) {
    case ShortcutAction::ToolDraw:
        return "ToolDraw";
    case ShortcutAction::ToolSelect:
        return "ToolSelect";
    case ShortcutAction::ToolMove:
        return "ToolMove";
    case ShortcutAction::ToolText:
        return "ToolText";
    case ShortcutAction::ToolLatex:
        return "ToolLatex";
    case ShortcutAction::ToolPan:
        return "ToolPan";
    case ShortcutAction::ToolErase:
        return "ToolErase";
    case ShortcutAction::New:
        return "New";
    case ShortcutAction::Undo:
        return "Undo";
    case ShortcutAction::Redo:
        return "Redo";
    case ShortcutAction::Open:
        return "Open";
    case ShortcutAction::Save:
        return "Save";
    case ShortcutAction::ExportPdf:
        return "ExportPdf";
    case ShortcutAction::Copy:
        return "Copy";
    case ShortcutAction::Cut:
        return "Cut";
    case ShortcutAction::Paste:
        return "Paste";
    case ShortcutAction::ResetView:
        return "ResetView";
    case ShortcutAction::ZoomPreset1:
        return "ZoomPreset1";
    case ShortcutAction::ZoomPreset2:
        return "ZoomPreset2";
    case ShortcutAction::ZoomPreset3:
        return "ZoomPreset3";
    case ShortcutAction::ZoomPreset4:
        return "ZoomPreset4";
    case ShortcutAction::ZoomPreset5:
        return "ZoomPreset5";
    case ShortcutAction::ToggleSettings:
        return "ToggleSettings";
    case ShortcutAction::TogglePdfSidebar:
        return "TogglePdfSidebar";
    case ShortcutAction::ToggleQuickSize:
        return "ToggleQuickSize";
    case ShortcutAction::ToggleWheel:
        return "ToggleWheel";
    case ShortcutAction::PreviousPage:
        return "PreviousPage";
    case ShortcutAction::NextPage:
        return "NextPage";
    }
    return "";
}

std::optional<ShortcutAction> ParseShortcutActionConfigName(std::string_view name) {
    for (const ShortcutDefinition& definition : kDefinitions) {
        if (name == ShortcutActionConfigName(definition.action)) {
            return definition.action;
        }
    }
    return std::nullopt;
}

int QtKeyFromVirtualKey(unsigned int virtualKey) {
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return static_cast<int>(Qt::Key_A) + static_cast<int>(virtualKey - 'A');
    }
    if (virtualKey >= '0' && virtualKey <= '9') {
        return static_cast<int>(Qt::Key_0) + static_cast<int>(virtualKey - '0');
    }
    if (virtualKey >= kVkF1 && virtualKey <= kVkF24) {
        return static_cast<int>(Qt::Key_F1) + static_cast<int>(virtualKey - kVkF1);
    }
    if (virtualKey >= kVkNumpad0 && virtualKey <= kVkNumpad9) {
        return NumpadKey(static_cast<int>(Qt::Key_0) + static_cast<int>(virtualKey - kVkNumpad0));
    }

    switch (virtualKey) {
    case kVkBack:
        return Qt::Key_Backspace;
    case kVkTab:
        return Qt::Key_Tab;
    case kVkClear:
        return Qt::Key_Clear;
    case kVkReturn:
        return Qt::Key_Return;
    case kVkEscape:
        return Qt::Key_Escape;
    case kVkSpace:
        return Qt::Key_Space;
    case kVkPrior:
        return Qt::Key_PageUp;
    case kVkNext:
        return Qt::Key_PageDown;
    case kVkEnd:
        return Qt::Key_End;
    case kVkHome:
        return Qt::Key_Home;
    case kVkLeft:
        return Qt::Key_Left;
    case kVkUp:
        return Qt::Key_Up;
    case kVkRight:
        return Qt::Key_Right;
    case kVkDown:
        return Qt::Key_Down;
    case kVkSnapshot:
        return Qt::Key_Print;
    case kVkInsert:
        return Qt::Key_Insert;
    case kVkDelete:
        return Qt::Key_Delete;
    case kVkPause:
        return Qt::Key_Pause;
    case kVkCapital:
        return Qt::Key_CapsLock;
    case kVkNumLock:
        return Qt::Key_NumLock;
    case kVkScroll:
        return Qt::Key_ScrollLock;
    case kVkLWin:
    case kVkRWin:
        return Qt::Key_Meta;
    case kVkApps:
        return Qt::Key_Menu;
    case kVkOem1:
        return Qt::Key_Semicolon;
    case kVkOemPlus:
        return Qt::Key_Equal;
    case kVkOemComma:
        return Qt::Key_Comma;
    case kVkOemMinus:
        return Qt::Key_Minus;
    case kVkOemPeriod:
        return Qt::Key_Period;
    case kVkOem2:
        return Qt::Key_Slash;
    case kVkOem3:
        return Qt::Key_QuoteLeft;
    case kVkOem4:
        return Qt::Key_BracketLeft;
    case kVkOem5:
        return Qt::Key_Backslash;
    case kVkOem6:
        return Qt::Key_BracketRight;
    case kVkOem7:
        return Qt::Key_Apostrophe;
    case kVkMultiply:
        return NumpadKey(Qt::Key_Asterisk);
    case kVkAdd:
        return NumpadKey(Qt::Key_Plus);
    case kVkSeparator:
        return NumpadKey(Qt::Key_Comma);
    case kVkSubtract:
        return NumpadKey(Qt::Key_Minus);
    case kVkDecimal:
        return NumpadKey(Qt::Key_Period);
    case kVkDivide:
        return NumpadKey(Qt::Key_Slash);
    // Modifier-only virtual keys are not capturable bindings (the legacy
    // settings dialog ignores them) and have no standalone Qt key here.
    case kVkShift:
    case kVkControl:
    case kVkMenu:
    default:
        return 0;
    }
}

std::optional<QKeySequence> KeySequenceFromLegacyRecord(const LegacyKeyRecord& record) {
    if (record.key == 0) {
        return QKeySequence {};
    }

    const int key = QtKeyFromVirtualKey(record.key);
    if (key == 0) {
        return std::nullopt;
    }

    int combination = key;
    if (record.ctrl) {
        combination |= static_cast<int>(Qt::CTRL);
    }
    if (record.shift) {
        combination |= static_cast<int>(Qt::SHIFT);
    }
    if (record.alt) {
        combination |= static_cast<int>(Qt::ALT);
    }

    const QKeySequence sequence(combination);
    if (sequence.isEmpty()) {
        return std::nullopt;
    }
    return sequence;
}

QKeySequence DefaultKeySequence(ShortcutAction action) {
    const ShortcutDefinition* definition = FindShortcutDefinition(action);
    if (definition == nullptr || definition->defaultRecord.key == 0) {
        return QKeySequence {};
    }
    return KeySequenceFromLegacyRecord(definition->defaultRecord).value_or(QKeySequence {});
}

QString ToPortableText(const QKeySequence& sequence) {
    return sequence.toString(QKeySequence::PortableText);
}

std::optional<QKeySequence> FromPortableText(std::string_view text) {
    const QString encoded = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    const QKeySequence sequence = QKeySequence::fromString(encoded, QKeySequence::PortableText);
    if (sequence.count() == 0) {
        return std::nullopt;
    }
    // Qt parses an unrecognised key name to a Qt::Key_unknown chord instead of
    // failing; treat that as malformed so the caller keeps the default.
    for (int chord = 0; chord < sequence.count(); ++chord) {
        if (sequence[chord].key() == Qt::Key_unknown) {
            return std::nullopt;
        }
    }
    return sequence;
}

}  // namespace cnote::settings
