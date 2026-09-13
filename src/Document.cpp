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

bool Document::IsPdfDocument() const noexcept {
    return pdfDocument_;
}

const std::filesystem::path& Document::FilePath() const noexcept {
    return filePath_;
}

const std::filesystem::path& Document::SourcePath() const noexcept {
    static const std::filesystem::path emptyPath;
    if (pdfSources_.empty()) {
        return emptyPath;
    }
    return pdfSources_.front().sourcePath;
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
    pdfDocument_ = false;
    pdfSources_.clear();
    pages_ = { Page {} };
    currentPageIndex_ = 0;
    newPageDefaults_ = {};
}

void Document::ReplaceWithEditable(std::filesystem::path filePath, std::vector<Page> pages, std::size_t currentPageIndex, PageDefaults pageDefaults) {
    filePath_ = std::move(filePath);
    pdfDocument_ = false;
    pdfSources_.clear();
    if (pages.empty()) {
        pages_ = { Page {} };
    } else {
        pages_ = std::move(pages);
    }
    currentPageIndex_ = pages_.empty() ? 0 : std::min(currentPageIndex, pages_.size() - 1U);
    newPageDefaults_ = pageDefaults;
}

std::int32_t Document::AppendPdfSource(std::filesystem::path sourcePath, std::shared_ptr<std::vector<std::uint8_t>> pdfData) {
    if (!pdfData || pdfData->empty()) {
        return -1;
    }

    for (std::size_t index = 0; index < pdfSources_.size(); ++index) {
        const PdfSource& existing = pdfSources_[index];
        if (existing.pdfData == pdfData) {
            return static_cast<std::int32_t>(index);
        }
        if (existing.sourcePath == sourcePath
            && existing.pdfData
            && existing.pdfData->size() == pdfData->size()
            && std::equal(existing.pdfData->begin(), existing.pdfData->end(), pdfData->begin())) {
            return static_cast<std::int32_t>(index);
        }
    }

    pdfSources_.push_back({ std::move(sourcePath), std::move(pdfData) });
    pdfDocument_ = !pdfSources_.empty();
    return static_cast<std::int32_t>(pdfSources_.size() - 1U);
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
    pdfDocument_ = !pdfSources_.empty();
    if (pages.empty()) {
        pages_ = { Page {} };
        pdfDocument_ = false;
        pdfSources_.clear();
    } else {
        pages_ = std::move(pages);
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
        && a.content == b.content;
}

bool PageEqual(const Page& a, const Page& b) {
    if (a.width != b.width
        || a.height != b.height
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

}  // namespace cnote
