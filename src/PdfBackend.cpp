#include "PdfBackend.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <list>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QSaveFile>

#include "PdfOverlay.h"
#include "Unicode.h"
#include "selection/InkOutline.h"
#include "selection/StrokeSmoothing.h"

#include "fpdf_attachment.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_save.h"
#include "fpdf_text.h"
#include "fpdfview.h"

namespace cnote {

namespace {

constexpr float kStrokeRenderWidthDivisor = 2.15f;
constexpr char16_t kEmbeddedCnoteAttachmentName[] = u"cnote-edit-data.cnote";

// PDFium wide strings are UTF-16 code units; build a null-terminated
// FPDF_WCHAR buffer explicitly rather than reinterpreting wchar_t.
std::vector<FPDF_WCHAR> PdfWideString(std::u16string_view value) {
    std::vector<FPDF_WCHAR> buffer;
    buffer.reserve(value.size() + 1U);
    for (char16_t unit : value) {
        buffer.push_back(static_cast<FPDF_WCHAR>(unit));
    }
    buffer.push_back(0);
    return buffer;
}

std::optional<std::u16string> AttachmentName(FPDF_ATTACHMENT attachment) {
    const unsigned long byteLength = FPDFAttachment_GetName(attachment, nullptr, 0);
    if (byteLength < sizeof(FPDF_WCHAR) || byteLength % sizeof(FPDF_WCHAR) != 0) {
        return std::nullopt;
    }

    std::vector<FPDF_WCHAR> buffer(byteLength / sizeof(FPDF_WCHAR));
    const unsigned long copiedLength = FPDFAttachment_GetName(attachment, buffer.data(), byteLength);
    if (copiedLength != byteLength || buffer.empty()) {
        return std::nullopt;
    }

    const std::size_t charCount = buffer.back() == 0 ? buffer.size() - 1U : buffer.size();
    std::u16string name;
    name.reserve(charCount);
    for (std::size_t index = 0; index < charCount; ++index) {
        name.push_back(static_cast<char16_t>(buffer[index]));
    }
    return name;
}

std::optional<std::vector<std::uint8_t>> AttachmentFileBytes(FPDF_ATTACHMENT attachment) {
    unsigned long byteLength = 0;
    if (FPDFAttachment_GetFile(attachment, nullptr, 0, &byteLength) == 0 || byteLength == 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(byteLength);
    unsigned long copiedLength = 0;
    if (FPDFAttachment_GetFile(attachment, bytes.data(), byteLength, &copiedLength) == 0
        || copiedLength != byteLength) {
        return std::nullopt;
    }
    return bytes;
}

bool AppendEmbeddedCnoteProject(
    FPDF_DOCUMENT documentHandle,
    std::span<const std::uint8_t> projectBytes) {
    // PDFium only supports attachment contents smaller than INT_MAX bytes;
    // reject earlier instead of handing it a length it cannot write.
    if (documentHandle == nullptr || projectBytes.empty()
        || projectBytes.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    const std::vector<FPDF_WCHAR> attachmentName = PdfWideString(kEmbeddedCnoteAttachmentName);
    FPDF_ATTACHMENT attachment = FPDFDoc_AddAttachment(documentHandle, attachmentName.data());
    if (attachment == nullptr) {
        return false;
    }

    return FPDFAttachment_SetFile(
        attachment,
        documentHandle,
        projectBytes.data(),
        static_cast<unsigned long>(projectBytes.size())) != 0;
}

// Hard ceiling for one PDFium bitmap: the screen path keeps its page rasters
// far below this, but export and stored-raster paths must still never hand
// PDFium/QImage a dimension that overflows or cannot exist.
constexpr int kMaxPdfBitmapDimension = 16384;

// Rounds a page size at a raster scale, or 0 when the resulting bitmap must not
// be allocated (non-finite, non-positive, past the hard ceiling, or out of the
// int range - casting a huge double to int is undefined).
int CheckedPdfPixelSize(float pageSize, float renderScale) {
    const double value = static_cast<double>(pageSize) * static_cast<double>(renderScale);
    if (!std::isfinite(value) || value < 1.0 || value > kMaxPdfBitmapDimension) {
        return 0;
    }
    return static_cast<int>(std::lround(value));
}

// Renders one page bitmap straight into the raster's own pixel buffer. Passing
// that buffer to FPDFBitmap_CreateEx means PDFium writes the final pixels
// exactly once: the caller gets owned BGRA bytes instead of a PDFium bitmap
// copied through an intermediate vector.
std::optional<RasterImage> RenderPageIntoOwnedRaster(FPDF_PAGE pageHandle,
    int bitmapWidth, int bitmapHeight, float renderScale,
    int drawX, int drawY, int drawWidth, int drawHeight) {
    if (pageHandle == nullptr || bitmapWidth <= 0 || bitmapHeight <= 0 || renderScale <= 0.0f) {
        return std::nullopt;
    }

    const int stride = bitmapWidth * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * static_cast<std::size_t>(bitmapHeight));
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        bitmapWidth, bitmapHeight, FPDFBitmap_BGRA, pixels.data(), stride);
    if (bitmap == nullptr) {
        return std::nullopt;
    }

    FPDFBitmap_FillRect(bitmap, 0, 0, bitmapWidth, bitmapHeight, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, pageHandle, drawX, drawY, drawWidth, drawHeight, 0, FPDF_ANNOT);

    RasterImage raster {};
    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = FPDFBitmap_GetStride(bitmap);
    raster.scale = renderScale;
    FPDFBitmap_Destroy(bitmap);
    raster.pixels = PixelBuffer(std::move(pixels));
    return raster;
}

std::optional<RasterImage> RenderPageBitmap(FPDF_PAGE pageHandle, float renderScale) {
    if (pageHandle == nullptr || renderScale <= 0.0f) {
        return std::nullopt;
    }

    const float pageWidth = std::max(1.0f, FPDF_GetPageWidthF(pageHandle));
    const float pageHeight = std::max(1.0f, FPDF_GetPageHeightF(pageHandle));
    const int bitmapWidth = CheckedPdfPixelSize(pageWidth, renderScale);
    const int bitmapHeight = CheckedPdfPixelSize(pageHeight, renderScale);
    if (bitmapWidth == 0 || bitmapHeight == 0) {
        return std::nullopt;
    }

    return RenderPageIntoOwnedRaster(
        pageHandle, bitmapWidth, bitmapHeight, renderScale, 0, 0, bitmapWidth, bitmapHeight);
}
float ToPdfY(float pageHeight, float pageY) {
    return pageHeight - pageY;
}

// Pressure ink as one nonzero-filled path of its outline pieces (see
// selection/InkOutline.h), in page units.
bool AppendVectorInk(FPDF_PAGE pageHandle, const Page& page, const Stroke& stroke, const InkPath& ink) {
    constexpr float kPdfInkTolerance = 0.02f;
    InkOutline outline;
    AppendInkOutline(outline,
        BuildInkCenterline(ink, 1.0f,
            [&stroke](float pressure) {
                return std::max(0.6f, stroke.style.width / kStrokeRenderWidthDivisor * InkWidthFactor(pressure));
            },
            kPdfInkTolerance),
        kPdfInkTolerance);
    if (outline.polygonEnds.empty()) {
        return true;
    }

    const Vec2 first = outline.vertices.front();
    FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(first.x, ToPdfY(page.height, first.y));
    if (path == nullptr) {
        return false;
    }
    bool ok = true;
    std::uint32_t begin = 0;
    for (const std::uint32_t end : outline.polygonEnds) {
        if (begin != 0U) {
            ok = ok && FPDFPath_MoveTo(path, outline.vertices[begin].x, ToPdfY(page.height, outline.vertices[begin].y)) != 0;
        }
        for (std::uint32_t index = begin + 1U; ok && index < end; ++index) {
            ok = FPDFPath_LineTo(path, outline.vertices[index].x, ToPdfY(page.height, outline.vertices[index].y)) != 0;
        }
        ok = ok && FPDFPath_Close(path) != 0;
        begin = end;
    }

    const auto alpha = static_cast<unsigned int>((stroke.style.color >> 24U) & 0xFFU);
    const auto red = static_cast<unsigned int>((stroke.style.color >> 16U) & 0xFFU);
    const auto green = static_cast<unsigned int>((stroke.style.color >> 8U) & 0xFFU);
    const auto blue = static_cast<unsigned int>(stroke.style.color & 0xFFU);
    ok = ok
        && FPDFPageObj_SetFillColor(path, red, green, blue, alpha) != 0
        && FPDFPath_SetDrawMode(path, FPDF_FILLMODE_WINDING, 0) != 0;
    if (!ok) {
        FPDFPageObj_Destroy(path);
        return false;
    }
    FPDFPage_InsertObject(pageHandle, path);
    return true;
}

bool AppendVectorStroke(FPDF_PAGE pageHandle, const Page& page, const Stroke& stroke, float strokeSmoothing, float dashLength, float dashGap) {
    if (stroke.points.empty()) {
        return true;
    }

    const std::shared_ptr<const InkPath> ink = CachedStrokeRenderPath(stroke, strokeSmoothing, true);
    if (StrokeHasPressure(stroke)) {
        return AppendVectorInk(pageHandle, page, stroke, *ink);
    }
    const std::vector<Vec2>& smoothedPoints = ink->points;
    if (smoothedPoints.empty()) {
        return true;
    }

    const Vec2 firstPoint = smoothedPoints.front();
    FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(firstPoint.x, ToPdfY(page.height, firstPoint.y));
    if (path == nullptr) {
        return false;
    }

    bool ok = true;
    if (smoothedPoints.size() == 1U) {
        ok = FPDFPath_LineTo(path, firstPoint.x + 0.01f, ToPdfY(page.height, firstPoint.y)) != 0;
    } else {
        for (std::size_t pointIndex = 1; ok && pointIndex < smoothedPoints.size(); ++pointIndex) {
            ok = FPDFPath_LineTo(
                path,
                smoothedPoints[pointIndex].x,
                ToPdfY(page.height, smoothedPoints[pointIndex].y)) != 0;
        }
    }

    const auto alpha = static_cast<unsigned int>((stroke.style.color >> 24U) & 0xFFU);
    const auto red = static_cast<unsigned int>((stroke.style.color >> 16U) & 0xFFU);
    const auto green = static_cast<unsigned int>((stroke.style.color >> 8U) & 0xFFU);
    const auto blue = static_cast<unsigned int>(stroke.style.color & 0xFFU);
    const float pdfStrokeWidth = std::max(0.6f, stroke.style.width / kStrokeRenderWidthDivisor);

    ok = ok
        && FPDFPageObj_SetStrokeColor(path, red, green, blue, alpha) != 0
        && FPDFPageObj_SetStrokeWidth(path, pdfStrokeWidth) != 0
        && FPDFPageObj_SetLineJoin(path, FPDF_LINEJOIN_ROUND) != 0
        && FPDFPageObj_SetLineCap(path, FPDF_LINECAP_ROUND) != 0
        && FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, 1) != 0;
    if (ok && stroke.style.dotted) {
        const float dashArray[] {
            std::max(0.01f, pdfStrokeWidth * std::max(0.5f, dashLength)),
            std::max(0.01f, pdfStrokeWidth * std::max(0.5f, dashGap)),
        };
        ok = FPDFPageObj_SetDashArray(path, dashArray, 2, 0.0f) != 0;
    }
    if (!ok) {
        return false;
    }

