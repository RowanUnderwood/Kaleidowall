#include "screensaver.h"
#include "window.h"
#include <QApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#define NOMINMAX
#include <windows.h>
using namespace kaleido;

class ScreensaverTests : public QObject {
    Q_OBJECT
  private slots:
    void launchArguments() {
        QCOMPARE(parseSaverArguments({}).mode, SaverMode::Configure);
        QCOMPARE(parseSaverArguments({"/S"}).mode, SaverMode::Run);
        QCOMPARE(parseSaverArguments({"-c"}).mode, SaverMode::Configure);
        QCOMPARE(parseSaverArguments({"/c:12345"}).parent, quintptr(12345));
        QCOMPARE(parseSaverArguments({"/p", "4294967297"}).parent, quintptr(4294967297ULL));
        QCOMPARE(parseSaverArguments({"/P:42"}).mode, SaverMode::Preview);
        for (const auto& args : {QStringList{"/p"}, QStringList{"/p", "0"}, QStringList{"/s:4"},
                                QStringList{"/p", "not-a-window"}, QStringList{"/z"},
                                QStringList{"/c", "1", "2"}, QStringList{"/c:"},
                                QStringList{"--test-seconds", "-1"}})
            QCOMPARE(parseSaverArguments(args).mode, SaverMode::Invalid);
    }
    void sessionsShareDefaultsButNotAudioOrShuffleWrites() {
        QTemporaryDir dir;
        Library lib(dir.filePath("library.sqlite"));
        Settings original;
        original.muted = false;
        lib.setValue("settings", original.json());
        lib.setValue("shuffle", {{"marker", 123}});
        const auto shuffle = lib.value("shuffle");
        Settings preferred = original;
        preferred.minSlots = preferred.maxSlots = 1;
        lib.setValue("preset:Chosen", preferred.json());
        lib.setValue("presetState", {{"defaultPreset", "Chosen"}});
        for (bool preview : {true, false}) {
            Canvas canvas(&lib, nullptr, saverCanvasOptions(preview, true));
            QCOMPARE(canvas.settings().minSlots, 1);
            QVERIFY(canvas.settings().muted);
            canvas.setAudio(false, 50);
            canvas.applySettings(original);
            QVERIFY(canvas.settings().muted);
        }
        QCOMPARE(lib.value("settings"), original.json());
        QCOMPARE(lib.value("shuffle"), shuffle);
        QVERIFY(!lib.value("screensaverShuffle").isEmpty());
        lib.setValue("presetState", {{"defaultPreset", "Removed"}});
        QCOMPARE(lib.startupSettings().json(), original.json());
    }
    void configurationReusesPanelsWithoutConsumingPlayerShuffle() {
        QTemporaryDir dir;
        {
            Library lib(dir.filePath("library.sqlite"));
            lib.setValue("shuffle", {{"marker", 456}});
        }
        {
            Window configuration(dir.path(), true);
            auto* mute = configuration.findChild<QCheckBox*>("screensaverMute");
            QVERIFY(mute && mute->isChecked());
            mute->setChecked(false);
            QVERIFY(!configuration.library()->value("screensaverPreferences").value("muted").toBool(true));
            QVERIFY(configuration.findChild<QPushButton*>("presetSave"));
            QVERIFY(!configuration.canvas()->isVisibleTo(&configuration));
        }
        Library lib(dir.filePath("library.sqlite"));
        QCOMPARE(lib.value("shuffle").value("marker").toInt(), 456);
    }
    void previewIsAChildAndExitsWhenHostCloses() {
        QTemporaryDir dir;
        struct NativeHost : QWidget { using QWidget::destroy; } host;
        host.resize(240, 160);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QProcess child;
        child.start(QCoreApplication::applicationDirPath() + "/Kaleidowall.scr",
                    {"/p", QString::number(host.winId()), "--data-dir", dir.path()});
        QVERIFY(child.waitForStarted());
        const HWND hwnd = reinterpret_cast<HWND>(host.winId());
        QTRY_VERIFY_WITH_TIMEOUT(FindWindowExW(hwnd, nullptr, nullptr, nullptr) != nullptr, 6000);
        const HWND saver = FindWindowExW(hwnd, nullptr, nullptr, nullptr);
        QCOMPARE(GetParent(saver), hwnd);
        RECT rect;
        QVERIFY(GetClientRect(saver, &rect));
        QVERIFY(rect.right > 0 && rect.bottom > 0);
        host.destroy();
        QVERIFY(child.waitForFinished(6000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        Library lib(dir.filePath("library.sqlite"));
        QVERIFY(lib.value("shuffle").isEmpty());
        QVERIFY(lib.value("screensaverShuffle").isEmpty());
    }
    void configurationRespectsAndRestoresNativeOwner() {
        QTemporaryDir dir;
        QWidget host;
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        const HWND hwnd = reinterpret_cast<HWND>(host.winId());
        QProcess child;
        child.start(QCoreApplication::applicationDirPath() + "/Kaleidowall.scr",
                    {"/c:" + QString::number(host.winId()), "--data-dir", dir.path(),
                     "--test-seconds", "2"});
        QVERIFY(child.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(!IsWindowEnabled(hwnd), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 6000);
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        QVERIFY(IsWindowEnabled(hwnd));
    }
};
QTEST_MAIN(ScreensaverTests)
#include "screensaver_tests.moc"
