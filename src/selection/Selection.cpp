#include "App.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "DpiUtil.h"
#include "selection/Geometry.h"
#include "text/TextGeometry.h"

namespace cnote {

namespace textgeom = cnote::text;

void App::ClearSelection() {
    selectedImageIndices_.assign(document_.PageCount(), {});
    selectedTextIndices_.assign(document_.PageCount(), {});
    selectedPdfTextBoxes_.assign(document_.PageCount(), {});
    selectedLatexIndices_.assign(document_.PageCount(), {});
    selectedStrokeIndices_.assign(document_.PageCount(), {});
}

bool App::HasSelectedPdfText() const {
    for (const auto& boxes : selectedPdfTextBoxes_) {
        if (!boxes.empty()) {
            return true;
        }
    }
    return false;
}

bool App::HasSelection() const {
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if ((pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty())
            || (pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty())
            || (pageIndex < selectedPdfTextBoxes_.size() && !selectedPdfTextBoxes_[pageIndex].empty())
            || (pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[pageIndex].empty())
            || (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty())) {
            return true;
        }
    }

    return false;
}

std::optional<std::size_t> App::FirstSelectedPageIndex() const {
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const bool hasStrokes = pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty();
        const bool hasTexts = pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty();
        const bool hasPdfText = pageIndex < selectedPdfTextBoxes_.size() && !selectedPdfTextBoxes_[pageIndex].empty();
        const bool hasLatexes = pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[pageIndex].empty();
        const bool hasImages = pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty();
        if (hasStrokes || hasTexts || hasPdfText || hasLatexes || hasImages) {
            return pageIndex;
        }
    }

    return std::nullopt;
}

void App::DeleteSelection(std::optional<std::size_t> pageIndex) {
    if (!pageIndex.has_value() && !HasSelection()) {
        return;
    }

    bool hadSelection = false;
    bool hadEditableSelection = false;
    for (std::size_t index = 0; index < document_.PageCount(); ++index) {
        if (pageIndex.has_value() && index != *pageIndex) {
            continue;
        }

        const bool hasSelectedStrokes = index < selectedStrokeIndices_.size() && !selectedStrokeIndices_[index].empty();
        const bool hasSelectedTexts = index < selectedTextIndices_.size() && !selectedTextIndices_[index].empty();
        const bool hasSelectedPdfText = index < selectedPdfTextBoxes_.size() && !selectedPdfTextBoxes_[index].empty();
        const bool hasSelectedLatexes = index < selectedLatexIndices_.size() && !selectedLatexIndices_[index].empty();
        const bool hasSelectedImages = index < selectedImageIndices_.size() && !selectedImageIndices_[index].empty();
        if (!hasSelectedImages && !hasSelectedTexts && !hasSelectedPdfText && !hasSelectedLatexes && !hasSelectedStrokes) {
            continue;
        }

        hadSelection = true;
        hadEditableSelection = hadEditableSelection || hasSelectedImages || hasSelectedTexts || hasSelectedLatexes || hasSelectedStrokes;

        if (hasSelectedTexts) {
            auto textIndices = selectedTextIndices_[index];
            std::sort(textIndices.rbegin(), textIndices.rend());
            auto& texts = document_.PageAt(index).texts;
            for (std::size_t textIndex : textIndices) {
                if (textIndex < texts.size()) {
                    texts.erase(texts.begin() + static_cast<std::ptrdiff_t>(textIndex));
                }
            }
            InvalidatePageSurfaceCache(index);
        }
        if (hasSelectedLatexes) {
            auto latexIndices = selectedLatexIndices_[index];
            std::sort(latexIndices.rbegin(), latexIndices.rend());
            auto& latexes = document_.PageAt(index).latexes;
            for (std::size_t latexIndex : latexIndices) {
                if (latexIndex < latexes.size()) {
                    latexes.erase(latexes.begin() + static_cast<std::ptrdiff_t>(latexIndex));
                }
            }
            InvalidatePageSurfaceCache(index);
        }

        if (hasSelectedImages) {
            auto imageIndices = selectedImageIndices_[index];
            std::sort(imageIndices.rbegin(), imageIndices.rend());
            auto& images = document_.PageAt(index).images;
            for (std::size_t imageIndex : imageIndices) {
                if (imageIndex < images.size()) {
                    images.erase(images.begin() + static_cast<std::ptrdiff_t>(imageIndex));
                }
            }
            InvalidatePageSurfaceCache(index);
        }

        if (hasSelectedStrokes) {
            auto strokeIndices = selectedStrokeIndices_[index];
            std::sort(strokeIndices.rbegin(), strokeIndices.rend());
            auto& strokes = document_.PageAt(index).strokes;
            for (std::size_t strokeIndex : strokeIndices) {
                if (strokeIndex < strokes.size()) {
                    strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(strokeIndex));
                }
            }
            InvalidatePageSurfaceCache(index);
        }
    }

    if (!hadSelection) {
        return;
    }

    ClearSelection();
    if (!hadEditableSelection) {
        InvalidateCanvas();
        return;
    }
    isMovingSelection_ = false;
    isTransformingImage_ = false;
    isTransformingStroke_ = false;
    transformingImagePageIndex_.reset();
    transformingImageIndex_.reset();
    transformingStrokePageIndex_.reset();
    imageTransformHandle_ = ImageTransformHandle::None;
    strokeTransformHandle_ = ImageTransformHandle::None;
    strokeTransformInitials_.clear();
    UpdateCaptureState();
    NotifyDocumentMutated();
    InvalidateCanvas();
}

void App::AdjustSelectionForRemoval(std::size_t pageIndex, std::size_t strokeIndex) {
    if (pageIndex >= selectedStrokeIndices_.size()) {
        return;
    }

    auto& selected = selectedStrokeIndices_[pageIndex];
    selected.erase(
        std::remove_if(
            selected.begin(),
            selected.end(),
            [&](std::size_t index) {
                if (index == strokeIndex) {
                    return true;
                }
                if (index > strokeIndex) {
                    --index;
                }
                return false;
            }),
        selected.end());

    for (std::size_t& index : selected) {
        if (index > strokeIndex) {
            --index;
        }
    }
}

void App::AdjustSelectionForInsertion(std::size_t pageIndex, std::size_t strokeIndex) {
    if (pageIndex >= selectedStrokeIndices_.size()) {
        return;
    }

    for (std::size_t& index : selectedStrokeIndices_[pageIndex]) {
        if (index >= strokeIndex) {
            ++index;
        }
    }
}

bool App::IsStrokeSelected(std::size_t pageIndex, std::size_t strokeIndex) const {
    if (pageIndex >= selectedStrokeIndices_.size()) {
        return false;
    }

    const auto& selected = selectedStrokeIndices_[pageIndex];
    return std::find(selected.begin(), selected.end(), strokeIndex) != selected.end();
}

bool App::IsImageSelected(std::size_t pageIndex, std::size_t imageIndex) const {
    if (pageIndex >= selectedImageIndices_.size()) {
        return false;
    }

    const auto& selected = selectedImageIndices_[pageIndex];
    return std::find(selected.begin(), selected.end(), imageIndex) != selected.end();
}

bool App::IsTextSelected(std::size_t pageIndex, std::size_t textIndex) const {
    if (pageIndex >= selectedTextIndices_.size()) {
        return false;
    }

    const auto& selected = selectedTextIndices_[pageIndex];
    return std::find(selected.begin(), selected.end(), textIndex) != selected.end();
}

bool App::IsLatexSelected(std::size_t pageIndex, std::size_t latexIndex) const {
    if (pageIndex >= selectedLatexIndices_.size()) {
        return false;
    }

    const auto& selected = selectedLatexIndices_[pageIndex];
    return std::find(selected.begin(), selected.end(), latexIndex) != selected.end();
}

bool App::IsEditingText(std::size_t pageIndex, std::size_t textIndex) const {
    return textEditWindow_ != nullptr
        && editingTextPageIndex_.has_value()
        && editingTextIndex_.has_value()
        && *editingTextPageIndex_ == pageIndex
        && *editingTextIndex_ == textIndex;
}

