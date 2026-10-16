#include "Document.h"

#include <algorithm>
#include <cmath>

namespace cnote {

Document Document::CreateBlank() {
    return CreateBlank(PageDefaults {});
}

Document Document::CreateBlank(PageDefaults pageDefaults) {
    Document document;
    document.ResetToBlank();
    document.SetNewPageDefaults(pageDefaults);
    document.CurrentPage().width = pageDefaults.width;
    document.CurrentPage().height = pageDefaults.height;
    return document;
}

Document Document::CreateEditable(std::filesystem::path filePath, std::vector<Page> pages, std::size_t currentPageIndex, PageDefaults pageDefaults) {
    Document document;
    document.ReplaceWithEditable(std::move(filePath), std::move(pages), currentPageIndex, pageDefaults);
    return document;
}

Document Document::CreatePdfDocument(
    std::filesystem::path sourcePath,
    std::shared_ptr<std::vector<std::uint8_t>> pdfData,
    std::vector<Page> pages,
    std::size_t currentPageIndex,
    PageDefaults pageDefaults) {
    Document document;
    std::vector<PdfSource> pdfSources;
    if (pdfData && !pdfData->empty()) {
        for (Page& page : pages) {
            if (page.sourcePdfPageIndex >= 0 && page.sourcePdfSourceIndex < 0) {
                page.sourcePdfSourceIndex = 0;
            }
        }
        pdfSources.push_back({ std::move(sourcePath), std::move(pdfData) });
    }
    document.ReplaceWithProject(pdfSources.empty() ? std::filesystem::path {} : pdfSources.front().sourcePath, std::move(pdfSources), std::move(pages), currentPageIndex, pageDefaults);
    return document;
}

Document Document::CreateProjectDocument(
    std::filesystem::path filePath,
    std::vector<PdfSource> pdfSources,
    std::vector<Page> pages,
    std::size_t currentPageIndex,
    PageDefaults pageDefaults) {
    Document document;
    document.ReplaceWithProject(std::move(filePath), std::move(pdfSources), std::move(pages), currentPageIndex, pageDefaults);
    return document;
}

const std::filesystem::path& Document::FilePath() const noexcept {
    return filePath_;
}

const std::shared_ptr<std::vector<std::uint8_t>>& Document::PdfData() const noexcept {
    static const std::shared_ptr<std::vector<std::uint8_t>> emptyPdfData;
    if (pdfSources_.empty()) {
        return emptyPdfData;
    }
    return pdfSources_.front().pdfData;
}

bool Document::HasPdfData() const noexcept {
    return !pdfSources_.empty();
}

std::size_t Document::PdfSourceCount() const noexcept {
    return pdfSources_.size();
}

const Document::PdfSource& Document::PdfSourceAt(std::size_t index) const noexcept {
    return pdfSources_.at(index);
}

const Document::PdfSource* Document::PdfSourceAtIndex(std::int32_t index) const noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= pdfSources_.size()) {
        return nullptr;
    }
    return &pdfSources_[static_cast<std::size_t>(index)];
}

std::size_t Document::PageCount() const noexcept {
    return pages_.size();
}

std::size_t Document::CurrentPageIndex() const noexcept {
    return currentPageIndex_;
}

Page& Document::CurrentPage() noexcept {
    return pages_.at(currentPageIndex_);
}

const Page& Document::CurrentPage() const noexcept {
    return pages_.at(currentPageIndex_);
}

Page& Document::PageAt(std::size_t index) noexcept {
    return pages_.at(index);
}

const Page& Document::PageAt(std::size_t index) const noexcept {
    return pages_.at(index);
}
std::uint64_t Document::NextPageEpoch() noexcept {
    // Process-wide counter (page epochs live only as long as the in-memory
    // document and its renderer caches): monotonic and collision-free across
    // page copies, undo snapshots and document reloads.
    static std::uint64_t next = 1;
    return next++;
}

std::uint64_t Document::PageEpoch(std::size_t index) const noexcept {
    return index < pageEpochs_.size() ? pageEpochs_[index] : 0U;
}

void Document::TouchPage(std::size_t index) noexcept {
    if (index < pageEpochs_.size()) {
        pageEpochs_[index] = NextPageEpoch();
    }
}


const Document::PageDefaults& Document::NewPageDefaults() const noexcept {
    return newPageDefaults_;
}

