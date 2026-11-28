// Qt PDF-export overlay composition (migration plan step 3.4).
//
// The export orchestration in PdfBackend.cpp is shared; only the page raster
// composition lives here. This is the only export overlay implementation (the
// legacy GDI+ exporter was deleted at cutover).

#include "PdfOverlay.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <queue>
#include <vector>

#include <QImage>
#include <QPainter>

#include "PdfBackend.h"
#include "latex/LatexEngine.h"
#include "render/PageRenderer.h"
#include "text/TextLayoutQt.h"

namespace cnote {

namespace {

// Antialiasing and glyph overhang can paint a little outside an object's
// logical bounds. Two page units match the canvas' own text damage margin and
// leave several device pixels of transparent padding at export scale.
constexpr float kOverlayPadding = 2.0f;

// Converts a premultiplied ARGB32 QImage to the straight-alpha BGRA bytes the
// PDFium bitmap path expects.
RasterImage ToStraightBgra(const QImage& image, float renderScale) {
    RasterImage raster {};
    raster.width = image.width();
    raster.height = image.height();
    raster.stride = image.width() * 4;
    raster.scale = renderScale;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(raster.stride) * static_cast<std::size_t>(raster.height));

    for (int y = 0; y < image.height(); ++y) {
        const auto* source = reinterpret_cast<const std::uint8_t*>(image.constScanLine(y));
        std::uint8_t* destination = pixels.data()
            + (static_cast<std::size_t>(y) * static_cast<std::size_t>(raster.stride));
        for (int x = 0; x < image.width(); ++x) {
            const std::uint8_t blue = source[(x * 4) + 0];
            const std::uint8_t green = source[(x * 4) + 1];
            const std::uint8_t red = source[(x * 4) + 2];
            const std::uint8_t alpha = source[(x * 4) + 3];
            if (alpha == 0) {
                destination[(x * 4) + 0] = 0;
                destination[(x * 4) + 1] = 0;
                destination[(x * 4) + 2] = 0;
                destination[(x * 4) + 3] = 0;
                continue;
            }
            destination[(x * 4) + 0] = static_cast<std::uint8_t>(std::min(255, (blue * 255) / alpha));
            destination[(x * 4) + 1] = static_cast<std::uint8_t>(std::min(255, (green * 255) / alpha));
            destination[(x * 4) + 2] = static_cast<std::uint8_t>(std::min(255, (red * 255) / alpha));
            destination[(x * 4) + 3] = alpha;
        }
    }
    raster.pixels = PixelBuffer(std::move(pixels));
    return raster;
}

// Page-unit bounds of one text/LaTeX object, rotation included. Rotation is
// about the object's own bounds centre, matching PaintText/PaintLatex.
struct PageBounds {
    float left = std::numeric_limits<float>::max();
    float top = std::numeric_limits<float>::max();
    float right = std::numeric_limits<float>::lowest();
    float bottom = std::numeric_limits<float>::lowest();

    void Include(float objectLeft, float objectTop, float objectRight, float objectBottom, float rotationDegrees) {
        if (std::abs(rotationDegrees) <= 0.0001f) {
            left = std::min(left, objectLeft);
            top = std::min(top, objectTop);
            right = std::max(right, objectRight);
            bottom = std::max(bottom, objectBottom);
            return;
        }
        const float radians = rotationDegrees * (3.14159265358979323846f / 180.0f);
        const float sine = std::sin(radians);
        const float cosine = std::cos(radians);
        const float centerX = (objectLeft + objectRight) * 0.5f;
        const float centerY = (objectTop + objectBottom) * 0.5f;
        for (const Vec2 corner : { Vec2 { objectLeft, objectTop }, Vec2 { objectRight, objectTop },
                 Vec2 { objectRight, objectBottom }, Vec2 { objectLeft, objectBottom } }) {
            const float dx = corner.x - centerX;
            const float dy = corner.y - centerY;
            left = std::min(left, centerX + (dx * cosine) - (dy * sine));
            top = std::min(top, centerY + (dx * sine) + (dy * cosine));
            right = std::max(right, centerX + (dx * cosine) - (dy * sine));
            bottom = std::max(bottom, centerY + (dx * sine) + (dy * cosine));
        }
    }

    [[nodiscard]] bool empty() const { return right <= left || bottom <= top; }
};

// Whole-page-unit crop rectangle of one or more painted objects.
struct OverlayCrop {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] bool empty() const { return right <= left || bottom <= top; }

