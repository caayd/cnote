#include "ui/PageDefaultsQt.h"

#include <algorithm>
#include <cmath>

#include "settings/SettingsStoreQt.h"

namespace cnote::ui {

std::vector<std::uint32_t> ResolvedPaletteProfile(
    const std::vector<std::uint32_t>& paletteProfile,
    const std::vector<std::uint32_t>& palette) {
    if (!paletteProfile.empty()) {
        return paletteProfile;
    }
    if (!palette.empty()) {
        return palette;
    }
    return settings::DefaultPalette();
}

std::uint32_t RemapPaletteColor(
    std::uint32_t color,
    const std::vector<std::uint32_t>& sourcePaletteProfile,
    const std::vector<std::uint32_t>& targetPaletteProfile) {
    const std::size_t remapCount = std::min(sourcePaletteProfile.size(), targetPaletteProfile.size());
    for (std::size_t index = 0; index < remapCount; ++index) {
        if (color == sourcePaletteProfile[index]) {
            return targetPaletteProfile[index];
        }
    }
    return color;
}

bool ApplyPaletteProfileToPage(Page& page, const std::vector<std::uint32_t>& targetPaletteProfile) {
    bool changed = page.paletteProfile != targetPaletteProfile;
    for (Stroke& stroke : page.strokes) {
        const std::uint32_t remappedColor = RemapPaletteColor(stroke.style.color, page.paletteProfile, targetPaletteProfile);
        if (remappedColor != stroke.style.color) {
            stroke.style.color = remappedColor;
            changed = true;
        }
    }
    page.paletteProfile = targetPaletteProfile;
    return changed;
}

namespace {

// The raster scale page defaults have always applied (legacy 1.5x bitmap).
constexpr float kPageApplyRasterScale = 1.5f;

// The work ApplyConfiguredSettingsToPage would do, decided once for both the
// predicate and the mutation so the two can never disagree.
struct PendingPageChanges {
    bool dimensions = false;
    bool raster = false;
    bool palette = false;

    [[nodiscard]] bool Any() const noexcept { return dimensions || raster || palette; }
};

PendingPageChanges ConfiguredSettingsChanges(
    const Page& page,
    float width,
    float height,
    std::uint32_t backgroundColor,
    const std::vector<std::uint32_t>& paletteProfile,
    bool applyPaletteProfileToExistingStrokes) {
    const float clampedWidth = std::max(kMinPageSize, width);
    const float clampedHeight = std::max(kMinPageSize, height);

    PendingPageChanges changes;
    changes.dimensions = page.width != clampedWidth || page.height != clampedHeight;
    // A PDF-backed page is flattened to its solid fill even when the stored
    // raster already looks like that fill, so its source indices are dropped.
    changes.raster = page.sourcePdfSourceIndex != -1
        || page.sourcePdfPageIndex != -1
        || !RasterImageMatchesSolidColor(
            page.raster, clampedWidth, clampedHeight, kPageApplyRasterScale, backgroundColor);
    if (applyPaletteProfileToExistingStrokes) {
        changes.palette = page.paletteProfile != paletteProfile;
        if (!changes.palette) {
            // Mirror ApplyPaletteProfileToPage exactly: it reports a change if
            // the profile differs or any stroke colour remaps.
            for (const Stroke& stroke : page.strokes) {
                if (RemapPaletteColor(stroke.style.color, page.paletteProfile, paletteProfile) != stroke.style.color) {
                    changes.palette = true;
                    break;
                }
            }
        }
    }
    return changes;
}

}  // namespace

bool ApplyConfiguredSettingsToPage(
    Page& page,
    float width,
    float height,
    std::uint32_t backgroundColor,
    const std::vector<std::uint32_t>& paletteProfile,
    bool applyPaletteProfileToExistingStrokes) {
    const PendingPageChanges changes = ConfiguredSettingsChanges(
        page, width, height, backgroundColor, paletteProfile, applyPaletteProfileToExistingStrokes);
    if (!changes.Any()) {
        return false;
    }

    if (changes.dimensions) {
        page.width = std::max(kMinPageSize, width);
        page.height = std::max(kMinPageSize, height);
    }
    if (changes.raster) {
        page.sourcePdfSourceIndex = -1;
        page.sourcePdfPageIndex = -1;
        page.raster = CreateSolidRaster(page.width, page.height, kPageApplyRasterScale, backgroundColor);
    }
    if (changes.palette) {
        ApplyPaletteProfileToPage(page, paletteProfile);
    }
    return true;
}

bool PageSettingsWouldChange(
    const Page& page,
    float width,
    float height,
    std::uint32_t backgroundColor,
    const std::vector<std::uint32_t>& paletteProfile,
    bool applyPaletteProfileToExistingStrokes) {
    return ConfiguredSettingsChanges(
        page, width, height, backgroundColor, paletteProfile, applyPaletteProfileToExistingStrokes).Any();
}

void RecordPreviousPageColor(
    std::vector<std::uint32_t>& recentColors,
    std::uint32_t previousColor,
    std::uint32_t newColor) {
    if (previousColor == newColor) {
        return;
    }

    recentColors.erase(std::remove(recentColors.begin(), recentColors.end(), previousColor), recentColors.end());
    recentColors.erase(std::remove(recentColors.begin(), recentColors.end(), newColor), recentColors.end());
    recentColors.insert(recentColors.begin(), previousColor);
    if (recentColors.size() > kMaxRecentPageColors) {
        recentColors.resize(kMaxRecentPageColors);
    }
}

}  // namespace cnote::ui
