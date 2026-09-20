#include "document/FileIO.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#include <memory>
#include <vector>

#include "PdfBackend.h"
#include "ProjectFile.h"
#include "Unicode.h"

namespace cnote {

namespace {

std::optional<std::vector<std::uint8_t>> ReadAllBytes(const std::filesystem::path& filePath) {
    QFile file(QString::fromUtf8(PathToUtf8(filePath).c_str()));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
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
