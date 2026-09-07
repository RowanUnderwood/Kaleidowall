#pragma once
#include "core.h"
#include <QObject>
#include <QSqlDatabase>
#include <QThread>
#include <atomic>

namespace kaleido {
class Library : public QObject {
    Q_OBJECT
  public:
    explicit Library(const QString& databasePath, QObject* parent = nullptr);
    ~Library() override;
    QString error() const {
        return dbError;
    }
    QVector<Folder> folders() const;
    void addFolder(const QString&);
    void removeFolder(const QString&);
    void setFolderEnabled(const QString& path, bool enabled);
    QVector<Video> videos() const;
    void updateVideo(const QString& id, bool enabled, double start, double end);
    QJsonObject value(const QString& key) const;
    bool setValue(const QString& key, const QJsonObject&);
    QStringList presets() const;
    bool removePreset(const QString& name);
    void scan();
    void cancelScan();
    bool scanning() const {
        return worker != nullptr;
    }
    QString probePath() const;
  signals:
    void changed();
    void scanStatus(const QString&);
    void scanFinished();

  private:
    void ingest(const Video&, qint64 size, qint64 modified);
    QSqlDatabase db;
    QString connectionName, dbError;
    QThread* worker = nullptr;
    std::atomic_bool canceled = false;
};
} // namespace kaleido
