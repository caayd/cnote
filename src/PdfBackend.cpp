#include "PdfBackend.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <objidl.h>
#include <gdiplus.h>

#include "fpdf_edit.h"
#include "fpdf_save.h"
#include "fpdfview.h"

namespace cnote {

namespace {

constexpr float kInitialRenderScale = 1.5f;
constexpr float kMinimumStrokeRenderWidth = 1.25f;
constexpr float kStrokeRenderWidthDivisor = 2.15f;

std::optional<std::vector<std::uint8_t>> ReadAllBytes(const std::wstring& filePath) {
    std::ifstream input(std::filesystem::path(filePath), std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            return std::nullopt;
        }
    }

    return bytes;
}

RasterImage CreateBlankRaster(float pageWidth, float pageHeight, float renderScale) {
    const int bitmapWidth = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageWidth) * renderScale)));
    const int bitmapHeight = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageHeight) * renderScale)));
    const int stride = bitmapWidth * 4;

    RasterImage raster {};
    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = stride;
    raster.scale = renderScale;
    raster.pixels.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(bitmapHeight), 0xFF);
    return raster;
}

std::optional<RasterImage> RenderPageBitmap(FPDF_PAGE pageHandle, float renderScale) {
    if (pageHandle == nullptr || renderScale <= 0.0f) {
        return std::nullopt;
    }

    const float pageWidth = std::max(1.0f, FPDF_GetPageWidthF(pageHandle));
    const float pageHeight = std::max(1.0f, FPDF_GetPageHeightF(pageHandle));
    const int bitmapWidth = std::max(1, static_cast<int>(std::lround(pageWidth * renderScale)));
    const int bitmapHeight = std::max(1, static_cast<int>(std::lround(pageHeight * renderScale)));

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
    raster.pixels.assign(source, source + totalBytes);
    FPDFBitmap_Destroy(bitmap);
    return raster;
}

float DistanceSquared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength) {
    if (points.size() <= 2U || strength <= 0.001f) {
        return points;
    }

    std::vector<Vec2> smoothed;
    smoothed.reserve(points.size());
    smoothed.push_back(points.front());

    const float sideWeight = 0.25f * std::clamp(strength, 0.0f, 1.0f);
    const float centerWeight = 1.0f - (sideWeight * 2.0f);

    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const Vec2 previous = points[index - 1];
        const Vec2 current = points[index];
        const Vec2 next = points[index + 1];
        const Vec2 filtered {
            (previous.x * sideWeight) + (current.x * centerWeight) + (next.x * sideWeight),
            (previous.y * sideWeight) + (current.y * centerWeight) + (next.y * sideWeight),
        };

        if (DistanceSquared(filtered, smoothed.back()) >= 0.05f) {
            smoothed.push_back(filtered);
        }
    }

    if (DistanceSquared(points.back(), smoothed.back()) >= 0.01f) {
        smoothed.push_back(points.back());
    }

    return smoothed;
}

void DrawPageImage(Gdiplus::Graphics& graphics, const PageImage& image, float renderScale) {
    if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0 || image.stride <= 0) {
        return;
    }

    Gdiplus::Bitmap bitmap(
        image.pixelWidth,
        image.pixelHeight,
        image.stride,
        PixelFormat32bppARGB,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data())));

    const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>((image.x + (image.width * 0.5f)) * renderScale);
    const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>((image.y + (image.height * 0.5f)) * renderScale);
    const Gdiplus::REAL drawWidth = static_cast<Gdiplus::REAL>(image.width * renderScale);
    const Gdiplus::REAL drawHeight = static_cast<Gdiplus::REAL>(image.height * renderScale);

    const Gdiplus::GraphicsState state = graphics.Save();
    graphics.TranslateTransform(centerX, centerY);
    graphics.RotateTransform(static_cast<Gdiplus::REAL>(image.rotationDegrees));
    graphics.DrawImage(
        &bitmap,
        Gdiplus::RectF(-drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight));
    graphics.Restore(state);
}

