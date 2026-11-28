#include "qt/QtDocumentIo.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "PdfBackend.h"
#include "ProjectFile.h"
#include "document/FileIO.h"

namespace cnote::qt {

namespace {

bool StartsWithPdfMagic(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 5> kPdfMagic { '%', 'P', 'D', 'F', '-' };
    return bytes.size() >= kPdfMagic.size()
        && std::equal(kPdfMagic.begin(), kPdfMagic.end(), bytes.begin());
}

}  // namespace

std::optional<Document> LoadDocument(
    const std::filesystem::path& filePath, PdfiumBackend& pdfBackend, std::string* error) {
    std::shared_ptr<std::vector<std::uint8_t>> bytes = ReadDocumentBytes(filePath);
    if (!bytes) {
        if (error != nullptr) {
            *error = "not a readable file: " + filePath.string();
        }
        return std::nullopt;
    }

    if (StartsWithPdfMagic(*bytes)) {
        if (std::optional<Document> document =
                LoadDocumentFileFromBytes(std::move(bytes), filePath, pdfBackend);
            document.has_value()) {
            return document;
        }
        if (error != nullptr) {
            *error = "cannot open PDF: " + filePath.string();
        }
        return std::nullopt;
    }

    std::optional<Document> document = ProjectFile::LoadFromBytes(*bytes, filePath);
    // A project decode copies everything it keeps; the file buffer is dead here
    // and must not be held while hydration opens the stored PDF sources.
    bytes.reset();
    if (!document.has_value()) {
        if (error != nullptr) {
            *error = "cannot decode cnote project: " + filePath.string();
        }
        return std::nullopt;
    }

    // Hydration materializes PDF-backed page rasters and falls back to the
    // stored page raster for missing/invalid sources, like the reference.
    pdfBackend.HydrateProject(*document);
    return document;
}

}  // namespace cnote::qt
