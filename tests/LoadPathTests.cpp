// File-open path regression test: a document open must read its file once,
// reuse one PDFium document for the attachment probe and the PDF open, and
// release an embedded project's container bytes/handle before hydration.
//
// Fixtures are generated in a per-run scratch directory (no external files).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "Document.h"
#include "PdfBackend.h"
#include "ProjectFile.h"
#include "document/FileIO.h"
#include "qt/QtDocumentIo.h"

namespace {

int gFailures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++gFailures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

// A valid one-page PDF whose comment padding makes the file large, so the
// transient buffers of the load path are the dominant allocation.
std::vector<std::uint8_t> BuildPdf(std::size_t paddingBytes) {
    const std::string content = "BT /F1 24 Tf 72 720 Td (Load path) Tj ET\n";
    const std::vector<std::string> objects {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };

    std::string pdf = "%PDF-1.4\n";
    if (paddingBytes != 0) {
        // Printable pseudo-random bytes (no newline) keep the padding
        // incompressible, so a project attachment carrying this PDF stays
        // large enough for the load path's file buffers to matter.
        pdf += "%";
        std::uint32_t state = 0x12345678U;
        pdf.reserve(pdf.size() + paddingBytes);
        for (std::size_t index = 0; index < paddingBytes; ++index) {
            state = (state * 1664525U) + 1013904223U;
            pdf.push_back(static_cast<char>(33U + ((state >> 24U) % 94U)));
        }
        pdf += "\n";
    }
    std::vector<std::size_t> offsets(1, 0);
    for (std::size_t index = 0; index < objects.size(); ++index) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(index + 1U) + " 0 obj\n" + objects[index] + "\nendobj\n";
    }

    const std::size_t xrefOffset = pdf.size();
    pdf += "xref\n0 6\n0000000000 65535 f \n";
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        std::string offset = std::to_string(offsets[index]);
        pdf += std::string(10U - offset.size(), '0') + offset + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n";
    pdf += std::to_string(xrefOffset) + "\n%%EOF\n";
    return std::vector<std::uint8_t>(pdf.begin(), pdf.end());
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return stream.good();
}

cnote::Document BuildProjectWithSource(const std::shared_ptr<std::vector<std::uint8_t>>& source) {
    cnote::Page page;
    page.width = 612.0f;
    page.height = 792.0f;
    page.sourcePdfSourceIndex = 0;
    page.sourcePdfPageIndex = 0;
    cnote::Document::PageDefaults defaults;
    defaults.width = 612.0f;
    defaults.height = 792.0f;
    defaults.sizeOverridden = true;
    return cnote::Document::CreateProjectDocument(
        std::filesystem::path(u"fixture.cnote"),
        { { std::filesystem::path(u"source.pdf"), source } },
        { page },
        0,
        defaults);
}