    FPDFPage_InsertObject(pageHandle, path);
    return true;
}

bool AppendBitmapObject(
    FPDF_DOCUMENT documentHandle,
    FPDF_PAGE pageHandle,
    int pixelWidth,
    int pixelHeight,
    int stride,
    const std::uint8_t* pixels,
    double a,
    double b,
    double c,
    double d,
    double e,
    double f,
    bool opaque,
    std::vector<FPDF_BITMAP>& ownedBitmaps) {
    if (documentHandle == nullptr || pageHandle == nullptr || pixels == nullptr
        || pixelWidth <= 0 || pixelHeight <= 0 || stride <= 0) {
        return false;
    }

    // PDFium only reads an external BGRA buffer it is handed for an image
    // object; the API just lacks a const overload. BGRx is the same byte
    // layout with the alpha byte ignored, and PDFium then skips cloning a
    // full-image alpha mask for opaque rasters.
    FPDF_BITMAP bitmapHandle = FPDFBitmap_CreateEx(
        pixelWidth,
        pixelHeight,
        opaque ? FPDFBitmap_BGRx : FPDFBitmap_BGRA,
        const_cast<std::uint8_t*>(pixels),
        stride);
    if (bitmapHandle == nullptr) {
        return false;
    }

    FPDF_PAGEOBJECT imageObject = FPDFPageObj_NewImageObj(documentHandle);
    if (imageObject == nullptr) {
        FPDFBitmap_Destroy(bitmapHandle);
        return false;
    }

    const bool ok = FPDFImageObj_SetBitmap(nullptr, 0, imageObject, bitmapHandle) != 0
        && FPDFImageObj_SetMatrix(imageObject, a, b, c, d, e, f) != 0;
    if (!ok) {
        FPDFPageObj_Destroy(imageObject);
        FPDFBitmap_Destroy(bitmapHandle);
        return false;
    }

    ownedBitmaps.push_back(bitmapHandle);
    FPDFPage_InsertObject(pageHandle, imageObject);
    return true;
}

// Places a raster over its page rectangle. PDF image space has v = 0 at the
// image's bottom row, so the matrix' y scale is positive and f is the page
// height of the rectangle's bottom edge.
bool AppendRasterObject(
    FPDF_DOCUMENT documentHandle,
    FPDF_PAGE pageHandle,
    const Page& page,
    const PlacedRaster& placed,
    std::vector<FPDF_BITMAP>& ownedBitmaps) {
    const double height = static_cast<double>(placed.bottom) - static_cast<double>(placed.top);
    if (height <= 0.0) {
        return true;
    }
    return AppendBitmapObject(
        documentHandle,
        pageHandle,
        placed.image.width,
        placed.image.height,
        placed.image.stride,
        placed.image.pixels.data(),
        static_cast<double>(placed.right) - static_cast<double>(placed.left),
        0.0,
        0.0,
        height,
        placed.left,
        static_cast<double>(page.height) - static_cast<double>(placed.bottom),
        placed.opaque,
        ownedBitmaps);
}

bool AppendPageImageObject(
    FPDF_DOCUMENT documentHandle,
    FPDF_PAGE pageHandle,
    const Page& page,
    const PageImage& image,
    std::vector<FPDF_BITMAP>& ownedBitmaps) {
    if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0 || image.stride <= 0) {
        return true;
    }

