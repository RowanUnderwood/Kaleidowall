#include "core.h"
#include "library.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace kaleido;
class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void clipNeverCrossesMargins() {
        std::mt19937 rng(123);
        Settings s;
        s.clipMin = 5;
        s.clipMax = 40;
        s.skipStart = 12;
        s.skipEnd = 20;
        Video v;
        v.duration = 73;
        for (int i = 0; i < 10000; ++i) {
            auto c = chooseClip(v, s, rng);
            QVERIFY(c);
            QVERIFY(c->start >= 12);
            QVERIFY(c->start + c->length <= 53.000001);
            QVERIFY(c->length >= 5 && c->length <= 40);
        }
    }
    void shortFilesAreRejected() {
        Settings s;
        s.clipMin = 10;
        s.skipStart = 20;
        s.skipEnd = 20;
        Video v;
        v.duration = 49;
        std::mt19937 rng(1);
        QVERIFY(!chooseClip(v, s, rng));
        v.duration = 50;
        auto c = chooseClip(v, s, rng);
        QVERIFY(c);
        QCOMPARE(c->start, 20.);
        QCOMPARE(c->length, 10.);
    }
    void percentAndOverrides() {
        Settings s;
        s.clipMin = s.clipMax = 10;
        s.skipPercent = true;
        s.skipStart = 10;
        s.skipEnd = 20;
        Video v;
        v.duration = 200;
        v.skipStart = 5;
        std::mt19937 rng(3);
        for (int i = 0; i < 1000; ++i) {
            auto c = chooseClip(v, s, rng);
            QVERIFY(c->start >= 5);
            QVERIFY(c->start + c->length <= 160);
        }
    }
    void noReuseUntilExhausted() {
        ShuffleBag b(5);
        b.reconcile({"a", "b", "c", "d"});
        QSet<QString> seen;
        for (int i = 0; i < 4; ++i) {
            auto id = b.reserve({});
            QVERIFY(id);
            QVERIFY(!seen.contains(*id));
            seen.insert(*id);
            b.commit(*id);
        }
        QCOMPARE(b.remainingCount(), 0);
        QVERIFY(b.reserve({}));
        QCOMPARE(b.cycle(), 2);
    }
    void reservationsPreventRaces() {
        ShuffleBag b(1);
        b.reconcile({"a", "b"});
        auto a = b.reserve({}), c = b.reserve({});
        QVERIFY(a && c);
        QVERIFY(*a != *c);
        QVERIFY(!b.reserve({}));
        b.cancel(*a);
        QCOMPARE(b.reserve({}), a);
    }
    void cancelDoesNotConsume() {
        ShuffleBag b(1);
        b.reconcile({"a"});
        auto a = b.reserve({});
        b.cancel(*a);
        QCOMPARE(b.remainingCount(), 1);
        QCOMPARE(b.reserve({}), a);
    }
    void activeFilesAreDeferred() {
        ShuffleBag b(4);
        b.reconcile({"a", "b"});
        auto id = b.reserve({"a"});
        QCOMPARE(*id, QString("b"));
        b.commit(*id);
        QVERIFY(!b.reserve({"a"}));
        QCOMPARE(*b.reserve({}), QString("a"));
    }
    void restorePreservesUnplayedReservations() {
        ShuffleBag b(1);
        b.reconcile({"a", "b", "c"});
        auto first = b.reserve({});
        b.commit(*first);
        auto reserved = b.reserve({});
        ShuffleBag restored(2);
        restored.restore(b.json());
        restored.reconcile({"a", "b", "c"});
        QSet<QString> seen;
        for (int i = 0; i < 2; ++i) {
            auto id = restored.reserve({});
            QVERIFY(id);
            QVERIFY(*id != *first);
            seen.insert(*id);
            restored.commit(*id);
        }
        QVERIFY(seen.contains(*reserved));
    }
    void newAndRemovedFilesReconcile() {
        ShuffleBag b(1);
        b.reconcile({"a"});
        b.commit(*b.reserve({}));
        b.reconcile({"a", "b"});
        QCOMPARE(*b.reserve({}), QString("b"));
        b.cancel("b");
        b.reconcile({"a"});
        QCOMPARE(*b.reserve({}), QString("a"));
    }
    void layoutsCoverWithoutOverlap() {
        std::mt19937 rng(4);
        for (auto mode : QStringList{"Split", "Grid", "Hero", "Masonry", "Circles", "Hexagons"})
            for (int n = 1; n <= 32; ++n) {
                auto rects = makeLayout(mode, n, 16. / 9, rng);
                QCOMPARE(rects.size(), n);
                double area = 0;
                for (int i = 0; i < n; ++i) {
                    auto r = rects[i];
                    QVERIFY(r.x() >= 0 && r.y() >= 0 && r.right() <= 1.000001 && r.bottom() <= 1.000001);
                    area += r.width() * r.height();
                    for (int j = 0; j < i; ++j) {
                        auto overlap = r.intersected(rects[j]);
                        QVERIFY(overlap.width() * overlap.height() < 1e-10);
                    }
                }
                QVERIFY(std::abs(area - 1) < 1e-9);
            }
    }
    void settingsRoundTrip() {
        Settings s;
        s.minSlots = 1;
        s.maxSlots = 12;
        s.skipPercent = true;
        s.skipStart = 12;
        s.modes = {"Hero", "Masonry"};
        s.weights = {{"Hero", 4}, {"Masonry", 2}};
        s.backgroundColor = "#123456";
        QCOMPARE(Settings::fromJson(s.json()).json(), s.json());
    }
    void invalidSettingsAreClamped() {
        Settings s;
        s.minSlots = 50;
        s.maxSlots = 0;
        s.clipMin = 10;
        s.clipMax = 1;
        s.layoutMin = 1;
        s.transition = 10;
        s.modes = {"bad"};
        s.backgroundColor = "invalid";
        s.normalize();
        QCOMPARE(s.minSlots, 32);
        QCOMPARE(s.maxSlots, 32);
        QCOMPARE(s.clipMax, 10.);
        QCOMPARE(s.transition, 1.);
        QCOMPARE(s.modes, QStringList{"Grid"});
        QCOMPARE(s.backgroundColor, QString("#06080c"));
    }
    void databasePersistsSettings() {
        QTemporaryDir temp;
        {
            Library lib(temp.filePath("test.db"));
            QVERIFY(lib.error().isEmpty());
            lib.setValue("settings", Settings{}.json());
            lib.addFolder(temp.path());
            lib.addFolder(temp.path());
            QCOMPARE(lib.folders().size(), 1);
        }
        Library reopened(temp.filePath("test.db"));
        QCOMPARE(reopened.value("settings"), Settings{}.json());
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"
