#include "qt/QtDocumentIo.h"

#include <array>
#include <fstream>

#include "PdfBackend.h"
#include "ProjectFile.h"
#include "document/FileIO.h"

namespace cnote::qt {

namespace {

bool StartsWithPdfMagic(const std::filesystem::path& filePath) {
    std::ifstream stream(filePath, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::array<char, 5> magic {};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (stream.gcount() != static_cast<std::streamsize>(magic.size())) {
        return false;
    }
    return magic[0] == '%' && magic[1] == 'P' && magic[2] == 'D' && magic[3] == 'F' && magic[4] == '-';
}

}  // namespace

std::optional<Document> LoadDocument(const std::filesystem::path& filePath, std::string* error) {
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(filePath, fileError)) {
        if (error != nullptr) {
            *error = "not a readable file: " + filePath.string();
        }
        return std::nullopt;
    }

    PdfiumBackend pdfBackend;

    if (StartsWithPdfMagic(filePath)) {
        if (std::optional<Document> document = LoadDocumentFile(filePath, pdfBackend); document.has_value()) {
            return document;
        }
        if (error != nullptr) {
            *error = "cannot open PDF: " + filePath.string();
        }
        return std::nullopt;
    }

    std::optional<Document> document = ProjectFile::Load(filePath);
    if (!document.has_value()) {
        if (error != nullptr) {
            *error = "cannot decode cnote project: " + filePath.string();
        }
        return std::nullopt;
    }

    // Missing/invalid PDF sources fall back to the stored page raster, exactly
    // as the reference frontend does after a successful decode.
    pdfBackend.HydrateProject(*document);
    return document;
}

}  // namespace cnote::qt
