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
    // Snapshot the source path before the call: function-argument evaluation
    // order is unspecified, so `std::move(pdfSources)` may empty the vector
    // before `pdfSources.front().sourcePath` is read (which left a PDF
    // container with an empty FilePath and defeated the save guard).
    const std::filesystem::path projectPath = pdfSources.empty()
        ? std::filesystem::path {}
        : pdfSources.front().sourcePath;
    document.ReplaceWithProject(
        projectPath, std::move(pdfSources), std::move(pages), currentPageIndex, pageDefaults);
    // ReplaceWithProject assigns the project kind; a PDF container is the one
    // case where the destination path must never receive a project save.
    document.kind_ = DocumentKind::PdfContainer;
    return document;
}

Document Document::CreateProjectDocument(
    std::filesystem::path filePath,
    std::vector<PdfSource> pdfSources,
    std::vector<Page> pages,
    std::size_t currentPageIndex,
    PageDefaults pageDefaults,
    DocumentKind kind) {
    Document document;
    document.ReplaceWithProject(std::move(filePath), std::move(pdfSources), std::move(pages), currentPageIndex, pageDefaults);
    document.kind_ = kind;
    return document;
}

const std::filesystem::path& Document::FilePath() const noexcept {
    return filePath_;
}

DocumentKind Document::Kind() const noexcept {
    return kind_;
}

void Document::SetKind(DocumentKind kind) noexcept {
    kind_ = kind;
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

bool Document::InsertPages(const std::vector<std::size_t>& positions, const std::vector<Page>& pages) {
    if (positions.empty() || positions.size() != pages.size()
        || !std::is_sorted(positions.begin(), positions.end())
        || std::adjacent_find(positions.begin(), positions.end()) != positions.end()) {
        return false;
    }
    // `positions[offset]` counts the pages inserted before it: in final-gap
    // coordinates the page lands at `positions[offset] - offset`.
    for (std::size_t offset = 0; offset < positions.size(); ++offset) {
        if (positions[offset] > pages_.size() + offset) {
            return false;
        }
    }

    // One pass over both lists: survivors keep their epoch, inserted slots get
    // a fresh one.  The current page is the original page at
    // `currentPageIndex_`; every insertion pushed before it moves its index.
    std::vector<Page> merged;
    std::vector<std::uint64_t> epochs;
    merged.reserve(pages_.size() + pages.size());
    epochs.reserve(pages_.size() + pages.size());
    const bool hadPages = !pages_.empty();
    std::size_t current = currentPageIndex_;
    std::size_t original = 0;
    for (std::size_t offset = 0; offset < positions.size(); ++offset) {
        const std::size_t gap = positions[offset] - offset;
        while (original < gap) {
            merged.push_back(std::move(pages_[original]));
            epochs.push_back(pageEpochs_[original]);
            ++original;
        }
        merged.push_back(pages[offset]);
        epochs.push_back(NextPageEpoch());
        if (hadPages && original <= currentPageIndex_) {
            ++current;
        }
    }
    while (original < pages_.size()) {
        merged.push_back(std::move(pages_[original]));
        epochs.push_back(pageEpochs_[original]);
        ++original;
    }
    pages_ = std::move(merged);
    pageEpochs_ = std::move(epochs);
    currentPageIndex_ = hadPages ? current : 0U;
    return true;
}

bool Document::RemovePages(const std::vector<std::size_t>& indices) {
    if (indices.empty() || indices.size() >= pages_.size()
        || !std::is_sorted(indices.begin(), indices.end())
        || std::adjacent_find(indices.begin(), indices.end()) != indices.end()
        || indices.back() >= pages_.size()) {
        return false;
    }

    // Compacts pages and epochs in one pass, then applies RemovePage's
    // current-page bookkeeping in the same descending order (without moving
    // the vectors again).
    std::size_t remaining = pages_.size();
    for (auto index = indices.rbegin(); index != indices.rend(); ++index) {
        --remaining;
        if (currentPageIndex_ > *index) {
            --currentPageIndex_;
        } else if (currentPageIndex_ >= remaining) {
            currentPageIndex_ = remaining - 1U;
        }
    }

    std::vector<Page> kept;
    std::vector<std::uint64_t> epochs;
    kept.reserve(pages_.size() - indices.size());
    epochs.reserve(pageEpochs_.size() - indices.size());
    std::size_t removed = 0;
    for (std::size_t index = 0; index < pages_.size(); ++index) {
        if (removed < indices.size() && indices[removed] == index) {
            ++removed;
            continue;
        }
        kept.push_back(std::move(pages_[index]));
        epochs.push_back(pageEpochs_[index]);
    }
    pages_ = std::move(kept);
    pageEpochs_ = std::move(epochs);
    return true;
}

bool Document::ReorderPages(const std::vector<std::size_t>& order) {
    if (order.size() != pages_.size()) {
        return false;
    }
    std::vector<bool> seen(order.size(), false);
    bool identity = true;
    for (std::size_t index = 0; index < order.size(); ++index) {
        if (order[index] >= order.size() || seen[order[index]]) {
            return false;
        }
        seen[order[index]] = true;
        identity = identity && order[index] == index;
    }
    if (identity) {
        return false;
    }

    std::vector<Page> pages;
    std::vector<std::uint64_t> epochs;
    pages.reserve(order.size());
    epochs.reserve(order.size());
    std::size_t current = currentPageIndex_;
    for (std::size_t index = 0; index < order.size(); ++index) {
        pages.push_back(std::move(pages_[order[index]]));
        epochs.push_back(pageEpochs_[order[index]]);
        if (order[index] == currentPageIndex_) {
            current = index;
        }
    }
    pages_ = std::move(pages);
    pageEpochs_ = std::move(epochs);
    currentPageIndex_ = current;
    return true;
}

std::vector<std::size_t> Document::MovedPageOrder(
    std::size_t pageCount, const std::vector<std::size_t>& pages, std::size_t slot) {
    std::vector<std::size_t> order;
    order.reserve(pageCount);
    std::size_t moved = 0;  // cursor into the sorted `pages`
    const auto insertMoved = [&]() { order.insert(order.end(), pages.begin(), pages.end()); };
    for (std::size_t index = 0; index < pageCount; ++index) {
        if (index == slot) {
            insertMoved();
        }
        if (moved < pages.size() && pages[moved] == index) {
            ++moved;
            continue;
        }
        order.push_back(index);
    }
    if (slot >= pageCount) {
        insertMoved();
    }
    return order;
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
    kind_ = DocumentKind::Blank;
}

void Document::ReplaceWithEditable(std::filesystem::path filePath, std::vector<Page> pages, std::size_t currentPageIndex, PageDefaults pageDefaults) {
    filePath_ = std::move(filePath);
    pdfSources_.clear();
    kind_ = DocumentKind::Project;
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
    kind_ = DocumentKind::Project;
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

namespace {
// Monotonic buffer identities: never reused, so a cached key can pin which
// bytes a thumbnail drew even after the buffer is freed.
std::uint64_t NextPixelBufferIdentity() noexcept {
    static std::uint64_t next = 1;
    return next++;
}
}  // namespace

PixelBuffer::PixelBuffer(std::vector<std::uint8_t> bytes)
    : bytes_(bytes.empty() ? nullptr : std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)))
    , identity_(bytes_ != nullptr ? NextPixelBufferIdentity() : 0U) {}

