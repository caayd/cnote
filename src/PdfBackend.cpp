#include "PdfBackend.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <windows.h>

#include "fpdfview.h"

namespace ctrainer {

namespace {

constexpr float kInitialRenderScale = 1.5f;

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
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

    const std::string utf8Path = WideToUtf8(filePath);
    if (utf8Path.empty()) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = FPDF_LoadDocument(utf8Path.c_str(), nullptr);
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

    return Document::CreatePdfDocument(filePath, std::move(pages));
}

std::optional<RasterImage> PdfiumBackend::RenderPage(const std::wstring& filePath, std::size_t pageIndex, float renderScale) const {
    if (!initialized_ || filePath.empty() || renderScale <= 0.0f) {
        return std::nullopt;
    }

    const std::string utf8Path = WideToUtf8(filePath);
    if (utf8Path.empty()) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = FPDF_LoadDocument(utf8Path.c_str(), nullptr);
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

}  // namespace ctrainer