bool Document::SetCurrentPageIndex(std::size_t index) {
    if (index >= pages_.size()) {
        return false;
    }

    currentPageIndex_ = index;
    return true;
}

std::size_t Document::InsertPage(std::size_t index, Page page) {
    const std::size_t insertIndex = std::min(index, pages_.size());
    pages_.insert(pages_.begin() + static_cast<std::ptrdiff_t>(insertIndex), std::move(page));
    // Fresh epoch for the slot: the inserted page must not inherit a stale
    // cache entry from whichever page previously sat at this index.
    pageEpochs_.insert(pageEpochs_.begin() + static_cast<std::ptrdiff_t>(insertIndex), NextPageEpoch());
    if (insertIndex <= currentPageIndex_ && pages_.size() > 1U) {
        ++currentPageIndex_;
    }
    return insertIndex;
}

std::size_t Document::RemovePage(std::size_t index) {
    if (pages_.size() <= 1U || index >= pages_.size()) {
        return currentPageIndex_;
    }

    pages_.erase(pages_.begin() + static_cast<std::ptrdiff_t>(index));
    pageEpochs_.erase(pageEpochs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (currentPageIndex_ > index) {
        --currentPageIndex_;
    } else if (currentPageIndex_ >= pages_.size()) {
        currentPageIndex_ = pages_.size() - 1U;
    }
    return currentPageIndex_;
}

void Document::SetFilePath(std::filesystem::path filePath) {
    filePath_ = std::move(filePath);
}

void Document::SetNewPageDefaults(PageDefaults pageDefaults) {
    newPageDefaults_ = pageDefaults;
}

void Document::ResetToBlank() {
    filePath_.clear();
    pdfSources_.clear();
    pages_ = { Page {} };
    pageEpochs_ = { NextPageEpoch() };
    currentPageIndex_ = 0;
    newPageDefaults_ = {};
}

void Document::ReplaceWithEditable(std::filesystem::path filePath, std::vector<Page> pages, std::size_t currentPageIndex, PageDefaults pageDefaults) {
    filePath_ = std::move(filePath);
    pdfSources_.clear();
    if (pages.empty()) {
        pages_ = { Page {} };
    } else {
        pages_ = std::move(pages);
    }
    pageEpochs_.resize(pages_.size());
    for (std::uint64_t& epoch : pageEpochs_) {
        epoch = NextPageEpoch();
    }
    currentPageIndex_ = pages_.empty() ? 0 : std::min(currentPageIndex, pages_.size() - 1U);
    newPageDefaults_ = pageDefaults;
}

void Document::ReplaceWithProject(
    std::filesystem::path filePath,
    std::vector<PdfSource> pdfSources,
    std::vector<Page> pages,
    std::size_t currentPageIndex,
    PageDefaults pageDefaults) {
    filePath_ = std::move(filePath);
    pdfSources_.clear();
    for (PdfSource& pdfSource : pdfSources) {
        if (pdfSource.pdfData && !pdfSource.pdfData->empty()) {
            pdfSources_.push_back(std::move(pdfSource));
        }
    }
    if (pages.empty()) {
        pages_ = { Page {} };
        pdfSources_.clear();
    } else {
        pages_ = std::move(pages);
    }
    pageEpochs_.resize(pages_.size());
    for (std::uint64_t& epoch : pageEpochs_) {
        epoch = NextPageEpoch();
    }
    currentPageIndex_ = pages_.empty() ? 0 : std::min(currentPageIndex, pages_.size() - 1U);
    newPageDefaults_ = pageDefaults;
}

bool RasterImageEqual(const RasterImage& a, const RasterImage& b) {
    return a.width == b.width
        && a.height == b.height
        && a.stride == b.stride
        && a.scale == b.scale
        && a.pixels == b.pixels;
}

RasterImage CreateSolidRaster(float pageWidth, float pageHeight, float renderScale, std::uint32_t argb) {
    const int bitmapWidth = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageWidth) * renderScale)));
    const int bitmapHeight = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageHeight) * renderScale)));
    const int stride = bitmapWidth * 4;

    RasterImage raster {};
    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = stride;
    raster.scale = renderScale;
    raster.pixels.resize(static_cast<std::size_t>(stride) * static_cast<std::size_t>(bitmapHeight));

    const auto alpha = static_cast<std::uint8_t>((argb >> 24U) & 0xFFU);
    const auto red = static_cast<std::uint8_t>((argb >> 16U) & 0xFFU);
    const auto green = static_cast<std::uint8_t>((argb >> 8U) & 0xFFU);
    const auto blue = static_cast<std::uint8_t>(argb & 0xFFU);
    for (std::size_t offset = 0; offset + 3U < raster.pixels.size(); offset += 4U) {
        raster.pixels[offset] = blue;
        raster.pixels[offset + 1U] = green;
        raster.pixels[offset + 2U] = red;
        raster.pixels[offset + 3U] = alpha;
    }

    return raster;
}

