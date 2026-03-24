#pragma once

#include <optional>
#include <string>

#include "Document.h"
#include "PdfBackend.h"

namespace cnote {

inline constexpr wchar_t kCnoteFileExtension[] = L".cnote";

class ProjectFile {
public:
    [[nodiscard]] static std::optional<Document> Load(const std::wstring& filePath, PdfiumBackend& pdfBackend);
    [[nodiscard]] static bool Save(const Document& document, const std::wstring& filePath);
};

}  // namespace cnote
