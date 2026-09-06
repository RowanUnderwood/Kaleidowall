#pragma once
#include "compositor.h"
#include "core.h"
#include "library.h"
#include "mpv_backend.h"
#include <QChronoTimer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QTimer>
#include <memory>
#include <vector>

namespace kaleido {
class Canvas : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
  public:
    explicit Canvas(Library*, QWidget* parent = nullptr);
    ~Canvas() override;
    void applySettings(const Settings&);
    void setAudio(bool muted, int volume);
    Settings settings() const {
        return config;
    }
    void playPause();
    void stop();
    void nextLayout();
    void nextClips();
    void nextAudio();
    void setExportLocked(bool locked) {
        exportLocked = locked;
    }
    bool playing() const {
        return running;
    }
    bool isPaused() const {
        return paused;
    }
    QJsonObject diagnostics() const;
    QString performanceText() const;
    void beginProfile(unsigned seed = 42) {
        rng.seed(seed);
        profiling = true;
        profileEvents = {};
    }
    QJsonArray profileData() const {
        return profileEvents;
    }
  signals:
    void status(const QString&);
    void playbackChanged();
    void settingsChanged();
    void interaction();
    void fullscreenRequested();

  protected:
    void initializeGL() override;
    void paintGL() override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

  private:
    struct Slot {
        int serial = 0;
        std::unique_ptr<Decoder> decoder;
        std::unique_ptr<QOpenGLFramebufferObject> fbo;
        Video video;
        int displayWidth = 1, displayHeight = 1;
        QString reserved;
        Clip clip;
        QRectF from, target;
        double opacityFrom = 1, opacityTarget = 1;
        bool loading = false, ready = false, retiring = false, hasFrame = false;
        bool cutRequested = false;
        double loadStarted = 0, retryAt = 0, cutAt = 0;
        double lastFrameAt = 0, visibleAt = 0;
        bool awaitingMotion = false;
    };
    void tick();
    void scheduleTick();
    void recordTiming(const QString& stage, double ms);
    void reloadLibrary();
    bool loadNext(Slot&);
    void prepareClips();
    void recycle(Slot&);
    void cutPreparedClips();
    void routeAudio();
    void persistShuffle();
    void clearSlots();
    void warmPool(int count);
    void finishTransition();
    QRectF rectangle(const Slot&) const;
    double progress() const;
    double wallSeconds() const {
        return clock.nsecsElapsed() / 1e9;
    }
    Library* library;
    Settings config;
    MpvApi api;
    ShuffleBag bag;
    QHash<QString, Video> videos;
    QSet<QString> failed;
    std::vector<std::unique_ptr<Slot>> players;
    std::vector<std::unique_ptr<Slot>> spares;
    std::mt19937 rng{std::random_device{}()};
    QChronoTimer timer;
    QElapsedTimer clock;
    double lastTick = 0, sessionTime = 0, nextLayoutAt = 0, transitionStart = 0, transitionDuration = 0;
    double lastStats = 0, paintMs = 0, fpsMeasured = 0;
    int frames = 0, nextSerial = 1, audioSerial = -1, fromMask = 0, targetMask = 0;
    bool initialized = false, available = false, running = false, paused = false;
    bool exportLocked = false;
    QString mode = "Grid", rendererName, lastError;
    Compositor compositor;
    bool profiling = false;
    QJsonArray profileEvents;
    double lastPaintAt = 0;
    double nextTickAt = 0;
    QVector<double> frameGaps;
    double frameGapP95 = 0, frameGapMax = 0, schedulerMax = 0, schedulerPeak = 0;
    int textureUpdates = 0, textureUpdatesPerSecond = 0;
    int cleanCuts = 0, delayedCuts = 0;
    double lastCutDelay = 0, lastPreloadMs = 0;
};
} // namespace kaleido
