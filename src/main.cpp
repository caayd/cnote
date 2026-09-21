// cnote: Qt desktop entry point (plan steps 7.1/7.2).
//
// Ordinary main(int, char**): QApplication, application identity, the startup
// file argument, AppManager-owned service lifetime (MicroTeX/LaTeX) and the
// macOS QFileOpenEvent route through the same open-file path.
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
#include <QEvent>
#include <QFileOpenEvent>
#include <QStringList>
#include <QMessageBox>
#include <QString>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

#include "Unicode.h"
#include "platform/AppResourcePaths.h"
#include "qt/QtDocumentController.h"
#include "qt/QtMainWindow.h"
#include "shell/AppManager.h"

namespace {

#if defined(Q_OS_WIN)
// The product is a GUI-subsystem executable, so Windows gives it no console of
// its own.  Two cases matter:
//   * started from a shell (or the packaging verifier) with stdout/stderr
//     redirected - the inherited handles carry the diagnostics, so they must be
//     left exactly as they are;
//   * started from Explorer or the Start menu - there are no usable handles,
//     and there is nothing to attach to, so the launch stays silent.
// When the handles are missing but the parent has a console (an interactive
// shell launch), attach to it so diagnostics remain visible.  AttachConsole
// never opens a console window, so a double-click cannot produce one.
void AdoptParentConsole() {
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    const auto usable = [](HANDLE handle) {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE
            && ::GetFileType(handle) != FILE_TYPE_UNKNOWN;
    };
    const bool haveOut = usable(out);
    const bool haveErr = usable(err);
    if (haveOut && haveErr) {
        return;
    }
    if (::AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) {
        return;
    }
    if (!haveOut) {
        std::freopen("CONOUT$", "w", stdout);
    }
    if (!haveErr) {
        std::freopen("CONOUT$", "w", stderr);
    }
    std::freopen("CONIN$", "r", stdin);
}
#endif

// Autosave: the manager's initiating view saves its session and only clears the
// dirty flag when the write succeeded (cnote::QtDocumentController::SaveTo does
// both).
// macOS delivers file-open requests (Finder double-click, a file dropped on the
// dock icon) as QFileOpenEvent; they can arrive before the window exists, so the
// path is buffered until the window's open handler is registered.  Other
// platforms never see the event.
class CnoteApplication : public QApplication {
public:
    CnoteApplication(int& argc, char** argv) : QApplication(argc, argv) {}

    void SetOpenHandler(std::function<void(const std::filesystem::path&)> handler) {
        openHandler_ = std::move(handler);
        if (pendingOpen_.has_value()) {
            const std::filesystem::path path = std::move(*pendingOpen_);
            pendingOpen_.reset();
            if (openHandler_) {
                openHandler_(path);
            }
        }
    }

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::FileOpen) {
            auto* openEvent = static_cast<QFileOpenEvent*>(event);
            const std::filesystem::path path =
                cnote::Utf8ToPath(openEvent->file().toUtf8().toStdString());
            if (path.empty()) {
                return true;
            }
            if (openHandler_) {
                openHandler_(path);
            } else {
                pendingOpen_ = path;
            }
            return true;
        }
        return QApplication::event(event);
    }

private:
    std::function<void(const std::filesystem::path&)> openHandler_;
    std::optional<std::filesystem::path> pendingOpen_;
};

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
#if defined(Q_OS_WIN)
    AdoptParentConsole();
#endif
    CnoteApplication app(argc, argv);
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
    cnote::AppManager manager;
    std::string serviceError;
    if (!manager.InitializeServices(resRoot, &serviceError)) {
        // A missing resource tree is a visible startup error, never silently
        // disabled equations (plan step 3.8).
        if (selfCheck) {
            std::fprintf(stderr, "self-check: MicroTeX resources not found at %s\n", cnote::PathToUtf8(resRoot).c_str());
        } else if (qEnvironmentVariableIsSet("CNOTE_HEADLESS")) {
            std::fprintf(stderr, "MODAL cnote: MicroTeX resources not found at:\n%s\n",
                         cnote::PathToUtf8(resRoot).c_str());
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
        manager.SetAutoSaveHandler(&AutoSaveSession);

        // The self-check runs without an event loop or a shown window, so a
        // modal error dialog would block forever (plan 7.3 requires packaging
        // verification to fail with a code instead).
        if (selfCheck) {
            cnote::qt::QtMainWindow::SetHeadlessModals(true);
        }

        cnote::qt::QtMainWindow window(manager);
        window.resize(1400, 960);
        // Finder/dock file-open requests use the same load path as the startup
        // argument (plan step 7.2).
        app.SetOpenHandler([&window](const std::filesystem::path& path) {
            static_cast<void>(window.OpenPath(path));
        });
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
    // The window and its caches are gone before the process-global services
    // shut down, and both happen before the QApplication object is destroyed.
    manager.ShutdownServices();
    return exitCode;
}
