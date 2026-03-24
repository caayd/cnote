#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "App.h"

namespace cnote {

struct ShortcutDefinition {
    App::ShortcutAction action;
    const wchar_t* label;
    App::KeyBinding defaultBinding;
};

std::span<const ShortcutDefinition> ShortcutDefinitions();
const ShortcutDefinition* FindShortcutDefinition(App::ShortcutAction action);
const char* ShortcutActionConfigName(App::ShortcutAction action);
std::optional<App::ShortcutAction> ParseShortcutActionConfigName(std::string_view name);

}  // namespace cnote
