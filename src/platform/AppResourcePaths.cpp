#include "platform/AppResourcePaths.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QString>

namespace {

std::filesystem::path PathFromQString(const QString& value) {
#if defined(_WIN32)
    // std::filesystem::path on Windows is wide-native; going through UTF-8
    // would reinterpret the bytes with the ANSI code page and corrupt
    // non-ASCII install paths.
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toStdString());
#endif
}

}  // namespace

// The Qt-generated resource initializer is a global symbol, and Q_INIT_RESOURCE
// declares it with block scope.  This function must therefore stay at global
// namespace scope: inside an anonymous namespace the declaration would name
// (anonymous namespace)::qInitResources_cnote_app_icons and fail to link.
static void InitAppIconResources() {
    Q_INIT_RESOURCE(cnote_app_icons);
}

namespace cnote::platform {

std::filesystem::path ApplicationDir() {
    return PathFromQString(QCoreApplication::applicationDirPath());
}

std::filesystem::path AppResourceDir() {
#if defined(Q_OS_MACOS)
    const std::filesystem::path executableDir = ApplicationDir();
    std::error_code error;
    if (std::filesystem::exists(executableDir / "latex-res", error)) {
        // Build tree or a non-bundle layout: resources sit beside the binary.
        return executableDir;
    }
    return executableDir.parent_path() / "Resources";
#else
    return ApplicationDir();
#endif
}

std::filesystem::path LatexResourceDir() {
    const QString overridePath = qEnvironmentVariable("CNOTE_LATEX_RES_ROOT");
    if (!overridePath.isEmpty()) {
        return PathFromQString(overridePath);
    }
    return AppResourceDir() / "latex-res";
}

void ApplyApplicationIcon() {
    InitAppIconResources();
    const QIcon icon(QStringLiteral(":/cnote/cnote.ico"));
    if (!icon.isNull()) {
        QGuiApplication::setWindowIcon(icon);
    }
}

}  // namespace cnote::platform
