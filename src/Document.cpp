#include "Document.h"

#include <algorithm>

namespace cnote {

Document Document::CreateBlank() {
    Document document;
    document.ResetToBlank();
    return document;
}

Document Document::CreateEditable(std::wstring filePath, std::vector<Page> pages, std::size_t currentPageIndex) {
    Document document;
    document.ReplaceWithEditable(std::move(filePath), std::move(pages), currentPageIndex);
    return document;
}

Document Document::CreatePdfDocument(
    std::wstring sourcePath,
    std::shared_ptr<std::vector<std::uint8_t>> pdfData,
    std::vector<Page> pages,
    std::size_t currentPageIndex) {
    Document document;
    document.ReplaceWithPdf(sourcePath, std::move(sourcePath), std::move(pdfData), std::move(pages), currentPageIndex);
    return document;
}

Document Document::CreateProjectDocument(
    std::wstring filePath,
    std::wstring sourcePath,
    std::shared_ptr<std::vector<std::uint8_t>> pdfData,
    std::vector<Page> pages,
    std::size_t currentPageIndex) {
    Document document;
    if (pdfData && !pdfData->empty()) {
        document.ReplaceWithPdf(std::move(filePath), std::move(sourcePath), std::move(pdfData), std::move(pages), currentPageIndex);
    } else {
        document.ReplaceWithEditable(std::move(filePath), std::move(pages), currentPageIndex);
    }
    return document;
}

bool Document::IsPdfDocument() const noexcept {
    return pdfDocument_;
}

const std::wstring& Document::FilePath() const noexcept {
    return filePath_;
}

const std::wstring& Document::SourcePath() const noexcept {
    return sourcePath_;
}

const std::shared_ptr<std::vector<std::uint8_t>>& Document::PdfData() const noexcept {
    return pdfData_;
}

bool Document::HasPdfData() const noexcept {
    return pdfData_ && !pdfData_->empty();
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

void Document::SetFilePath(std::wstring filePath) {
    filePath_ = std::move(filePath);
}

void Document::ResetToBlank() {
    filePath_.clear();
    sourcePath_.clear();
    pdfDocument_ = false;
    pdfData_.reset();
    pages_ = { Page {} };
    currentPageIndex_ = 0;
}

void Document::ReplaceWithEditable(std::wstring filePath, std::vector<Page> pages, std::size_t currentPageIndex) {
    filePath_ = std::move(filePath);
    sourcePath_.clear();
    pdfDocument_ = false;
    pdfData_.reset();
    if (pages.empty()) {
        pages_ = { Page {} };
    } else {
        pages_ = std::move(pages);
    }
    currentPageIndex_ = pages_.empty() ? 0 : std::min(currentPageIndex, pages_.size() - 1U);
}

void Document::ReplaceWithPdf(
    std::wstring filePath,
    std::wstring sourcePath,
    std::shared_ptr<std::vector<std::uint8_t>> pdfData,
    std::vector<Page> pages,
    std::size_t currentPageIndex) {
    filePath_ = std::move(filePath);
    sourcePath_ = std::move(sourcePath);
    pdfDocument_ = true;
    pdfData_ = std::move(pdfData);
    if (pages.empty()) {
        pages_ = { Page {} };
        pdfDocument_ = false;
        pdfData_.reset();
    } else {
        pages_ = std::move(pages);
    }
    currentPageIndex_ = pages_.empty() ? 0 : std::min(currentPageIndex, pages_.size() - 1U);
}

}  // namespace cnote