std::optional<std::pair<std::size_t, std::size_t>> App::SingleSelectedImage() const {
    std::optional<std::pair<std::size_t, std::size_t>> result;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (HasSelectedPdfText()
            || (pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty())
            || (pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty())
            || (pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[pageIndex].empty())) {
            return std::nullopt;
        }
        if (pageIndex >= selectedImageIndices_.size()) {
            continue;
        }
        for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
            if (result.has_value()) {
                return std::nullopt;
            }
            result = std::make_pair(pageIndex, imageIndex);
        }
    }
    return result;
}

std::optional<std::pair<std::size_t, std::size_t>> App::SingleSelectedText() const {
    std::optional<std::pair<std::size_t, std::size_t>> result;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (HasSelectedPdfText()
            || (pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty())
            || (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty())
            || (pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[pageIndex].empty())) {
            return std::nullopt;
        }
        if (pageIndex >= selectedTextIndices_.size()) {
            continue;
        }
        for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
            if (result.has_value()) {
                return std::nullopt;
            }
            result = std::make_pair(pageIndex, textIndex);
        }
    }
    return result;
}

std::optional<std::pair<std::size_t, std::size_t>> App::SingleSelectedLatex() const {
    std::optional<std::pair<std::size_t, std::size_t>> result;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (HasSelectedPdfText()
            || (pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty())
            || (pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty())
            || (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty())) {
            return std::nullopt;
        }
        if (pageIndex >= selectedLatexIndices_.size()) {
            continue;
        }
        for (std::size_t latexIndex : selectedLatexIndices_[pageIndex]) {
            if (result.has_value()) {
                return std::nullopt;
            }
            result = std::make_pair(pageIndex, latexIndex);
        }
    }
    return result;
}

std::optional<std::size_t> App::SingleSelectedStrokePage() const {
    std::optional<std::size_t> result;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (HasSelectedPdfText()
            || (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty())
            || (pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty())
            || (pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[pageIndex].empty())) {
            return std::nullopt;
        }
        if (pageIndex >= selectedStrokeIndices_.size() || selectedStrokeIndices_[pageIndex].empty()) {
            continue;
        }
        if (result.has_value()) {
            return std::nullopt;
        }
        result = pageIndex;
    }
    return result;
}