bool Vec2Equal(const Vec2& a, const Vec2& b) {
    return a.x == b.x && a.y == b.y;
}

bool StrokeStyleEqual(const StrokeStyle& a, const StrokeStyle& b) {
    return a.color == b.color && a.width == b.width && a.dotted == b.dotted;
}

bool StrokeEqual(const Stroke& a, const Stroke& b) {
    if (!StrokeStyleEqual(a.style, b.style) || a.smoothing != b.smoothing || a.points.size() != b.points.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.points.size(); ++index) {
        if (!Vec2Equal(a.points[index], b.points[index])) {
            return false;
        }
    }
    return true;
}

bool PageImageEqual(const PageImage& a, const PageImage& b) {
    return a.x == b.x
        && a.y == b.y
        && a.width == b.width
        && a.height == b.height
        && a.rotationDegrees == b.rotationDegrees
        && a.pixelWidth == b.pixelWidth
        && a.pixelHeight == b.pixelHeight
        && a.stride == b.stride
        && a.pixels == b.pixels;
}

bool PageTextStyleEqual(const PageTextStyle& a, const PageTextStyle& b) {
    return std::abs(a.fontSize - b.fontSize) < 0.05f
        && a.color == b.color
        && a.fontFamily == b.fontFamily
        && a.bold == b.bold
        && a.italic == b.italic
        && a.underline == b.underline;
}

bool PageTextEqual(const PageText& a, const PageText& b) {
    return a.id == b.id
        && a.x == b.x
        && a.y == b.y
        && a.width == b.width
        && a.height == b.height
        && a.fontSize == b.fontSize
        && a.color == b.color
        && a.alignment == b.alignment
        && a.fontFamily == b.fontFamily
        && a.bold == b.bold
        && a.italic == b.italic
        && a.underline == b.underline
        && a.content == b.content
        && a.rotationDegrees == b.rotationDegrees
        && a.styleSpans.size() == b.styleSpans.size()
        && std::equal(
            a.styleSpans.begin(),
            a.styleSpans.end(),
            b.styleSpans.begin(),
            [](const PageTextStyleSpan& left, const PageTextStyleSpan& right) {
                return left.start == right.start
                    && left.length == right.length
                    && PageTextStyleEqual(left.style, right.style);
            });
}

bool PageLatexEqual(const PageLatex& a, const PageLatex& b) {
    return a.id == b.id
        && a.x == b.x
        && a.y == b.y
        && a.fontSize == b.fontSize
        && a.color == b.color
        && a.content == b.content
        && a.rotationDegrees == b.rotationDegrees;
}