    // Overlapping or touching crops share one raster: separate images would
    // round the overlapping antialiased edges independently, and a dense page
    // would emit one image object per object.
    [[nodiscard]] bool touches(const OverlayCrop& other) const {
        return left <= other.right && other.left <= right && top <= other.bottom && other.top <= bottom;
    }
};

// Crop an object's painted bounds to whole-page-unit steps (the padding's own
// grid), clamped to the page. Besides keeping every painted device pixel
// inside the crop, that makes the crop origin a whole number of device pixels
// at integral render scales and at half scales, so PDFium's image sampler
// stays on the same grid as the old full-page raster instead of picking up a
// half-pixel phase at zooms below the export resolution.
std::optional<OverlayCrop> CropForBounds(const Page& page, const PageBounds& bounds) {
    if (page.width <= 0.0f || page.height <= 0.0f || bounds.empty()) {
        return std::nullopt;
    }
    constexpr float kCropGrid = kOverlayPadding;
    const auto alignDown = [](float value) { return std::floor(value / kCropGrid) * kCropGrid; };
    const auto alignUp = [](float value) { return std::ceil(value / kCropGrid) * kCropGrid; };
    OverlayCrop crop;
    crop.left = std::max(0.0f, alignDown(bounds.left - kOverlayPadding));
    crop.top = std::max(0.0f, alignDown(bounds.top - kOverlayPadding));
    crop.right = std::min(page.width, alignUp(bounds.right + kOverlayPadding));
    crop.bottom = std::min(page.height, alignUp(bounds.bottom + kOverlayPadding));
    if (crop.empty()) {
        return std::nullopt;
    }
    return crop;
}

