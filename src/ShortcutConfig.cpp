#include "ShortcutConfig.h"

#include <array>
#include <string>

namespace cnote {

namespace {

constexpr std::array<ShortcutDefinition, 18> kShortcutDefinitions {{
    { App::ShortcutAction::ToolDraw, L"Draw Tool", { 'D', false, false, false } },
    { App::ShortcutAction::ToolSelect, L"Select Tool", { 'S', false, false, false } },
    { App::ShortcutAction::ToolMove, L"Move Tool", { 'M', false, false, false } },
    { App::ShortcutAction::ToolPan, L"Pan Tool", { 'H', false, false, false } },
    { App::ShortcutAction::ToolErase, L"Erase Tool", { 'E', false, false, false } },
    { App::ShortcutAction::New, L"New", { 'N', true, false, false } },
    { App::ShortcutAction::Undo, L"Undo", { 'Z', true, false, false } },
    { App::ShortcutAction::Redo, L"Redo", { 'Y', true, false, false } },
    { App::ShortcutAction::Open, L"Open", { 'O', true, false, false } },
    { App::ShortcutAction::Save, L"Save Cnote", { 'S', true, false, false } },
    { App::ShortcutAction::ExportPdf, L"Export PDF", { 0, false, false, false } },
    { App::ShortcutAction::Copy, L"Copy", { 'C', true, false, false } },
    { App::ShortcutAction::Paste, L"Paste", { 'V', true, false, false } },
    { App::ShortcutAction::ResetView, L"Reset View", { '0', false, false, false } },
    { App::ShortcutAction::ToggleSettings, L"Settings", { 'B', true, false, false } },
    { App::ShortcutAction::ToggleWheel, L"Wheel", { 'Q', true, false, false } },
    { App::ShortcutAction::PreviousPage, L"Prev Page", { VK_PRIOR, false, false, false } },
    { App::ShortcutAction::NextPage, L"Next Page", { VK_NEXT, false, false, false } },
}};

}  // namespace

std::span<const ShortcutDefinition> ShortcutDefinitions() {
    return std::span<const ShortcutDefinition>(kShortcutDefinitions);
}

const ShortcutDefinition* FindShortcutDefinition(App::ShortcutAction action) {
    for (const ShortcutDefinition& definition : kShortcutDefinitions) {
        if (definition.action == action) {
            return &definition;
        }
    }
    return nullptr;
}

const char* ShortcutActionConfigName(App::ShortcutAction action) {
    switch (action) {
    case App::ShortcutAction::ToolDraw:
        return "ToolDraw";
    case App::ShortcutAction::ToolSelect:
        return "ToolSelect";
    case App::ShortcutAction::ToolMove:
        return "ToolMove";
    case App::ShortcutAction::ToolPan:
        return "ToolPan";
    case App::ShortcutAction::ToolErase:
        return "ToolErase";
    case App::ShortcutAction::New:
        return "New";
    case App::ShortcutAction::Undo:
        return "Undo";
    case App::ShortcutAction::Redo:
        return "Redo";
    case App::ShortcutAction::Open:
        return "Open";
    case App::ShortcutAction::Save:
        return "Save";
    case App::ShortcutAction::ExportPdf:
        return "ExportPdf";
    case App::ShortcutAction::Copy:
        return "Copy";
    case App::ShortcutAction::Paste:
        return "Paste";
    case App::ShortcutAction::ResetView:
        return "ResetView";
    case App::ShortcutAction::ToggleSettings:
        return "ToggleSettings";
    case App::ShortcutAction::ToggleWheel:
        return "ToggleWheel";
    case App::ShortcutAction::PreviousPage:
        return "PreviousPage";
    case App::ShortcutAction::NextPage:
        return "NextPage";
    default:
        return "";
    }
}

std::optional<App::ShortcutAction> ParseShortcutActionConfigName(std::string_view name) {
    for (const ShortcutDefinition& definition : kShortcutDefinitions) {
        if (name == ShortcutActionConfigName(definition.action)) {
            return definition.action;
        }
    }
    return std::nullopt;
}

}  // namespace cnote