std::optional<App::ImageTransformHandle> App::HitTestSelectedImageHandle(POINT cursor, std::size_t* pageIndex, std::size_t* imageIndex) const {
    const auto singleImage = SingleSelectedImage();
    if (!singleImage.has_value() || window_ == nullptr) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, singleImage->first);
    if (!pageView.valid) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(singleImage->first);
    if (singleImage->second >= page.images.size()) {
        return std::nullopt;
    }

    const PageImage& image = page.images[singleImage->second];
    const UINT dpi = WindowDpi(window_);
    const auto corners = ImageCorners(image);
    const Vec2 topMid {
        (corners[0].x + corners[1].x) * 0.5f,
        (corners[0].y + corners[1].y) * 0.5f,
    };
    const Vec2 bottomMid {
        (corners[2].x + corners[3].x) * 0.5f,
        (corners[2].y + corners[3].y) * 0.5f,
    };
    const Vec2 leftMid {
        (corners[0].x + corners[3].x) * 0.5f,
        (corners[0].y + corners[3].y) * 0.5f,
    };
    const Vec2 rightMid {
        (corners[1].x + corners[2].x) * 0.5f,
        (corners[1].y + corners[2].y) * 0.5f,
    };
    const Vec2 center = ImageCenter(image);
    const Vec2 rotateHandle = RotatePointAround({ center.x, image.y - SelectionRotateOffsetPageUnits(pageView.scale, dpi) }, center, image.rotationDegrees);

    const struct {
        ImageTransformHandle handle;
        Vec2 point;
    } handlePoints[] {
        { ImageTransformHandle::ResizeNorthWest, ScreenPointFromPagePoint(corners[0], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorth, ScreenPointFromPagePoint(topMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorthEast, ScreenPointFromPagePoint(corners[1], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeEast, ScreenPointFromPagePoint(rightMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthEast, ScreenPointFromPagePoint(corners[2], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouth, ScreenPointFromPagePoint(bottomMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthWest, ScreenPointFromPagePoint(corners[3], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeWest, ScreenPointFromPagePoint(leftMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::Rotate, ScreenPointFromPagePoint(rotateHandle, pageView.rect, pageView.scale) },
    };
    const float handleRadiusPixels = SelectionHandleHitRadiusPixels(dpi);
    const Vec2 cursorPoint { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
    for (const auto& handlePoint : handlePoints) {
        if (DistanceSquared(handlePoint.point, cursorPoint) <= (handleRadiusPixels * handleRadiusPixels)) {
            if (pageIndex != nullptr) {
                *pageIndex = singleImage->first;
            }
            if (imageIndex != nullptr) {
                *imageIndex = singleImage->second;
            }
            return handlePoint.handle;
        }
    }

    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>> App::HitTestText(POINT cursor) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value() || hitPage->pageIndex >= document_.PageCount()) {
        return std::nullopt;
    }

    const Vec2 pagePoint = ScreenToPage(*hitPage, cursor, true);
    const Page& page = document_.PageAt(hitPage->pageIndex);
    for (std::size_t textIndex = page.texts.size(); textIndex > 0; --textIndex) {
        const std::size_t actualIndex = textIndex - 1U;
        if (textgeom::TextHitsPoint(page.texts[actualIndex], page.width, pagePoint)) {
            return std::make_pair(hitPage->pageIndex, actualIndex);
        }
    }

    return std::nullopt;
}
std::optional<std::pair<std::size_t, std::size_t>> App::HitTestLatex(POINT cursor) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value() || hitPage->pageIndex >= document_.PageCount()) {
        return std::nullopt;
    }

    const Vec2 pagePoint = ScreenToPage(*hitPage, cursor, true);
    const Page& page = document_.PageAt(hitPage->pageIndex);
    for (std::size_t latexIndex = page.latexes.size(); latexIndex > 0; --latexIndex) {
        const std::size_t actualIndex = latexIndex - 1U;
        if (LatexHitsPoint(page.latexes[actualIndex], page.width, pagePoint)) {
            return std::make_pair(hitPage->pageIndex, actualIndex);
        }
    }

    return std::nullopt;
}

std::optional<App::ImageTransformHandle> App::HitTestSelectedTextHandle(POINT cursor, std::size_t* pageIndex, std::size_t* textIndex) const {
    const auto singleText = SingleSelectedText();
    if (!singleText.has_value() || window_ == nullptr) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, singleText->first);
    if (!pageView.valid) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(singleText->first);
    if (singleText->second >= page.texts.size()) {
        return std::nullopt;
    }

    const auto boundsRect = textgeom::PageTextBoundsRect(page.texts[singleText->second], page.width, pageView.scale, 0);
    if (!boundsRect.has_value()) {
        return std::nullopt;
    }
    RECT screenBounds = *boundsRect;
    OffsetRect(&screenBounds, pageView.rect.left, pageView.rect.top);
    const Vec2 center {
        static_cast<float>((screenBounds.left + screenBounds.right) / 2),
        static_cast<float>((screenBounds.top + screenBounds.bottom) / 2),
    };
    const struct {
        ImageTransformHandle handle;
        Vec2 point;
    } handlePoints[] {
        { ImageTransformHandle::ResizeNorthWest, { static_cast<float>(screenBounds.left), static_cast<float>(screenBounds.top) } },
        { ImageTransformHandle::ResizeNorth, { center.x, static_cast<float>(screenBounds.top) } },
        { ImageTransformHandle::ResizeNorthEast, { static_cast<float>(screenBounds.right), static_cast<float>(screenBounds.top) } },
        { ImageTransformHandle::ResizeEast, { static_cast<float>(screenBounds.right), center.y } },
        { ImageTransformHandle::ResizeSouthEast, { static_cast<float>(screenBounds.right), static_cast<float>(screenBounds.bottom) } },
        { ImageTransformHandle::ResizeSouth, { center.x, static_cast<float>(screenBounds.bottom) } },
        { ImageTransformHandle::ResizeSouthWest, { static_cast<float>(screenBounds.left), static_cast<float>(screenBounds.bottom) } },
        { ImageTransformHandle::ResizeWest, { static_cast<float>(screenBounds.left), center.y } },
    };
    const float handleRadiusPixels = SelectionHandleHitRadiusPixels(WindowDpi(window_));
    const Vec2 cursorPoint { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
    for (const auto& handlePoint : handlePoints) {
        if (DistanceSquared(handlePoint.point, cursorPoint) <= (handleRadiusPixels * handleRadiusPixels)) {
            if (pageIndex != nullptr) {
                *pageIndex = singleText->first;
            }
            if (textIndex != nullptr) {
                *textIndex = singleText->second;
            }
            return handlePoint.handle;
        }
    }

    return std::nullopt;
}
std::optional<App::ImageTransformHandle> App::HitTestSelectedLatexHandle(POINT cursor, std::size_t* pageIndex, std::size_t* latexIndex) const {
    const auto singleLatex = SingleSelectedLatex();
    if (!singleLatex.has_value() || window_ == nullptr) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, singleLatex->first);
    if (!pageView.valid) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(singleLatex->first);
    if (singleLatex->second >= page.latexes.size()) {
        return std::nullopt;
    }

    const auto boundsRect = PageLatexBoundsRect(page.latexes[singleLatex->second], page.width, pageView.scale, 0);
    if (!boundsRect.has_value()) {
        return std::nullopt;
    }
    RECT screenBounds = *boundsRect;
    OffsetRect(&screenBounds, pageView.rect.left, pageView.rect.top);
    const Vec2 center {
        static_cast<float>((screenBounds.left + screenBounds.right) / 2),
        static_cast<float>((screenBounds.top + screenBounds.bottom) / 2),
    };
    const struct {
        ImageTransformHandle handle;
        Vec2 point;
    } handlePoints[] {
        { ImageTransformHandle::ResizeNorthWest, { static_cast<float>(screenBounds.left), static_cast<float>(screenBounds.top) } },
        { ImageTransformHandle::ResizeNorth, { center.x, static_cast<float>(screenBounds.top) } },
        { ImageTransformHandle::ResizeNorthEast, { static_cast<float>(screenBounds.right), static_cast<float>(screenBounds.top) } },
        { ImageTransformHandle::ResizeEast, { static_cast<float>(screenBounds.right), center.y } },
        { ImageTransformHandle::ResizeSouthEast, { static_cast<float>(screenBounds.right), static_cast<float>(screenBounds.bottom) } },
        { ImageTransformHandle::ResizeSouth, { center.x, static_cast<float>(screenBounds.bottom) } },
        { ImageTransformHandle::ResizeSouthWest, { static_cast<float>(screenBounds.left), static_cast<float>(screenBounds.bottom) } },
        { ImageTransformHandle::ResizeWest, { static_cast<float>(screenBounds.left), center.y } },
    };
    const float handleRadiusPixels = SelectionHandleHitRadiusPixels(WindowDpi(window_));
    const Vec2 cursorPoint { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
    for (const auto& handlePoint : handlePoints) {
        if (DistanceSquared(handlePoint.point, cursorPoint) <= (handleRadiusPixels * handleRadiusPixels)) {
            if (pageIndex != nullptr) {
                *pageIndex = singleLatex->first;
            }
            if (latexIndex != nullptr) {
                *latexIndex = singleLatex->second;
            }
            return handlePoint.handle;
        }
    }

    return std::nullopt;
}

std::optional<App::ImageTransformHandle> App::HitTestSelectedStrokeHandle(POINT cursor, std::size_t* pageIndex) const {
    const auto strokePage = SingleSelectedStrokePage();
    if (!strokePage.has_value() || window_ == nullptr || *strokePage >= selectedStrokeIndices_.size() || selectedStrokeIndices_[*strokePage].empty()) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *strokePage);
    if (!pageView.valid) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(*strokePage);
    const UINT dpi = WindowDpi(window_);
    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    for (std::size_t strokeIndex : selectedStrokeIndices_[*strokePage]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        for (const Vec2& point : page.strokes[strokeIndex].points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
    }

    const Vec2 center { (minX + maxX) * 0.5f, (minY + maxY) * 0.5f };
    const struct {
        ImageTransformHandle handle;
        Vec2 point;
    } handlePoints[] {
        { ImageTransformHandle::ResizeNorthWest, ScreenPointFromPagePoint({ minX, minY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorth, ScreenPointFromPagePoint({ center.x, minY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorthEast, ScreenPointFromPagePoint({ maxX, minY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeEast, ScreenPointFromPagePoint({ maxX, center.y }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthEast, ScreenPointFromPagePoint({ maxX, maxY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouth, ScreenPointFromPagePoint({ center.x, maxY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthWest, ScreenPointFromPagePoint({ minX, maxY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeWest, ScreenPointFromPagePoint({ minX, center.y }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::Rotate, ScreenPointFromPagePoint({ center.x, minY - SelectionRotateOffsetPageUnits(pageView.scale, dpi) }, pageView.rect, pageView.scale) },
    };
    const float handleRadiusPixels = SelectionHandleHitRadiusPixels(dpi);
    const Vec2 cursorPoint { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
    for (const auto& handlePoint : handlePoints) {
        if (DistanceSquared(handlePoint.point, cursorPoint) <= (handleRadiusPixels * handleRadiusPixels)) {
            if (pageIndex != nullptr) {
                *pageIndex = *strokePage;
            }
            return handlePoint.handle;
        }
    }

    return std::nullopt;
}

std::vector<Vec2> App::CurrentSelectionLassoPolygon() const {
    std::vector<Vec2> polygon;
    polygon.reserve(selectionLassoCursorPoints_.size() + 1U);
    for (POINT point : selectionLassoCursorPoints_) {
        polygon.push_back(PointToVec2(point));
    }
    if (polygon.empty()) {
        return polygon;
    }

    if (DistanceSquared(polygon.back(), PointToVec2(selectionCurrentCursor_)) > 0.0001f) {
        polygon.push_back(PointToVec2(selectionCurrentCursor_));
    }
    return polygon;
}

bool App::CursorHitsSelectedContent(POINT cursor) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        return false;
    }

    const std::size_t pageIndex = hitPage->pageIndex;
    if (pageIndex >= selectedStrokeIndices_.size()
        && pageIndex >= selectedTextIndices_.size()
        && pageIndex >= selectedLatexIndices_.size()
        && pageIndex >= selectedImageIndices_.size()) {
        return false;
    }

    const Page& page = document_.PageAt(pageIndex);
    const float pageX = (static_cast<float>(cursor.x - hitPage->rect.left)) / hitPage->scale;
    const float pageY = (static_cast<float>(cursor.y - hitPage->rect.top)) / hitPage->scale;
    const Vec2 pagePoint {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
    const float hitRadius = std::max(8.0f, brushWidth_ * 2.5f) / std::max(0.1f, hitPage->scale);

    if (pageIndex < selectedStrokeIndices_.size()) {
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
            if (strokeIndex < page.strokes.size() && StrokeHitsPoint(page.strokes[strokeIndex], pagePoint, hitRadius, settings_.strokeSmoothing)) {
                return true;
            }
        }
    }

    if (pageIndex < selectedTextIndices_.size()) {
        for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
            if (textIndex < page.texts.size() && textgeom::TextHitsPoint(page.texts[textIndex], page.width, pagePoint)) {
                return true;
            }
        }
    }
    if (pageIndex < selectedLatexIndices_.size()) {
        for (std::size_t latexIndex : selectedLatexIndices_[pageIndex]) {
            if (latexIndex < page.latexes.size() && LatexHitsPoint(page.latexes[latexIndex], page.width, pagePoint)) {
                return true;
            }
        }
    }

    if (pageIndex < selectedImageIndices_.size()) {
        for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
            if (imageIndex < page.images.size() && ImageHitsPoint(page.images[imageIndex], pagePoint)) {
                return true;
            }
        }
    }

    return false;
}

bool App::CursorHitsPdfText(POINT cursor) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value() || hitPage->pageIndex >= document_.PageCount()) {
        return false;
    }
    const Page& page = document_.PageAt(hitPage->pageIndex);
    if (page.sourcePdfPageIndex < 0) {
        return false;
    }
    const Document::PdfSource* pdfSource = document_.PdfSourceAtIndex(page.sourcePdfSourceIndex);
    if (pdfSource == nullptr) {
        return false;
    }
    const std::optional<std::vector<PdfTextCharBox>> boxes = pdfBackend_.ExtractTextCharBoxes(
        pdfSource->pdfData,
        static_cast<std::size_t>(page.sourcePdfPageIndex));
    if (!boxes.has_value()) {
        return false;
    }
    const Vec2 pagePoint = ScreenToPage(*hitPage, cursor, false);
    for (const PdfTextCharBox& box : *boxes) {
        if (pagePoint.x >= box.left
            && pagePoint.x <= box.right
            && pagePoint.y >= box.top
            && pagePoint.y <= box.bottom) {
            return true;
        }
    }
    return false;
}

bool App::CursorHitsSelectionMoveGrip(POINT cursor) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        return false;
    }

    const std::size_t pageIndex = hitPage->pageIndex;
    if (pageIndex >= document_.PageCount()) {
        return false;
    }

    const bool hasSelectedStrokes = pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty();
    const bool hasSelectedTexts = pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty();
    const bool hasSelectedLatexes = pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[pageIndex].empty();
    const bool hasSelectedImages = pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty();
    if (!hasSelectedStrokes && !hasSelectedTexts && !hasSelectedLatexes && !hasSelectedImages) {
        return false;
    }

    const Page& page = document_.PageAt(pageIndex);
    const UINT dpi = window_ != nullptr ? WindowDpi(window_) : SystemDpi();
    const int gripPadding = std::max(2, ScaleForDpi(2, dpi));

    if (hasSelectedImages) {
        for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
            if (imageIndex >= page.images.size()) {
                continue;
            }

            std::vector<Vec2> screenCorners;
            screenCorners.reserve(4U);
            for (const Vec2& corner : ImageCorners(page.images[imageIndex])) {
                screenCorners.push_back(ScreenPointFromPagePoint(corner, hitPage->rect, hitPage->scale));
            }
            if (PolygonContainsPoint(screenCorners, cursor)) {
                return true;
            }
        }
    }

    if (hasSelectedTexts) {
        for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
            if (textIndex >= page.texts.size()) {
                continue;
            }

            const auto textRect = textgeom::PageTextBoundsRect(page.texts[textIndex], page.width, hitPage->scale, 4);
            if (!textRect.has_value()) {
                continue;
            }

            RECT screenRect = *textRect;
            OffsetRect(&screenRect, hitPage->rect.left, hitPage->rect.top);
            if (RectContainsPointWithPadding(screenRect, cursor, gripPadding)) {
                return true;
            }
        }
    }
    if (hasSelectedLatexes) {
        for (std::size_t latexIndex : selectedLatexIndices_[pageIndex]) {
            if (latexIndex >= page.latexes.size()) {
                continue;
            }

            const auto latexRect = PageLatexBoundsRect(page.latexes[latexIndex], page.width, hitPage->scale, 4);
            if (!latexRect.has_value()) {
                continue;
            }

            RECT screenRect = *latexRect;
            OffsetRect(&screenRect, hitPage->rect.left, hitPage->rect.top);
            if (RectContainsPointWithPadding(screenRect, cursor, gripPadding)) {
                return true;
            }
        }
    }

    if (hasSelectedStrokes) {
        float minX = page.width;
        float minY = page.height;
        float maxX = 0.0f;
        float maxY = 0.0f;
        bool hasStrokeBounds = false;
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
            if (strokeIndex >= page.strokes.size()) {
                continue;
            }
            for (const Vec2& point : page.strokes[strokeIndex].points) {
                minX = std::min(minX, point.x);
                minY = std::min(minY, point.y);
                maxX = std::max(maxX, point.x);
                maxY = std::max(maxY, point.y);
                hasStrokeBounds = true;
            }
        }

        if (hasStrokeBounds) {
            const RECT screenRect {
                hitPage->rect.left + static_cast<int>(std::lround(minX * hitPage->scale)),
                hitPage->rect.top + static_cast<int>(std::lround(minY * hitPage->scale)),
                hitPage->rect.left + static_cast<int>(std::lround(maxX * hitPage->scale)),
                hitPage->rect.top + static_cast<int>(std::lround(maxY * hitPage->scale)),
            };
            if (RectContainsPointWithPadding(screenRect, cursor, gripPadding)) {
                return true;
            }
        }
    }

    return false;
}

bool App::CursorInteractsWithSelection(POINT cursor) const {
    return HitTestSelectedStrokeHandle(cursor).has_value()
        || HitTestSelectedTextHandle(cursor).has_value()
        || HitTestSelectedLatexHandle(cursor).has_value()
        || HitTestSelectedImageHandle(cursor).has_value()
        || CursorHitsSelectionMoveGrip(cursor)
        || CursorHitsSelectedContent(cursor);
}

void App::TranslateSelectedContent(std::size_t pageIndex, Vec2 delta, bool clampToPage, bool notify) {
    if (pageIndex >= document_.PageCount()) {
        return;
    }

    Page& page = document_.PageAt(pageIndex);
    if (pageIndex >= selectedStrokeIndices_.size()
        || pageIndex >= selectedTextIndices_.size()
        || pageIndex >= selectedLatexIndices_.size()
        || pageIndex >= selectedImageIndices_.size()) {
        return;
    }

    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    bool hasSelection = false;

    for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        for (const Vec2& point : page.strokes[strokeIndex].points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
            hasSelection = true;
        }
    }

    for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
        if (textIndex >= page.texts.size()) {
            continue;
        }
        const textgeom::PageTextBounds bounds = textgeom::MeasurePageTextBounds(page.texts[textIndex], page.width);
        minX = std::min(minX, bounds.left);
        minY = std::min(minY, bounds.top);
        maxX = std::max(maxX, bounds.right);
        maxY = std::max(maxY, bounds.bottom);
        hasSelection = true;
    }
    for (std::size_t latexIndex : selectedLatexIndices_[pageIndex]) {
        if (latexIndex >= page.latexes.size()) {
            continue;
        }
        const PageLatexBounds bounds = MeasurePageLatexBounds(page.latexes[latexIndex], page.width);
        minX = std::min(minX, bounds.left);
        minY = std::min(minY, bounds.top);
        maxX = std::max(maxX, bounds.right);
        maxY = std::max(maxY, bounds.bottom);
        hasSelection = true;
    }

    for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
        if (imageIndex >= page.images.size()) {
            continue;
        }
        const PageImage& image = page.images[imageIndex];
        const auto corners = ImageCorners(image);
        for (const Vec2& corner : corners) {
            minX = std::min(minX, corner.x);
            minY = std::min(minY, corner.y);
            maxX = std::max(maxX, corner.x);
            maxY = std::max(maxY, corner.y);
        }
        hasSelection = true;
    }

    if (!hasSelection) {
        return;
    }

    if (clampToPage) {
        delta.x = std::clamp(delta.x, -minX, page.width - maxX);
        delta.y = std::clamp(delta.y, -minY, page.height - maxY);
    }
    if (std::abs(delta.x) < 0.001f && std::abs(delta.y) < 0.001f) {
        return;
    }

    for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        for (Vec2& point : page.strokes[strokeIndex].points) {
            point.x += delta.x;
            point.y += delta.y;
        }
    }

    for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
        if (textIndex >= page.texts.size()) {
            continue;
        }
        page.texts[textIndex].x += delta.x;
        page.texts[textIndex].y += delta.y;
    }
    for (std::size_t latexIndex : selectedLatexIndices_[pageIndex]) {
        if (latexIndex >= page.latexes.size()) {
            continue;
        }
        page.latexes[latexIndex].x += delta.x;
        page.latexes[latexIndex].y += delta.y;
    }

    for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
        if (imageIndex >= page.images.size()) {
            continue;
        }
        page.images[imageIndex].x += delta.x;
        page.images[imageIndex].y += delta.y;
    }

    InvalidatePageSurfaceCache(pageIndex);
    if (notify) {
        NotifyDocumentMutated();
        InvalidateCanvas();
    }
}

bool App::RehomeSelectedContentAcrossPages(bool clampToPage) {
    if (window_ == nullptr || document_.PageCount() == 0U) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
    if (pageViews.size() != document_.PageCount()) {
        return false;
    }

    const auto targetPageForScreenPoint = [&](POINT screenPoint, std::size_t fallbackPageIndex) {
        std::size_t bestPageIndex = std::min(fallbackPageIndex, pageViews.size() - 1U);
        long bestDistanceSquared = std::numeric_limits<long>::max();
        for (const PageView& pageView : pageViews) {
            if (!pageView.valid) {
                continue;
            }
            if (PtInRect(&pageView.rect, screenPoint) != 0) {
                return pageView.pageIndex;
            }

            const long dx = static_cast<long>(std::max<LONG>(pageView.rect.left - screenPoint.x, 0))
                + static_cast<long>(std::max<LONG>(screenPoint.x - pageView.rect.right, 0));
            const long dy = static_cast<long>(std::max<LONG>(pageView.rect.top - screenPoint.y, 0))
                + static_cast<long>(std::max<LONG>(screenPoint.y - pageView.rect.bottom, 0));
            const long distanceSquared = (dx * dx) + (dy * dy);
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestPageIndex = pageView.pageIndex;
            }
        }
        return bestPageIndex;
    };

    bool changed = false;
    std::vector<bool> invalidatedPages(document_.PageCount(), false);
    std::vector<std::vector<Stroke>> movedStrokes(document_.PageCount());
    std::vector<std::vector<PageImage>> movedImages(document_.PageCount());
    std::vector<std::vector<PageLatex>> movedLatexes(document_.PageCount());

    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        Page& page = document_.PageAt(pageIndex);
        const PageView& sourceView = pageViews[pageIndex];

        if (pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty()) {
            std::vector<bool> selectedFlags(page.strokes.size(), false);
            for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
                if (strokeIndex < selectedFlags.size()) {
                    selectedFlags[strokeIndex] = true;
                }
            }

            std::vector<Stroke> retainedStrokes;
            retainedStrokes.reserve(page.strokes.size());
            std::vector<std::size_t> retainedSelected;
            for (std::size_t strokeIndex = 0; strokeIndex < page.strokes.size(); ++strokeIndex) {
                Stroke stroke = std::move(page.strokes[strokeIndex]);
                if (!selectedFlags[strokeIndex]) {
                    retainedStrokes.push_back(std::move(stroke));
                    continue;
                }

                float minX = 0.0f;
                float minY = 0.0f;
                float maxX = 0.0f;
                float maxY = 0.0f;
                if (!StrokeBounds(stroke, &minX, &minY, &maxX, &maxY)) {
                    retainedStrokes.push_back(std::move(stroke));
                    retainedSelected.push_back(retainedStrokes.size() - 1U);
                    continue;
                }

                const POINT centerPoint {
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.left) + ((minX + maxX) * 0.5f * sourceView.scale))),
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.top) + ((minY + maxY) * 0.5f * sourceView.scale))),
                };
                const std::size_t targetPageIndex = targetPageForScreenPoint(centerPoint, pageIndex);
                if (targetPageIndex != pageIndex) {
                    const PageView& targetView = pageViews[targetPageIndex];
                    for (Vec2& point : stroke.points) {
                        point = RemapPagePointBetweenViews(sourceView, targetView, point);
                    }
                    if (clampToPage) {
                        TranslateStrokePoints(stroke, ClampStrokeDeltaToPage(stroke, document_.PageAt(targetPageIndex), {}));
                    }
                    movedStrokes[targetPageIndex].push_back(std::move(stroke));
                    invalidatedPages[pageIndex] = true;
                    invalidatedPages[targetPageIndex] = true;
                    changed = true;
                    continue;
                }

                if (clampToPage) {
                    const Vec2 clampDelta = ClampStrokeDeltaToPage(stroke, page, {});
                    if (std::abs(clampDelta.x) >= 0.001f || std::abs(clampDelta.y) >= 0.001f) {
                        TranslateStrokePoints(stroke, clampDelta);
                        invalidatedPages[pageIndex] = true;
                        changed = true;
                    }
                }

                retainedStrokes.push_back(std::move(stroke));
                retainedSelected.push_back(retainedStrokes.size() - 1U);
            }

            page.strokes = std::move(retainedStrokes);
            selectedStrokeIndices_[pageIndex] = std::move(retainedSelected);
        }

        if (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty()) {
            std::vector<bool> selectedFlags(page.images.size(), false);
            for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
                if (imageIndex < selectedFlags.size()) {
                    selectedFlags[imageIndex] = true;
                }
            }

            std::vector<PageImage> retainedImages;
            retainedImages.reserve(page.images.size());
            std::vector<std::size_t> retainedSelected;
            for (std::size_t imageIndex = 0; imageIndex < page.images.size(); ++imageIndex) {
                PageImage image = std::move(page.images[imageIndex]);
                if (!selectedFlags[imageIndex]) {
                    retainedImages.push_back(std::move(image));
                    continue;
                }

                const Vec2 center = ImageCenter(image);
                const POINT centerPoint {
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.left) + (center.x * sourceView.scale))),
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.top) + (center.y * sourceView.scale))),
                };
                const std::size_t targetPageIndex = targetPageForScreenPoint(centerPoint, pageIndex);
                if (targetPageIndex != pageIndex) {
                    const PageView& targetView = pageViews[targetPageIndex];
                    const Vec2 remappedTopLeft = RemapPagePointBetweenViews(sourceView, targetView, { image.x, image.y });
                    image.x = remappedTopLeft.x;
                    image.y = remappedTopLeft.y;
                    if (clampToPage) {
                        const Vec2 clampDelta = ClampImageDeltaToPage(image, document_.PageAt(targetPageIndex), {});
                        image.x += clampDelta.x;
                        image.y += clampDelta.y;
                    }
                    movedImages[targetPageIndex].push_back(std::move(image));
                    invalidatedPages[pageIndex] = true;
                    invalidatedPages[targetPageIndex] = true;
                    changed = true;
                    continue;
                }

                if (clampToPage) {
                    const Vec2 clampDelta = ClampImageDeltaToPage(image, page, {});
                    if (std::abs(clampDelta.x) >= 0.001f || std::abs(clampDelta.y) >= 0.001f) {
                        image.x += clampDelta.x;
                        image.y += clampDelta.y;
                        invalidatedPages[pageIndex] = true;
                        changed = true;
                    }
                }

                retainedImages.push_back(std::move(image));
                retainedSelected.push_back(retainedImages.size() - 1U);
            }

            page.images = std::move(retainedImages);
            selectedImageIndices_[pageIndex] = std::move(retainedSelected);
        }
        if (pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[pageIndex].empty()) {
            std::vector<bool> selectedFlags(page.latexes.size(), false);
            for (std::size_t latexIndex : selectedLatexIndices_[pageIndex]) {
                if (latexIndex < selectedFlags.size()) {
                    selectedFlags[latexIndex] = true;
                }
            }

            std::vector<PageLatex> retainedLatexes;
            retainedLatexes.reserve(page.latexes.size());
            std::vector<std::size_t> retainedSelected;
            for (std::size_t latexIndex = 0; latexIndex < page.latexes.size(); ++latexIndex) {
                PageLatex latex = std::move(page.latexes[latexIndex]);
                if (!selectedFlags[latexIndex]) {
                    retainedLatexes.push_back(std::move(latex));
                    continue;
                }

                const PageLatexBounds bounds = MeasurePageLatexBounds(latex, page.width);
                const Vec2 center { (bounds.left + bounds.right) * 0.5f, (bounds.top + bounds.bottom) * 0.5f };
                const POINT centerPoint {
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.left) + (center.x * sourceView.scale))),
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.top) + (center.y * sourceView.scale))),
                };
                const std::size_t targetPageIndex = targetPageForScreenPoint(centerPoint, pageIndex);
                if (targetPageIndex != pageIndex) {
                    const PageView& targetView = pageViews[targetPageIndex];
                    const Vec2 remappedTopLeft = RemapPagePointBetweenViews(sourceView, targetView, { latex.x, latex.y });
                    latex.x = remappedTopLeft.x;
                    latex.y = remappedTopLeft.y;
                    if (clampToPage) {
                        const Vec2 clampDelta = ClampLatexDeltaToPage(latex, document_.PageAt(targetPageIndex), {});
                        latex.x += clampDelta.x;
                        latex.y += clampDelta.y;
                    }
                    movedLatexes[targetPageIndex].push_back(std::move(latex));
                    invalidatedPages[pageIndex] = true;
                    invalidatedPages[targetPageIndex] = true;
                    changed = true;
                    continue;
                }

                if (clampToPage) {
                    const Vec2 clampDelta = ClampLatexDeltaToPage(latex, page, {});
                    if (std::abs(clampDelta.x) >= 0.001f || std::abs(clampDelta.y) >= 0.001f) {
                        latex.x += clampDelta.x;
                        latex.y += clampDelta.y;
                        invalidatedPages[pageIndex] = true;
                        changed = true;
                    }
                }

                retainedLatexes.push_back(std::move(latex));
                retainedSelected.push_back(retainedLatexes.size() - 1U);
            }

            page.latexes = std::move(retainedLatexes);
            selectedLatexIndices_[pageIndex] = std::move(retainedSelected);
        }
    }

    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (!movedStrokes[pageIndex].empty()) {
            auto& strokes = document_.PageAt(pageIndex).strokes;
            for (Stroke& stroke : movedStrokes[pageIndex]) {
                strokes.push_back(std::move(stroke));
                selectedStrokeIndices_[pageIndex].push_back(strokes.size() - 1U);
            }
            invalidatedPages[pageIndex] = true;
        }

        if (!movedImages[pageIndex].empty()) {
            auto& images = document_.PageAt(pageIndex).images;
            for (PageImage& image : movedImages[pageIndex]) {
                images.push_back(std::move(image));
                selectedImageIndices_[pageIndex].push_back(images.size() - 1U);
            }
            invalidatedPages[pageIndex] = true;
        }
        if (!movedLatexes[pageIndex].empty()) {
            auto& latexes = document_.PageAt(pageIndex).latexes;
            for (PageLatex& latex : movedLatexes[pageIndex]) {
                latexes.push_back(std::move(latex));
                selectedLatexIndices_[pageIndex].push_back(latexes.size() - 1U);
            }
            invalidatedPages[pageIndex] = true;
        }
    }

    for (std::size_t pageIndex = 0; pageIndex < invalidatedPages.size(); ++pageIndex) {
        if (invalidatedPages[pageIndex]) {
            InvalidatePageSurfaceCache(pageIndex);
        }
    }

    return changed;
}

