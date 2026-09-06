#include "library.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace kaleido {
static QString canonical(const QString& path) {
    QFileInfo f(path);
    auto p = f.canonicalFilePath();
    return QDir::cleanPath(p.isEmpty() ? f.absoluteFilePath() : p).toLower();
}
// Folder membership is prefix-only: video ids are lowercased canonical paths, so a video
// belongs to every root it sits beneath. Overlapping roots therefore share their videos.
static bool covered(const QStringList& roots, const QString& id) {
    for (const auto& root : roots)
        if (id.startsWith(root + "/", Qt::CaseInsensitive))
            return true;
    return false;
}
Library::Library(const QString& path, QObject* parent) : QObject(parent) {
    connectionName = QUuid::createUuid().toString();
    db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    db.setDatabaseName(path);
    if (!db.open()) {
        dbError = db.lastError().text();
        return;
    }
    QSqlQuery q(db);
    for (const auto& sql :
         {"PRAGMA journal_mode=WAL",
          "CREATE TABLE IF NOT EXISTS folders(path TEXT PRIMARY KEY, enabled INTEGER DEFAULT 1)",
          "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value TEXT NOT NULL)",
          "CREATE TABLE IF NOT EXISTS videos(id TEXT PRIMARY KEY,path TEXT,title TEXT,duration REAL,width "
          "INTEGER,height INTEGER,codec TEXT,audio INTEGER,enabled INTEGER DEFAULT 1,skipStart REAL DEFAULT "
          "-1,skipEnd REAL DEFAULT -1,error TEXT,missing INTEGER DEFAULT 0,size INTEGER,modified INTEGER)"})
        if (!q.exec(sql))
            dbError = q.lastError().text();
    // CREATE TABLE IF NOT EXISTS is a no-op on a database written by an older build, and SQLite has
    // no ADD COLUMN IF NOT EXISTS, so probe before altering. Without this every folders query on an
    // existing library fails and the library silently reads as empty.
    bool hasEnabled = false;
    if (q.exec("PRAGMA table_info(folders)"))
        while (q.next())
            if (q.value(1).toString().compare("enabled", Qt::CaseInsensitive) == 0)
                hasEnabled = true;
    if (!hasEnabled && !q.exec("ALTER TABLE folders ADD COLUMN enabled INTEGER DEFAULT 1"))
        dbError = q.lastError().text();
}
Library::~Library() {
    canceled = true;
    if (worker) {
        worker->quit();
        worker->wait();
    }
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}
QVector<Folder> Library::folders() const {
    QSqlQuery q(db);
    q.exec("SELECT path,enabled FROM folders ORDER BY path");
    QVector<Folder> out;
    while (q.next())
        out << Folder{q.value(0).toString(), q.value(1).toBool()};
    return out;
}

