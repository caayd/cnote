#include "document/FileIO.h"

#include <QFile>
#include <QString>

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "PdfBackend.h"
#include "ProjectFile.h"
#include "Unicode.h"

namespace cnote {

namespace {

std::atomic<std::size_t> documentFileReads = 0;

}  // namespace

std::shared_ptr<std::vector<std::uint8_t>> ReadDocumentBytes(const std::filesystem::path& filePath) {
    QFile file(QString::fromUtf8(PathToUtf8(filePath).c_str()));
    if (!file.open(QIODevice::ReadOnly)) {
        return nullptr;
    }

    const qint64 size = file.size();
    if (size < 0) {
        return nullptr;
    }
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(size));
    if (size != 0
        && file.read(reinterpret_cast<char*>(bytes->data()), size) != size) {
        return nullptr;
    }
    documentFileReads.fetch_add(1U, std::memory_order_relaxed);
    return bytes;
}

std::size_t DocumentFileReadCount() {
    return documentFileReads.load(std::memory_order_relaxed);
}

std::optional<Document> LoadDocumentFileFromBytes(
    std::shared_ptr<std::vector<std::uint8_t>> bytes,
    const std::filesystem::path& filePath,
    PdfiumBackend& pdfBackend) {
    if (!bytes) {
        return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> embeddedProject =
        pdfBackend.ExtractEmbeddedProject(bytes);
    if (embeddedProject.has_value()) {
        std::optional<Document> projectDocument = ProjectFile::LoadFromBytes(*embeddedProject, filePath);
        // The project copies its own PDF sources out of the attachment, so the
        // attachment and the unopened container bytes are not needed once the
        // decode succeeds. Release them (and the probe's PDFium handle) before
        // hydration opens the project's stored source.
        embeddedProject.reset();
        if (projectDocument.has_value()) {
            bytes.reset();
            PdfiumBackend::ReleaseUnreferencedDocuments();
            pdfBackend.HydrateProject(*projectDocument);
            return projectDocument;
        }
    }
    return pdfBackend.OpenFromMemory(filePath, std::move(bytes));
}

std::optional<Document> LoadDocumentFile(const std::filesystem::path& filePath, PdfiumBackend& pdfBackend) {
    std::shared_ptr<std::vector<std::uint8_t>> bytes = ReadDocumentBytes(filePath);
    if (!bytes) {
        return std::nullopt;
    }
    return LoadDocumentFileFromBytes(std::move(bytes), filePath, pdfBackend);
}

}  // namespace cnote
