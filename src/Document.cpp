#include "Document.h"

#include <algorithm>

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

Document Document::CreateEditable(std::wstring filePath, std::vector<Page> pages, std::size_t currentPageIndex, PageDefaults pageDefaults) {
    Document document;
    document.ReplaceWithEditable(std::move(filePath), std::move(pages), currentPageIndex, pageDefaults);
    return document;
}

Document Document::CreatePdfDocument(
    std::wstring sourcePath,
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
    document.ReplaceWithProject(pdfSources.empty() ? std::wstring {} : pdfSources.front().sourcePath, std::move(pdfSources), std::move(pages), currentPageIndex, pageDefaults);
    return document;
}

Document Document::CreateProjectDocument(
    std::wstring filePath,
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

const std::wstring& Document::FilePath() const noexcept {
    return filePath_;
}

const std::wstring& Document::SourcePath() const noexcept {
    static const std::wstring emptyPath;
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

void Document::SetFilePath(std::wstring filePath) {
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

void Document::ReplaceWithEditable(std::wstring filePath, std::vector<Page> pages, std::size_t currentPageIndex, PageDefaults pageDefaults) {
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

std::int32_t Document::AppendPdfSource(std::wstring sourcePath, std::shared_ptr<std::vector<std::uint8_t>> pdfData) {
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
    std::wstring filePath,
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

}  // namespace cnote