void DrawStroke(Gdiplus::Graphics& graphics, const Stroke& stroke, float renderScale, float strokeSmoothing) {
    if (stroke.points.empty()) {
        return;
    }

    const std::vector<Vec2> smoothedPoints = BuildSmoothedPoints(stroke.points, strokeSmoothing);
    std::vector<Gdiplus::PointF> screenPoints;
    screenPoints.reserve(smoothedPoints.size());
    for (const Vec2& point : smoothedPoints) {
        screenPoints.emplace_back(
            static_cast<Gdiplus::REAL>(point.x * renderScale),
            static_cast<Gdiplus::REAL>(point.y * renderScale));
    }

    const auto alpha = static_cast<BYTE>((stroke.style.color >> 24U) & 0xFFU);
    const auto red = static_cast<BYTE>((stroke.style.color >> 16U) & 0xFFU);
    const auto green = static_cast<BYTE>((stroke.style.color >> 8U) & 0xFFU);
    const auto blue = static_cast<BYTE>(stroke.style.color & 0xFFU);
    const Gdiplus::Color color(alpha, red, green, blue);
    const Gdiplus::REAL penWidth = static_cast<Gdiplus::REAL>(
        std::max(kMinimumStrokeRenderWidth, stroke.style.width * renderScale / kStrokeRenderWidthDivisor));

    Gdiplus::Pen pen(color, penWidth);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);

    if (screenPoints.size() == 1U) {
        Gdiplus::SolidBrush brush(color);
        const Gdiplus::REAL radius = penWidth * 0.5f;
        graphics.FillEllipse(
            &brush,
            screenPoints.front().X - radius,
            screenPoints.front().Y - radius,
            penWidth,
            penWidth);
        return;
    }

    if (screenPoints.size() == 2U) {
        graphics.DrawLines(&pen, screenPoints.data(), static_cast<INT>(screenPoints.size()));
        return;
    }

    Gdiplus::GraphicsPath path;
    path.AddCurve(screenPoints.data(), static_cast<INT>(screenPoints.size()), 0.1f + (strokeSmoothing * 0.8f));
    graphics.DrawPath(&pen, &path);
}

RasterImage ComposePageRaster(
    const PdfiumBackend& backend,
    const Document& document,
    std::size_t pageIndex,
    float renderScale,
    float strokeSmoothing,
    bool includeStrokes) {
    const Page& page = document.PageAt(pageIndex);
    RasterImage raster = document.HasPdfData()
        ? backend.RenderPage(document.PdfData(), pageIndex, renderScale).value_or(CreateBlankRaster(page.width, page.height, renderScale))
        : CreateBlankRaster(page.width, page.height, renderScale);

    Gdiplus::Bitmap bitmap(
        raster.width,
        raster.height,
        raster.stride,
        PixelFormat32bppARGB,
        reinterpret_cast<BYTE*>(raster.pixels.data()));

    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    for (const PageImage& image : page.images) {
        DrawPageImage(graphics, image, renderScale);
    }

    if (includeStrokes) {
        for (const Stroke& stroke : page.strokes) {
            DrawStroke(graphics, stroke, renderScale, strokeSmoothing);
        }
    }

    return raster;
}

float ToPdfY(float pageHeight, float pageY) {
    return pageHeight - pageY;
}

bool AppendVectorStroke(FPDF_PAGE pageHandle, const Page& page, const Stroke& stroke, float strokeSmoothing) {
    if (stroke.points.empty()) {
        return true;
    }

    const std::vector<Vec2> smoothedPoints = BuildSmoothedPoints(stroke.points, strokeSmoothing);
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
    if (!ok) {
        return false;
    }

    FPDFPage_InsertObject(pageHandle, path);
    return true;
}

struct PdfFileWriter {
    FPDF_FILEWRITE writer {};
    std::FILE* file = nullptr;

    static int WriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
        auto* fileWriter = reinterpret_cast<PdfFileWriter*>(self);
        return std::fwrite(data, 1, size, fileWriter->file) == size ? 1 : 0;
    }
};

}  // namespace

PdfiumBackend::PdfiumBackend() {
    FPDF_InitLibrary();
    initialized_ = true;
}

PdfiumBackend::~PdfiumBackend() {
    if (initialized_) {
        FPDF_DestroyLibrary();
    }
}

std::optional<Document> PdfiumBackend::Open(const std::wstring& filePath) {
    if (!initialized_ || filePath.empty()) {
        return std::nullopt;
    }

    const auto pdfBytes = ReadAllBytes(filePath);
    if (!pdfBytes.has_value()) {
        return std::nullopt;
    }

    return OpenFromMemory(filePath, std::make_shared<std::vector<std::uint8_t>>(std::move(*pdfBytes)));
}