void Library::addFolder(const QString& path) {
    QSqlQuery q(db);
    // Name the column: the positional form breaks as soon as the table gains one.
    q.prepare("INSERT OR IGNORE INTO folders(path) VALUES(?)");
    q.addBindValue(canonical(path));
    q.exec();
    emit changed();
}
void Library::setFolderEnabled(const QString& path, bool enabled) {
    QSqlQuery q(db);
    q.prepare("UPDATE folders SET enabled=? WHERE path=?");
    q.addBindValue(enabled);
    q.addBindValue(path);
    q.exec();
    emit changed();
}
void Library::removeFolder(const QString& path) {
    if (scanning())
        return;
    QSqlQuery q(db);
    q.prepare("DELETE FROM folders WHERE path=?");
    q.addBindValue(path);
    q.exec();
    // Every remaining root keeps its videos, disabled ones included: a disabled folder is parked,
    // not forgotten, so removing an overlapping folder must not delete what it still covers.
    QStringList roots;
    for (const auto& f : folders())
        roots << f.path;
    auto all = videos();
    db.transaction();
    for (const auto& v : all)
        if (!covered(roots, v.id)) {
            q.prepare("DELETE FROM videos WHERE id=?");
            q.addBindValue(v.id);
            q.exec();
        }
    db.commit();
    emit changed();
}
QVector<Video> Library::videos() const {
    QStringList roots, enabledRoots;
    for (const auto& f : folders()) {
        roots << f.path;
        if (f.enabled)
            enabledRoots << f.path;
    }
    QSqlQuery q(db);
    q.exec("SELECT id,path,title,duration,width,height,codec,audio,enabled,skipStart,skipEnd,error,missing "
           "FROM videos ORDER BY title");
    QVector<Video> out;
    while (q.next()) {
        Video v;
        v.id = q.value(0).toString();
        v.path = q.value(1).toString();
        v.title = q.value(2).toString();
        v.duration = q.value(3).toDouble();
        v.width = q.value(4).toInt();
        v.height = q.value(5).toInt();
        v.codec = q.value(6).toString();
        v.audio = q.value(7).toBool();
        v.enabled = q.value(8).toBool();
        v.skipStart = q.value(9).toDouble();
        v.skipEnd = q.value(10).toDouble();
        v.error = q.value(11).toString();
        v.missing = q.value(12).toBool();
        // Any enabled folder containing the video includes it. A row that matches no root at all
        // stays eligible, so an unexpected path shape can never silently empty the library.
        v.folderEnabled = !covered(roots, v.id) || covered(enabledRoots, v.id);
        out << v;
    }
    return out;
}
void Library::updateVideo(const QString& id, bool enabled, double a, double b) {
    QSqlQuery q(db);
    q.prepare("UPDATE videos SET enabled=?,skipStart=?,skipEnd=? WHERE id=?");
    q.addBindValue(enabled);
    q.addBindValue(a);
    q.addBindValue(b);
    q.addBindValue(id);
    q.exec();
    emit changed();
}
QJsonObject Library::value(const QString& key) const {
    QSqlQuery q(db);
    q.prepare("SELECT value FROM kv WHERE key=?");
    q.addBindValue(key);
    if (q.exec() && q.next())
        return QJsonDocument::fromJson(q.value(0).toByteArray()).object();
    return {};
}
void Library::setValue(const QString& key, const QJsonObject& value) {
    QSqlQuery q(db);
    q.prepare("INSERT OR REPLACE INTO kv VALUES(?,?)");
    q.addBindValue(key);
    q.addBindValue(QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)));
    q.exec();
}
QStringList Library::presets() const {
    QSqlQuery q(db);
    q.exec("SELECT key FROM kv WHERE key LIKE 'preset:%' ORDER BY key");
    QStringList out;
    while (q.next())
        out << q.value(0).toString().mid(7);
    return out;
}
QString Library::probePath() const {
    auto local = QCoreApplication::applicationDirPath() + "/ffprobe.exe";
    if (QFileInfo::exists(local))
        return local;
    return QStandardPaths::findExecutable("ffprobe");
}
void Library::ingest(const Video& v, qint64 size, qint64 modified) {
    QSqlQuery q(db);
    q.prepare("INSERT INTO videos(id,path,title,duration,width,height,codec,audio,error,size,modified) "
              "VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
              "path=excluded.path,title=excluded.title,duration=excluded.duration,width=excluded.width,"
              "height=excluded.height,codec=excluded.codec,audio=excluded.audio,error=excluded.error,size="
              "excluded.size,modified=excluded.modified,missing=0");
    for (const QVariant& a : QVariantList{v.id, v.path, v.title, v.duration, v.width, v.height, v.codec,
                                          v.audio, v.error, size, modified})
        q.addBindValue(a);
    q.exec();
}
void Library::scan() {
    if (worker)
        return;
    QString probe = probePath();
    if (probe.isEmpty()) {
        emit scanStatus("FFprobe missing. Place ffprobe.exe next to the player or on PATH.");
        return;
    }
    auto all = folders();
    if (all.empty()) {
        emit scanStatus("Add a folder to start indexing.");
        return;
    }
    // Disabled folders are skipped entirely, so a parked drive costs nothing to rescan.
    QStringList roots;
    for (const auto& f : all)
        if (f.enabled)
            roots << f.path;
    if (roots.empty()) {
        emit scanStatus("All library folders are disabled. Tick one to index it.");
        return;
    }
    QHash<QString, QPair<qint64, qint64>> cached;
    QSqlQuery q(db);
    q.exec("SELECT id,size,modified FROM videos WHERE error='' OR error IS NULL");
    while (q.next())
        cached[q.value(0).toString()] = {q.value(1).toLongLong(), q.value(2).toLongLong()};
    canceled = false;
    worker = QThread::create([this, roots, probe, cached] {
        QSet<QString> seen;
        int total = 0, probed = 0;
        const QSet<QString> extensions = {"mp4",  "mkv", "avi",  "mov",  "wmv",  "asf", "flv", "webm", "mpg",
                                          "mpeg", "m4v", "ts",   "mts",  "m2ts", "vob", "ogv", "ogg",  "3gp",
                                          "3g2",  "rm",  "rmvb", "divx", "m2v",  "m1v", "f4v", "mxf",  "dv",
                                          "nut",  "bik", "smk",  "roq",  "y4m",  "wtv"};
        for (const auto& root : roots) {
            QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
            while (!canceled && it.hasNext()) {
                QString path = it.next();
                QFileInfo info(path);
                if (!extensions.contains(info.suffix().toLower()))
                    continue;
                QString id = canonical(path);
                if (seen.contains(id))
                    continue;
                seen.insert(id);
                ++total;
                qint64 size = info.size(), modified = info.lastModified().toMSecsSinceEpoch();
                if (cached.contains(id) && cached[id] == qMakePair(size, modified))
                    continue;
                Video v;
                v.id = id;
                v.path = path;
                v.title = info.completeBaseName();
                QProcess process;
                process.setProcessChannelMode(QProcess::SeparateChannels);
                process.start(probe, {"-v", "error", "-show_format", "-show_streams", "-of", "json", path});
                QElapsedTimer timeout;
                timeout.start();
                while (!process.waitForFinished(100) && !canceled && timeout.elapsed() < 20000) {
                }
                if (canceled || process.state() != QProcess::NotRunning) {
                    process.kill();
                    process.waitForFinished(1000);
                    v.error = "Probe timed out";
                } else if (process.exitCode() != 0)
                    v.error = "Unsupported or unreadable media";
                else {
                    auto data = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
                    v.duration = data["format"].toObject()["duration"].toString().toDouble();
                    bool found = false;
                    for (auto stream : data["streams"].toArray()) {
                        auto s = stream.toObject();
                        auto type = s["codec_type"].toString();
                        if (type == "audio")
                            v.audio = true;
                        if (type == "video" && !s["disposition"].toObject()["attached_pic"].toInt() &&
                            !found) {
                            found = true;
                            v.codec = s["codec_name"].toString();
                            v.width = s["width"].toInt();
                            v.height = s["height"].toInt();
                            if (v.duration <= 0)
                                v.duration = s["duration"].toString().toDouble();
                        }
                    }
                    if (!found)
                        v.error = "No video stream";
                    else if (v.duration <= 0)
                        v.error = "Unknown duration";
                }
                if (canceled)
                    break;
                ++probed;
                QMetaObject::invokeMethod(
                    this, [this, v, size, modified] { ingest(v, size, modified); }, Qt::QueuedConnection);
                emit scanStatus(QString("Indexing %1 files · %2 probed · %3")
                                    .arg(total)
                                    .arg(probed)
                                    .arg(info.fileName()));
            }
            if (canceled)
                break;
        }
        bool completed = !canceled;
        QMetaObject::invokeMethod(
            this,
            [this, seen, completed, total, roots] {
                if (completed) {
                    db.transaction();
                    QSqlQuery q(db);
                    // Only folders this scan actually walked can testify that a file is gone.
                    // Videos under a disabled root were never looked for, so leave them alone.
                    for (const auto& v : videos())
                        if (covered(roots, v.id)) {
                            q.prepare("UPDATE videos SET missing=? WHERE id=?");
                            q.addBindValue(!seen.contains(v.id));
                            q.addBindValue(v.id);
                            q.exec();
                        }
                    db.commit();
                }
                emit changed();
                emit scanStatus(completed ? QString("Scan complete · %1 files").arg(total)
                                          : "Scan canceled; indexed files retained.");
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, this, [this] {
        auto old = worker;
        worker = nullptr;
        old->deleteLater();
        emit scanFinished();
    });
    emit scanStatus("Scanning folders…");
    worker->start();
}
void Library::cancelScan() {
    canceled = true;
}
} // namespace kaleido
