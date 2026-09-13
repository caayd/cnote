// cnote_qt: runnable Qt frontend for the migration slice (plan step 3.9).
//
// Startup order matters: QApplication exists before MicroTeX is initialized
// (the Qt adapter uses QFontDatabase/QPainter), the window and its caches are
// destroyed before latex::Shutdown(), and Shutdown runs before the QApplication
// object is destroyed.

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QMessageBox>
#include <QString>

#include <filesystem>
#include <string>

#include "latex/LatexEngine.h"
#include "qt/QtMainWindow.h"

namespace {

std::filesystem::path LatexResourceRoot() {
    const QByteArray overridePath = qgetenv("CNOTE_LATEX_RES_ROOT");
    if (!overridePath.isEmpty()) {
        return std::filesystem::path(std::string(overridePath.constData()));
    }
    return std::filesystem::path(QCoreApplication::applicationDirPath().toStdString()) / "latex-res";
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    // No organization name: Windows settings keep the %APPDATA%/cnote location
    // (plan step 6.4) and macOS/Linux use the bare application name.

    const std::filesystem::path resRoot = LatexResourceRoot();
    if (!cnote::latex::Initialize(resRoot)) {
        // A missing resource tree is a visible startup error, never silently
        // disabled equations (plan step 3.8).
        QMessageBox::critical(
            nullptr,
            QStringLiteral("cnote"),
            QStringLiteral("MicroTeX resources not found at:\n%1")
                .arg(QString::fromStdString(resRoot.string())));
        return 2;
    }

    int exitCode = 0;
    {
        cnote::qt::QtMainWindow window;
        window.resize(1400, 960);
        window.show();

        if (argc > 1) {
            if (!window.OpenPath(std::filesystem::path(argv[1]))) {
                exitCode = 3;
            }
        }
        if (exitCode == 0) {
            exitCode = app.exec();
        }
    }

    cnote::latex::Shutdown();
    return exitCode;
}