    const double rotationRadians = -static_cast<double>(image.rotationDegrees) * std::numbers::pi / 180.0;
    const double cosine = std::cos(rotationRadians);
    const double sine = std::sin(rotationRadians);
    const double width = static_cast<double>(image.width);
    const double height = static_cast<double>(image.height);
    const double centerX = static_cast<double>(image.x) + (width * 0.5);
    const double centerY = static_cast<double>(page.height) - static_cast<double>(image.y) - (height * 0.5);
    const double a = cosine * width;
    const double b = sine * width;
    const double c = -sine * height;
    const double d = cosine * height;
    const double e = centerX - ((a + c) * 0.5);
    const double f = centerY - ((b + d) * 0.5);

    return AppendBitmapObject(
        documentHandle,
        pageHandle,
        image.pixelWidth,
        image.pixelHeight,
        image.stride,
        image.pixels.data(),
        a,
        b,
        c,
        d,
        e,
        f,
        false,
        ownedBitmaps);
}

struct PdfFileWriter {
    FPDF_FILEWRITE writer {};
    QSaveFile* file = nullptr;

    static int WriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
        auto* fileWriter = reinterpret_cast<PdfFileWriter*>(self);
        const qint64 expected = static_cast<qint64>(size);
        return fileWriter->file->write(static_cast<const char*>(data), expected) == expected ? 1 : 0;
    }
};

