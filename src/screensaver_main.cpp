#include "screensaver.h"
#include <QApplication>
#include <QIcon>
#include <QStandardPaths>
#include <QSurfaceFormat>

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1);
    format.setDepthBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    // Match the player exactly so both resolve the same per-user library.sqlite.
    app.setApplicationName("Kaleidowall");
    app.setOrganizationName("Kaleidowall");
    app.setWindowIcon(QIcon(":/icons/kaleidowall.ico"));
    auto arguments = kaleido::parseSaverArguments(app.arguments().mid(1));
    if (arguments.mode == kaleido::SaverMode::Invalid)
        return 2;
    if (arguments.dataDir.isEmpty())
        arguments.dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return kaleido::runScreensaver(arguments);
}
