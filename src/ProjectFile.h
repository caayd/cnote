#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Document.h"
#include "PdfBackend.h"

namespace cnote {

inline constexpr std::u16string_view kCnoteFileExtension = u".cnote";

[[nodiscard]] bool HasExtension(const std::filesystem::path& path, std::u16string_view extension);

class ProjectFile {
public:
    [[nodiscard]] static std::optional<Document> Load(const std::filesystem::path& filePath, PdfiumBackend& pdfBackend);
    [[nodiscard]] static std::optional<Document> LoadFromBytes(
        const std::vector<std::uint8_t>& bytes,
        const std::filesystem::path& filePath,
        PdfiumBackend& pdfBackend);
    [[nodiscard]] static std::vector<std::uint8_t> Serialize(const Document& document);
    [[nodiscard]] static bool Save(const Document& document, const std::filesystem::path& filePath);
};

}  // namespace cnote