// Owns one export page and the bitmaps that back the image objects inserted
// into it, releasing both at the end of the page instead of at the end of the
// document.
//
// Lifetime contract (PDFium 148 sources):
//   * FPDFImageObj_SetBitmap -> CPDF_ImageObject::SetImage ->
//     CPDF_Image::SetImage copies the whole bitmap into a document-owned
//     CPDF_Stream (and clones the alpha mask into its own stream), then drops
//     its local CFX_DIBitmap reference. The public header does not document
//     that lifetime, so the buffers are kept until the page's
//     FPDFPage_GenerateContent, the last call that reads page objects
//     (CPDF_PageContentGenerator::ProcessImage reads CPDF_Image::GetStream()).
//   * FPDF_ClosePage drops the last reference to the CPDF_Page view; page
//     dictionaries and content streams stay in the CPDF_Document, which
//     FPDF_SaveAsCopy writes through CPDF_Creator. Closing before the save is
//     therefore safe and frees each page's parsed objects and image caches.
struct ExportPageScope {
    FPDF_PAGE page = nullptr;
    std::vector<FPDF_BITMAP> bitmaps;

    ExportPageScope() = default;
    ExportPageScope(const ExportPageScope&) = delete;
    ExportPageScope& operator=(const ExportPageScope&) = delete;

    ~ExportPageScope() {
        for (FPDF_BITMAP bitmap : bitmaps) {
            FPDFBitmap_Destroy(bitmap);
        }
        if (page != nullptr) {
            FPDF_ClosePage(page);
        }
    }
};

}  // namespace

namespace {

// One open PDFium document per PDF source byte buffer, for as long as that
// buffer is alive. PDFium keeps parsing from the caller's memory, so an entry
// pins nothing itself: it holds a weak reference to the PdfSource buffer and
// closes the handle as soon as the buffer is gone. Reacquiring by a different
// shared_ptr to the same buffer (the controller and Document share one) is the
// common case and must reuse the handle; a source replaced with a new
// allocation is a different address, so it cannot collide with a dead entry
// whose address the allocator reused.
//
// All calls are GUI-thread-only, like PDFium itself, and completion/modal
// nested event loops still run on that thread. The cache is an LRU of a few
// documents, because holding every PDF a session ever opened would retain
// parsed page trees and font caches.
class PdfDocumentCache {
public:
    static constexpr std::size_t kMaxCachedDocuments = 8;

