#pragma once
#include "core.h"
#include <vector>

namespace kaleido {
struct ExportSlot {
    int serial = 0, stream = 0;
    Video video;
    Clip clip;
    double begins = 0, usableEnd = 0;
    QRectF from, target;
    double opacityFrom = 0, opacityTarget = 1;
    bool retiring = false;
};
struct AudioSpan {
    QString path;
    double sourceStart = 0, start = 0, end = 0;
    int stream = 0;
};
// Independent export session. Shares selection/layout/easing/shaders with Play, but never its clock or
// database.
class ExportTimeline {
  public:
    ExportTimeline(Settings settings, QVector<Video> media, unsigned seed, double aspect);
    void advance(double time);
    const std::vector<ExportSlot>& segments() const {
        return active;
    }
    double progress() const;
    int oldMask = 0, newMask = 0;
    const ExportSlot* audioSlot() const;
    QString error;

  private:
    void layout();
    bool replace(ExportSlot& slot);
    void routeAudio();
    Settings config;
    QHash<QString, Video> videos;
    ShuffleBag bag;
    std::mt19937 rng;
    std::vector<ExportSlot> active;
    QString mode = "Grid";
    double now = 0, nextLayout = 0, transitionStart = 0, transitionDuration = 0, aspect;
    int nextSerial = 1, nextStream = 1, audioSerial = -1;
};
} // namespace kaleido
