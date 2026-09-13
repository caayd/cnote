#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "Document.h"

namespace cnote {
class PdfiumBackend;
}

namespace cnote::qt {

// Loads a cnote project or a PDF document through the shared portable services:
//   * PDF containers (with or without an embedded cnote project) go through
//     cnote::LoadDocumentFile, which prefers the attachment and otherwise opens
//     the PDF itself.
//   * cnote projects decode with ProjectFile::Load and are then hydrated from
//     their stored PDF sources by PdfiumBackend::HydrateProject.
// Returns nullopt with a reason in `error` when the file cannot be loaded; the
// caller's current document is left untouched.
[[nodiscard]] std::optional<Document> LoadDocument(
    const std::filesystem::path& filePath, PdfiumBackend& pdfBackend, std::string* error);

}  // namespace cnote::qt
