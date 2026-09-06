#pragma once
#include <QJsonObject>
#include <QRectF>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <optional>
#include <random>

namespace kaleido {
struct Settings {
    int minSlots = 2, maxSlots = 6;
    double clipMin = 12, clipMax = 35, layoutMin = 15, layoutMax = 30, transition = 1.4;
    double skipStart = 0, skipEnd = 0;
    bool skipPercent = false, muted = true, reducedMotion = false, duplicates = false, crop = true,
         hwdec = true;
    int volume = 65, fps = 60, bufferMiB = 64, textureLimit = 1920;
    QString backgroundColor = "#06080c";
    QStringList modes = {"Split", "Grid", "Hero", "Masonry", "Circles", "Hexagons"};
    QJsonObject weights;
    void normalize();
    QJsonObject json() const;
    static Settings fromJson(const QJsonObject&);
};
struct Folder {
    QString path;
    bool enabled = true;
};
struct Video {
    QString id, path, title, codec, error;
    double duration = 0, skipStart = -1, skipEnd = -1;
    int width = 0, height = 0;
    bool audio = false, enabled = true, missing = false;
    // Derived from the owning folders at query time; never stored on the video row.
    bool folderEnabled = true;
};
struct Clip {
    double start = 0, length = 0;
};
std::optional<Clip> chooseClip(const Video&, const Settings&, std::mt19937&);
QString eligibilityReason(const Video&, const Settings&);

// A reservation is consumed only after playback starts. Stored state includes
// reservations as remaining entries, so a crash cannot silently consume clips.
class ShuffleBag {
  public:
    explicit ShuffleBag(unsigned seed = std::random_device{}()) : rng(seed) {}
    void reconcile(const QStringList& eligible);
    std::optional<QString> reserve(const QSet<QString>& active, bool duplicates = false);
    void commit(const QString&);
    void cancel(const QString&);
    QJsonObject json() const;
    void restore(const QJsonObject&);
    int remainingCount() const {
        return remaining.size();
    }
    int cycle() const {
        return cycleNumber;
    }

  private:
    QStringList pool, remaining;
    QSet<QString> used, reserved;
    std::mt19937 rng;
    int cycleNumber = 1;
};
QVector<QRectF> makeLayout(const QString& mode, int count, double aspect, std::mt19937&);
QString pickMode(const Settings&, std::mt19937&, const QString& previous = {});
double randomRange(std::mt19937&, double low, double high);
} // namespace kaleido
