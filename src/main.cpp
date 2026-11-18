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
//
// `--capture=<png>` and `--script=<file>` run the app headless for agents and
// tests (shell/CaptureDriver.h): no window ever appears, input comes from the
// script, and the window is saved as PNG.  `--size=WxH` (default 1400x960) and
// `--scale=S` (device pixel ratio, default 1) set the capture's pixel size.
// The run uses a scratch settings directory and a copy of the startup file,
// so it never changes the user's settings or documents.

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFileOpenEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QStringList>
#include <QMessageBox>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

#include "Unicode.h"
#include "platform/AppResourcePaths.h"
#include "qt/QtDocumentController.h"
#include "qt/QtMainWindow.h"
#include "shell/AppManager.h"
#include "shell/CaptureDriver.h"

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

// --capture/--script settings; see the file comment.
struct CaptureOptions {
    std::filesystem::path output;
    std::filesystem::path script;
    QSize size { 1400, 960 };
    double scale = 1.0;
    bool sizeOrScaleGiven = false;
};

// Runs before QApplication exists, because the flag decides the platform.
bool RequestsCapture(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument.rfind("--capture=", 0) == 0 || argument.rfind("--script=", 0) == 0) {
            return true;
        }
    }
    return false;
}

bool ParseSize(const QString& text, QSize* size) {
    const QStringList parts = text.split(QLatin1Char('x'));
    bool widthOk = false;
    bool heightOk = false;
    if (parts.size() != 2) {
        return false;
    }
    const int width = parts.at(0).toInt(&widthOk);
    const int height = parts.at(1).toInt(&heightOk);
    if (!widthOk || !heightOk || width <= 0 || height <= 0) {
        return false;
    }
    *size = QSize(width, height);
    return true;
}

// Builds the step list: the script's steps, then the --capture output.
bool LoadCaptureSteps(const CaptureOptions& options, std::vector<cnote::shell::CaptureStep>* steps) {
    if (!options.script.empty()) {
        std::ifstream file(options.script, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "capture: cannot read script %s\n", cnote::PathToUtf8(options.script).c_str());
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string error;
        if (!cnote::shell::ParseCaptureScript(text, steps, &error)) {
            std::fprintf(stderr, "capture: %s: %s\n", cnote::PathToUtf8(options.script).c_str(), error.c_str());
            return false;
        }
    }
    if (!options.output.empty()) {
        cnote::shell::CaptureStep step;
        step.kind = cnote::shell::CaptureStepKind::Capture;
        step.path = options.output;
        steps->push_back(std::move(step));
    }
    for (const cnote::shell::CaptureStep& step : *steps) {
        if (step.kind == cnote::shell::CaptureStepKind::Capture) {
            return true;
        }
    }
    std::fprintf(stderr, "capture: nothing to capture: pass --capture=<png> or add a 'capture PATH' step\n");
    return false;
}

// Points the settings store at the scratch directory before any window loads
// it, so a capture run neither reads nor writes the user's settings.
void RedirectSettings(const std::filesystem::path& scratch) {
    const std::string value = cnote::PathToUtf8(scratch / "settings");
#if defined(Q_OS_WIN)
    qputenv("APPDATA", QByteArray::fromStdString(value));
#else
    qputenv("XDG_CONFIG_HOME", QByteArray::fromStdString(value));
#endif
}

}  // namespace

