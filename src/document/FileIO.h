#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "Document.h"

namespace cnote {

class PdfiumBackend;

// Reads a whole document file once into the shared buffer that will become the
// PDF source (or the project decode input). Returns null when the file cannot
// be opened or read in full. Sniffing and decoding happen on this buffer, so an
// open never reads the path a second time.
[[nodiscard]] std::shared_ptr<std::vector<std::uint8_t>> ReadDocumentBytes(
    const std::filesystem::path& filePath);

// Number of files read through ReadDocumentBytes since process start.
// Diagnostics only: one document open must read its file once.
[[nodiscard]] std::size_t DocumentFileReadCount();

// Shared file-loading service for bytes already read from `filePath`: prefers a
// valid embedded cnote project (decode + hydrate), and otherwise opens the PDF
// itself. The buffer is consumed and, for a PDF, becomes the document's source
// and the cache-pinned PDFium buffer, so the attachment probe and the open
// share one PDFium document. Returns nullopt when the bytes are not a usable
// PDF or project.
[[nodiscard]] std::optional<Document> LoadDocumentFileFromBytes(
    std::shared_ptr<std::vector<std::uint8_t>> bytes,
    const std::filesystem::path& filePath,
    PdfiumBackend& pdfBackend);

// Reads the path once and dispatches through LoadDocumentFileFromBytes.
[[nodiscard]] std::optional<Document> LoadDocumentFile(
    const std::filesystem::path& filePath, PdfiumBackend& pdfBackend);

}  // namespace cnote