// Composes a group of objects, painted in the caller's order, into one raster
// cropped to `crop`. The old overlay painted every object at identity on a
// full-page image; shifting by the integer crop origin keeps each glyph's
// rasterization bit-identical while the raster only covers the group.
template <typename Paint>
std::optional<PlacedRaster> ComposeOverlayRaster(
    const Page& page, const OverlayCrop& crop, float renderScale, Paint&& paint) {
    if (page.width <= 0.0f || page.height <= 0.0f || renderScale <= 0.0f || crop.empty()) {
        return std::nullopt;
    }

    const int pageWidthPx = std::max(1, static_cast<int>(std::lround(page.width * renderScale)));
    const int pageHeightPx = std::max(1, static_cast<int>(std::lround(page.height * renderScale)));
    const int leftPx = std::clamp(static_cast<int>(std::lround(crop.left * renderScale)), 0, pageWidthPx);
    const int topPx = std::clamp(static_cast<int>(std::lround(crop.top * renderScale)), 0, pageHeightPx);
    const int rightPx = std::clamp(static_cast<int>(std::lround(crop.right * renderScale)), 0, pageWidthPx);
    const int bottomPx = std::clamp(static_cast<int>(std::lround(crop.bottom * renderScale)), 0, pageHeightPx);
    if (rightPx <= leftPx || bottomPx <= topPx) {
        return std::nullopt;
    }

    QImage image(rightPx - leftPx, bottomPx - topPx, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.translate(-leftPx, -topPx);
    paint(painter);
    painter.end();

    PlacedRaster placed;
    placed.image = ToStraightBgra(image, renderScale);
    // Use the full-page raster's own pixel-to-page mapping, not 1/renderScale:
    // when round(page.width * renderScale) is not exact, the old full-page
    // matrix and the crop's matrix then sample the page at identical positions.
    placed.left = static_cast<float>(leftPx) * page.width / static_cast<float>(pageWidthPx);
    placed.right = static_cast<float>(rightPx) * page.width / static_cast<float>(pageWidthPx);
    placed.top = static_cast<float>(topPx) * page.height / static_cast<float>(pageHeightPx);
    placed.bottom = static_cast<float>(bottomPx) * page.height / static_cast<float>(pageHeightPx);
    return placed;
}

// One paintable text/LaTeX object and the crop it needs.
struct OverlayItem {
    OverlayCrop crop;
    std::size_t objectIndex = 0;
    bool latex = false;
};

std::size_t FindGroupRoot(std::vector<std::size_t>& parents, std::size_t index) {
    while (parents[index] != index) {
        parents[index] = parents[parents[index]];
        index = parents[index];
    }
    return index;
}

void JoinGroups(std::vector<std::size_t>& parents, std::size_t first, std::size_t second) {
    const std::size_t firstRoot = FindGroupRoot(parents, first);
    const std::size_t secondRoot = FindGroupRoot(parents, second);
    if (firstRoot != secondRoot) {
        parents[secondRoot] = firstRoot;
    }
}

// One raster's worth of overlay objects: the crop it paints into and the
// object indices in paint order.
struct OverlayGroup {
    OverlayCrop crop;
    std::vector<std::size_t> members;
    bool alive = true;
    std::uint32_t version = 0;
};

double CropArea(const OverlayCrop& crop) {
    return std::max(0.0, static_cast<double>(crop.right) - static_cast<double>(crop.left))
        * std::max(0.0, static_cast<double>(crop.bottom) - static_cast<double>(crop.top));
}

OverlayCrop UnionCrop(const OverlayCrop& first, const OverlayCrop& second) {
    OverlayCrop united;
    united.left = std::min(first.left, second.left);
    united.top = std::min(first.top, second.top);
    united.right = std::max(first.right, second.right);
    united.bottom = std::max(first.bottom, second.bottom);
    return united;
}

// A nearby-group merge proposal. `growth` is the empty area the union adds, so
// the cheapest proposals are the ones that waste the least raster.
struct MergeCandidate {
    double growth = 0.0;
    double unionArea = 0.0;
    std::size_t first = 0;
    std::size_t second = 0;
    std::uint32_t firstVersion = 0;
    std::uint32_t secondVersion = 0;
};

struct MergeCandidateIsCheaper {
    bool operator()(const MergeCandidate& left, const MergeCandidate& right) const {
        if (left.growth != right.growth) {
            return left.growth > right.growth;
        }
        if (left.unionArea != right.unionArea) {
            return left.unionArea > right.unionArea;
        }
        if (left.first != right.first) {
            return left.first > right.first;
        }
        return left.second > right.second;
    }
};

// At most this many overlay rasters per page; above it everything is merged
// into one page-bounded crop, which can never exceed the old full-page overlay.
constexpr std::size_t kMaxOverlayRasters = 64;

// Nearby lines and paragraphs collapse into blocks while far-apart objects keep
// their small crops. A merge is allowed only while the union rectangle is at
// most twice the two crops' combined area, so the raster budget stays within
// twice the painted bounds; merges are taken cheapest-growth first.
std::vector<OverlayGroup> MergeGroupsByArea(std::vector<OverlayGroup> groups) {
    std::priority_queue<MergeCandidate, std::vector<MergeCandidate>, MergeCandidateIsCheaper> candidates;
    const auto propose = [&groups, &candidates](std::size_t first, std::size_t second) {
        if (first == second || !groups[first].alive || !groups[second].alive) {
            return;
        }
        if (first > second) {
            std::swap(first, second);
        }
        const double combinedArea = CropArea(groups[first].crop) + CropArea(groups[second].crop);
        const double unionArea = CropArea(UnionCrop(groups[first].crop, groups[second].crop));
        if (unionArea > combinedArea * 2.0) {
            return;
        }
        candidates.push({ unionArea - combinedArea, unionArea, first, second,
            groups[first].version, groups[second].version });
    };

    for (std::size_t first = 0; first < groups.size(); ++first) {
        for (std::size_t second = first + 1; second < groups.size(); ++second) {
            propose(first, second);
        }
    }

    std::size_t aliveCount = groups.size();
    while (aliveCount > 1 && !candidates.empty()) {
        const MergeCandidate candidate = candidates.top();
        candidates.pop();
        if (!groups[candidate.first].alive || !groups[candidate.second].alive
            || groups[candidate.first].version != candidate.firstVersion
            || groups[candidate.second].version != candidate.secondVersion) {
            continue;
        }
        const double combinedArea = CropArea(groups[candidate.first].crop)
            + CropArea(groups[candidate.second].crop);
        const double unionArea = CropArea(UnionCrop(groups[candidate.first].crop, groups[candidate.second].crop));
        if (unionArea > combinedArea * 2.0) {
            continue;
        }

        OverlayGroup& kept = groups[candidate.first];
        OverlayGroup& removed = groups[candidate.second];
        kept.crop = UnionCrop(kept.crop, removed.crop);
        std::vector<std::size_t> mergedMembers;
        mergedMembers.reserve(kept.members.size() + removed.members.size());
        std::merge(kept.members.begin(), kept.members.end(), removed.members.begin(), removed.members.end(),
            std::back_inserter(mergedMembers));
        kept.members = std::move(mergedMembers);
        ++kept.version;
        removed.alive = false;
        removed.members.clear();
        --aliveCount;

        for (std::size_t other = 0; other < groups.size(); ++other) {
            if (other != candidate.first) {
                propose(candidate.first, other);
            }
        }
    }

    if (aliveCount > kMaxOverlayRasters) {
        OverlayGroup combined;
        bool haveCombined = false;
        for (OverlayGroup& group : groups) {
            if (!group.alive) {
                continue;
            }
            if (!haveCombined) {
                combined = std::move(group);
                haveCombined = true;
                continue;
            }
            combined.crop = UnionCrop(combined.crop, group.crop);
            std::vector<std::size_t> mergedMembers;
            mergedMembers.reserve(combined.members.size() + group.members.size());
            std::merge(combined.members.begin(), combined.members.end(), group.members.begin(), group.members.end(),
                std::back_inserter(mergedMembers));
            combined.members = std::move(mergedMembers);
            group.alive = false;
            group.members.clear();
        }
        if (haveCombined) {
            groups.clear();
            groups.push_back(std::move(combined));
        }
    }

    groups.erase(std::remove_if(groups.begin(), groups.end(),
                     [](const OverlayGroup& group) { return !group.alive; }),
        groups.end());
    return groups;
}

}  // namespace

