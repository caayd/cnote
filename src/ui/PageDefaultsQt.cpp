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

bool ApplyConfiguredSettingsToPage(
    Page& page,
    float width,
    float height,
    std::uint32_t backgroundColor,
    const std::vector<std::uint32_t>& paletteProfile,
    bool applyPaletteProfileToExistingStrokes) {
    bool changed = false;
    const float clampedWidth = std::max(kMinPageSize, width);
    const float clampedHeight = std::max(kMinPageSize, height);
    if (page.width != clampedWidth) {
        page.width = clampedWidth;
        changed = true;
    }
    if (page.height != clampedHeight) {
        page.height = clampedHeight;
        changed = true;
    }

    RasterImage raster = CreateSolidRaster(page.width, page.height, 1.5f, backgroundColor);
    if (page.sourcePdfSourceIndex != -1
        || page.sourcePdfPageIndex != -1
        || !RasterImageEqual(page.raster, raster)) {
        page.sourcePdfSourceIndex = -1;
        page.sourcePdfPageIndex = -1;
        page.raster = std::move(raster);
        changed = true;
    }

    if (applyPaletteProfileToExistingStrokes) {
        changed = ApplyPaletteProfileToPage(page, paletteProfile) || changed;
    }

    return changed;
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
