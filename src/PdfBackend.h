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

class StubPdfBackend final : public IPdfBackend {
public:
    [[nodiscard]] std::optional<Document> Open(const std::wstring& filePath) override;
};

}  // namespace ctrainer