bool PageEqual(const Page& a, const Page& b) {
    if (a.width != b.width
        || a.height != b.height
        || a.rotationDegrees != b.rotationDegrees
        || a.sourcePdfSourceIndex != b.sourcePdfSourceIndex
        || a.sourcePdfPageIndex != b.sourcePdfPageIndex
        || a.paletteProfile != b.paletteProfile
        || !RasterImageEqual(a.raster, b.raster)
        || a.images.size() != b.images.size()
        || a.texts.size() != b.texts.size()
        || a.latexes.size() != b.latexes.size()
        || a.strokes.size() != b.strokes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.images.size(); ++index) {
        if (!PageImageEqual(a.images[index], b.images[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < a.texts.size(); ++index) {
        if (!PageTextEqual(a.texts[index], b.texts[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < a.latexes.size(); ++index) {
        if (!PageLatexEqual(a.latexes[index], b.latexes[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < a.strokes.size(); ++index) {
        if (!StrokeEqual(a.strokes[index], b.strokes[index])) {
            return false;
        }
    }
    return true;
}

Vec2 RotatePointQuarterTurn(Vec2 point, float width, float height, int quarterTurns) {
    const int turns = ((quarterTurns % 4) + 4) % 4;
    switch (turns) {
    case 1:
        return { height - point.y, point.x };
    case 2:
        return { width - point.x, height - point.y };
    case 3:
        return { point.y, width - point.x };
    default:
        return point;
    }
}

RasterImage RotateRasterQuarterTurn(const RasterImage& raster, int quarterTurns) {
    const int turns = ((quarterTurns % 4) + 4) % 4;
    if (turns == 0 || raster.pixels.empty() || raster.width <= 0 || raster.height <= 0 || raster.stride < raster.width * 4) {
        return raster;
    }

    RasterImage rotated {};
    rotated.width = (turns % 2 == 0) ? raster.width : raster.height;
    rotated.height = (turns % 2 == 0) ? raster.height : raster.width;
    rotated.stride = rotated.width * 4;
    rotated.scale = raster.scale;
    rotated.pixels.resize(static_cast<std::size_t>(rotated.stride) * static_cast<std::size_t>(rotated.height));

    for (int y = 0; y < rotated.height; ++y) {
        for (int x = 0; x < rotated.width; ++x) {
            // Map the destination pixel back to its source: the inverse turn.
            int sourceX = 0;
            int sourceY = 0;
            switch (turns) {
            case 1:
                sourceX = y;
                sourceY = raster.height - 1 - x;
                break;
            case 2:
                sourceX = raster.width - 1 - x;
                sourceY = raster.height - 1 - y;
                break;
            default:
                sourceX = raster.width - 1 - y;
                sourceY = x;
                break;
            }
            const auto* source = raster.pixels.data()
                + (static_cast<std::size_t>(sourceY) * static_cast<std::size_t>(raster.stride))
                + (static_cast<std::size_t>(sourceX) * 4U);
            std::uint8_t* destination = rotated.pixels.data()
                + (static_cast<std::size_t>(y) * static_cast<std::size_t>(rotated.stride))
                + (static_cast<std::size_t>(x) * 4U);
            std::copy_n(source, 4U, destination);
        }
    }
    return rotated;
}

void RotatePageContents(Page& page, int quarterTurns) {
    const int turns = ((quarterTurns % 4) + 4) % 4;
    if (turns == 0) {
        return;
    }
    const float width = page.width;
    const float height = page.height;

    for (Stroke& stroke : page.strokes) {
        for (Vec2& point : stroke.points) {
            point = RotatePointQuarterTurn(point, width, height, turns);
        }
    }
    const auto rotateCenteredRect = [width, height, turns](float& x, float& y, float& rectWidth, float& rectHeight) {
        const Vec2 center = RotatePointQuarterTurn({ x + rectWidth * 0.5f, y + rectHeight * 0.5f }, width, height, turns);
        if (turns % 2 != 0) {
            std::swap(rectWidth, rectHeight);
        }
        x = center.x - rectWidth * 0.5f;
        y = center.y - rectHeight * 0.5f;
    };
    const auto normalizeDegrees = [](float value) {
        const float wrapped = std::fmod(value, 360.0f);
        return wrapped < 0.0f ? wrapped + 360.0f : wrapped;
    };
    for (PageImage& image : page.images) {
        rotateCenteredRect(image.x, image.y, image.width, image.height);
        image.rotationDegrees = normalizeDegrees(image.rotationDegrees + static_cast<float>(turns) * 90.0f);
    }
    for (PageText& text : page.texts) {
        rotateCenteredRect(text.x, text.y, text.width, text.height);
        text.rotationDegrees = normalizeDegrees(text.rotationDegrees + static_cast<float>(turns) * 90.0f);
    }
    for (PageLatex& latex : page.latexes) {
        // The box is derived from latex::Measure, so callers remap (x,y) by
        // the measured bounds centre instead of stored extents.
        latex.rotationDegrees = normalizeDegrees(latex.rotationDegrees + static_cast<float>(turns) * 90.0f);
    }
    page.raster = RotateRasterQuarterTurn(page.raster, turns);
    if (turns % 2 != 0) {
        std::swap(page.width, page.height);
    }
    page.rotationDegrees = normalizeDegrees(page.rotationDegrees + static_cast<float>(turns) * 90.0f);
}

}  // namespace cnote
