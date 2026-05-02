#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Document.h"
#include "PdfBackend.h"

namespace cnote {

inline constexpr wchar_t kCnoteFileExtension[] = L".cnote";

[[nodiscard]] std::string WideToUtf8(std::wstring_view value);
[[nodiscard]] std::wstring Utf8ToWide(std::string_view value);
[[nodiscard]] bool HasExtension(const std::wstring& path, const wchar_t* extension);

class ProjectFile {
public:
    [[nodiscard]] static std::optional<Document> Load(const std::wstring& filePath, PdfiumBackend& pdfBackend);
    [[nodiscard]] static std::optional<Document> LoadFromBytes(
        const std::vector<std::uint8_t>& bytes,
        const std::wstring& filePath,
        PdfiumBackend& pdfBackend);
    [[nodiscard]] static std::vector<std::uint8_t> Serialize(const Document& document);
    [[nodiscard]] static bool Save(const Document& document, const std::wstring& filePath);
};

}  // namespace cnote
