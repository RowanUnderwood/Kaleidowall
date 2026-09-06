#pragma once
#include "core.h"
#include <QImage>
#include <QMutex>
#include <QOffscreenSurface>
#include <QThread>
#include <atomic>

namespace kaleido {
struct ExportOptions {
    QSize size{1920, 1080};
    int fps = 60, quality = 2, volume = 65;
    double duration = 60;
    QString destination, audioFile;
    // 0: no audio, 1: follow clips, 2: imported soundtrack
    int audioMode = 0;
    bool softwareEncoder = false, overwrite = false;
    unsigned seed = 0;
    qint64 frameCount() const;
    double outputDuration() const;
    QString validate() const;
};
QString mediaTool(const QString& name);

class ExportWorker : public QThread {
    Q_OBJECT
  public:
    ExportWorker(ExportOptions options, Settings settings, QVector<Video> media, QOffscreenSurface* surface,
                 QObject* parent = nullptr);
    ~ExportWorker() override;
    void cancel() {
        canceled.store(true);
    }
    QImage takePreview();
    QJsonObject report; // Read only after finished().
  signals:
    void progress(const QString& stage, qint64 completed, qint64 total, double elapsed);
    void result(bool success, bool wasCanceled, const QString& message);

  protected:
    void run() override;

  private:
    ExportOptions options;
    Settings settings;
    QVector<Video> media;
    QOffscreenSurface* surface;
    std::atomic_bool canceled{false};
    QMutex previewMutex;
    QImage preview;
};
} // namespace kaleido