void App::BeginSelection(POINT cursor, bool boxOverride) {
    if (isPanning_) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT canvasRect = CanvasRect(clientRect);
    if (PtInRect(&canvasRect, cursor) == 0) {
        ClearSelection();
        selectionBoxOverride_ = false;
        InvalidateCanvas();
        return;
    }

    selectionAnchorCursor_ = cursor;
    selectionCurrentCursor_ = cursor;
    selectionLassoCursorPoints_.clear();
    selectionLassoCursorPoints_.push_back(cursor);
    selectionBoxOverride_ = boxOverride;
    isSelecting_ = true;
    if (const auto hitPage = HitTestPage(cursor); hitPage.has_value()) {
        document_.SetCurrentPageIndex(hitPage->pageIndex);
        MarkPdfSidebarCompositionDirty();
    }
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::ExtendSelection(POINT cursor) {
    if (!isSelecting_) {
        return;
    }

    selectionCurrentCursor_ = cursor;

    if (selectionMode_ == SelectionMode::Lasso) {
        const Vec2 currentPoint = PointToVec2(selectionCurrentCursor_);
        if (selectionLassoCursorPoints_.empty()
            || DistanceSquared(currentPoint, PointToVec2(selectionLassoCursorPoints_.back())) >= 36.0f) {
            selectionLassoCursorPoints_.push_back(selectionCurrentCursor_);
        }
    }

    InvalidateCanvas();
}

void App::EndSelection() {
    if (!isSelecting_) {
        return;
    }

    const SelectionMode effectiveSelectionMode = selectionBoxOverride_ ? SelectionMode::Box : selectionMode_;
    const bool clickSelection = DistanceSquared(
        PointToVec2(selectionAnchorCursor_),
        PointToVec2(selectionCurrentCursor_)) <= (18.0f * 18.0f);
    const std::vector<Vec2> selectionPolygon = effectiveSelectionMode == SelectionMode::Lasso
        ? CurrentSelectionLassoPolygon()
        : std::vector<Vec2> {};

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    ClearSelection();
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid || pageView.pageIndex >= document_.PageCount()) {
            continue;
        }

        const Page& page = document_.PageAt(pageView.pageIndex);
        const Vec2 anchorPage = ScreenToPage(pageView, selectionAnchorCursor_, false);
        const Vec2 currentPage = ScreenToPage(pageView, selectionCurrentCursor_, false);
        std::vector<Vec2> pagePolygon;
        if (effectiveSelectionMode == SelectionMode::Lasso) {
            pagePolygon.reserve(selectionPolygon.size());
            for (const Vec2& point : selectionPolygon) {
                const POINT screenPoint {
                    static_cast<LONG>(std::lround(point.x)),
                    static_cast<LONG>(std::lround(point.y)),
                };
                pagePolygon.push_back(ScreenToPage(pageView, screenPoint, false));
            }
        }

        for (std::size_t strokeIndex = 0; strokeIndex < page.strokes.size(); ++strokeIndex) {
            const Stroke& stroke = page.strokes[strokeIndex];
            bool hit = false;
            if (effectiveSelectionMode == SelectionMode::Box) {
                hit = clickSelection
                    ? StrokeHitsPoint(stroke, currentPage, 10.0f / std::max(0.1f, pageView.scale), settings_.strokeSmoothing)
                    : StrokeIntersectsRectSelection(stroke, anchorPage, currentPage, settings_.strokeSmoothing);
            } else {
                hit = (pagePolygon.size() < 3U)
                    ? StrokeHitsPoint(stroke, currentPage, 10.0f / std::max(0.1f, pageView.scale), settings_.strokeSmoothing)
                    : StrokeIntersectsPolygonSelection(stroke, pagePolygon, settings_.strokeSmoothing);
            }

            if (hit && pageView.pageIndex < selectedStrokeIndices_.size()) {
                selectedStrokeIndices_[pageView.pageIndex].push_back(strokeIndex);
            }
        }

        for (std::size_t imageIndex = 0; imageIndex < page.images.size(); ++imageIndex) {
            const PageImage& image = page.images[imageIndex];
            bool hit = false;
            if (effectiveSelectionMode == SelectionMode::Box) {
                hit = clickSelection
                    ? ImageHitsPoint(image, currentPage)
                    : ImageIntersectsRectSelection(image, anchorPage, currentPage);
            } else {
                hit = (pagePolygon.size() < 3U)
                    ? ImageHitsPoint(image, currentPage)
                    : ImageIntersectsPolygonSelection(image, pagePolygon);
            }
            if (hit && pageView.pageIndex < selectedImageIndices_.size()) {
                selectedImageIndices_[pageView.pageIndex].push_back(imageIndex);
            }
        }

        for (std::size_t textIndex = 0; textIndex < page.texts.size(); ++textIndex) {
            const PageText& text = page.texts[textIndex];
            bool hit = false;
            if (effectiveSelectionMode == SelectionMode::Box) {
                hit = clickSelection
                    ? textgeom::TextHitsPoint(text, page.width, currentPage)
                    : textgeom::TextIntersectsRectSelection(text, page.width, anchorPage, currentPage);
            } else {
                hit = (pagePolygon.size() < 3U)
                    ? textgeom::TextHitsPoint(text, page.width, currentPage)
                    : textgeom::TextIntersectsPolygonSelection(text, page.width, pagePolygon);
            }
            if (hit && pageView.pageIndex < selectedTextIndices_.size()) {
                selectedTextIndices_[pageView.pageIndex].push_back(textIndex);
            }
        }
        for (std::size_t latexIndex = 0; latexIndex < page.latexes.size(); ++latexIndex) {
            const PageLatex& latex = page.latexes[latexIndex];
            bool hit = false;
            if (effectiveSelectionMode == SelectionMode::Box) {
                hit = clickSelection
                    ? LatexHitsPoint(latex, page.width, currentPage)
                    : LatexIntersectsRectSelection(latex, page.width, anchorPage, currentPage);
            } else {
                hit = (pagePolygon.size() < 3U)
                    ? LatexHitsPoint(latex, page.width, currentPage)
                    : LatexIntersectsPolygonSelection(latex, page.width, pagePolygon);
            }
            if (hit && pageView.pageIndex < selectedLatexIndices_.size()) {
                selectedLatexIndices_[pageView.pageIndex].push_back(latexIndex);
            }
        }
        const bool genericTextSelection = !selectionBoxOverride_
            && effectiveSelectionMode == SelectionMode::Lasso
            && CursorHitsPdfText(selectionAnchorCursor_);
        const Document::PdfSource* pdfSource = document_.PdfSourceAtIndex(page.sourcePdfSourceIndex);
        if (!genericTextSelection
            || pdfSource == nullptr
            || page.sourcePdfPageIndex < 0
            || pageView.pageIndex >= selectedPdfTextBoxes_.size()) {
            continue;
        }
        const std::optional<std::vector<PdfTextCharBox>> pdfTextBoxes = pdfBackend_.ExtractTextCharBoxes(
            pdfSource->pdfData,
            static_cast<std::size_t>(page.sourcePdfPageIndex));
        if (!pdfTextBoxes.has_value() || pdfTextBoxes->empty()) {
            continue;
        }
        const auto boxContainsPoint = [](const PdfTextCharBox& box, Vec2 point) {
            return point.x >= box.left
                && point.x <= box.right
                && point.y >= box.top
                && point.y <= box.bottom;
        };
        const auto anchorHitPage = HitTestPage(selectionAnchorCursor_);
        const auto currentHitPage = HitTestPage(selectionCurrentCursor_);
        if (!anchorHitPage.has_value()
            || !currentHitPage.has_value()
            || anchorHitPage->pageIndex != pageView.pageIndex
            || currentHitPage->pageIndex != pageView.pageIndex) {
            continue;
        }
        const Vec2 pdfAnchorPoint = ScreenToPage(*anchorHitPage, selectionAnchorCursor_, false);
        const Vec2 pdfCurrentPoint = ScreenToPage(*currentHitPage, selectionCurrentCursor_, false);
        const auto nearestBoxIndex = [&](Vec2 point) {
            std::size_t nearestIndex = 0;
            float nearestDistance = std::numeric_limits<float>::max();
            for (std::size_t index = 0; index < pdfTextBoxes->size(); ++index) {
                const PdfTextCharBox& box = (*pdfTextBoxes)[index];
                if (boxContainsPoint(box, point)) {
                    return index;
                }
                const Vec2 center {
                    (box.left + box.right) * 0.5f,
                    (box.top + box.bottom) * 0.5f,
                };
                const float distance = DistanceSquared(center, point);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearestIndex = index;
                }
            }
            return nearestIndex;
        };
        const std::size_t anchorIndex = nearestBoxIndex(pdfAnchorPoint);
        const std::size_t currentIndex = nearestBoxIndex(pdfCurrentPoint);
        const std::size_t firstIndex = std::min(anchorIndex, currentIndex);
        const std::size_t lastIndex = std::max(anchorIndex, currentIndex);
        for (std::size_t index = firstIndex; index <= lastIndex; ++index) {
            selectedPdfTextBoxes_[pageView.pageIndex].push_back((*pdfTextBoxes)[index]);
        }
    }

    if (HasSelectedPdfText()) {
        for (std::vector<std::size_t>& indices : selectedImageIndices_) {
            indices.clear();
        }
        for (std::vector<std::size_t>& indices : selectedTextIndices_) {
            indices.clear();
        }
        for (std::vector<std::size_t>& indices : selectedLatexIndices_) {
            indices.clear();
        }
        for (std::vector<std::size_t>& indices : selectedStrokeIndices_) {
            indices.clear();
        }
    }
    isSelecting_ = false;
    selectionLassoCursorPoints_.clear();
    selectionBoxOverride_ = false;
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::BeginMoveSelection(POINT cursor) {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        if (HasSelection()) {
            ClearSelection();
            InvalidateCanvas();
        }
        return;
    }

    const std::size_t pageIndex = hitPage->pageIndex;
    const Vec2 pagePoint = ScreenToPage(cursor, nullptr);
    const float hitRadius = std::max(8.0f, brushWidth_ * 2.5f) / std::max(0.1f, hitPage->scale);
    const Page& page = document_.PageAt(pageIndex);
    std::size_t strokeHandlePageIndex = 0;
    const auto strokeHandle = HitTestSelectedStrokeHandle(cursor, &strokeHandlePageIndex);
    if (strokeHandle.has_value() && strokeHandlePageIndex == pageIndex) {
        BeginStrokeTransform(pageIndex, *strokeHandle, cursor);
        return;
    }

    std::size_t imageHandlePageIndex = 0;
    std::size_t imageHandleImageIndex = 0;
    const auto imageHandle = HitTestSelectedImageHandle(cursor, &imageHandlePageIndex, &imageHandleImageIndex);
    if (imageHandle.has_value() && imageHandlePageIndex == pageIndex) {
        BeginImageTransform(pageIndex, imageHandleImageIndex, *imageHandle, cursor);
        return;
    }

    std::size_t textHandlePageIndex = 0;
    std::size_t textHandleTextIndex = 0;
    const auto textHandle = HitTestSelectedTextHandle(cursor, &textHandlePageIndex, &textHandleTextIndex);
    if (textHandle.has_value() && textHandlePageIndex == pageIndex) {
        BeginTextTransform(pageIndex, textHandleTextIndex, *textHandle, cursor);
        return;
    }
    std::size_t latexHandlePageIndex = 0;
    std::size_t latexHandleLatexIndex = 0;
    const auto latexHandle = HitTestSelectedLatexHandle(cursor, &latexHandlePageIndex, &latexHandleLatexIndex);
    if (latexHandle.has_value() && latexHandlePageIndex == pageIndex) {
        BeginLatexTransform(pageIndex, latexHandleLatexIndex, *latexHandle, cursor);
        return;
    }

    const bool clickedSelectionMoveGrip = CursorHitsSelectionMoveGrip(cursor);
    std::optional<std::size_t> clickedSelectedStroke {};
    std::optional<std::size_t> clickedSelectedText {};
    std::optional<std::size_t> clickedSelectedLatex {};
    std::optional<std::size_t> clickedSelectedImage {};
    std::optional<std::size_t> clickedStroke {};
    std::optional<std::size_t> clickedText {};
    std::optional<std::size_t> clickedLatex {};
    std::optional<std::size_t> clickedImage {};
    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value()
        && pageIndex < selectedLatexIndices_.size()) {
        for (std::size_t latexIndex : selectedLatexIndices_[pageIndex]) {
            if (latexIndex < page.latexes.size() && LatexHitsPoint(page.latexes[latexIndex], page.width, pagePoint)) {
                clickedSelectedLatex = latexIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedLatex.has_value() && pageIndex < selectedStrokeIndices_.size()) {
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
            if (strokeIndex < page.strokes.size() && StrokeHitsPoint(page.strokes[strokeIndex], pagePoint, hitRadius, settings_.strokeSmoothing)) {
                clickedSelectedStroke = strokeIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedLatex.has_value() && !clickedSelectedStroke.has_value()
        && pageIndex < selectedTextIndices_.size()) {
        for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
            if (textIndex < page.texts.size() && textgeom::TextHitsPoint(page.texts[textIndex], page.width, pagePoint)) {
                clickedSelectedText = textIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value()
        && !clickedSelectedLatex.has_value() && pageIndex < selectedImageIndices_.size()) {
        for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
            if (imageIndex < page.images.size() && ImageHitsPoint(page.images[imageIndex], pagePoint)) {
                clickedSelectedImage = imageIndex;
                break;
            }
        }
    }

    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value()
        && !clickedSelectedLatex.has_value() && !clickedSelectedImage.has_value() && !clickedStroke.has_value()
        && !clickedText.has_value()) {
        for (std::size_t latexIndex = page.latexes.size(); latexIndex > 0; --latexIndex) {
            const std::size_t actualIndex = latexIndex - 1U;
            if (LatexHitsPoint(page.latexes[actualIndex], page.width, pagePoint)) {
                clickedLatex = actualIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value()
        && !clickedSelectedLatex.has_value() && !clickedSelectedImage.has_value()) {
        for (std::size_t strokeIndex = page.strokes.size(); strokeIndex > 0; --strokeIndex) {
            const std::size_t actualIndex = strokeIndex - 1U;
            if (StrokeHitsPoint(page.strokes[actualIndex], pagePoint, hitRadius, settings_.strokeSmoothing)) {
                clickedStroke = actualIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value()
        && !clickedSelectedLatex.has_value() && !clickedSelectedImage.has_value() && !clickedStroke.has_value()) {
        for (std::size_t textIndex = page.texts.size(); textIndex > 0; --textIndex) {
            const std::size_t actualIndex = textIndex - 1U;
            if (textgeom::TextHitsPoint(page.texts[actualIndex], page.width, pagePoint)) {
                clickedText = actualIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value()
        && !clickedSelectedLatex.has_value() && !clickedSelectedImage.has_value()
        && !clickedStroke.has_value() && !clickedText.has_value() && !clickedLatex.has_value()) {
        for (std::size_t imageIndex = page.images.size(); imageIndex > 0; --imageIndex) {
            const std::size_t actualIndex = imageIndex - 1U;
            if (ImageHitsPoint(page.images[actualIndex], pagePoint)) {
                clickedImage = actualIndex;
                break;
            }
        }
    }

    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value()
        && !clickedSelectedLatex.has_value() && !clickedSelectedImage.has_value()
        && !clickedStroke.has_value() && !clickedText.has_value() && !clickedLatex.has_value() && !clickedImage.has_value()) {
        ClearSelection();
        BeginSelection(cursor, true);
        return;
    }

    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value()
        && !clickedSelectedLatex.has_value() && !clickedSelectedImage.has_value()) {
        ClearSelection();
        if (clickedStroke.has_value() && pageIndex < selectedStrokeIndices_.size()) {
            selectedStrokeIndices_[pageIndex].push_back(*clickedStroke);
        }
        if (clickedText.has_value() && pageIndex < selectedTextIndices_.size()) {
            selectedTextIndices_[pageIndex].push_back(*clickedText);
        }
        if (clickedLatex.has_value() && pageIndex < selectedLatexIndices_.size()) {
            selectedLatexIndices_[pageIndex].push_back(*clickedLatex);
        }
        if (clickedImage.has_value() && pageIndex < selectedImageIndices_.size()) {
            selectedImageIndices_[pageIndex].push_back(*clickedImage);
        }
    }

    if (!clickedSelectionMoveGrip && activeTool_ == Tool::Move && clickedSelectedText.has_value()
        && !clickedSelectedStroke.has_value() && !clickedSelectedLatex.has_value() && !clickedSelectedImage.has_value()) {
        pendingTextEditClick_ = true;
        pendingTextEditCursor_ = cursor;
        pendingTextEditPageIndex_ = pageIndex;
        pendingTextEditIndex_ = *clickedSelectedText;
        UpdateCaptureState();
        return;
    }

    pendingTextEditClick_ = false;
    pendingTextEditPageIndex_.reset();
    pendingTextEditIndex_.reset();
    isMovingSelection_ = true;
    moveSelectionPageIndex_ = pageIndex;
    interactionPreviewMode_ = InteractionPreviewMode::MoveSelection;
    interactionPreviewStartCursor_ = cursor;
    lastMoveCursor_ = cursor;
    moveSelectionScale_ = std::max(0.1f, hitPage->scale);
    EnsureInteractionPreviewStorage();
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
    for (const PageView& previewPageView : pageViews) {
        const bool hasSelectedStrokes = previewPageView.pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[previewPageView.pageIndex].empty();
        const bool hasSelectedTexts = previewPageView.pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[previewPageView.pageIndex].empty();
        const bool hasSelectedLatexes = previewPageView.pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[previewPageView.pageIndex].empty();
        const bool hasSelectedImages = previewPageView.pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[previewPageView.pageIndex].empty();
        if (hasSelectedStrokes || hasSelectedTexts || hasSelectedLatexes || hasSelectedImages) {
            BuildInteractionPageSnapshot(previewPageView);
        }
    }
    RebuildMoveSelectionTextCache();
    RebuildMoveSelectionPreview(cursor);
    document_.SetCurrentPageIndex(pageIndex);
    MarkPdfSidebarCompositionDirty();
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::UpdateMoveSelection(POINT cursor) {
    if (pendingTextEditClick_) {
        if (DistanceSquared(PointToVec2(cursor), PointToVec2(pendingTextEditCursor_)) < (5.0f * 5.0f)
            || !pendingTextEditPageIndex_.has_value()) {
            return;
        }
        pendingTextEditClick_ = false;
        const std::size_t pageIndex = *pendingTextEditPageIndex_;
        pendingTextEditPageIndex_.reset();
        pendingTextEditIndex_.reset();

        const auto hitPage = HitTestPage(pendingTextEditCursor_);
        if (!hitPage.has_value() || pageIndex >= document_.PageCount()) {
            UpdateCaptureState();
            return;
        }

        isMovingSelection_ = true;
        moveSelectionPageIndex_ = pageIndex;
        interactionPreviewMode_ = InteractionPreviewMode::MoveSelection;
        interactionPreviewStartCursor_ = pendingTextEditCursor_;
        lastMoveCursor_ = pendingTextEditCursor_;
        moveSelectionScale_ = std::max(0.1f, hitPage->scale);
        EnsureInteractionPreviewStorage();
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
        for (const PageView& previewPageView : pageViews) {
            const bool hasSelectedStrokes = previewPageView.pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[previewPageView.pageIndex].empty();
            const bool hasSelectedTexts = previewPageView.pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[previewPageView.pageIndex].empty();
            const bool hasSelectedLatexes = previewPageView.pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[previewPageView.pageIndex].empty();
            const bool hasSelectedImages = previewPageView.pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[previewPageView.pageIndex].empty();
            if (hasSelectedStrokes || hasSelectedTexts || hasSelectedLatexes || hasSelectedImages) {
                BuildInteractionPageSnapshot(previewPageView);
            }
        }
        RebuildMoveSelectionTextCache();
        RebuildMoveSelectionPreview(cursor);
        document_.SetCurrentPageIndex(pageIndex);
        MarkPdfSidebarCompositionDirty();
        UpdateCaptureState();
        InvalidateCanvas();
        return;
    }

    if (!isMovingSelection_) {
        return;
    }

    if (cursor.x == lastMoveCursor_.x && cursor.y == lastMoveCursor_.y) {
        return;
    }
    RebuildMoveSelectionPreview(cursor);
    lastMoveCursor_ = cursor;
}

void App::EndMoveSelection() {
    if (!isMovingSelection_) {
        return;
    }

    if (lastMoveCursor_.x == interactionPreviewStartCursor_.x && lastMoveCursor_.y == interactionPreviewStartCursor_.y) {
        ClearInteractionPreview();
        isMovingSelection_ = false;
        moveSelectionPageIndex_.reset();
        InvalidateCanvas();
        UpdateCaptureState();
        return;
    }

    std::vector<std::size_t> affectedPageIndices;
    affectedPageIndices.reserve(document_.PageCount());
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const bool hasSelectedStrokes = pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty();
        const bool hasSelectedTexts = pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty();
        const bool hasSelectedLatexes = pageIndex < selectedLatexIndices_.size() && !selectedLatexIndices_[pageIndex].empty();
        const bool hasSelectedImages = pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty();
        const bool hasPreviewStrokes = pageIndex < interactionPreviewStrokes_.size() && !interactionPreviewStrokes_[pageIndex].empty();
        const bool hasPreviewTexts = pageIndex < interactionPreviewTexts_.size() && !interactionPreviewTexts_[pageIndex].empty();
        const bool hasPreviewLatexes = pageIndex < interactionPreviewLatexes_.size() && !interactionPreviewLatexes_[pageIndex].empty();
        const bool hasPreviewImages = pageIndex < interactionPreviewImages_.size() && !interactionPreviewImages_[pageIndex].empty();
        if (hasSelectedStrokes || hasSelectedTexts || hasSelectedLatexes || hasSelectedImages
            || hasPreviewStrokes || hasPreviewTexts || hasPreviewLatexes || hasPreviewImages) {
            affectedPageIndices.push_back(pageIndex);
        }

    }
    std::vector<Page> beforePages;
    beforePages.reserve(affectedPageIndices.size());
    for (std::size_t pageIndex : affectedPageIndices) {
        beforePages.push_back(document_.PageAt(pageIndex));
    }
    const std::size_t focusPageIndex = std::min(document_.CurrentPageIndex(), document_.PageCount() - 1U);

    CommitMoveSelectionPreview();
    PushReplacePagesHistoryAction(std::move(affectedPageIndices), std::move(beforePages), focusPageIndex);
    ClearInteractionPreview();
    isMovingSelection_ = false;
    moveSelectionPageIndex_.reset();
    NotifyDocumentMutated();
    InvalidateCanvas();
    UpdateCaptureState();
}

}  // namespace cnote