    [[nodiscard]] FPDF_DOCUMENT Acquire(const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes) {
        assert(ThreadId() == std::this_thread::get_id());
        if (!pdfBytes) {
            return nullptr;
        }
        PurgeExpiredAddresses();
        const std::vector<std::uint8_t>* address = pdfBytes.get();
        for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
            if (entry->address != address) {
                continue;
            }
            // Same live buffer object: a hit. Keep it MRU without touching the
            // handle.
            entries_.splice(entries_.end(), entries_, entry);
            return entries_.back().handle;
        }

        if (pdfBytes->empty()) {
            return nullptr;
        }
        ++loadCount_;
        FPDF_DOCUMENT handle = FPDF_LoadMemDocument64(pdfBytes->data(), pdfBytes->size(), nullptr);
        entries_.push_back(CachedDocument {
            std::weak_ptr<const std::vector<std::uint8_t>>(pdfBytes),
            address,
            handle,
        });
        while (entries_.size() > kMaxCachedDocuments) {
            Close(entries_.front());
            entries_.pop_front();
        }
        return handle;
    }

    [[nodiscard]] std::size_t LoadCount() const noexcept { return loadCount_; }

    void Start() {
        // The library was initialized for the first time; nothing can be open.
        entries_.clear();
        loadCount_ = 0;
    }

    // Closes every open document. Must run before FPDF_DestroyLibrary.
    void Shutdown() {
        for (CachedDocument& entry : entries_) {
            Close(entry);
        }
        entries_.clear();
    }

private:
    struct CachedDocument {
        std::weak_ptr<const std::vector<std::uint8_t>> bytes;
        const std::vector<std::uint8_t>* address = nullptr;
        // Null records a failed open, so a broken source is not reparsed per
        // render or pointer probe. It expires with the buffer like a success.
        FPDF_DOCUMENT handle = nullptr;
    };

    static const std::thread::id& ThreadId() {
        static const std::thread::id id = std::this_thread::get_id();
        return id;
    }

    static void Close(CachedDocument& entry) {
        if (entry.handle != nullptr) {
            FPDF_CloseDocument(entry.handle);
            entry.handle = nullptr;
        }
    }

    // Drops entries whose byte buffer is gone. A new std::vector allocated at a
    // dead entry's address must never match it, so expiry is decided by the
    // weak reference and the address only breaks ties between live buffers.
    void PurgeExpiredAddresses() {
        for (auto entry = entries_.begin(); entry != entries_.end();) {
            if (entry->bytes.expired()) {
                Close(*entry);
                entry = entries_.erase(entry);
            } else {
                ++entry;
            }
        }
    }

    std::list<CachedDocument> entries_;
    std::size_t loadCount_ = 0;
};

PdfDocumentCache& PdfDocuments() {
    static PdfDocumentCache cache;
    return cache;
}

}  // namespace

RasterImage CreateBlankRaster(float pageWidth, float pageHeight, float renderScale) {
    const int bitmapWidth = CheckedPdfPixelSize(std::max(1.0f, pageWidth), renderScale);
    const int bitmapHeight = CheckedPdfPixelSize(std::max(1.0f, pageHeight), renderScale);
    if (bitmapWidth == 0 || bitmapHeight == 0) {
        // Refused size: an empty raster makes the export path fail cleanly
        // instead of allocating an impossible bitmap.
        return {};
    }
    const int stride = bitmapWidth * 4;

    RasterImage raster {};
    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = stride;
    raster.scale = renderScale;
    raster.pixels = PixelBuffer(std::vector<std::uint8_t>(static_cast<std::size_t>(stride) * static_cast<std::size_t>(bitmapHeight), 0xFF));
    return raster;
}

std::optional<std::vector<std::uint8_t>> PdfiumBackend::ExtractEmbeddedProject(
    const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes) const {
    if (!pdfBytes || pdfBytes->empty()) {
        return std::nullopt;
    }
    FPDF_DOCUMENT documentHandle = PdfDocuments().Acquire(pdfBytes);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> projectBytes;
    const int attachmentCount = FPDFDoc_GetAttachmentCount(documentHandle);
    for (int attachmentIndex = 0; attachmentIndex < attachmentCount; ++attachmentIndex) {
        FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(documentHandle, attachmentIndex);
        if (attachment == nullptr) {
            continue;
        }

        const std::optional<std::u16string> name = AttachmentName(attachment);
        if (name.has_value() && *name == kEmbeddedCnoteAttachmentName) {
            projectBytes = AttachmentFileBytes(attachment);
            break;
        }
    }

    return projectBytes;
}

namespace { unsigned int pdfiumUsers = 0; }

PdfiumBackend::PdfiumBackend() {
    if (pdfiumUsers++ == 0) {
        FPDF_InitLibrary();
        PdfDocuments().Start();
    }
    initialized_ = true;
}

PdfiumBackend::~PdfiumBackend() {
    if (initialized_ && --pdfiumUsers == 0) {
        // Cached documents must close while the library still exists.
        PdfDocuments().Shutdown();
        FPDF_DestroyLibrary();
    }
}

