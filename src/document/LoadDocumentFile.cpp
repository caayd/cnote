#include "document/FileIO.h"

#include <fstream>
#include <memory>
#include <vector>

#include "PdfBackend.h"
#include "ProjectFile.h"

namespace cnote {

namespace {

std::optional<std::vector<std::uint8_t>> ReadAllBytes(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            return std::nullopt;
        }
    }
    return bytes;
}

}  // namespace

std::optional<Document> LoadDocumentFile(const std::filesystem::path& filePath, PdfiumBackend& pdfBackend) {
    const auto bytes = ReadAllBytes(filePath);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    auto pdfBytes = std::make_shared<std::vector<std::uint8_t>>(std::move(*bytes));
    if (const auto embeddedProject = pdfBackend.ExtractEmbeddedProject(pdfBytes); embeddedProject.has_value()) {
        if (auto projectDocument = ProjectFile::LoadFromBytes(*embeddedProject, filePath);
            projectDocument.has_value()) {
            pdfBackend.HydrateProject(*projectDocument);
            return projectDocument;
        }
    }
    return pdfBackend.OpenFromMemory(filePath, std::move(pdfBytes));
}

}  // namespace cnote
