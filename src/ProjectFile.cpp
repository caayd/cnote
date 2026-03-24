#include "ProjectFile.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

namespace cnote {

namespace {

constexpr char kCnoteMagic[] = { 'C', 'N', 'O', 'T', 'E', '1', '\0', '\0' };
constexpr char kLegacyProjectMagic[] = { 'C', 'T', 'R', 'P', 'R', 'J', '1', '\0' };
constexpr std::uint32_t kProjectVersion = 1;
constexpr std::uint32_t kProjectFlagHasPdf = 1U << 0U;

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), size);
    return wide;
}

std::optional<std::vector<std::uint8_t>> ReadAllBytes(const std::wstring& filePath) {
    std::ifstream input(std::filesystem::path(filePath), std::ios::binary);
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

bool WriteAllBytes(const std::wstring& filePath, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(std::filesystem::path(filePath), std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    return static_cast<bool>(output);
}

struct ByteReader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t offset = 0;

    [[nodiscard]] bool ReadExact(void* destination, std::size_t size) {
        if (offset + size > bytes.size()) {
            return false;
        }
        std::memcpy(destination, bytes.data() + offset, size);
        offset += size;
        return true;
    }

    template <typename T>
    [[nodiscard]] bool Read(T& value) {
        return ReadExact(&value, sizeof(value));
    }

    [[nodiscard]] bool ReadString(std::wstring& value) {
        std::uint32_t size = 0;
        if (!Read(size)) {
            return false;
        }
        if (offset + size > bytes.size()) {
            return false;
        }
        const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
        std::string utf8(begin, begin + size);
        value = Utf8ToWide(utf8);
        offset += size;
        return true;
    }

    [[nodiscard]] bool ReadBlob(std::vector<std::uint8_t>& value) {
        std::uint64_t size = 0;
        if (!Read(size)) {
            return false;
        }
        if (size > bytes.size() || offset + static_cast<std::size_t>(size) > bytes.size()) {
            return false;
        }
        value.resize(static_cast<std::size_t>(size));
        if (!value.empty()) {
            std::memcpy(value.data(), bytes.data() + offset, value.size());
        }
        offset += value.size();
        return true;
    }
};

void AppendBytes(std::vector<std::uint8_t>& output, const void* data, std::size_t size) {
    const auto* begin = static_cast<const std::uint8_t*>(data);
    output.insert(output.end(), begin, begin + size);
}

template <typename T>
void AppendValue(std::vector<std::uint8_t>& output, const T& value) {
    AppendBytes(output, &value, sizeof(value));
}

void AppendString(std::vector<std::uint8_t>& output, const std::wstring& value) {
    const std::string utf8 = WideToUtf8(value);
    const auto size = static_cast<std::uint32_t>(utf8.size());
    AppendValue(output, size);
    if (!utf8.empty()) {
        AppendBytes(output, utf8.data(), utf8.size());
    }
}

void AppendBlob(std::vector<std::uint8_t>& output, const std::vector<std::uint8_t>& value) {
    const auto size = static_cast<std::uint64_t>(value.size());
    AppendValue(output, size);
    if (!value.empty()) {
        AppendBytes(output, value.data(), value.size());
    }
}

void AppendStroke(std::vector<std::uint8_t>& output, const Stroke& stroke) {
    AppendValue(output, stroke.style.color);
    AppendValue(output, stroke.style.width);
    const auto pointCount = static_cast<std::uint32_t>(stroke.points.size());
    AppendValue(output, pointCount);
    for (const Vec2& point : stroke.points) {
        AppendValue(output, point.x);
        AppendValue(output, point.y);
    }
}

bool ReadStroke(ByteReader& reader, Stroke& stroke) {
    std::uint32_t pointCount = 0;
    if (!reader.Read(stroke.style.color) || !reader.Read(stroke.style.width) || !reader.Read(pointCount)) {
        return false;
    }

    stroke.points.clear();
    stroke.points.reserve(pointCount);
    for (std::uint32_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
        Vec2 point {};
        if (!reader.Read(point.x) || !reader.Read(point.y)) {
            return false;
        }
        stroke.points.push_back(point);
    }
    return true;
}

void AppendImage(std::vector<std::uint8_t>& output, const PageImage& image) {
    AppendValue(output, image.x);
    AppendValue(output, image.y);
    AppendValue(output, image.width);
    AppendValue(output, image.height);
    AppendValue(output, image.rotationDegrees);
    AppendValue(output, image.pixelWidth);
    AppendValue(output, image.pixelHeight);
    AppendValue(output, image.stride);
    AppendBlob(output, image.pixels);
}

bool ReadImage(ByteReader& reader, PageImage& image) {
    if (!reader.Read(image.x) || !reader.Read(image.y) || !reader.Read(image.width) || !reader.Read(image.height)
        || !reader.Read(image.rotationDegrees) || !reader.Read(image.pixelWidth)
        || !reader.Read(image.pixelHeight) || !reader.Read(image.stride)) {
        return false;
    }

    return reader.ReadBlob(image.pixels);
}

void OverlayPageContent(Page& destination, const Page& source) {
    destination.strokes = source.strokes;
    destination.images = source.images;
}

}  // namespace

