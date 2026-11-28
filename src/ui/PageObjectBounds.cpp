#include "ui/PageObjectBounds.h"

#include <algorithm>
#include <array>
#include <utility>

#include "text/TextLayoutQt.h"
#include "ui/SelectionMath.h"

namespace cnote::ui {
namespace {

ObjectBounds CornersBounds(const std::array<Vec2, 4>& corners) {
    ObjectBounds bounds;
    bounds.left = corners.front().x;
    bounds.top = corners.front().y;
    bounds.right = corners.front().x;
    bounds.bottom = corners.front().y;
    for (const Vec2& corner : corners) {
        bounds.left = std::min(bounds.left, corner.x);
        bounds.top = std::min(bounds.top, corner.y);
        bounds.right = std::max(bounds.right, corner.x);
        bounds.bottom = std::max(bounds.bottom, corner.y);
    }
    bounds.valid = true;
    return bounds;
}

PageObjectBounds MeasurePageObjectBounds(const Page& page) {
    PageObjectBounds bounds;
    bounds.images.reserve(page.images.size());
    bounds.texts.reserve(page.texts.size());
    bounds.latexes.reserve(page.latexes.size());
    bounds.strokes.reserve(page.strokes.size());

    for (const PageImage& image : page.images) {
        bounds.images.push_back(CornersBounds(ImageCorners(image)));
    }
    for (const PageText& text : page.texts) {
        const text::QtTextBounds measured = text::MeasurePageTextBoundsQt(text, page.width);
        bounds.texts.push_back(CornersBounds(RotatedRectCorners(
            measured.left, measured.top, measured.right, measured.bottom, text.rotationDegrees)));
    }
    for (const PageLatex& latex : page.latexes) {
        const LatexBounds measured = MeasurePageLatexBounds(latex, page.width);
        bounds.latexes.push_back(CornersBounds(RotatedRectCorners(
            measured.left, measured.top, measured.right, measured.bottom, latex.rotationDegrees)));
    }
    for (const Stroke& stroke : page.strokes) {
        ObjectBounds object;
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        if (StrokeBounds(stroke, &minX, &minY, &maxX, &maxY)) {
            // The smoothed centreline stays inside the raw points' convex hull,
            // so raw bounds plus the pen's half-width enclose all of the ink.
            const float inflate = std::max(0.0f, stroke.style.width * 0.5f);
            object = ObjectBounds {
                minX - inflate, minY - inflate, maxX + inflate, maxY + inflate, true };
        }
        bounds.strokes.push_back(object);
    }
    return bounds;
}

}  // namespace

const PageObjectBounds& PageObjectBoundsCache::ForPage(
    const Document& document, std::size_t pageIndex, std::uint64_t transientGeneration) const {
    static const PageObjectBounds kEmpty;
    if (pageIndex >= document.PageCount()) {
        return kEmpty;
    }
    const std::uint64_t epoch = document.PageEpoch(pageIndex);
    const auto entry = entries_.find(pageIndex);
    if (entry != entries_.end() && entry->second.epoch == epoch
        && entry->second.transientGeneration == transientGeneration) {
        return entry->second.bounds;
    }

    Entry fresh { epoch, transientGeneration, MeasurePageObjectBounds(document.PageAt(pageIndex)) };
    ++recomputeCount_;
    if (entry == entries_.end()) {
        return entries_.emplace(pageIndex, std::move(fresh)).first->second.bounds;
    }
    entry->second = std::move(fresh);
    return entry->second.bounds;
}

}  // namespace cnote::ui