bool IsOpaqueBgra(const RasterImage& raster) {
    if (raster.width <= 0 || raster.height <= 0 || raster.stride < raster.width * 4 || raster.pixels.empty()) {
        return false;
    }
    const std::uint8_t* pixels = raster.pixels.data();
    for (int y = 0; y < raster.height; ++y) {
        const std::uint8_t* row = pixels + (static_cast<std::size_t>(y) * static_cast<std::size_t>(raster.stride));
        for (int x = 0; x < raster.width; ++x) {
            if (row[(x * 4) + 3] != 0xFFU) {
                return false;
            }
        }
    }
    return true;
}

std::vector<PlacedRaster> ComposeTextOverlayRasters(const Page& page, float renderScale) {
    std::vector<PlacedRaster> rasters;
    if (page.width <= 0.0f || page.height <= 0.0f || renderScale <= 0.0f) {
        return rasters;
    }

    std::vector<OverlayItem> items;
    items.reserve(page.texts.size() + page.latexes.size());
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        const PageText& text = page.texts[index];
        if (text.content.empty()) {
            continue;
        }
        const text::QtTextBounds textBounds = text::MeasurePageTextBoundsQt(text, page.width);
        PageBounds bounds;
        bounds.Include(textBounds.left, textBounds.top, textBounds.right, textBounds.bottom, text.rotationDegrees);
        if (const auto crop = CropForBounds(page, bounds); crop.has_value()) {
            items.push_back({ *crop, index, false });
        }
    }
    if (latex::IsInitialized()) {
        for (std::size_t index = 0; index < page.latexes.size(); ++index) {
            const PageLatex& latexBlock = page.latexes[index];
            if (latexBlock.content.empty()) {
                continue;
            }
            const latex::FormulaMetrics metrics = latex::Measure(latex::Parse(latexBlock));
            if (metrics.width <= 0.0f || metrics.height <= 0.0f) {
                continue;
            }
            PageBounds bounds;
            bounds.Include(latexBlock.x, latexBlock.y, latexBlock.x + metrics.width,
                latexBlock.y + metrics.height, latexBlock.rotationDegrees);
            if (const auto crop = CropForBounds(page, bounds); crop.has_value()) {
                items.push_back({ *crop, index, true });
            }
        }
    }
    if (items.empty()) {
        return rasters;
    }

    // Connected components of overlapping or touching crops. Objects in one
    // component keep their paint order and share one image, as they did in the
    // old single full-page overlay; disjoint groups stay small.
    std::vector<std::size_t> parents(items.size());
    for (std::size_t index = 0; index < parents.size(); ++index) {
        parents[index] = index;
    }
    for (std::size_t first = 0; first < items.size(); ++first) {
        for (std::size_t second = first + 1; second < items.size(); ++second) {
            if (items[first].crop.touches(items[second].crop)) {
                JoinGroups(parents, first, second);
            }
        }
    }

    std::vector<OverlayGroup> groups;
    std::vector<int> groupForRoot(items.size(), -1);
    for (std::size_t index = 0; index < items.size(); ++index) {
        const std::size_t root = FindGroupRoot(parents, index);
        if (groupForRoot[root] < 0) {
            groupForRoot[root] = static_cast<int>(groups.size());
            groups.push_back({ items[index].crop, {}, true, 0 });
        }
        groups[static_cast<std::size_t>(groupForRoot[root])].members.push_back(index);
    }

    groups = MergeGroupsByArea(std::move(groups));

    rasters.reserve(groups.size());
    for (const OverlayGroup& group : groups) {
        const auto paintGroup = [&](QPainter& painter) {
            for (const std::size_t itemIndex : group.members) {
                const OverlayItem& item = items[itemIndex];
                if (item.latex) {
                    render::PaintPageLatex(painter, page.latexes[item.objectIndex], renderScale, 1.0f);
                } else {
                    render::PaintPageText(painter, page.texts[item.objectIndex], page.width, renderScale,
                        std::nullopt, 1.0f);
                }
            }
        };
        if (auto placed = ComposeOverlayRaster(page, group.crop, renderScale, paintGroup); placed.has_value()) {
            rasters.push_back(std::move(*placed));
        }
    }
    return rasters;
}