const std::vector<std::uint8_t>& PixelBuffer::bytes() const noexcept {
    static const std::vector<std::uint8_t> kEmpty;
    return bytes_ != nullptr ? *bytes_ : kEmpty;
}

bool operator==(const PixelBuffer& a, const PixelBuffer& b) noexcept {
    return a.bytes_ == b.bytes_ || a.bytes() == b.bytes();
}

bool RasterImageEqual(const RasterImage& a, const RasterImage& b) {
    return a.width == b.width
        && a.height == b.height
        && a.stride == b.stride
        && a.scale == b.scale
        && a.pixels == b.pixels;
}

namespace {

// Geometry of the image CreateSolidRaster builds: the shared rounding and
// clamping rule, so the matcher below can never disagree about dimensions.
struct SolidRasterGeometry {
    int width = 0;
    int height = 0;
    int stride = 0;
};

SolidRasterGeometry SolidRasterGeometryFor(float pageWidth, float pageHeight, float renderScale) {
    SolidRasterGeometry geometry;
    geometry.width = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageWidth) * renderScale)));
    geometry.height = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageHeight) * renderScale)));
    geometry.stride = geometry.width * 4;
    return geometry;
}

std::uint64_t g_solidRasterBuildCount = 0;

}  // namespace

std::uint64_t SolidRasterBuildCount() noexcept {
    return g_solidRasterBuildCount;
}

void ResetSolidRasterBuildCount() noexcept {
    g_solidRasterBuildCount = 0;
}

