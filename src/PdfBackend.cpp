#include "PdfBackend.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numbers>
#include <string>
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
// Matches the stored-raster convention (representative 612x792 -> 918x1188).
constexpr float kHydratedPdfRasterScale = 1.5f;
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

    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(bitmapWidth, bitmapHeight, FPDFBitmap_BGRA, nullptr, 0);
    if (bitmap == nullptr) {
        return std::nullopt;
    }

    RasterImage raster {};
    FPDFBitmap_FillRect(bitmap, 0, 0, bitmapWidth, bitmapHeight, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, pageHandle, 0, 0, bitmapWidth, bitmapHeight, 0, FPDF_ANNOT);

    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = FPDFBitmap_GetStride(bitmap);
    raster.scale = renderScale;

    const auto* source = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
    const std::size_t totalBytes = static_cast<std::size_t>(raster.stride) * static_cast<std::size_t>(bitmapHeight);
    raster.pixels = PixelBuffer(std::vector<std::uint8_t>(source, source + totalBytes));
    FPDFBitmap_Destroy(bitmap);
    return raster;
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

    const InkPath ink = BuildStrokeRenderPath(stroke, strokeSmoothing);
    if (StrokeHasPressure(stroke)) {
        return AppendVectorInk(pageHandle, page, stroke, ink);
    }
    const std::vector<Vec2>& smoothedPoints = ink.points;
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
    std::vector<FPDF_BITMAP>& ownedBitmaps) {
    if (documentHandle == nullptr || pageHandle == nullptr || pixels == nullptr
        || pixelWidth <= 0 || pixelHeight <= 0 || stride <= 0) {
        return false;
    }

    // PDFium only reads an external BGRA buffer it is handed for an image
    // object; the API just lacks a const overload.
    FPDF_BITMAP bitmapHandle = FPDFBitmap_CreateEx(
        pixelWidth,
        pixelHeight,
        FPDFBitmap_BGRA,
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

bool AppendFullPageRasterObject(
    FPDF_DOCUMENT documentHandle,
    FPDF_PAGE pageHandle,
    const Page& page,
    const RasterImage& raster,
    std::vector<FPDF_BITMAP>& ownedBitmaps) {
    return AppendBitmapObject(
        documentHandle,
        pageHandle,
        raster.width,
        raster.height,
        raster.stride,
        raster.pixels.data(),
        page.width,
        0.0,
        0.0,
        page.height,
        0.0,
        0.0,
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
    FPDF_DOCUMENT documentHandle = FPDF_LoadMemDocument64(pdfBytes->data(), pdfBytes->size(), nullptr);
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

    FPDF_CloseDocument(documentHandle);
    return projectBytes;
}

namespace { unsigned int pdfiumUsers = 0; }

PdfiumBackend::PdfiumBackend() {
    if (pdfiumUsers++ == 0) FPDF_InitLibrary();
    initialized_ = true;
}

PdfiumBackend::~PdfiumBackend() {
    if (initialized_ && --pdfiumUsers == 0) {
        FPDF_DestroyLibrary();
    }
}

std::optional<Document> PdfiumBackend::OpenFromMemory(
    const std::filesystem::path& sourcePath,
    std::shared_ptr<std::vector<std::uint8_t>> pdfBytes) const {
    if (!initialized_ || !pdfBytes || pdfBytes->empty()) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = FPDF_LoadMemDocument64(pdfBytes->data(), pdfBytes->size(), nullptr);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0) {
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    std::vector<Page> pages;
    pages.reserve(static_cast<std::size_t>(pageCount));

    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, pageIndex);
        if (pageHandle == nullptr) {
            // A page that cannot be opened becomes raster-only with a solid
            // fallback instead of silently shifting later page indices: the
            // page must never claim a PDF source it cannot render.
            Page page {};
            FS_SIZEF size {};
            if (FPDF_GetPageSizeByIndexF(documentHandle, pageIndex, &size) != 0) {
                page.width = std::max(1.0f, size.width);
                page.height = std::max(1.0f, size.height);
            }
            page.sourcePdfSourceIndex = -1;
            page.sourcePdfPageIndex = -1;
            page.raster = CreateSolidRaster(page.width, page.height, kHydratedPdfRasterScale, 0xFFFFFFFFU);
            pages.push_back(std::move(page));
            continue;
        }

        Page page {};
        page.width = std::max(1.0f, FPDF_GetPageWidthF(pageHandle));
        page.height = std::max(1.0f, FPDF_GetPageHeightF(pageHandle));
        page.sourcePdfSourceIndex = 0;
        page.sourcePdfPageIndex = pageIndex;

        pages.push_back(std::move(page));
        FPDF_ClosePage(pageHandle);
    }

    FPDF_CloseDocument(documentHandle);

    if (pages.empty()) {
        return std::nullopt;
    }

    return Document::CreatePdfDocument(sourcePath, std::move(pdfBytes), std::move(pages), 0, {});
}

void PdfiumBackend::HydrateProject(Document& document) const {
    std::vector<std::optional<Document>> openedSources;
    openedSources.reserve(document.PdfSourceCount());
    for (std::size_t sourceIndex = 0; sourceIndex < document.PdfSourceCount(); ++sourceIndex) {
        const Document::PdfSource& pdfSource = document.PdfSourceAt(sourceIndex);
        if (!pdfSource.pdfData || pdfSource.pdfData->empty()) {
            openedSources.push_back(std::nullopt);
            continue;
        }
        openedSources.push_back(OpenFromMemory(pdfSource.sourcePath, pdfSource.pdfData));
    }

    for (std::size_t pageIndex = 0; pageIndex < document.PageCount(); ++pageIndex) {
        // Hydration mutates page contents on a document that may already be
        // attached to a session; keep the page-epoch caches honest.
        document.TouchPage(pageIndex);
        Page& page = document.PageAt(pageIndex);
        const bool hasValidPdfSource = page.sourcePdfSourceIndex >= 0
            && static_cast<std::size_t>(page.sourcePdfSourceIndex) < openedSources.size()
            && openedSources[static_cast<std::size_t>(page.sourcePdfSourceIndex)].has_value()
            && page.sourcePdfPageIndex >= 0
            && static_cast<std::size_t>(page.sourcePdfPageIndex)
                < openedSources[static_cast<std::size_t>(page.sourcePdfSourceIndex)]->PageCount();
        if (hasValidPdfSource) {
            const Document& sourceDocument = *openedSources[static_cast<std::size_t>(page.sourcePdfSourceIndex)];
            const Page& sourcePage = sourceDocument.PageAt(static_cast<std::size_t>(page.sourcePdfPageIndex));
            if (sourcePage.sourcePdfPageIndex < 0) {
                // OpenFromMemory marked this source page as unreadable with a
                // raster-only placeholder; this page must not keep claiming it.
                page.sourcePdfSourceIndex = -1;
                page.sourcePdfPageIndex = -1;
                if (page.raster.pixels.empty()) {
                    page.raster = CreateSolidRaster(page.width, page.height, kHydratedPdfRasterScale, 0xFFFFFFFFU);
                }
            } else if (!sourcePage.raster.pixels.empty()) {
                // A source raster that exists replaces the stored snapshot.
                page.raster = sourcePage.raster;
            }
            // Otherwise the background stays lazy: the source page raster is
            // materialized on demand by the PDF background provider, so the
            // stored raster must not be overwritten with an empty one.
        } else {
            page.sourcePdfSourceIndex = -1;
            page.sourcePdfPageIndex = -1;
            if (page.raster.pixels.empty()) {
                page.raster = CreateSolidRaster(page.width, page.height, kHydratedPdfRasterScale, 0xFFFFFFFFU);
            }
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

    FPDF_DOCUMENT documentHandle = FPDF_LoadMemDocument64(pdfBytes->data(), pdfBytes->size(), nullptr);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0 || pageIndex >= static_cast<std::size_t>(pageCount)) {
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, static_cast<int>(pageIndex));
    if (pageHandle == nullptr) {
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    const auto raster = RenderPageBitmap(pageHandle, renderScale);

    FPDF_ClosePage(pageHandle);
    FPDF_CloseDocument(documentHandle);
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

    FPDF_DOCUMENT documentHandle = FPDF_LoadMemDocument64(pdfBytes->data(), pdfBytes->size(), nullptr);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0 || pageIndex >= static_cast<std::size_t>(pageCount)) {
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, static_cast<int>(pageIndex));
    if (pageHandle == nullptr) {
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    const float pageWidth = std::max(1.0f, FPDF_GetPageWidthF(pageHandle));
    const float pageHeight = std::max(1.0f, FPDF_GetPageHeightF(pageHandle));
    const int fullWidth = CheckedPdfPixelSize(pageWidth, renderScale);
    const int fullHeight = CheckedPdfPixelSize(pageHeight, renderScale);
    if (fullWidth == 0 || fullHeight == 0) {
        FPDF_ClosePage(pageHandle);
        FPDF_CloseDocument(documentHandle);
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

    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(bitmapWidth, bitmapHeight, FPDFBitmap_BGRA, nullptr, 0);
    if (bitmap == nullptr) {
        FPDF_ClosePage(pageHandle);
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    RasterImage raster {};
    FPDFBitmap_FillRect(bitmap, 0, 0, bitmapWidth, bitmapHeight, 0xFFFFFFFF);
    // Same call and same page -> bitmap mapping as the full RenderPage path,
    // with the excerpt's integer device origin as the start position: PDFium
    // then rasterizes each pixel exactly as the full-page render does, so
    // bounded excerpts composite without seams.
    FPDF_RenderPageBitmap(
        bitmap, pageHandle, -left, -top, fullWidth, fullHeight, 0, FPDF_ANNOT);

    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = FPDFBitmap_GetStride(bitmap);
    raster.scale = renderScale;

    const auto* source = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
    const std::size_t totalBytes = static_cast<std::size_t>(raster.stride) * static_cast<std::size_t>(bitmapHeight);
    raster.pixels = PixelBuffer(std::vector<std::uint8_t>(source, source + totalBytes));
    FPDFBitmap_Destroy(bitmap);

    FPDF_ClosePage(pageHandle);
    FPDF_CloseDocument(documentHandle);
    return raster;
}
std::optional<std::vector<PdfTextCharBox>> PdfiumBackend::ExtractTextCharBoxes(
    const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
    std::size_t pageIndex) const {
    if (!initialized_ || !pdfBytes || pdfBytes->empty()) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = FPDF_LoadMemDocument64(pdfBytes->data(), pdfBytes->size(), nullptr);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0 || pageIndex >= static_cast<std::size_t>(pageCount)) {
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, static_cast<int>(pageIndex));
    if (pageHandle == nullptr) {
        FPDF_CloseDocument(documentHandle);
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
    FPDF_CloseDocument(documentHandle);
    return boxes;
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

    std::vector<RasterImage> flattenedPages;
    std::vector<FPDF_BITMAP> ownedBitmaps;
    std::vector<FPDF_DOCUMENT> sourceDocuments(document.PdfSourceCount(), nullptr);
    std::vector<FPDF_PAGE> createdPages;
    flattenedPages.reserve(document.PageCount());
    ownedBitmaps.reserve(document.PageCount());
    createdPages.reserve(document.PageCount());

    bool ok = true;
    for (std::size_t pageIndex = 0; ok && pageIndex < document.PageCount(); ++pageIndex) {
        const Page& sourcePage = document.PageAt(pageIndex);
        FPDF_PAGE pageHandle = nullptr;
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
                    pageHandle = FPDF_LoadPage(outputDocument, static_cast<int>(pageIndex));
                    if (pageHandle == nullptr) {
                        ok = false;
                        break;
                    }
                }
            }
        }

        if (pageHandle == nullptr) {
            pageHandle = FPDFPage_New(
                outputDocument,
                static_cast<int>(pageIndex),
                sourcePage.width,
                sourcePage.height);
            if (pageHandle == nullptr) {
                ok = false;
                break;
            }

            bool hasBackgroundRaster = false;
            if (!importedSourcePage && sourcePage.sourcePdfPageIndex >= 0) {
                flattenedPages.push_back(ComposePageRaster(*this, document, pageIndex, renderScale, strokeSmoothing, dashLength, dashGap, false));
                hasBackgroundRaster = true;
                imagesAlreadyFlattened = true;
            } else if (!sourcePage.raster.pixels.empty()
                && sourcePage.raster.width > 0
                && sourcePage.raster.height > 0
                && sourcePage.raster.stride > 0) {
                flattenedPages.push_back(sourcePage.raster);
                hasBackgroundRaster = true;
            }

            if (hasBackgroundRaster && !AppendFullPageRasterObject(
                    outputDocument,
                    pageHandle,
                    sourcePage,
                    flattenedPages.back(),
                    ownedBitmaps)) {
                ok = false;
                break;
            }
        }
        createdPages.push_back(pageHandle);

        if (!imagesAlreadyFlattened) {
            for (const PageImage& image : sourcePage.images) {
                ok = AppendPageImageObject(outputDocument, pageHandle, sourcePage, image, ownedBitmaps);
                if (!ok) {
                    break;
                }
            }
            if (!ok) {
                break;
            }
        }

        if (const auto textOverlay = ComposeTextOverlayRaster(sourcePage, renderScale); textOverlay.has_value()) {
            flattenedPages.push_back(std::move(*textOverlay));
            ok = AppendFullPageRasterObject(
                outputDocument,
                pageHandle,
                sourcePage,
                flattenedPages.back(),
                ownedBitmaps);
            if (!ok) {
                break;
            }
        }

        for (const Stroke& stroke : sourcePage.strokes) {
            ok = AppendVectorStroke(pageHandle, sourcePage, stroke, strokeSmoothing, dashLength, dashGap);
            if (!ok) {
                break;
            }
        }
        ok = FPDFPage_GenerateContent(pageHandle) != 0;
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

    for (FPDF_BITMAP bitmapHandle : ownedBitmaps) {
        FPDFBitmap_Destroy(bitmapHandle);
    }
    for (FPDF_PAGE pageHandle : createdPages) {
        FPDF_ClosePage(pageHandle);
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
