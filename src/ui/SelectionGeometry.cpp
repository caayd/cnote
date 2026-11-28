#include "ui/SelectionGeometry.h"

#include <algorithm>

#include "text/TextLayoutQt.h"
#include "ui/SelectionMath.h"

namespace cnote::ui {
namespace {

bool SameSelection(const PageSelection& a, const PageSelection& b) {
    return a.strokes == b.strokes && a.images == b.images && a.texts == b.texts
        && a.latexes == b.latexes;
}

SelectionOutlines MeasureSelectionOutlines(const Page& page, const PageSelection& selection) {
    SelectionOutlines geometry;
    for (const std::size_t index : selection.strokes) {
        if (index >= page.strokes.size()) {
            continue;
        }
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        if (StrokeBounds(page.strokes[index], &minX, &minY, &maxX, &maxY)) {
            geometry.outlines.push_back({{ { minX, minY }, { maxX, minY }, { maxX, maxY }, { minX, maxY } }});
        }
    }
    for (const std::size_t index : selection.images) {
        if (index < page.images.size()) {
            geometry.outlines.push_back(ImageCorners(page.images[index]));
        }
    }
    for (const std::size_t index : selection.texts) {
        if (index >= page.texts.size()) {
            continue;
        }
        const text::QtTextBounds textBounds = text::MeasurePageTextBoundsQt(page.texts[index], page.width);
        geometry.outlines.push_back(RotatedRectCorners(
            textBounds.left, textBounds.top, textBounds.right, textBounds.bottom,
            page.texts[index].rotationDegrees));
    }
    for (const std::size_t index : selection.latexes) {
        if (index >= page.latexes.size()) {
            continue;
        }
        const LatexBounds latexBounds = MeasurePageLatexBounds(page.latexes[index], page.width);
        geometry.outlines.push_back(RotatedRectCorners(
            latexBounds.left, latexBounds.top, latexBounds.right, latexBounds.bottom,
            page.latexes[index].rotationDegrees));
    }
    if (!geometry.outlines.empty()) {
        const Vec2 first = geometry.outlines.front().front();
        SelectionBounds bounds { first.x, first.y, first.x, first.y, true };
        for (const std::array<Vec2, 4>& outline : geometry.outlines) {
            for (const Vec2& corner : outline) {
                bounds.left = std::min(bounds.left, corner.x);
                bounds.top = std::min(bounds.top, corner.y);
                bounds.right = std::max(bounds.right, corner.x);
                bounds.bottom = std::max(bounds.bottom, corner.y);
            }
        }
        geometry.bounds = bounds;
    }
    return geometry;
}

}  // namespace

const SelectionOutlines& SelectionGeometryCache::ForPage(const Document& document,
    std::size_t pageIndex, const PageSelection& selection, std::uint64_t transientGeneration) const {
    static const SelectionOutlines kEmpty;
    if (selection.Empty() || pageIndex >= document.PageCount()) {
        return kEmpty;
    }
    const std::uint64_t epoch = document.PageEpoch(pageIndex);
    const auto entry = entries_.find(pageIndex);
    if (entry != entries_.end() && entry->second.epoch == epoch
        && entry->second.transientGeneration == transientGeneration
        && SameSelection(entry->second.selection, selection)) {
        return entry->second.geometry;
    }

    Entry fresh { epoch, transientGeneration, selection,
        MeasureSelectionOutlines(document.PageAt(pageIndex), selection) };
    ++recomputeCount_;
    if (entry == entries_.end()) {
        return entries_.emplace(pageIndex, std::move(fresh)).first->second.geometry;
    }
    entry->second = std::move(fresh);
    return entry->second.geometry;
}

}  // namespace cnote::ui