int main(int argc, char** argv) {
#if defined(Q_OS_WIN)
    AdoptParentConsole();
#endif
    const bool capture = RequestsCapture(argc, argv);
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    // Linux has its real fonts on the offscreen platform, and an agent's
    // machine may have no display at all.
    if (capture && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#endif
    CnoteApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    // No organization name: Windows settings keep the %APPDATA%/cnote location
    // (plan step 6.4) and macOS/Linux use the bare application name.
    cnote::platform::ApplyApplicationIcon();

    bool selfCheck = false;
    std::filesystem::path selfCheckFixture;
    std::filesystem::path startupFile;
    CaptureOptions captureOptions;
    // Qt's argument list is the wide command line; argv is code-page encoded and
    // std::filesystem::path(narrow) decodes as UTF-8 on MinGW, which throws for
    // non-ASCII paths (a double-clicked project with an accented name).
    const QStringList argumentList = QCoreApplication::arguments();
    for (int index = 1; index < argumentList.size(); ++index) {
        const std::string argument = argumentList.at(index).toUtf8().toStdString();
        constexpr const char* kSelfCheck = "--self-check=";
        const auto value = [&argument](const char* prefix) {
            return argument.substr(std::char_traits<char>::length(prefix));
        };
        if (argument == "--self-check") {
            selfCheck = true;
        } else if (argument.rfind(kSelfCheck, 0) == 0) {
            selfCheck = true;
            selfCheckFixture = cnote::Utf8ToPath(value(kSelfCheck));
        } else if (argument.rfind("--capture=", 0) == 0) {
            captureOptions.output = cnote::Utf8ToPath(value("--capture="));
        } else if (argument.rfind("--script=", 0) == 0) {
            captureOptions.script = cnote::Utf8ToPath(value("--script="));
        } else if (argument.rfind("--size=", 0) == 0) {
            captureOptions.sizeOrScaleGiven = true;
            if (!ParseSize(QString::fromStdString(value("--size=")), &captureOptions.size)) {
                std::fprintf(stderr, "cnote: --size expects WxH, for example --size=1400x960\n");
                return 2;
            }
        } else if (argument.rfind("--scale=", 0) == 0) {
            captureOptions.sizeOrScaleGiven = true;
            bool ok = false;
            captureOptions.scale = QString::fromStdString(value("--scale=")).toDouble(&ok);
            if (!ok || captureOptions.scale <= 0.0) {
                std::fprintf(stderr, "cnote: --scale expects a positive device pixel ratio, for example --scale=1.75\n");
                return 2;
            }
        } else if (argument.rfind("--", 0) != 0 && startupFile.empty()) {
            startupFile = cnote::Utf8ToPath(argument);
        }
    }
    if (selfCheck && selfCheckFixture.empty()) {
        selfCheckFixture = startupFile;
    }
    if (captureOptions.sizeOrScaleGiven && !capture) {
        std::fprintf(stderr, "cnote: --size and --scale only apply with --capture or --script\n");
        return 2;
    }
    if (capture && selfCheck) {
        std::fprintf(stderr, "cnote: --self-check cannot be combined with --capture or --script\n");
        return 2;
    }

    std::vector<cnote::shell::CaptureStep> captureSteps;
    std::optional<QTemporaryDir> captureScratch;
    if (capture) {
        if (!LoadCaptureSteps(captureOptions, &captureSteps)) {
            return 2;
        }
        captureScratch.emplace();
        if (!captureScratch->isValid()) {
            std::fprintf(stderr, "capture: cannot create a scratch directory\n");
            return 2;
        }
        const std::filesystem::path scratch = cnote::Utf8ToPath(captureScratch->path().toUtf8().toStdString());
        RedirectSettings(scratch);
        // The run opens a copy: a script may edit and save, and the user's
        // document must stay untouched.
        if (!startupFile.empty()) {
            std::error_code error;
            const std::filesystem::path copy = scratch / startupFile.filename();
            std::filesystem::copy_file(startupFile, copy, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                std::fprintf(stderr, "capture: cannot open %s: %s\n", cnote::PathToUtf8(startupFile).c_str(),
                    error.message().c_str());
                return 3;
            }
            startupFile = copy;
        }
        cnote::qt::QtMainWindow::SetHeadlessModals(true);
        cnote::shell::SetScreenScale(captureOptions.scale);
        // Closing the main window from a script must not end the run early.
        app.setQuitOnLastWindowClosed(false);
    }

    const std::filesystem::path resRoot = cnote::platform::LatexResourceDir();
    cnote::AppManager manager;
    std::string serviceError;
    if (!manager.InitializeServices(resRoot, &serviceError)) {
        // A missing resource tree is a visible startup error, never silently
        // disabled equations (plan step 3.8).
        if (selfCheck) {
            std::fprintf(stderr, "self-check: MicroTeX resources not found at %s\n", cnote::PathToUtf8(resRoot).c_str());
        } else if (capture || qEnvironmentVariableIsSet("CNOTE_HEADLESS")) {
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

        cnote::shell::OffscreenWindows offscreen;
        if (capture) {
            app.installEventFilter(&offscreen);
        }

        cnote::qt::QtMainWindow window(manager);
        window.resize(1400, 960);
        // Finder/dock file-open requests use the same load path as the startup
        // argument (plan step 7.2).
        app.SetOpenHandler([&window](const std::filesystem::path& path) {
            static_cast<void>(window.OpenPath(path));
        });
        if (capture) {
            offscreen.SetPrimaryWindow(&window);
            if (const QScreen* screen = QGuiApplication::primaryScreen(); screen != nullptr) {
                window.move(screen->availableGeometry().topLeft());
            }
            window.show();
            // showEvent applies the preferred start size first.
            window.resize(captureOptions.size);
            if (window.size() != captureOptions.size) {
                std::fprintf(stderr, "capture: window is %dx%d, not the requested %dx%d (screen limit)\n",
                    window.width(), window.height(), captureOptions.size.width(), captureOptions.size.height());
            }
        } else if (!selfCheck) {
            window.show();
        }
        const std::filesystem::path& fileToOpen = selfCheck ? selfCheckFixture : startupFile;
        if (!fileToOpen.empty()) {
            if (!window.OpenPath(fileToOpen)) {
                if (selfCheck || capture) {
                    std::fprintf(stderr, "%s: cannot open %s\n", selfCheck ? "self-check" : "capture",
                        cnote::PathToUtf8(fileToOpen).c_str());
                }
                exitCode = 3;
            }
        }

        if (capture && exitCode == 0) {
            cnote::shell::CaptureDriver driver(window, std::move(captureSteps), [](int code) {
                QCoreApplication::exit(code);
            });
            driver.Start();
            exitCode = app.exec();
        } else if (!selfCheck && exitCode == 0) {
            exitCode = app.exec();
        }
    }
    // The window and its caches are gone before the process-global services
    // shut down, and both happen before the QApplication object is destroyed.
    manager.ShutdownServices();
    return exitCode;
}