std::optional<Document> PdfiumBackend::OpenFromMemory(
    const std::wstring& sourcePath,
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
            continue;
        }

        Page page {};
        page.width = std::max(1.0f, FPDF_GetPageWidthF(pageHandle));
        page.height = std::max(1.0f, FPDF_GetPageHeightF(pageHandle));

        if (const auto raster = RenderPageBitmap(pageHandle, kInitialRenderScale); raster.has_value()) {
            page.raster = std::move(*raster);
        }

        pages.push_back(std::move(page));
        FPDF_ClosePage(pageHandle);
    }

    FPDF_CloseDocument(documentHandle);

    if (pages.empty()) {
        return std::nullopt;
    }

    return Document::CreatePdfDocument(sourcePath, std::move(pdfBytes), std::move(pages));
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

bool PdfiumBackend::ExportPdf(
    const Document& document,
    const std::wstring& filePath,
    float renderScale,
    float strokeSmoothing) const {
    if (!initialized_ || filePath.empty() || document.PageCount() == 0U || renderScale <= 0.0f) {
        return false;
    }

    FPDF_DOCUMENT outputDocument = FPDF_CreateNewDocument();
    if (outputDocument == nullptr) {
        return false;
    }

    std::vector<RasterImage> flattenedPages;
    std::vector<FPDF_BITMAP> ownedBitmaps;
    std::vector<FPDF_PAGE> createdPages;
    flattenedPages.reserve(document.PageCount());
    ownedBitmaps.reserve(document.PageCount());
    createdPages.reserve(document.PageCount());

    bool ok = true;
    for (std::size_t pageIndex = 0; ok && pageIndex < document.PageCount(); ++pageIndex) {
        const Page& sourcePage = document.PageAt(pageIndex);
        flattenedPages.push_back(ComposePageRaster(*this, document, pageIndex, renderScale, strokeSmoothing, false));
        RasterImage& raster = flattenedPages.back();

        FPDF_PAGE pageHandle = FPDFPage_New(
            outputDocument,
            static_cast<int>(pageIndex),
            sourcePage.width,
            sourcePage.height);
        if (pageHandle == nullptr) {
            ok = false;
            break;
        }
        createdPages.push_back(pageHandle);

        FPDF_BITMAP bitmapHandle = FPDFBitmap_CreateEx(
            raster.width,
            raster.height,
            FPDFBitmap_BGRA,
            raster.pixels.data(),
            raster.stride);
        if (bitmapHandle == nullptr) {
            ok = false;
            break;
        }
        ownedBitmaps.push_back(bitmapHandle);

        FPDF_PAGEOBJECT imageObject = FPDFPageObj_NewImageObj(outputDocument);
        if (imageObject == nullptr) {
            ok = false;
            break;
        }

        ok = FPDFImageObj_SetBitmap(nullptr, 0, imageObject, bitmapHandle) != 0
            && FPDFImageObj_SetMatrix(imageObject, sourcePage.width, 0.0, 0.0, sourcePage.height, 0.0, 0.0) != 0;
        if (!ok) {
            break;
        }

        FPDFPage_InsertObject(pageHandle, imageObject);
        for (const Stroke& stroke : sourcePage.strokes) {
            ok = AppendVectorStroke(pageHandle, sourcePage, stroke, strokeSmoothing);
            if (!ok) {
                break;
            }
        }
        ok = FPDFPage_GenerateContent(pageHandle) != 0;
    }

    if (ok) {
        std::FILE* file = _wfopen(filePath.c_str(), L"wb");
        if (file == nullptr) {
            ok = false;
        } else {
            PdfFileWriter fileWriter {};
            fileWriter.writer.version = 1;
            fileWriter.writer.WriteBlock = &PdfFileWriter::WriteBlock;
            fileWriter.file = file;
            ok = FPDF_SaveAsCopy(outputDocument, &fileWriter.writer, FPDF_NO_INCREMENTAL) != 0;
            std::fclose(file);
        }
    }

    for (FPDF_BITMAP bitmapHandle : ownedBitmaps) {
        FPDFBitmap_Destroy(bitmapHandle);
    }
    for (FPDF_PAGE pageHandle : createdPages) {
        FPDF_ClosePage(pageHandle);
    }
    FPDF_CloseDocument(outputDocument);
    return ok;
}

}  // namespace cnote
