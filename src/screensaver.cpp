#include "screensaver.h"
#include "window.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QScreen>
#include <QVBoxLayout>
#include <QWindow>
#include <limits>
#define NOMINMAX
#include <windows.h>

namespace kaleido {
SaverArguments parseSaverArguments(const QStringList& input) {
    SaverArguments result;
    QStringList launch;
    for (int i = 0; i < input.size(); ++i) {
        const auto arg = input[i];
        if (arg == "--test-windowed") {
            result.testWindowed = true;
        } else if (arg == "--data-dir" || arg == "--test-report" || arg == "--test-seconds" ||
                   arg == "--test-mirrors") {
            if (++i >= input.size()) {
                result.mode = SaverMode::Invalid;
                return result;
            }
            if (arg == "--data-dir") result.dataDir = input[i];
            else if (arg == "--test-report") result.report = input[i];
            else {
                bool ok = false;
                const int n = input[i].toInt(&ok);
                if (!ok || n < 0 || n > (arg == "--test-seconds" ? 300 : 8)) {
                    result.mode = SaverMode::Invalid;
                    return result;
                }
                if (arg == "--test-seconds") result.testSeconds = n;
                else result.testMirrors = n;
            }
        } else launch << arg;
    }
    if (launch.isEmpty()) return result;
    const auto command = launch.takeFirst().toLower();
    if (command.size() < 2 || (command[0] != '/' && command[0] != '-')) {
        result.mode = SaverMode::Invalid;
        return result;
    }
    result.mode = command[1] == 's' ? SaverMode::Run : command[1] == 'c' ? SaverMode::Configure :
                  command[1] == 'p' ? SaverMode::Preview : SaverMode::Invalid;
    if (command.size() > 2) {
        if (command[2] != ':' || command.size() == 3) result.mode = SaverMode::Invalid;
        else launch.prepend(command.mid(3));
    }
    if (!launch.isEmpty()) {
        bool ok = false;
        const auto value = launch.first().toULongLong(&ok, 10);
        if (!ok || value > std::numeric_limits<quintptr>::max() || launch.size() != 1 ||
            result.mode == SaverMode::Run)
            result.mode = SaverMode::Invalid;
        else result.parent = quintptr(value);
    }
    if (result.mode == SaverMode::Preview && !result.parent) result.mode = SaverMode::Invalid;
    return result;
}
CanvasOptions saverCanvasOptions(bool preview, bool mute) {
    return {preview ? QString() : QString("screensaverShuffle"), false, true, preview || mute};
}

SaverMirror::SaverMirror(Canvas* canvas, QWidget* parent) : QOpenGLWidget(parent), source(canvas) {
    source->enableMirrors();
    connect(source, &Canvas::frameReady, this, qOverload<>(&QWidget::update));
}
SaverMirror::~SaverMirror() {
    if (initialized) {
        makeCurrent();
        compositor.release();
        doneCurrent();
    }
}
void SaverMirror::initializeGL() {
    initialized = initializeOpenGLFunctions() && compositor.initialize();
}
void SaverMirror::paintGL() {
    if (!initialized || !source) return;
    const QSize pixels(qRound(width() * devicePixelRatioF()), qRound(height() * devicePixelRatioF()));
    if (source->renderedFence()) glWaitSync(source->renderedFence(), 0, GL_TIMEOUT_IGNORED);
    source->renderMirror(compositor, defaultFramebufferObject(), pixels);
    const auto fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    source->mirrorReadFinished(fence);
}

class SaverInput : public QObject {
  public:
    SaverInput() { elapsed.start(); origin = QCursor::pos(); }
    bool moved() {
        if (elapsed.elapsed() < 1000) { origin = QCursor::pos(); return false; }
        return (QCursor::pos() - origin).manhattanLength() > 6;
    }
  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        const auto type = event->type();
        if (type == QEvent::KeyPress || type == QEvent::MouseButtonPress || type == QEvent::Wheel ||
            type == QEvent::TouchBegin || (type == QEvent::MouseMove && moved()) ||
            (type == QEvent::ApplicationDeactivate && elapsed.elapsed() > 1000)) {
            qApp->quit();
            return true;
        }
        return false;
    }
  private:
    QElapsedTimer elapsed;
    QPoint origin;
};

