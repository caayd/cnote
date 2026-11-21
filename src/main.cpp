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
// so it never changes the user's settings or documents.  Headless runs
// (capture and --self-check) report crashes and Qt messages on stderr instead
// of the desktop (shell/HeadlessFailures.h).

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
#include <deque>
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
#include "settings/SettingsStoreQt.h"
#include "shell/AppManager.h"
#include "shell/CaptureDriver.h"
#include "shell/HeadlessFailures.h"

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
        while (openHandler_ && !pendingOpen_.empty()) {
            const std::filesystem::path path = std::move(pendingOpen_.front());
            pendingOpen_.pop_front();
            openHandler_(path);
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
                pendingOpen_.push_back(path);
            }
            return true;
        }
        return QApplication::event(event);
    }

private:
    std::function<void(const std::filesystem::path&)> openHandler_;
    // Several file-open requests can arrive before the window's handler exists.
    std::deque<std::filesystem::path> pendingOpen_;
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

enum class StartupMode {
    Interactive,
    SelfCheck,
    Capture,
};

// Before QApplication exists, headless runs route failures to stderr
// (shell/HeadlessFailures.h); the flags are ASCII, so argv serves on every
// platform even where LaunchOptions is parsed from Qt's wide command line.
bool HasArgument(int argc, char** argv, std::string_view prefix) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]).rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

// One parse of the command line, shared by the pre-QApplication platform
// decision and the run itself.  Tokens are UTF-8: argv on Linux, and
// QString::toUtf8 after QApplication so Windows/macOS keep non-ASCII paths
// from Qt's wide command line.
struct LaunchOptions {
    StartupMode mode = StartupMode::Interactive;
    std::filesystem::path openPath;
    std::filesystem::path selfCheckFixture;
    std::filesystem::path captureOutput;
    std::filesystem::path captureScript;
    QSize captureSize { 1400, 960 };
    double captureScale = 1.0;
};

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
// Only the Linux path decides the platform before QApplication exists; there
// argv is UTF-8, so the same parser can read it directly.
std::vector<std::string> RawArguments(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return arguments;
}
#endif

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
// Qt's wide command line preserves paths the code-page encoded argv cannot.
std::vector<std::string> Utf8Arguments(const QStringList& arguments) {
    // Qt's list starts with the executable; the parser takes only the
    // arguments after it, matching argv in RawArguments.
    std::vector<std::string> tokens;
    tokens.reserve(static_cast<std::size_t>(arguments.size()));
    for (int index = 1; index < arguments.size(); ++index) {
        tokens.push_back(arguments.at(index).toUtf8().toStdString());
    }
    return tokens;
}
#endif

