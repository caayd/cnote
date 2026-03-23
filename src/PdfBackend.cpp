#include "PdfBackend.h"

namespace ctrainer {

std::optional<Document> StubPdfBackend::Open(const std::wstring& filePath) {
    if (filePath.empty()) {
        return std::nullopt;
    }

    return Document::CreatePdfPlaceholder(filePath);
}

}  // namespace ctrainer
