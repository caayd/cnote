#pragma once

#include <optional>
#include <string>

#include "Document.h"

namespace ctrainer {

class IPdfBackend {
public:
    virtual ~IPdfBackend() = default;

    [[nodiscard]] virtual std::optional<Document> Open(const std::wstring& filePath) = 0;
};

class PdfiumBackend final : public IPdfBackend {
public:
    PdfiumBackend();
    ~PdfiumBackend() override;

    [[nodiscard]] std::optional<Document> Open(const std::wstring& filePath) override;
    [[nodiscard]] std::optional<RasterImage> RenderPage(const std::wstring& filePath, std::size_t pageIndex, float renderScale) const;

private:
    bool initialized_ = false;
};

}  // namespace ctrainer
