// cnote_qt: runnable Qt frontend for the migration slice (plan steps 3.9, 4-7).
//
// Startup order matters: QApplication exists before MicroTeX is initialized
// (the Qt adapter uses QFontDatabase/QPainter), the window and its caches are
// destroyed before latex::Shutdown(), and Shutdown runs before the QApplication
// object is destroyed.
//
// `--self-check[=<fixture>]` runs a non-interactive resource/load verification
// for packaging without entering the event loop or showing dialogs.

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>
#include <QMessageBox>
#include <QString>

#include <cstdio>
#include <filesystem>
#include <string>

#include "Unicode.h"
#include "latex/LatexEngine.h"
#include "platform/AppResourcePaths.h"
#include "qt/QtDocumentController.h"
#include "qt/QtMainWindow.h"
#include "shell/AppManager.h"

namespace {

// Autosave: the manager's initiating view saves its session and only clears the
// dirty flag when the write succeeded (cnote::QtDocumentController::SaveTo does
// both).
void AutoSaveSession(const std::shared_ptr<cnote::ProjectSession>& session, QObject* owner) {
    if (session == nullptr || session->document.FilePath().empty()) {
        return;
    }
    auto* controller = qobject_cast<cnote::qt::QtDocumentController*>(owner);
    if (controller == nullptr) {
        return;
    }
    std::string error;
    static_cast<void>(controller->SaveTo(session->document.FilePath(), &error));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    // No organization name: Windows settings keep the %APPDATA%/cnote location
    // (plan step 6.4) and macOS/Linux use the bare application name.
    cnote::platform::ApplyApplicationIcon();

    bool selfCheck = false;
    std::filesystem::path selfCheckFixture;
    std::filesystem::path startupFile;
    // Qt's argument list is the wide command line; argv is code-page encoded and
    // std::filesystem::path(narrow) decodes as UTF-8 on MinGW, which throws for
    // non-ASCII paths (a double-clicked project with an accented name).
    const QStringList argumentList = QCoreApplication::arguments();
    for (int index = 1; index < argumentList.size(); ++index) {
        const std::string argument = argumentList.at(index).toUtf8().toStdString();
        constexpr const char* kSelfCheck = "--self-check=";
        if (argument == "--self-check") {
            selfCheck = true;
        } else if (argument.rfind(kSelfCheck, 0) == 0) {
            selfCheck = true;
            selfCheckFixture = cnote::Utf8ToPath(argument.substr(std::char_traits<char>::length(kSelfCheck)));
        } else if (argument.rfind("--", 0) != 0 && startupFile.empty()) {
            startupFile = cnote::Utf8ToPath(argument);
        }
    }
    if (selfCheck && selfCheckFixture.empty()) {
        selfCheckFixture = startupFile;
    }

    const std::filesystem::path resRoot = cnote::platform::LatexResourceDir();
    if (!cnote::latex::Initialize(resRoot)) {
        // A missing resource tree is a visible startup error, never silently
        // disabled equations (plan step 3.8).
        if (selfCheck) {
            std::fprintf(stderr, "self-check: MicroTeX resources not found at %s\n", cnote::PathToUtf8(resRoot).c_str());
        } else {
            QMessageBox::critical(
                nullptr,
                QStringLiteral("cnote"),
                QStringLiteral("MicroTeX resources not found at:\n%1")
                    .arg(QString::fromStdString(resRoot.string())));
        }
        return 2;
    }

    int exitCode = 0;
    {
        cnote::AppManager manager;
        manager.SetAutoSaveHandler(&AutoSaveSession);

        cnote::qt::QtMainWindow window(manager);
        window.resize(1400, 960);
        if (!selfCheck) {
            window.show();
        }

        const std::filesystem::path& fileToOpen = selfCheck ? selfCheckFixture : startupFile;
        if (!fileToOpen.empty()) {
            if (!window.OpenPath(fileToOpen)) {
                if (selfCheck) {
                    std::fprintf(stderr, "self-check: cannot open %s\n", cnote::PathToUtf8(fileToOpen).c_str());
                }
                exitCode = 3;
            }
        }

        if (!selfCheck && exitCode == 0) {
            exitCode = app.exec();
        }
    }

    cnote::latex::Shutdown();
    return exitCode;
}
