#include "Unicode.h"

#include <QByteArray>
#include <QString>

namespace cnote {

std::string Utf16ToUtf8(std::u16string_view value) {
    if (value.empty()) {
        return {};
    }

    const QString text = QString::fromUtf16(value.data(), static_cast<qsizetype>(value.size()));
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

std::u16string Utf8ToUtf16(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const QString text = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    return text.toStdU16String();
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path Utf8ToPath(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

}  // namespace cnote