int runScreensaver(const SaverArguments& args) {
    QDir().mkpath(args.dataDir);
    const HWND parent = reinterpret_cast<HWND>(args.parent);
    if (args.parent && !IsWindow(parent)) return 2;
    if (args.mode == SaverMode::Configure) {
        Window configuration(args.dataDir, true);
        configuration.winId();
        bool ownerWasEnabled = false;
        if (parent) {
            SetWindowLongPtrW(reinterpret_cast<HWND>(configuration.winId()), GWLP_HWNDPARENT,
                              reinterpret_cast<LONG_PTR>(parent));
            ownerWasEnabled = IsWindowEnabled(parent);
            if (ownerWasEnabled) EnableWindow(parent, FALSE);
        }
        configuration.show();
        QTimer ownerWatch;
        if (parent) {
            QObject::connect(&ownerWatch, &QTimer::timeout, &configuration, [&] {
                if (!IsWindow(parent)) configuration.close();
            });
            ownerWatch.start(250);
        }
        if (args.testSeconds) QTimer::singleShot(args.testSeconds * 1000, &configuration, &QWidget::close);
        const int code = qApp->exec();
        if (!args.report.isEmpty()) configuration.grab().save(args.report + ".png");
        if (parent && IsWindow(parent) && ownerWasEnabled) {
            EnableWindow(parent, TRUE);
            SetForegroundWindow(parent);
        }
        return code;
    }
    const bool preview = args.mode == SaverMode::Preview;
    QLockFile sessionLock(args.dataDir + "/screensaver-session.lock");
    if (!preview && !sessionLock.tryLock(0)) return 0;
    Library library(args.dataDir + "/library.sqlite");
    if (!library.error().isEmpty()) return 1;
    QWidget master;
    master.setWindowTitle("Kaleidowall Screensaver");
    master.setStyleSheet("background:black;");
    master.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    if (preview) master.setAttribute(Qt::WA_ShowWithoutActivating);
    const bool mute = library.value("screensaverPreferences").value("muted").toBool(true);
    auto* canvas = new Canvas(&library, &master, saverCanvasOptions(preview, mute));
    canvas->setMinimumSize(1, 1);
    auto* layout = new QVBoxLayout(&master);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(canvas);
    std::vector<std::unique_ptr<QWidget>> mirrors;
    QTimer hostWatch;
    if (preview) {
        const HWND child = reinterpret_cast<HWND>(master.winId());
        auto style = GetWindowLongPtrW(child, GWL_STYLE);
        SetWindowLongPtrW(child, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD);
        SetLastError(0);
        if (!SetParent(child, parent) && GetLastError()) return 2;
        auto resizePreview = [&, child] {
            if (!IsWindow(parent)) { qApp->quit(); return; }
            RECT r;
            if (GetClientRect(parent, &r)) {
                const int w = std::max(1L, r.right), h = std::max(1L, r.bottom);
                // Keep Qt's logical widget size and the native child bounds in sync across DPI settings.
                master.resize(qRound(w / master.devicePixelRatioF()), qRound(h / master.devicePixelRatioF()));
                SetWindowPos(child, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        };
        master.show();
        resizePreview();
        QObject::connect(&hostWatch, &QTimer::timeout, &master, resizePreview);
        hostWatch.start(100);
    } else {
        auto screens = QGuiApplication::screens();
        auto* primary = QGuiApplication::primaryScreen();
        if (!primary) return 1;
        master.winId();
        master.windowHandle()->setScreen(primary);
        if (args.testWindowed) {
            master.setGeometry(40, 40, 640, 360);
            master.show();
        } else {
            master.setGeometry(primary->geometry());
            master.showFullScreen();
        }
        auto addMirror = [&](QScreen* screen, int index) {
            auto host = std::make_unique<QWidget>();
            host->setWindowTitle("Kaleidowall Screensaver Mirror");
            host->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
            host->setAttribute(Qt::WA_ShowWithoutActivating);
            host->setStyleSheet("background:black;");
            auto* mirror = new SaverMirror(canvas, host.get());
            const QRect geometry = args.testWindowed ? QRect(720, 40 + index * 240, 400, 225) : screen->geometry();
            host->winId();
            host->windowHandle()->setScreen(screen);
            host->setGeometry(geometry);
            // Fit the primary mosaic unchanged; different monitor shapes get black outer borders.
            QSize fitted = canvas->size().scaled(geometry.size(), Qt::KeepAspectRatio);
            mirror->setGeometry((geometry.width() - fitted.width()) / 2,
                                (geometry.height() - fitted.height()) / 2, fitted.width(), fitted.height());
            if (args.testWindowed) host->show(); else host->showFullScreen();
            mirrors.push_back(std::move(host));
        };
        if (args.testWindowed) {
            for (int i = 0; i < args.testMirrors; ++i) addMirror(primary, i);
        } else {
            for (auto* screen : screens) if (screen != primary) addMirror(screen, 0);
        }
        master.raise();
        master.activateWindow();
        canvas->setFocus();
        QObject::connect(qApp, &QGuiApplication::screenRemoved, &master, [] { qApp->quit(); });
        QObject::connect(qApp, &QGuiApplication::screenAdded, &master, [] { qApp->quit(); });
        for (auto* screen : screens)
            QObject::connect(screen, &QScreen::geometryChanged, &master, [] { qApp->quit(); });
    }
    SaverInput input;
    QTimer inputWatch;
    if (!preview) {
        qApp->setOverrideCursor(Qt::BlankCursor);
        qApp->installEventFilter(&input);
        QObject::connect(&inputWatch, &QTimer::timeout, &master, [&] { if (input.moved()) qApp->quit(); });
        inputWatch.start(100);
    }
    // Wait for the widget's GL initialization without moving any decoding into the paint path.
    QTimer start;
    QObject::connect(&start, &QTimer::timeout, &master, [&] {
        if (canvas->isValid()) { start.stop(); canvas->playPause(); }
    });
    start.start(100);
    if (args.testSeconds) QTimer::singleShot(args.testSeconds * 1000, qApp, &QCoreApplication::quit);
    const int result = qApp->exec();
    if (!preview) {
        qApp->removeEventFilter(&input);
        qApp->restoreOverrideCursor();
    }
    if (!args.report.isEmpty()) {
        auto report = canvas->diagnostics();
        report["mirrors"] = int(mirrors.size());
        report["preview"] = preview;
        report["muted"] = canvas->settings().muted;
        QFile file(args.report);
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(report).toJson());
        canvas->grabFramebuffer().save(args.report + ".png");
        for (size_t i = 0; i < mirrors.size(); ++i)
            mirrors[i]->findChild<QOpenGLWidget*>()->grabFramebuffer().save(args.report + QString(".mirror%1.png").arg(i));
    }
    mirrors.clear(); // All shared-context readers must go away before the decoder textures.
    canvas->stop();
    return result;
}
} // namespace kaleido