std::optional<Document> PdfiumBackend::OpenFromMemory(
    const std::filesystem::path& sourcePath,
    std::shared_ptr<std::vector<std::uint8_t>> pdfBytes) const {
    if (!initialized_ || !pdfBytes || pdfBytes->empty()) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = PdfDocuments().Acquire(pdfBytes);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0) {
        return std::nullopt;
    }

    // Page sizes come from the page dictionaries; no page is loaded (and its
    // content parsed) just to build the page list, and no raster is allocated.
    // A page whose dictionary cannot be read keeps the default size, claims no
    // source, and paints as blank paper, so later pages keep their indices.
    std::vector<Page> pages;
    pages.reserve(static_cast<std::size_t>(pageCount));
    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        Page page {};
        FS_SIZEF size {};
        if (FPDF_GetPageSizeByIndexF(documentHandle, pageIndex, &size) != 0) {
            page.width = std::max(1.0f, size.width);
            page.height = std::max(1.0f, size.height);
            page.sourcePdfSourceIndex = 0;
            page.sourcePdfPageIndex = pageIndex;
        } else {
            page.sourcePdfSourceIndex = -1;
            page.sourcePdfPageIndex = -1;
        }
        pages.push_back(std::move(page));
    }

    return Document::CreatePdfDocument(sourcePath, std::move(pdfBytes), std::move(pages), 0, {});
}

void PdfiumBackend::HydrateProject(Document& document) const {
    // The open handles live in the shared document cache, so every source is
    // parsed once per session; pages are validated individually and only when
    // a page actually references them.
    for (std::size_t pageIndex = 0; pageIndex < document.PageCount(); ++pageIndex) {
        // Hydration mutates page contents on a document that may already be
        // attached to a session; keep the page-epoch caches honest.
        document.TouchPage(pageIndex);
        Page& page = document.PageAt(pageIndex);
        bool hasValidPdfSource = false;
        if (page.sourcePdfSourceIndex >= 0 && page.sourcePdfPageIndex >= 0) {
            const Document::PdfSource* pdfSource = document.PdfSourceAtIndex(page.sourcePdfSourceIndex);
            if (pdfSource != nullptr && pdfSource->pdfData && !pdfSource->pdfData->empty()) {
                FPDF_DOCUMENT documentHandle = PdfDocuments().Acquire(pdfSource->pdfData);
                const std::size_t sourcePageIndex = static_cast<std::size_t>(page.sourcePdfPageIndex);
                if (documentHandle != nullptr
                    && sourcePageIndex < static_cast<std::size_t>(FPDF_GetPageCount(documentHandle))) {
                    FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, page.sourcePdfPageIndex);
                    if (pageHandle != nullptr) {
                        hasValidPdfSource = true;
                        FPDF_ClosePage(pageHandle);
                    }
                }
            }
        }
        if (!hasValidPdfSource) {
            // The page must not claim a source it cannot render. Its stored
            // raster (if any) stays; with none, the renderer paints blank paper
            // instead of this allocating a full-page fallback raster.
            page.sourcePdfSourceIndex = -1;
            page.sourcePdfPageIndex = -1;
        }
    }
}

std::optional<RasterImage> PdfiumBackend::RenderPage(
    const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
    std::size_t pageIndex,
    float renderScale) const {
    if (!initialized_ || !pdfBytes || pdfBytes->empty() || renderScale <= 0.0f) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = PdfDocuments().Acquire(pdfBytes);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0 || pageIndex >= static_cast<std::size_t>(pageCount)) {
        return std::nullopt;
    }

    FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, static_cast<int>(pageIndex));
    if (pageHandle == nullptr) {
        return std::nullopt;
    }

    const auto raster = RenderPageBitmap(pageHandle, renderScale);

    FPDF_ClosePage(pageHandle);
    return raster;
}