std::optional<Document> ProjectFile::Load(const std::wstring& filePath, PdfiumBackend& pdfBackend) {
    const auto bytes = ReadAllBytes(filePath);
    if (!bytes.has_value()) {
        return std::nullopt;
    }

    ByteReader reader { *bytes };
    char magic[sizeof(kCnoteMagic)] {};
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t currentPageIndex = 0;
    std::uint32_t pageCount = 0;
    std::wstring sourcePath;
    std::vector<std::uint8_t> pdfBytes;

    const bool validMagic = reader.ReadExact(magic, sizeof(magic))
        && (std::memcmp(magic, kCnoteMagic, sizeof(kCnoteMagic)) == 0
            || std::memcmp(magic, kLegacyProjectMagic, sizeof(kLegacyProjectMagic)) == 0);
    if (!validMagic
        || !reader.Read(version) || version != kProjectVersion || !reader.Read(flags)
        || !reader.Read(currentPageIndex) || !reader.Read(pageCount) || !reader.ReadString(sourcePath)
        || !reader.ReadBlob(pdfBytes)) {
        return std::nullopt;
    }

    std::vector<Page> savedPages;
    savedPages.reserve(pageCount);
    for (std::uint32_t pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        Page page {};
        std::uint32_t strokeCount = 0;
        std::uint32_t imageCount = 0;
        if (!reader.Read(page.width) || !reader.Read(page.height) || !reader.Read(strokeCount) || !reader.Read(imageCount)) {
            return std::nullopt;
        }

        page.strokes.reserve(strokeCount);
        for (std::uint32_t strokeIndex = 0; strokeIndex < strokeCount; ++strokeIndex) {
            Stroke stroke {};
            if (!ReadStroke(reader, stroke)) {
                return std::nullopt;
            }
            page.strokes.push_back(std::move(stroke));
        }

        page.images.reserve(imageCount);
        for (std::uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
            PageImage image {};
            if (!ReadImage(reader, image)) {
                return std::nullopt;
            }
            page.images.push_back(std::move(image));
        }

        savedPages.push_back(std::move(page));
    }

    if ((flags & kProjectFlagHasPdf) != 0U) {
        auto sharedPdfBytes = std::make_shared<std::vector<std::uint8_t>>(std::move(pdfBytes));
        auto opened = pdfBackend.OpenFromMemory(sourcePath, sharedPdfBytes);
        if (!opened.has_value()) {
            return std::nullopt;
        }
        if (opened->PageCount() != savedPages.size()) {
            return std::nullopt;
        }

        for (std::size_t pageIndex = 0; pageIndex < savedPages.size(); ++pageIndex) {
            OverlayPageContent(opened->PageAt(pageIndex), savedPages[pageIndex]);
        }
        opened->SetFilePath(filePath);
        static_cast<void>(opened->SetCurrentPageIndex(currentPageIndex));
        return opened;
    }

    return Document::CreateEditable(filePath, std::move(savedPages), currentPageIndex);
}

bool ProjectFile::Save(const Document& document, const std::wstring& filePath) {
    std::vector<std::uint8_t> bytes;
    const std::uint32_t flags = document.HasPdfData() ? kProjectFlagHasPdf : 0U;
    const std::uint32_t currentPageIndex = static_cast<std::uint32_t>(document.CurrentPageIndex());
    const std::uint32_t pageCount = static_cast<std::uint32_t>(document.PageCount());
    const std::vector<std::uint8_t> emptyPdfBytes;
    const auto& pdfBytes = document.HasPdfData() ? *document.PdfData() : emptyPdfBytes;

    AppendBytes(bytes, kCnoteMagic, sizeof(kCnoteMagic));
    AppendValue(bytes, kProjectVersion);
    AppendValue(bytes, flags);
    AppendValue(bytes, currentPageIndex);
    AppendValue(bytes, pageCount);
    AppendString(bytes, document.SourcePath());
    AppendBlob(bytes, pdfBytes);

    for (std::size_t pageIndex = 0; pageIndex < document.PageCount(); ++pageIndex) {
        const Page& page = document.PageAt(pageIndex);
        const auto strokeCount = static_cast<std::uint32_t>(page.strokes.size());
        const auto imageCount = static_cast<std::uint32_t>(page.images.size());
        AppendValue(bytes, page.width);
        AppendValue(bytes, page.height);
        AppendValue(bytes, strokeCount);
        AppendValue(bytes, imageCount);
        for (const Stroke& stroke : page.strokes) {
            AppendStroke(bytes, stroke);
        }
        for (const PageImage& image : page.images) {
            AppendImage(bytes, image);
        }
    }

    return WriteAllBytes(filePath, bytes);
}

}  // namespace cnote
