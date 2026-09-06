#include "window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QSurfaceFormat>

int main(int argc, char** argv) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1);
    format.setDepthBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    QApplication::setApplicationName("Kaleidowall");
    QApplication::setOrganizationName("Kaleidowall");
    QCommandLineParser args;
    args.addHelpOption();
    args.addOption({"data-dir", "Override application data directory", "path"});
    args.addOption({"smoke", "Run a timed playback check and save diagnostics", "seconds"});
    args.addOption({"library", "Index a folder on startup", "path"});
    args.addOption({"benchmark", "Profile repeatable playback and layout changes", "seconds"});
    args.addOption({"benchmark-fps", "Benchmark frame rate target", "fps", "60"});
    args.addOption({"export", "Export a fresh sequence to an MP4 file and exit", "path"});
    args.addOption({"export-seconds", "Export duration in seconds", "seconds", "60"});
    args.addOption({"export-size", "Export height: 720, 1080 or 2160", "height", "1080"});
    args.addOption({"export-fps", "Export frame rate: 30 or 60", "fps", "60"});
    args.addOption({"export-quality", "Export quality: low, medium or high", "quality", "high"});
    args.addOption({"export-audio", "Use an MP3/WAV soundtrack (trim/pad to export-seconds)", "path"});
    args.addOption({"export-clip-audio", "Follow clip audio during export"});
    args.addOption({"export-software", "Use CPU H.264 encoding instead of NVENC"});
    args.addOption({"export-overwrite", "Replace the destination only after a successful export"});
    args.addOption({"export-settings", "Applied session settings JSON for a reproducible export", "path"});
    args.addOption({"export-seed", "Random seed for reproducible export", "seed", "42"});
    args.addOption(
        {"export-cancel-ms", "Cancel an export after this many milliseconds (validation)", "milliseconds"});
    args.process(app);
    auto data = args.value("data-dir");
    if (data.isEmpty())
        data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    kaleido::Window window(data);
    window.show();
    QJsonArray samples;
    QTimer sampleTimer;
    QTimer benchmarkEnd;
    if (args.isSet("export")) {
        if (args.isSet("smoke") || args.isSet("benchmark") ||
            (args.isSet("export-audio") && args.isSet("export-clip-audio")))
            args.showHelp(2);
        if (args.isSet("export-settings")) {
            QFile file(args.value("export-settings"));
            if (!file.open(QIODevice::ReadOnly))
                return 2;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(file.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                return 2;
            window.canvas()->applySettings(kaleido::Settings::fromJson(document.object()));
        }
        kaleido::ExportOptions options;
        options.destination = args.value("export");
        options.duration = args.value("export-seconds").toDouble();
        options.fps = args.value("export-fps").toInt();
        const int height = args.value("export-size").toInt();
        options.size = QSize(height == 720    ? 1280
                             : height == 1080 ? 1920
                             : height == 2160 ? 3840
                                              : 0,
                             height);
        options.quality = QStringList{"low", "medium", "high"}.indexOf(args.value("export-quality"));
        options.audioMode = args.isSet("export-audio") ? 2 : args.isSet("export-clip-audio") ? 1 : 0;
        options.audioFile = args.value("export-audio");
        options.volume = window.canvas()->settings().volume;
        options.seed = args.value("export-seed").toUInt();
        options.softwareEncoder = args.isSet("export-software");
        options.overwrite = args.isSet("export-overwrite");
        QObject::connect(&window, &kaleido::Window::exportFinished, &app,
                         [&](bool success, bool canceled, const QString& message, QJsonObject report) {
                             report["success"] = success;
                             report["canceled"] = canceled;
                             report["message"] = message;
                             QFile out(data + "/export.json");
                             if (out.open(QIODevice::WriteOnly))
                                 out.write(QJsonDocument(report).toJson());
                             window.grab().save(data + "/export-window.png");
                             app.exit(success ? 0 : canceled ? 3 : 1);
                         });
        auto start = [&, options] {
            window.startExport(options);
            QTimer::singleShot(1500, &window, [&] { window.grab().save(data + "/export-preview.png"); });
            if (args.isSet("export-cancel-ms"))
                QTimer::singleShot(std::max(1, args.value("export-cancel-ms").toInt()), &window,
                                   &kaleido::Window::cancelExport);
        };
        if (args.isSet("library"))
            QObject::connect(window.library(), &kaleido::Library::scanFinished, &window, start);
        else
            QTimer::singleShot(500, &window, start);
    }
    if (args.isSet("benchmark")) {
        kaleido::Settings s;
        s.minSlots = 2;
        s.maxSlots = 4;
        s.clipMin = 3;
        s.clipMax = 5;
        s.layoutMin = 3;
        s.layoutMax = 4;
        s.transition = 1.2;
        s.fps = args.value("benchmark-fps").toInt();
        s.muted = true;
        window.canvas()->applySettings(s);
        window.canvas()->beginProfile();
        QObject::connect(&sampleTimer, &QTimer::timeout, &app,
                         [&] { samples.append(window.canvas()->diagnostics()); });
        benchmarkEnd.setSingleShot(true);
        QObject::connect(&benchmarkEnd, &QTimer::timeout, &app, [&] {
            QFile out(data + "/benchmark.json");
            if (out.open(QIODevice::WriteOnly))
                out.write(QJsonDocument(QJsonObject{{"events", window.canvas()->profileData()},
                                                    {"samples", samples},
                                                    {"final", window.canvas()->diagnostics()}})
                              .toJson());
            window.grab().save(data + "/performance.png");
            app.quit();
        });
        auto start = [&] {
            window.canvas()->playPause();
            window.showPanel(2);
            sampleTimer.start(1000);
            benchmarkEnd.start(std::max(15, args.value("benchmark").toInt()) * 1000);
        };
        if (args.isSet("library"))
            QObject::connect(window.library(), &kaleido::Library::scanFinished, &window, start);
        else
            QTimer::singleShot(500, &window, start);
    }
    if (args.isSet("smoke")) {
        kaleido::Settings s;
        s.minSlots = 2;
        s.maxSlots = 4;
        s.clipMin = 2;
        s.clipMax = 4;
        s.layoutMin = 3;
        s.layoutMax = 5;
        s.transition = .8;
        s.skipStart = 1;
        s.skipEnd = 1;
        s.muted = false;
        window.canvas()->applySettings(s);
        QObject::connect(&sampleTimer, &QTimer::timeout, &app,
                         [&] { samples.append(window.canvas()->diagnostics()); });
        sampleTimer.start(1000);
        auto start = [&] {
            window.canvas()->playPause();
            QTimer::singleShot(4000, &window, [&] { window.canvas()->nextLayout(); });
            QTimer::singleShot(6000, &window, [&] { window.canvas()->playPause(); });
            QTimer::singleShot(9000, &window, [&] { window.canvas()->playPause(); });
            QTimer::singleShot(11000, &window, [&] { window.toggleFullscreen(); });
            QTimer::singleShot(15000, &window, [&] {
                window.grab().save(data + "/smoke-fullscreen.png");
                window.toggleFullscreen();
            });
            QTimer::singleShot(16000, &window, [&] { window.showPanel(1); });
            QTimer::singleShot(17000, &window, [&] {
                window.grab().save(data + "/smoke-library.png");
                window.showPanel(2);
            });
            QTimer::singleShot(18000, &window, [&] { window.grab().save(data + "/smoke-performance.png"); });
            QTimer::singleShot(19000, &window, [&] {
                auto s = window.canvas()->settings();
                s.minSlots = s.maxSlots = 1;
                s.reducedMotion = true;
                window.canvas()->applySettings(s);
            });
            QTimer::singleShot(22000, &window, [&] { window.grab().save(data + "/smoke-single.png"); });
        };
        if (args.isSet("library"))
            QObject::connect(window.library(), &kaleido::Library::scanFinished, &window, start);
        else
            QTimer::singleShot(1000, &window, start);
        int seconds = std::max(12, args.value("smoke").toInt());
        QTimer::singleShot(seconds * 1000, &window, [&] {
            QDir().mkpath(data);
            window.grab().save(data + "/smoke-window.png");
            window.canvas()->grabFramebuffer().save(data + "/smoke-frame.png");
            QFile out(data + "/smoke.json");
            if (out.open(QIODevice::WriteOnly))
                out.write(QJsonDocument(
                              QJsonObject{{"samples", samples}, {"final", window.canvas()->diagnostics()}})
                              .toJson());
            app.quit();
        });
    }
    if (args.isSet("library")) {
        window.library()->addFolder(args.value("library"));
        QTimer::singleShot(200, &window, [&] { window.library()->scan(); });
    }
    return app.exec();
}