std::optional<RasterImage> PdfiumBackend::RenderPageRegion(
    const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
    std::size_t pageIndex,
    float renderScale,
    PdfPixelRect region) const {
    if (!initialized_ || !pdfBytes || pdfBytes->empty() || renderScale <= 0.0f) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = PdfDocuments().Acquire(pdfBytes);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0 || pageIndex >= static_cast<std::size_t>(pageCount)) {
        return std::nullopt;
    }

    FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, static_cast<int>(pageIndex));
    if (pageHandle == nullptr) {
        return std::nullopt;
    }

    const float pageWidth = std::max(1.0f, FPDF_GetPageWidthF(pageHandle));
    const float pageHeight = std::max(1.0f, FPDF_GetPageHeightF(pageHandle));
    const int fullWidth = CheckedPdfPixelSize(pageWidth, renderScale);
    const int fullHeight = CheckedPdfPixelSize(pageHeight, renderScale);
    if (fullWidth == 0 || fullHeight == 0) {
        FPDF_ClosePage(pageHandle);
        return std::nullopt;
    }

    // Clamp to the full-page raster RenderPage would produce; the excerpt is
    // exactly that raster's pixels.
    const int left = std::clamp(region.x, 0, fullWidth - 1);
    const int top = std::clamp(region.y, 0, fullHeight - 1);
    const int right = std::clamp(region.x + std::max(1, region.width), left + 1, fullWidth);
    const int bottom = std::clamp(region.y + std::max(1, region.height), top + 1, fullHeight);
    const int bitmapWidth = right - left;
    const int bitmapHeight = bottom - top;

    // Same call and same page -> bitmap mapping as the full RenderPage path,
    // with the excerpt's integer device origin as the start position: PDFium
    // then rasterizes each pixel exactly as the full-page render does, so
    // bounded excerpts composite without seams.
    const auto raster = RenderPageIntoOwnedRaster(
        pageHandle, bitmapWidth, bitmapHeight, renderScale, -left, -top, fullWidth, fullHeight);

    FPDF_ClosePage(pageHandle);
    return raster;
}
std::optional<std::vector<PdfTextCharBox>> PdfiumBackend::ExtractTextCharBoxes(
    const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
    std::size_t pageIndex) const {
    if (!initialized_ || !pdfBytes || pdfBytes->empty()) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = PdfDocuments().Acquire(pdfBytes);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0 || pageIndex >= static_cast<std::size_t>(pageCount)) {
        return std::nullopt;
    }

    FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, static_cast<int>(pageIndex));
    if (pageHandle == nullptr) {
        return std::nullopt;
    }

    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(pageHandle);
    std::vector<PdfTextCharBox> boxes;
    if (textPage != nullptr) {
        const int characterCount = FPDFText_CountChars(textPage);
        const float pageHeight = std::max(1.0f, FPDF_GetPageHeightF(pageHandle));
        if (characterCount > 0) {
            boxes.reserve(static_cast<std::size_t>(characterCount));
        }

        for (int characterIndex = 0; characterIndex < characterCount; ++characterIndex) {
            const unsigned int unicode = FPDFText_GetUnicode(textPage, characterIndex);
            if (unicode == 0U) {
                continue;
            }

            double left = 0.0;
            double right = 0.0;
            double bottom = 0.0;
            double top = 0.0;
            if (FPDFText_GetCharBox(textPage, characterIndex, &left, &right, &bottom, &top) == 0
                || !std::isfinite(left)
                || !std::isfinite(right)
                || !std::isfinite(bottom)
                || !std::isfinite(top)) {
                continue;
            }

            // PDFium reports whitespace as a degenerate box: space chars that
            // carry a word gap sit at a point, and its inserted \r/\n line
            // markers always are.  Those chars are the only source of spaces
            // and line breaks for copied text, so they stay in the list (a
            // point/zero-area box still cannot contain a hit-test point);
            // anything else without extent is a broken glyph and goes.
            const bool whitespace = unicode == static_cast<unsigned int>(L' ')
                || unicode == static_cast<unsigned int>(L'\t')
                || unicode == static_cast<unsigned int>(L'\r')
                || unicode == static_cast<unsigned int>(L'\n');
            if (!whitespace && (right <= left || top <= bottom)) {
                continue;
            }

            boxes.push_back({
                unicode,
                static_cast<float>(left),
                static_cast<float>(pageHeight - top),
                static_cast<float>(right),
                static_cast<float>(pageHeight - bottom),
            });
        }

        FPDFText_ClosePage(textPage);
    }

    FPDF_ClosePage(pageHandle);
    return boxes;
}

std::size_t PdfiumBackend::DocumentLoadCount() const {
    return PdfDocuments().LoadCount();
}

