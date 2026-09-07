#include "compositor.h"
#include "export_dialog.h"
#include "export_timeline.h"
#include "exporter.h"
#include "gpu.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLTexture>
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
    void insetKeepsCountDistinctClipsAndAudioAcrossTransitions() {
        for (int count = 1; count <= 8; ++count) {
            Settings s;
            s.modes = {"Inset", "Grid"};
            s.minSlots = s.maxSlots = count;
            s.clipMin = s.clipMax = .5;
            s.layoutMin = s.layoutMax = 1;
            s.transition = .3;
            ExportTimeline scene(s, fixtures(), 42, 16. / 9);
            bool sawInset = false, sawGrid = false;
            for (int frame = 0; frame < 150; ++frame) {
                scene.advance(frame / 30.);
                QCOMPARE(scene.segments().size(), size_t(count));
                QSet<QString> occupied;
                for (const auto& slot : scene.segments()) {
                    QVERIFY(slot.stream > 0);
                    QVERIFY(!occupied.contains(slot.video.id));
                    occupied.insert(slot.video.id);
                }
                if (scene.newMask >= 4) {
                    sawInset = true;
                    QVERIFY(scene.newMask <= (count >= 4 ? 6 : 5));
                    QCOMPARE(scene.segments().front().target, QRectF(0, 0, 1, 1));
                } else {
                    sawGrid = true;
                }
                if (const auto* audio = scene.audioSlot())
                    QVERIFY(occupied.contains(audio->video.id));
            }
            QVERIFY(sawInset && sawGrid);
        }
    }
    void insetCompositorFillsBackgroundAndRevealsItThroughFit() {
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        QOpenGLContext context;
        context.setFormat(format);
        QVERIFY(context.create());
        QOffscreenSurface surface;
        surface.setFormat(context.format());
        surface.create();
        QVERIFY(context.makeCurrent(&surface));
        Compositor compositor;
        QVERIFY2(compositor.initialize(), qPrintable(compositor.error()));
        QImage red(16, 64, QImage::Format_RGBA8888), green(64, 16, QImage::Format_RGBA8888);
        red.fill(Qt::red);
        green.fill(Qt::green);
        QOpenGLTexture background(red), foreground(green);
        QOpenGLFramebufferObject target(QSize(400, 300));
        QVERIFY(target.isValid());
        std::vector<DrawTile> tiles = {
            {background.textureId(), red.size(), QRectF(0, 0, 1, 1), 1, 0, true},
            {foreground.textureId(), green.size(), QRectF(0, 0, 1, 1), 1, 0, false}};
        for (const auto& shape : {QString("Inset Circles"), QString("Inset Hexagons")}) {
            const int mask = maskKind(shape);
            for (bool crop : {false, true}) {
                compositor.draw(target.handle(), target.size(), target.size(), Qt::blue, crop,
                                0, mask, 1, tiles);
                const auto result = target.toImage();
                QCOMPARE(result.pixelColor(0, 0), QColor(Qt::red));
                QCOMPARE(result.pixelColor(399, 299), QColor(Qt::red));
                QCOMPARE(result.pixelColor(200, 150), QColor(Qt::green));
                QCOMPARE(result.pixelColor(200, 60), QColor(crop ? Qt::green : Qt::red));
            }
        }
        // A foreground frame arriving before the background must retain its shaped mask.
        tiles.erase(tiles.begin());
        compositor.draw(target.handle(), target.size(), target.size(), Qt::blue, false,
                        0, maskKind("Inset Circles"), 1, tiles);
        QCOMPARE(target.toImage().pixelColor(0, 0), QColor(Qt::blue));
        compositor.release();
    }
    void honeycombUsesSharedGeometryAndSmallCountFallback() {
        for (int count = 1; count <= 8; ++count) {
            Settings s;
            s.modes = {"Honeycomb"};
            s.minSlots = s.maxSlots = count;
            ExportTimeline scene(s, fixtures(), 42, 16. / 9);
            scene.advance(0);
            QCOMPARE(scene.newMask, count == 1 ? 0 : maskKind(count == 2 ? "Hexagons" : "Honeycomb"));
            std::mt19937 rng(1);
            const auto expected = makeLayout("Honeycomb", count, 16. / 9, rng);
            QCOMPARE(scene.segments().size(), size_t(count));
            for (int i = 0; i < count; ++i)
                QCOMPARE(scene.segments()[i].target, expected[i]);
        }
    }
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
    // FFmpeg addresses GPUs through three disagreeing index spaces, so the export resolves its
    // device by name. These cover the parsing that resolution rests on; no hardware needed.
    void nvencDeviceListIsParsedByOrdinal() {
        const QString log =
            "[h264_nvenc @ 0000022d] [ GPU #0 - < NVIDIA GeForce RTX 4090 > has Compute SM 8.9 ]\n"
            "[h264_nvenc @ 0000022d] [ GPU #1 - < NVIDIA GeForce RTX 5090 > has Compute SM 12.0 ]\n"
            "[h264_nvenc @ 0000022d] [ GPU #2 - < NVIDIA GeForce RTX 3090 > has Compute SM 8.6 ]\n"
            "[vost#0:0/h264_nvenc @ 0000022d] Error while opening encoder\n";
        const auto devices = parseNvencDevices(log);
        QCOMPARE(devices.size(), 3);
        QCOMPARE(devices[0], QString("NVIDIA GeForce RTX 4090"));
        QCOMPARE(devices[1], QString("NVIDIA GeForce RTX 5090"));
        QCOMPARE(devices[2], QString("NVIDIA GeForce RTX 3090"));
    }
    void nvencParsingIgnoresUnrelatedOutput() {
        QVERIFY(parseNvencDevices("Nvenc initialized successfully\nStream #0:0 -> #0:0\n").isEmpty());
        QVERIFY(parseNvencDevices(QString()).isEmpty());
    }
    void rendererNameDropsTheDriverSuffix() {
        QCOMPARE(normalizeRendererName("NVIDIA GeForce RTX 5090/PCIe/SSE2"),
                 QString("NVIDIA GeForce RTX 5090"));
        QCOMPARE(normalizeRendererName("NVIDIA GeForce RTX 5090"), QString("NVIDIA GeForce RTX 5090"));
        QCOMPARE(normalizeRendererName(QString()), QString());
    }
    void unmatchedRendererFallsBackToTheFirstDevice() {
        // An unknown renderer must still produce usable indices rather than -1.
        const auto selection = selectExportGpu("Some Unknown Adapter");
        QCOMPARE(selection.dxgi, 0);
        QCOMPARE(selection.nvenc, 0);
        QVERIFY(!selection.decodeMatched);
        QVERIFY(!selection.encodeMatched);
    }
};
QTEST_MAIN(ExportTests)
#include "export_tests.moc"
