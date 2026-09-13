#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Document.h"

namespace cnote::ui {

// New-page settings model and page-application policy, ported from the legacy
// App helpers in src/AppSettings.cpp (ApplyConfiguredSettingsToPage,
// ApplyPaletteProfileToPage, RemapPaletteColor, ResolvedPaletteProfile).
// Pure model mutation: the caller owns history, dirty state and cache
// invalidation, exactly as App did.

inline constexpr float kMinPageSize = 72.0f;
inline constexpr std::size_t kMaxRecentPageColors = 8;

// The dialog's editable copy (legacy NewPageSettingsWindow draft fields).
struct NewPageDraft {
    float width = 612.0f;
    float height = 792.0f;
    std::uint32_t backgroundColor = 0xFFFFFFFFU;
    std::vector<std::uint32_t> paletteProfile {};
    bool applyPaletteProfileToExistingStrokes = false;

    bool operator==(const NewPageDraft&) const = default;
};

// The applied "future pages" state (legacy App::newPageSettings_; the document
// only stores the first four fields).
struct NewPageDefaults {
    float width = 612.0f;
    float height = 792.0f;
    std::uint32_t backgroundColor = 0xFFFFFFFFU;
    std::vector<std::uint32_t> paletteProfile {};
    bool applyPaletteProfileToExistingStrokes = false;

    bool operator==(const NewPageDefaults&) const = default;
};

enum class NewPageApplyScope {
    CurrentPage,
    AllPages,
    FuturePages,
};

// Empty profile resolves to the current palette, then the default palette.
std::vector<std::uint32_t> ResolvedPaletteProfile(
    const std::vector<std::uint32_t>& paletteProfile,
    const std::vector<std::uint32_t>& palette);

std::uint32_t RemapPaletteColor(
    std::uint32_t color,
    const std::vector<std::uint32_t>& sourcePaletteProfile,
    const std::vector<std::uint32_t>& targetPaletteProfile);

// Remaps every stroke colour from the page's stored profile to `target` and
// stores the target profile on the page.  Returns true when anything changed.
bool ApplyPaletteProfileToPage(Page& page, const std::vector<std::uint32_t>& targetPaletteProfile);

// Clamps width/height to >= 72pt, replaces the page raster (flattening any PDF
// source page) and, when requested, remaps existing stroke colours.  Returns
// true when the page changed.
bool ApplyConfiguredSettingsToPage(
    Page& page,
    float width,
    float height,
    std::uint32_t backgroundColor,
    const std::vector<std::uint32_t>& paletteProfile,
    bool applyPaletteProfileToExistingStrokes);

// Legacy ApplyDraftToFuturePages recent-colour bookkeeping: when the colour
// changed, the previous colour is promoted to the front and the new colour is
// de-duplicated; the list is capped at kMaxRecentPageColors.
void RecordPreviousPageColor(
    std::vector<std::uint32_t>& recentColors,
    std::uint32_t previousColor,
    std::uint32_t newColor);

}  // namespace cnote::ui