cnote::Document BuildBlankProject() {
    cnote::Document document = cnote::Document::CreateBlank();
    document.CurrentPage().width = 200.0f;
    document.CurrentPage().height = 200.0f;
    cnote::Stroke stroke {};
    stroke.style.color = 0xFF204060U;
    stroke.style.width = 4.0f;
    stroke.points = { { 10.0f, 10.0f }, { 190.0f, 190.0f } };
    document.CurrentPage().strokes.push_back(stroke);
    return document;
}

}  // namespace

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path()
        / ("cnote-load-path-"
            + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count())
            + "-" + std::to_string(std::random_device {}()));
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        std::printf("FAIL: cannot create scratch directory\n");
        return 1;
    }

    cnote::PdfiumBackend backend;

    // --- a plain PDF: one read, one PDFium open (probe + open share it) ------
    {
        constexpr std::size_t kPaddingBytes = 4U * 1024U * 1024U;
        const std::filesystem::path path = dir / "large.pdf";
        const std::vector<std::uint8_t> fileBytes = BuildPdf(kPaddingBytes);
        Check(WriteBytes(path, fileBytes), "large PDF fixture written");

        cnote::PdfiumBackend::ReleaseUnreferencedDocuments();
        const std::size_t readsBefore = cnote::DocumentFileReadCount();
        const std::size_t loadsBefore = backend.DocumentLoadCount();
        const std::size_t closesBefore = backend.DocumentCloseCount();
        {
            std::string loadError;
            std::optional<cnote::Document> document = cnote::qt::LoadDocument(path, backend, &loadError);
            Check(document.has_value(), "large PDF loads: " + loadError);
            Check(cnote::DocumentFileReadCount() == readsBefore + 1U,
                "a PDF open reads its file once");
            Check(backend.DocumentLoadCount() == loadsBefore + 1U,
                "a PDF open loads one PDFium document (probe and open share it)");
            if (document.has_value()) {
                Check(document->PageCount() == 1U, "large PDF keeps its page");
                Check(document->PdfData() != nullptr && document->PdfData()->size() == fileBytes.size(),
                    "the read buffer becomes the PDF source without a staging copy");
            }
        }
        cnote::PdfiumBackend::ReleaseUnreferencedDocuments();
        Check(backend.DocumentCloseCount() == closesBefore + 1U,
            "the PDF handle closes with its document");
    }

    // --- a standalone project: one read, the stored source opens once --------
    {
        const std::filesystem::path path = dir / "fixture.cnote";
        const std::vector<std::uint8_t> fileBytes =
            cnote::ProjectFile::Serialize(BuildProjectWithSource(
                std::make_shared<std::vector<std::uint8_t>>(BuildPdf(0))));
        Check(WriteBytes(path, fileBytes), "project fixture written");

        cnote::PdfiumBackend::ReleaseUnreferencedDocuments();
        const std::size_t readsBefore = cnote::DocumentFileReadCount();
        const std::size_t loadsBefore = backend.DocumentLoadCount();
        {
            std::string loadError;
            std::optional<cnote::Document> document = cnote::qt::LoadDocument(path, backend, &loadError);
            Check(document.has_value(), "project loads: " + loadError);
            Check(cnote::DocumentFileReadCount() == readsBefore + 1U,
                "a project open reads its file once");
            Check(backend.DocumentLoadCount() == loadsBefore + 1U,
                "hydration opens the stored PDF source once");
            if (document.has_value()) {
                Check(document->Kind() == cnote::DocumentKind::Project, "a .cnote stays a project");
                Check(document->PdfSourceCount() == 1U, "project keeps its PDF source");
                Check(document->PageAt(0).sourcePdfSourceIndex == 0
                        && document->PageAt(0).sourcePdfPageIndex == 0,
                    "hydration keeps a valid source reference");
                Check(cnote::ProjectFile::Serialize(*document) == fileBytes,
                    "a reloaded project re-serializes byte-for-byte");
            }
        }
    }

    // --- an embedded project without PDF sources: the probe is the only open,
    // and its container bytes/handle are released before the document returns.
    {
        const std::filesystem::path path = dir / "embedded.pdf";
        const cnote::Document project = BuildBlankProject();
        const std::vector<std::uint8_t> embeddedProject = cnote::ProjectFile::Serialize(project);
        Check(backend.ExportPdf(project, path, 1.0f, 0.55f, 3.0f, 2.0f, embeddedProject),
            "embedded PDF fixture exported");

        cnote::PdfiumBackend::ReleaseUnreferencedDocuments();
        const std::size_t readsBefore = cnote::DocumentFileReadCount();
        const std::size_t loadsBefore = backend.DocumentLoadCount();
        const std::size_t closesBefore = backend.DocumentCloseCount();
        const std::size_t cachedBefore = backend.CachedDocumentCount();
        {
            std::string loadError;
            std::optional<cnote::Document> document = cnote::qt::LoadDocument(path, backend, &loadError);
            Check(document.has_value(), "embedded project loads: " + loadError);
            Check(cnote::DocumentFileReadCount() == readsBefore + 1U,
                "an embedded PDF open reads its file once");
            Check(backend.DocumentLoadCount() == loadsBefore + 1U,
                "the attachment probe and the load use one PDFium open");
            Check(backend.DocumentCloseCount() == closesBefore + 1U,
                "the container handle closes before the project document returns");
            Check(backend.CachedDocumentCount() == cachedBefore,
                "the container's cached document is released with its bytes");
            if (document.has_value()) {
                Check(document->Kind() == cnote::DocumentKind::EmbeddedProject,
                    "a project recovered from a PDF reports EmbeddedProject");
                Check(document->PageAt(0).strokes.size() == 1U
                        && document->PageAt(0).strokes.front().points.size() == 2U,
                    "embedded project content round-trips");
            }
        }
    }

    // --- an embedded project with a stored PDF source: the container and the
    // source are distinct PDFs, so each opens once; the container bytes and
    // handle are released before the source is hydrated, and the rebuilt
    // document re-serializes to the exact attachment bytes.
    {
        constexpr std::size_t kSourcePaddingBytes = 2U * 1024U * 1024U;
        const std::filesystem::path path = dir / "embedded-source.pdf";
        std::vector<std::uint8_t> embeddedProject;
        std::size_t sourceSize = 0;
        {
            const auto source = std::make_shared<std::vector<std::uint8_t>>(BuildPdf(kSourcePaddingBytes));
            sourceSize = source->size();
            const cnote::Document project = BuildProjectWithSource(source);
            embeddedProject = cnote::ProjectFile::Serialize(project);
            Check(backend.ExportPdf(project, path, 1.0f, 0.55f, 3.0f, 2.0f, embeddedProject),
                "embedded PDF with a stored source exported");
        }
        cnote::PdfiumBackend::ReleaseUnreferencedDocuments();

        const std::size_t readsBefore = cnote::DocumentFileReadCount();
        const std::size_t loadsBefore = backend.DocumentLoadCount();
        const std::size_t closesBefore = backend.DocumentCloseCount();
        const std::size_t cachedBefore = backend.CachedDocumentCount();
        {
            std::string loadError;
            std::optional<cnote::Document> document = cnote::qt::LoadDocument(path, backend, &loadError);
            Check(document.has_value(), "embedded project with a source loads: " + loadError);
            Check(cnote::DocumentFileReadCount() == readsBefore + 1U,
                "an embedded project open reads its file once");
            Check(backend.DocumentLoadCount() == loadsBefore + 2U,
                "the container probe and the distinct stored source each load once");
            Check(backend.DocumentCloseCount() == closesBefore + 1U,
                "the container handle closes before the source is hydrated");
            Check(backend.CachedDocumentCount() == cachedBefore + 1U,
                "only the stored source stays cached");
            if (document.has_value()) {
                Check(document->Kind() == cnote::DocumentKind::EmbeddedProject,
                    "the recovered document stays an embedded project");
                Check(document->PdfSourceCount() == 1U
                        && document->PdfSourceAt(0).pdfData != nullptr
                        && document->PdfSourceAt(0).pdfData->size() == sourceSize,
                    "the stored source survives with its bytes");
                Check(document->PageAt(0).sourcePdfSourceIndex == 0
                        && document->PageAt(0).sourcePdfPageIndex == 0,
                    "the hydrated page keeps its source reference");
                Check(cnote::ProjectFile::Serialize(*document) == embeddedProject,
                    "the embedded project re-serializes to the attached bytes");
            }
        }
        cnote::PdfiumBackend::ReleaseUnreferencedDocuments();
        Check(backend.DocumentCloseCount() == closesBefore + 2U,
            "the stored source handle closes with its document");
    }

    // --- a missing file reads nothing and reports the readable-file error ----
    {
        const std::size_t readsBefore = cnote::DocumentFileReadCount();
        std::string loadError;
        Check(!cnote::qt::LoadDocument(dir / "missing.pdf", backend, &loadError).has_value(),
            "a missing file fails to load");
        Check(loadError.find("not a readable file") != std::string::npos,
            "a missing file reports the readable-file error");
        Check(cnote::DocumentFileReadCount() == readsBefore,
            "a failed open reads no bytes");
    }

    std::filesystem::remove_all(dir, error);
    if (gFailures != 0) {
        std::printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("load path checks passed\n");
    return 0;
}
