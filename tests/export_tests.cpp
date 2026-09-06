#include "compositor.h"
#include "export_dialog.h"
#include "export_timeline.h"
#include "exporter.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>
#include <set>
using namespace kaleido;

class ExportTests : public QObject {
    Q_OBJECT
    QVector<Video> fixtures() {
        QVector<Video> media;
        for (int i = 0; i < 8; ++i) {
            Video v;
            v.id = QString::number(i);
            v.path = v.id + ".mp4";
            v.duration = 30;
            v.width = 1920;
            v.height = 1080;
            v.audio = i % 2 == 0;
            media << v;
        }
        return media;
    }
  private slots:
    void roundsOnlyUpToWholeFrames() {
        ExportOptions o;
        o.duration = 1.001;
        o.fps = 30;
        QCOMPARE(o.frameCount(), 31);
        QVERIFY(o.outputDuration() >= o.duration);
        QVERIFY(o.outputDuration() - o.duration < 1.0 / o.fps);
        o.duration = 1.0;
        QCOMPARE(o.frameCount(), 30);
        o.fps = 60;
        QCOMPARE(o.frameCount(), 60);
    }
    void timelineIsDeterministicAndRespectsClipBounds() {
        Settings s;
        s.minSlots = 2;
        s.maxSlots = 4;
        s.skipStart = 3;
        s.skipEnd = 4;
        s.clipMin = .5;
        s.clipMax = 2;
        s.layoutMin = 1;
        s.layoutMax = 3;
        s.transition = .5;
        ExportTimeline a(s, fixtures(), 42, 16.0 / 9), b(s, fixtures(), 42, 16.0 / 9);
        for (int f = 0; f < 1800; ++f) {
            const double t = f / 60.0;
            a.advance(t);
            b.advance(t);
            QCOMPARE(a.segments().size(), b.segments().size());
            QCOMPARE(a.oldMask, b.oldMask);
            QCOMPARE(a.newMask, b.newMask);
            QSet<QString> occupied;
            for (size_t i = 0; i < a.segments().size(); ++i) {
                const auto& slot = a.segments()[i];
                const auto& other = b.segments()[i];
                QCOMPARE(slot.stream, other.stream);
                QCOMPARE(slot.video.id, other.video.id);
                QCOMPARE(slot.target, other.target);
                QCOMPARE(slot.clip.start, other.clip.start);
                if (!slot.stream)
                    continue;
                QVERIFY(!occupied.contains(slot.video.id));
                occupied.insert(slot.video.id);
                QVERIFY(slot.clip.start >= 3);
                QVERIFY(slot.clip.start + slot.clip.length <= 26.000001);
                QVERIFY(slot.usableEnd == 26);
                QVERIFY(slot.begins <= t);
            }
            if (const auto* audio = a.audioSlot()) {
                QVERIFY(audio->video.audio);
                QVERIFY(!audio->retiring);
            }
            QVERIFY(a.progress() >= 0 && a.progress() <= 1);
        }
    }
    void tinyLibraryAndAudioLessSources() {
        auto media = fixtures();
        media.resize(1);
        media[0].audio = false;
        Settings s;
        s.minSlots = 4;
        s.maxSlots = 8;
        s.clipMin = .5;
        s.clipMax = 1;
        ExportTimeline scene(s, media, 1, 16.0 / 9);
        for (int f = 0; f < 300; ++f) {
            scene.advance(f / 30.0);
            QCOMPARE(scene.segments().size(), size_t(1));
            QVERIFY(scene.segments()[0].stream > 0);
            QVERIFY(scene.audioSlot() == nullptr);
            QCOMPARE(scene.newMask, 0);
        }
        media[0].enabled = false;
        ExportTimeline empty(s, media, 1, 16.0 / 9);
        QVERIFY(!empty.error.isEmpty());
    }
    void cutsKeepGeometryAndTransitionClock() {
        Settings s;
        s.minSlots = s.maxSlots = 2;
        s.clipMin = s.clipMax = .5;
        s.layoutMin = s.layoutMax = 10;
        s.transition = 2;
        ExportTimeline scene(s, fixtures(), 4, 16.0 / 9);
        scene.advance(0);
        const auto first = scene.segments()[0];
        scene.advance(.5);
        const auto second = scene.segments()[0];
        QCOMPARE(first.serial, second.serial);
        QVERIFY(first.stream != second.stream);
        QCOMPARE(first.from, second.from);
        QCOMPARE(first.target, second.target);
        QCOMPARE(scene.progress(), easedProgress(.5, 0, 2));
    }
    void invalidOptionsDoNotOverwrite() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ExportOptions o;
        o.destination = dir.filePath("existing.mp4");
        QFile file(o.destination);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("keep");
        file.close();
        QVERIFY(!o.validate().isEmpty());
        o.overwrite = true;
        QVERIFY(o.validate().isEmpty());
        o.fps = 59;
        QVERIFY(!o.validate().isEmpty());
        o.fps = 60;
        o.duration = std::numeric_limits<double>::infinity();
        QVERIFY(!o.validate().isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("keep"));
    }
    void dialogDefaultsAndIndependentExportOptions() {
        QTemporaryDir dir;
        Settings s;
        s.fps = 30;
        s.muted = false;
        ExportDialog dialog(s, {{"folder", dir.path()}});
        dialog.show();
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        auto* start = buttons->buttons().last();
        for (auto* b : buttons->buttons())
            if (buttons->buttonRole(b) == QDialogButtonBox::AcceptRole)
                start = b;
        QTest::mouseClick(start, Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.options.fps, 60);
        QCOMPARE(s.fps, 30);
        QCOMPARE(dialog.options.size, QSize(1920, 1080));
        QCOMPARE(dialog.options.quality, 2);
        QCOMPARE(dialog.options.audioMode, 1);
        QCOMPARE(dialog.options.duration, 60.0);
        QVERIFY(dialog.options.validate().isEmpty());
    }
    void importedMp3SnapsToAudibleDuration() {
        if (mediaTool("ffmpeg").isEmpty())
            QSKIP("FFmpeg unavailable");
        QTemporaryDir dir;
        const QString mp3 = dir.filePath("soundtrack.mp3");
        QCOMPARE(
            QProcess::execute(mediaTool("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                                                    "sine=frequency=440:sample_rate=48000:duration=3.123",
                                                    "-c:a", "libmp3lame", "-y", mp3}),
            0);
        ExportDialog dialog(Settings{}, {{"folder", dir.path()}});
        dialog.show();
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QAbstractButton* start = nullptr;
        for (auto* b : buttons->buttons())
            if (buttons->buttonRole(b) == QDialogButtonBox::AcceptRole)
                start = b;
        QVERIFY(start);
        dialog.loadSoundtrack(mp3);
        QVERIFY(!start->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 15000);
        auto* duration = dialog.findChild<QDoubleSpinBox*>();
        QVERIFY(!duration->isEnabled());
        QVERIFY(std::abs(duration->value() - 3.123) < .001);
        const auto output = qEnvironmentVariable("KALEIDO_TEST_OUTPUT");
        if (!output.isEmpty())
            dialog.grab().save(output + "/export-dialog.png");
        QTest::mouseClick(start, Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.options.audioMode, 2);
        QVERIFY(std::abs(dialog.options.duration - 3.123) < .0001);
        QCOMPARE(dialog.options.frameCount(), 188);
    }
};
QTEST_MAIN(ExportTests)
#include "export_tests.moc"
