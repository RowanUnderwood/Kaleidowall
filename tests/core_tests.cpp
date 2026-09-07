#include "core.h"
#include "library.h"
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
using namespace kaleido;
// Videos only reach the table through a scan, which needs ffprobe and real media. These tests are
// about folder bookkeeping, so they seed rows directly over a second connection.
static void seedVideo(const QString& dbPath, const QString& id) {
    static int counter = 0;
    const QString name = "seed-" + QString::number(++counter);
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare("INSERT OR REPLACE INTO videos(id,path,title,duration,width,height,codec,audio,error,"
                  "size,modified) VALUES(?,?,?,?,?,?,?,?,'',0,0)");
        for (const QVariant& a : QVariantList{id, id, QString("clip"), 60.0, 1920, 1080, QString("h264"), 0})
            q.addBindValue(a);
        QVERIFY2(q.exec(), qPrintable(q.lastError().text()));
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
}
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
    void honeycombIsCenteredConnectedAndDoesNotOverlap() {
        std::mt19937 rng(4);
        for (double aspect : {0.3, 9. / 16, 1., 16. / 9, 3.5}) {
            for (int n = 3; n <= 32; ++n) {
                const auto rects = makeLayout("Honeycomb", n, aspect, rng);
                QCOMPARE(rects.size(), n);
                QRectF bounds;
                QVector<QPointF> centers;
                for (const auto& r : rects) {
                    QVERIFY(r.left() >= -1e-9 && r.top() >= -1e-9);
                    QVERIFY(r.right() <= 1 + 1e-9 && r.bottom() <= 1 + 1e-9);
                    QVERIFY(std::abs(r.width() * aspect / r.height() - 2 / std::sqrt(3.)) < 1e-9);
                    QVERIFY(std::abs(r.height() - rects[0].height()) < 1e-9);
                    bounds = bounds.united(r);
                    centers << QPointF(r.center().x() * aspect, r.center().y());
                }
                QVERIFY(std::abs(bounds.center().x() - .5) < 1e-9);
                QVERIFY(std::abs(bounds.center().y() - .5) < 1e-9);
                const double spacing = rects[0].height();
                QVector<QVector<int>> neighbors(n);
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < i; ++j) {
                        const auto d = centers[i] - centers[j];
                        // Separating-axis test on the three hexagon edge normals.
                        const double separation = std::max({std::abs(d.y()),
                            std::abs(std::sqrt(3.) / 2 * d.x() + .5 * d.y()),
                            std::abs(std::sqrt(3.) / 2 * d.x() - .5 * d.y())});
                        QVERIFY(separation >= spacing - 1e-9);
                        if (std::abs(std::hypot(d.x(), d.y()) - spacing) < 1e-9) {
                            neighbors[i] << j;
                            neighbors[j] << i;
                        }
                    }
                QSet<int> reached{0};
                QVector<int> pending{0};
                while (!pending.empty())
                    for (int j : neighbors[pending.takeLast()])
                        if (!reached.contains(j)) {
                            reached.insert(j);
                            pending << j;
                        }
                QCOMPARE(reached.size(), n);
            }
        }
        for (int n = 0; n <= 2; ++n)
            QCOMPARE(makeLayout("Honeycomb", n, 16. / 9, rng), makeLayout("Hexagons", n, 16. / 9, rng));
    }
    void insetResolvesEligibleShapesAndCountsBackground() {
        std::mt19937 rng(28);
        for (int count = 1; count <= 32; ++count) {
            QSet<QString> seen;
            for (int trial = 0; trial < 90; ++trial) {
                const auto mode = resolveLayoutMode("Inset", count, rng);
                seen.insert(mode);
                QVERIFY(mode == "Inset Circles" || mode == "Inset Hexagons" ||
                        (count >= 4 && mode == "Inset Honeycomb"));
                const auto rects = makeLayout(mode, count, 16. / 9, rng);
                QCOMPARE(rects.size(), count);
                QCOMPARE(rects.front(), QRectF(0, 0, 1, 1));
                QCOMPARE(rects.mid(1), makeLayout(mode.mid(6), count - 1, 16. / 9, rng));
            }
            QCOMPARE(seen.size(), count >= 4 ? 3 : 2);
        }
        Settings s;
        s.modes = {"Inset"};
        s.weights = {{"Inset", 5}, {"Circles", 0}, {"Hexagons", 0}, {"Honeycomb", 0}};
        QCOMPARE(Settings::fromJson(s.json()).json(), s.json());
        QCOMPARE(pickMode(s, rng), QString("Inset"));
    }
    void settingsRoundTrip() {
        Settings s;
        s.minSlots = 1;
        s.maxSlots = 12;
        s.skipPercent = true;
        s.skipStart = 12;
        s.modes = {"Hero", "Masonry", "Honeycomb"};
        s.weights = {{"Hero", 4}, {"Masonry", 2}, {"Honeycomb", 3}};
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
    void disabledFolderExcludesItsVideos() {
        QTemporaryDir temp;
        const QString dbPath = temp.filePath("folders.db");
        QString root;
        {
            Library lib(dbPath);
            QVERIFY(lib.error().isEmpty());
            lib.addFolder(temp.path());
            root = lib.folders().first().path;
            QVERIFY(lib.folders().first().enabled);
            seedVideo(dbPath, root + "/clip.mp4");
            QCOMPARE(lib.videos().size(), 1);
            QVERIFY(lib.videos().first().folderEnabled);
            QVERIFY(eligibilityReason(lib.videos().first(), Settings{}).isEmpty());
            lib.setFolderEnabled(root, false);
            QVERIFY(!lib.folders().first().enabled);
            // The video row itself is untouched; only the derived flag changes.
            QVERIFY(lib.videos().first().enabled);
            QVERIFY(!lib.videos().first().folderEnabled);
            QCOMPARE(eligibilityReason(lib.videos().first(), Settings{}), QString("Folder disabled"));
        }
        Library reopened(dbPath);
        QVERIFY(!reopened.folders().first().enabled);
        reopened.setFolderEnabled(root, true);
        QVERIFY(reopened.videos().first().folderEnabled);
        QVERIFY(eligibilityReason(reopened.videos().first(), Settings{}).isEmpty());
    }
    void overlappingFoldersIncludeWhenEitherIsEnabled() {
        QTemporaryDir temp;
        QVERIFY(QDir(temp.path()).mkpath("sub"));
        const QString dbPath = temp.filePath("overlap.db");
        Library lib(dbPath);
        lib.addFolder(temp.path());
        lib.addFolder(temp.path() + "/sub");
        QCOMPARE(lib.folders().size(), 2);
        QString parent, child;
        for (const auto& f : lib.folders())
            (f.path.endsWith("/sub") ? child : parent) = f.path;
        QVERIFY(!parent.isEmpty() && !child.isEmpty());
        seedVideo(dbPath, child + "/clip.mp4");
        // Disabling one covering folder is not enough while another still covers the video.
        lib.setFolderEnabled(child, false);
        QVERIFY(lib.videos().first().folderEnabled);
        lib.setFolderEnabled(parent, false);
        QVERIFY(!lib.videos().first().folderEnabled);
        lib.setFolderEnabled(child, true);
        QVERIFY(lib.videos().first().folderEnabled);
    }
    void removingAFolderKeepsVideosCoveredByADisabledOne() {
        QTemporaryDir temp;
        QVERIFY(QDir(temp.path()).mkpath("sub"));
        const QString dbPath = temp.filePath("remove.db");
        Library lib(dbPath);
        lib.addFolder(temp.path());
        lib.addFolder(temp.path() + "/sub");
        QString parent, child;
        for (const auto& f : lib.folders())
            (f.path.endsWith("/sub") ? child : parent) = f.path;
        seedVideo(dbPath, child + "/clip.mp4");
        lib.setFolderEnabled(child, false);
        lib.removeFolder(parent);
        // A disabled folder is parked, not forgotten: its rows survive the prune.
        QCOMPARE(lib.videos().size(), 1);
        QVERIFY(!lib.videos().first().folderEnabled);
    }
    void legacyFolderTableGainsTheEnabledColumn() {
        QTemporaryDir temp;
        const QString dbPath = temp.filePath("legacy.db");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy");
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE folders(path TEXT PRIMARY KEY)"));
            QVERIFY(q.exec("INSERT INTO folders VALUES('c:/videos')"));
            db.close();
        }
        QSqlDatabase::removeDatabase("legacy");
        Library lib(dbPath);
        QVERIFY2(lib.error().isEmpty(), qPrintable(lib.error()));
        QCOMPARE(lib.folders().size(), 1);
        QCOMPARE(lib.folders().first().path, QString("c:/videos"));
        // Pre-existing folders default to enabled, so an upgrade never silently empties a library.
        QVERIFY(lib.folders().first().enabled);
        seedVideo(dbPath, "c:/videos/clip.mp4");
        QCOMPARE(lib.videos().size(), 1);
        QVERIFY(lib.videos().first().folderEnabled);
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"