bool RequestsCapture(int argc, char** argv) {
    return HasArgument(argc, argv, "--capture=") || HasArgument(argc, argv, "--script=");
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

// Fills *options from the command line.  On failure returns false and sets
// *error to a message without the "cnote: " prefix.  Unknown "--" options are
// errors instead of being silently ignored.
bool ParseLaunchOptions(const std::vector<std::string>& arguments, LaunchOptions* options, std::string* error) {
    bool selfCheck = false;
    bool capture = false;
    bool sizeOrScaleGiven = false;
    for (const std::string& argument : arguments) {
        const auto value = [&argument](std::string_view prefix) {
            return argument.substr(prefix.size());
        };
        if (argument == "--self-check") {
            selfCheck = true;
        } else if (argument.rfind("--self-check=", 0) == 0) {
            selfCheck = true;
            options->selfCheckFixture = cnote::Utf8ToPath(value("--self-check="));
        } else if (argument.rfind("--capture=", 0) == 0) {
            capture = true;
            options->captureOutput = cnote::Utf8ToPath(value("--capture="));
        } else if (argument.rfind("--script=", 0) == 0) {
            capture = true;
            options->captureScript = cnote::Utf8ToPath(value("--script="));
        } else if (argument.rfind("--size=", 0) == 0) {
            sizeOrScaleGiven = true;
            if (!ParseSize(QString::fromStdString(value("--size=")), &options->captureSize)) {
                *error = "--size expects WxH, for example --size=1400x960";
                return false;
            }
        } else if (argument.rfind("--scale=", 0) == 0) {
            sizeOrScaleGiven = true;
            bool ok = false;
            options->captureScale = QString::fromStdString(value("--scale=")).toDouble(&ok);
            if (!ok || options->captureScale <= 0.0) {
                *error = "--scale expects a positive device pixel ratio, for example --scale=1.75";
                return false;
            }
        } else if (argument.rfind("--", 0) == 0) {
            *error = "unknown option " + argument;
            return false;
        } else if (options->openPath.empty()) {
            options->openPath = cnote::Utf8ToPath(argument);
        }
    }
    if (selfCheck && capture) {
        *error = "--self-check cannot be combined with --capture or --script";
        return false;
    }
    if (sizeOrScaleGiven && !capture) {
        *error = "--size and --scale only apply with --capture or --script";
        return false;
    }
    if (selfCheck && options->selfCheckFixture.empty()) {
        options->selfCheckFixture = options->openPath;
    }
    options->mode = capture ? StartupMode::Capture : (selfCheck ? StartupMode::SelfCheck : StartupMode::Interactive);
    return true;
}

// Builds the step list: the script's steps, then the --capture output.
bool LoadCaptureSteps(const LaunchOptions& options, std::vector<cnote::shell::CaptureStep>* steps) {
    if (!options.captureScript.empty()) {
        std::ifstream file(options.captureScript, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "capture: cannot read script %s\n", cnote::PathToUtf8(options.captureScript).c_str());
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string error;
        if (!cnote::shell::ParseCaptureScript(text, steps, &error)) {
            std::fprintf(stderr, "capture: %s: %s\n", cnote::PathToUtf8(options.captureScript).c_str(), error.c_str());
            return false;
        }
    }
    if (!options.captureOutput.empty()) {
        cnote::shell::CaptureStep step;
        step.kind = cnote::shell::CaptureStepKind::Capture;
        step.path = options.captureOutput;
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

}  // namespace

int main(int argc, char** argv) {
#if defined(Q_OS_WIN)
    AdoptParentConsole();
#endif
    if (const bool capture = RequestsCapture(argc, argv); capture || HasArgument(argc, argv, "--self-check")) {
        cnote::shell::RouteHeadlessFailuresToStderr(capture ? "capture" : "self-check");
    }
    // The command line is parsed once.  Linux must choose the platform before
    // QApplication exists, and argv is UTF-8 there, so that parse also feeds
    // the run.  Windows and macOS parse Qt's wide command line after
    // QApplication, because argv is code-page encoded and non-ASCII paths (a
    // double-clicked project with an accented name) would be lost.
    LaunchOptions launch;
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    // Linux has its real fonts on the offscreen platform, and an agent's
    // machine may have no display at all.
    std::string platformError;
    if (!ParseLaunchOptions(RawArguments(argc, argv), &launch, &platformError)) {
        std::fprintf(stderr, "cnote: %s\n", platformError.c_str());
        return 2;
    }
    if (launch.mode == StartupMode::Capture && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#endif
    CnoteApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    // No organization name: Windows settings keep the %APPDATA%/cnote location
    // (plan step 6.4) and macOS/Linux use the bare application name.
    cnote::platform::ApplyApplicationIcon();

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    std::string launchError;
    if (!ParseLaunchOptions(Utf8Arguments(QCoreApplication::arguments()), &launch, &launchError)) {
        std::fprintf(stderr, "cnote: %s\n", launchError.c_str());
        return 2;
    }
#endif
    const bool capture = launch.mode == StartupMode::Capture;
    const bool selfCheck = launch.mode == StartupMode::SelfCheck;

    std::vector<cnote::shell::CaptureStep> captureSteps;
    std::optional<QTemporaryDir> captureScratch;
    if (capture) {
        if (!LoadCaptureSteps(launch, &captureSteps)) {
            return 2;
        }
        captureScratch.emplace();
        if (!captureScratch->isValid()) {
            std::fprintf(stderr, "capture: cannot create a scratch directory\n");
            return 2;
        }
        const std::filesystem::path scratch = cnote::Utf8ToPath(captureScratch->path().toUtf8().toStdString());
        // Scratch settings before any window loads them: a capture run neither
        // reads nor writes the user's settings (macOS ignores XDG, so the
        // store is redirected directly rather than through the environment).
        cnote::settings::SettingsStoreQt::OverrideSettingsPath(scratch / "settings.ini");
        // The run opens a copy: a script may edit and save, and the user's
        // document must stay untouched.
        if (!launch.openPath.empty()) {
            std::error_code error;
            const std::filesystem::path copy = scratch / launch.openPath.filename();
            std::filesystem::copy_file(launch.openPath, copy, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                std::fprintf(stderr, "capture: cannot open %s: %s\n", cnote::PathToUtf8(launch.openPath).c_str(),
                    error.message().c_str());
                return 3;
            }
            // The window opens the same parsed path the run copied.
            launch.openPath = copy;
        }
        cnote::qt::QtMainWindow::SetHeadlessModals(true);
        cnote::shell::SetScreenScale(launch.captureScale);
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
            window.resize(launch.captureSize);
            if (window.size() != launch.captureSize) {
                std::fprintf(stderr, "capture: window is %dx%d, not the requested %dx%d (screen limit)\n",
                    window.width(), window.height(), launch.captureSize.width(), launch.captureSize.height());
            }
        } else if (!selfCheck) {
            window.show();
        }
        const std::filesystem::path& fileToOpen = selfCheck ? launch.selfCheckFixture : launch.openPath;
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
