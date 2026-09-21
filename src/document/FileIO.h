#pragma once

#include <filesystem>
#include <optional>

#include "Document.h"

namespace cnote {

class PdfiumBackend;

// Shared file-loading service: reads a PDF, prefers a valid embedded cnote
// project (decode + hydrate), and otherwise opens the PDF itself. Returns
// nullopt when the file cannot be read or rendered.
[[nodiscard]] std::optional<Document> LoadDocumentFile(const std::filesystem::path& filePath, PdfiumBackend& pdfBackend);

}  // namespace cnote
