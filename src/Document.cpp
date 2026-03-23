#include "Document.h"

namespace ctrainer {

Document Document::CreateBlank() {
    Document document;
    document.ResetToBlank();
    return document;
}

Document Document::CreatePdfPlaceholder(std::wstring sourcePath) {
    Document document;
    document.ReplaceWithPlaceholder(std::move(sourcePath));
    return document;
}

bool Document::IsPdfPlaceholder() const noexcept {
    return pdfPlaceholder_;
}

const std::wstring& Document::SourcePath() const noexcept {
    return sourcePath_;
}

Page& Document::CurrentPage() noexcept {
    return page_;
}

const Page& Document::CurrentPage() const noexcept {
    return page_;
}

void Document::ResetToBlank() {
    sourcePath_.clear();
    pdfPlaceholder_ = false;
    page_ = {};
}

void Document::ReplaceWithPlaceholder(std::wstring sourcePath) {
    sourcePath_ = std::move(sourcePath);
    pdfPlaceholder_ = true;
    page_ = {};
}

}  // namespace ctrainer