RasterImage CreateSolidRaster(float pageWidth, float pageHeight, float renderScale, std::uint32_t argb) {
    ++g_solidRasterBuildCount;
    const SolidRasterGeometry geometry = SolidRasterGeometryFor(pageWidth, pageHeight, renderScale);
    const int bitmapWidth = geometry.width;
    const int bitmapHeight = geometry.height;
    const int stride = geometry.stride;

    RasterImage raster {};
    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = stride;
    raster.scale = renderScale;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * static_cast<std::size_t>(bitmapHeight));

    const auto alpha = static_cast<std::uint8_t>((argb >> 24U) & 0xFFU);
    const auto red = static_cast<std::uint8_t>((argb >> 16U) & 0xFFU);
    const auto green = static_cast<std::uint8_t>((argb >> 8U) & 0xFFU);
    const auto blue = static_cast<std::uint8_t>(argb & 0xFFU);
    for (std::size_t offset = 0; offset + 3U < pixels.size(); offset += 4U) {
        pixels[offset] = blue;
        pixels[offset + 1U] = green;
        pixels[offset + 2U] = red;
        pixels[offset + 3U] = alpha;
    }
    raster.pixels = PixelBuffer(std::move(pixels));

    return raster;
}

bool RasterImageMatchesSolidColor(
    const RasterImage& raster, float pageWidth, float pageHeight, float renderScale, std::uint32_t argb) {
    const SolidRasterGeometry geometry = SolidRasterGeometryFor(pageWidth, pageHeight, renderScale);
    if (raster.width != geometry.width
        || raster.height != geometry.height
        || raster.stride != geometry.stride
        || raster.scale != renderScale) {
        return false;
    }
    const std::size_t expectedBytes =
        static_cast<std::size_t>(geometry.stride) * static_cast<std::size_t>(geometry.height);
    if (raster.pixels.size() != expectedBytes) {
        return false;
    }
    const std::uint8_t* pixels = raster.pixels.data();
    if (pixels == nullptr) {
        return false;
    }

    const std::uint8_t alpha = static_cast<std::uint8_t>((argb >> 24U) & 0xFFU);
    const std::uint8_t red = static_cast<std::uint8_t>((argb >> 16U) & 0xFFU);
    const std::uint8_t green = static_cast<std::uint8_t>((argb >> 8U) & 0xFFU);
    const std::uint8_t blue = static_cast<std::uint8_t>(argb & 0xFFU);
    for (std::size_t offset = 0; offset < expectedBytes; offset += 4U) {
        if (pixels[offset] != blue
            || pixels[offset + 1U] != green
            || pixels[offset + 2U] != red
            || pixels[offset + 3U] != alpha) {
            return false;
        }
    }
    return true;
}

bool Vec2Equal(const Vec2& a, const Vec2& b) {
    return a.x == b.x && a.y == b.y;
}

bool StrokeStyleEqual(const StrokeStyle& a, const StrokeStyle& b) {
    return a.color == b.color && a.width == b.width && a.dotted == b.dotted;
}

bool StrokeEqual(const Stroke& a, const Stroke& b) {
    if (!StrokeStyleEqual(a.style, b.style) || a.smoothing != b.smoothing
        || a.sampling.streamline != b.sampling.streamline || a.sampling.spacing != b.sampling.spacing
        || a.points.size() != b.points.size() || a.pressures != b.pressures) {
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
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rotated.stride) * static_cast<std::size_t>(rotated.height));

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
            std::uint8_t* destination = pixels.data()
                + (static_cast<std::size_t>(y) * static_cast<std::size_t>(rotated.stride))
                + (static_cast<std::size_t>(x) * 4U);
            std::copy_n(source, 4U, destination);
        }
    }
    rotated.pixels = PixelBuffer(std::move(pixels));
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
    const auto normalizeDegrees = [](float value) {
        const float wrapped = std::fmod(value, 360.0f);
        return wrapped < 0.0f ? wrapped + 360.0f : wrapped;
    };
    for (PageImage& image : page.images) {
        // The image draws into its own width x height box rotated about the
        // box centre, so a turn moves the centre and adds to the rotation;
        // swapping the box's sides as well stretched the pixels.
        const Vec2 center = RotatePointQuarterTurn(
            { image.x + image.width * 0.5f, image.y + image.height * 0.5f }, width, height, turns);
        image.x = center.x - image.width * 0.5f;
        image.y = center.y - image.height * 0.5f;
        image.rotationDegrees = normalizeDegrees(image.rotationDegrees + static_cast<float>(turns) * 90.0f);
    }
    for (PageText& text : page.texts) {
        // Like LaTeX, the box's height comes from its layout, so callers
        // remap (x,y) by the measured bounds centre.
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
