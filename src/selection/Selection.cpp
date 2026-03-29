#include "App.h"

#include <algorithm>

namespace cnote {

void App::ClearSelection() {
    selectedImageIndices_.assign(document_.PageCount(), {});
    selectedTextIndices_.assign(document_.PageCount(), {});
    selectedStrokeIndices_.assign(document_.PageCount(), {});
}

bool App::HasSelection() const {
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if ((pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty())
            || (pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty())
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
        const bool hasImages = pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty();
        if (hasStrokes || hasTexts || hasImages) {
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
    for (std::size_t index = 0; index < document_.PageCount(); ++index) {
        if (pageIndex.has_value() && index != *pageIndex) {
            continue;
        }

        const bool hasSelectedStrokes = index < selectedStrokeIndices_.size() && !selectedStrokeIndices_[index].empty();
        const bool hasSelectedTexts = index < selectedTextIndices_.size() && !selectedTextIndices_[index].empty();
        const bool hasSelectedImages = index < selectedImageIndices_.size() && !selectedImageIndices_[index].empty();
        if (!hasSelectedImages && !hasSelectedTexts && !hasSelectedStrokes) {
            continue;
        }

        hadSelection = true;

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
        if ((pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty())
            || (pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty())) {
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
        if ((pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty())
            || (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty())) {
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

std::optional<std::size_t> App::SingleSelectedStrokePage() const {
    std::optional<std::size_t> result;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if ((pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty())
            || (pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty())) {
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

}  // namespace cnote
