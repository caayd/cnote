#include "Document.h"

namespace ctrainer {

Document Document::CreateBlank() {
    Document document;
    document.ResetToBlank();
    return document;
}

Document Document::CreatePdfDocument(std::wstring sourcePath, std::vector<Page> pages) {
    Document document;
    document.ReplaceWithPdf(std::move(sourcePath), std::move(pages));
    return document;
}

bool Document::IsPdfDocument() const noexcept {
    return pdfDocument_;
}

const std::wstring& Document::SourcePath() const noexcept {
    return sourcePath_;
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

void Document::ResetToBlank() {
    sourcePath_.clear();
    pdfDocument_ = false;
    pages_ = { Page {} };
    currentPageIndex_ = 0;
}

void Document::ReplaceWithPdf(std::wstring sourcePath, std::vector<Page> pages) {
    sourcePath_ = std::move(sourcePath);
    pdfDocument_ = true;
    if (pages.empty()) {
        pages_ = { Page {} };
        pdfDocument_ = false;
    } else {
        pages_ = std::move(pages);
    }
    currentPageIndex_ = 0;
}

}  // namespace ctrainer