bool PdfiumBackend::ExportPdf(
    const Document& document,
    const std::filesystem::path& filePath,
    float renderScale,
    float strokeSmoothing,
    float dashLength,
    float dashGap,
    std::span<const std::uint8_t> embeddedProject) const {
    if (!initialized_ || filePath.empty() || document.PageCount() == 0U || renderScale <= 0.0f) {
        return false;
    }

    FPDF_DOCUMENT outputDocument = FPDF_CreateNewDocument();
    if (outputDocument == nullptr) {
        return false;
    }

    std::vector<FPDF_DOCUMENT> sourceDocuments(document.PdfSourceCount(), nullptr);

    bool ok = true;
    for (std::size_t pageIndex = 0; ok && pageIndex < document.PageCount(); ++pageIndex) {
        const Page& sourcePage = document.PageAt(pageIndex);
        // Releases this page's bitmaps and handle at the end of the iteration,
        // after FPDFPage_GenerateContent.
        ExportPageScope page;
        bool importedSourcePage = false;
        bool imagesAlreadyFlattened = false;

        if (const Document::PdfSource* pdfSource = document.PdfSourceAtIndex(sourcePage.sourcePdfSourceIndex);
            pdfSource != nullptr && sourcePage.sourcePdfPageIndex >= 0) {
            FPDF_DOCUMENT& sourceDocument = sourceDocuments[static_cast<std::size_t>(sourcePage.sourcePdfSourceIndex)];
            if (sourceDocument == nullptr && pdfSource->pdfData && !pdfSource->pdfData->empty()) {
                sourceDocument = FPDF_LoadMemDocument64(pdfSource->pdfData->data(), pdfSource->pdfData->size(), nullptr);
            }

            if (sourceDocument != nullptr) {
                const int sourcePageIndex = sourcePage.sourcePdfPageIndex;
                importedSourcePage = FPDF_ImportPagesByIndex(
                    outputDocument,
                    sourceDocument,
                    &sourcePageIndex,
                    1,
                    static_cast<int>(pageIndex)) != 0;
                if (importedSourcePage) {
                    page.page = FPDF_LoadPage(outputDocument, static_cast<int>(pageIndex));
                    if (page.page == nullptr) {
                        ok = false;
                        break;
                    }
                }
            }
        }

        if (page.page == nullptr) {
            page.page = FPDFPage_New(
                outputDocument,
                static_cast<int>(pageIndex),
                sourcePage.width,
                sourcePage.height);
            if (page.page == nullptr) {
                ok = false;
                break;
            }

            std::optional<PlacedRaster> background;
            if (!importedSourcePage && sourcePage.sourcePdfPageIndex >= 0) {
                background = ComposePageRaster(*this, document, pageIndex, renderScale, strokeSmoothing, dashLength, dashGap, false);
                imagesAlreadyFlattened = true;
            } else if (!sourcePage.raster.pixels.empty()
                && sourcePage.raster.width > 0
                && sourcePage.raster.height > 0
                && sourcePage.raster.stride > 0) {
                background = PlacedRaster {
                    sourcePage.raster,
                    0.0f,
                    0.0f,
                    sourcePage.width,
                    sourcePage.height,
                    IsOpaqueBgra(sourcePage.raster),
                };
            }

            if (background.has_value()
                && !AppendRasterObject(outputDocument, page.page, sourcePage, *background, page.bitmaps)) {
                ok = false;
                break;
            }
        }

        if (!imagesAlreadyFlattened) {
            for (const PageImage& image : sourcePage.images) {
                ok = AppendPageImageObject(outputDocument, page.page, sourcePage, image, page.bitmaps);
                if (!ok) {
                    break;
                }
            }
            if (!ok) {
                break;
            }
        }

        for (const PlacedRaster& textOverlay : ComposeTextOverlayRasters(sourcePage, renderScale)) {
            ok = AppendRasterObject(outputDocument, page.page, sourcePage, textOverlay, page.bitmaps);
            if (!ok) {
                break;
            }
        }
        if (!ok) {
            break;
        }

        for (const Stroke& stroke : sourcePage.strokes) {
            ok = AppendVectorStroke(page.page, sourcePage, stroke, strokeSmoothing, dashLength, dashGap);
            if (!ok) {
                break;
            }
        }
        if (!ok) {
            break;
        }
        ok = FPDFPage_GenerateContent(page.page) != 0;
    }

    if (ok && !embeddedProject.empty()) {
        ok = AppendEmbeddedCnoteProject(outputDocument, embeddedProject);
    }

    if (ok) {
        const std::string utf8Path = PathToUtf8(filePath);
        QSaveFile file(QString::fromUtf8(utf8Path.c_str()));
        if (!file.open(QIODevice::WriteOnly)) {
            ok = false;
        } else {
            PdfFileWriter fileWriter {};
            fileWriter.writer.version = 1;
            fileWriter.writer.WriteBlock = &PdfFileWriter::WriteBlock;
            fileWriter.file = &file;
            ok = FPDF_SaveAsCopy(outputDocument, &fileWriter.writer, FPDF_NO_INCREMENTAL) != 0;
            // Only a successful copy may replace the destination; otherwise the
            // temporary is discarded by the QSaveFile destructor.
            if (ok) {
                ok = file.commit();
            }
        }
    }

    for (FPDF_DOCUMENT sourceDocument : sourceDocuments) {
        if (sourceDocument != nullptr) {
            FPDF_CloseDocument(sourceDocument);
        }
    }
    FPDF_CloseDocument(outputDocument);
    return ok;
}

}  // namespace cnote