PlacedRaster ComposePageRaster(
    const PdfiumBackend& backend,
    const Document& document,
    std::size_t pageIndex,
    float renderScale,
    float strokeSmoothing,
    float dashLength,
    float dashGap,
    bool includeStrokes) {
    const Page& page = document.PageAt(pageIndex);
    std::optional<RasterImage> pdfRaster;
    if (const Document::PdfSource* pdfSource = document.PdfSourceAtIndex(page.sourcePdfSourceIndex);
        pdfSource != nullptr && page.sourcePdfPageIndex >= 0) {
        pdfRaster = backend.RenderPage(pdfSource->pdfData, static_cast<std::size_t>(page.sourcePdfPageIndex), renderScale);
    }

    const bool useStoredRaster = !pdfRaster.has_value()
        && !page.raster.pixels.empty()
        && page.raster.width > 0
        && page.raster.height > 0;
    const int pageWidthPx = std::max(1, static_cast<int>(std::lround(std::max(1.0f, page.width) * renderScale)));
    const int pageHeightPx = std::max(1, static_cast<int>(std::lround(std::max(1.0f, page.height) * renderScale)));

    // No content to paint over the base: the flattened page is the base itself.
    // The base here is already straight BGRA (a source-page render or the blank
    // fallback), so skip the premultiplied white composition and conversion.
    if (!useStoredRaster && page.images.empty() && !includeStrokes) {
        return { CreateBlankRaster(page.width, page.height, renderScale), 0.0f, 0.0f, page.width, page.height, true };
    }
    // A stored raster already at the export resolution needs no resampling;
    // returning it shares the buffer and leaves nothing to composite.
    if (useStoredRaster && page.images.empty() && !includeStrokes
        && page.raster.stride >= page.raster.width * 4
        && page.raster.width == pageWidthPx
        && page.raster.height == pageHeightPx
        && IsOpaqueBgra(page.raster)) {
        return { page.raster, 0.0f, 0.0f, page.width, page.height, true };
    }

    RasterImage raster = pdfRaster.value_or(CreateBlankRaster(page.width, page.height, renderScale));
    if (pdfRaster.has_value() && std::abs(page.rotationDegrees) > 0.0001f) {
        // Source-page rasters render at source orientation; rotate into the
        // page's final space like the canvas does at paint time.
        raster = RotateRasterQuarterTurn(*pdfRaster,
            static_cast<int>(std::lround(page.rotationDegrees / 90.0f)));
    }

    // The flattened page is opaque white underneath every object, so paint
    // straight into the BGRA buffer PDFium reads instead of composing a
    // premultiplied image and converting it afterwards: for an opaque image the
    // premultiplied bytes are already straight BGRA.
    const int stride = raster.width * 4;
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(stride) * static_cast<std::size_t>(raster.height), 0xFFU);
    {
        QImage image(pixels.data(), raster.width, raster.height, stride, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        if (useStoredRaster) {
            const QImage view = render::WrapBgraImage(
                page.raster.pixels.data(),
                page.raster.width,
                page.raster.height,
                page.raster.stride);
            painter.drawImage(QRectF(0.0, 0.0, raster.width, raster.height), view);
        }

        render::PageRenderParams params;
        params.scale = renderScale;
        params.strokeSmoothing = strokeSmoothing;
        params.dashLength = dashLength;
        params.dashGap = dashGap;
        for (const PageImage& imageObject : page.images) {
            render::PaintImage(painter, imageObject, params, 1.0f);
        }
        if (includeStrokes) {
            for (const Stroke& stroke : page.strokes) {
                render::PaintStrokePreview(painter, stroke, params, 1.0f);
            }
        }
        painter.end();
    }

    RasterImage flattened {};
    flattened.width = raster.width;
    flattened.height = raster.height;
    flattened.stride = stride;
    flattened.scale = renderScale;
    flattened.pixels = PixelBuffer(std::move(pixels));
    return { std::move(flattened), 0.0f, 0.0f, page.width, page.height, true };
}

}  // namespace cnote
