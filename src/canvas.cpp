#include "canvas.h"
#include <QJsonArray>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QScopeGuard>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <algorithm>
#include <cmath>

namespace kaleido {
Canvas::Canvas(Library* lib, QWidget* parent) : QOpenGLWidget(parent), library(lib) {
    setMinimumSize(320, 240);
    setMouseTracking(true);
    config = Settings::fromJson(library->value("settings"));
    bag.restore(library->value("shuffle"));
    reloadLibrary();
    connect(library, &Library::changed, this, &Canvas::reloadLibrary);
    clock.start();
    connect(&timer, &QChronoTimer::timeout, this, &Canvas::tick);
    timer.setTimerType(Qt::PreciseTimer);
    timer.setSingleShot(true);
    nextTickAt = wallSeconds() + 1.0 / config.fps;
    timer.setInterval(std::chrono::nanoseconds(1000000000 / config.fps));
    timer.start();
    connect(this, &QOpenGLWidget::frameSwapped, this, [this] {
        for (auto* collection : {&players, &spares})
            for (auto& s : *collection)
                if (s->ready)
                    api.render_context_report_swap(s->decoder->renderer);
    });
}
Canvas::~Canvas() {
    timer.stop();
    persistShuffle();
    makeCurrent();
    clearSlots();
    if (initialized) {
        compositor.release();
    }
    doneCurrent();
}
void Canvas::initializeGL() {
    initialized = initializeOpenGLFunctions();
    if (!initialized) {
        lastError = "OpenGL 3.3 is unavailable.";
        emit status(lastError);
        return;
    }
    rendererName = QString::fromLatin1(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    available = api.load();
    if (!available) {
        lastError = "libmpv could not load: " + api.error;
        emit status(lastError);
    }
    if (!compositor.initialize()) {
        available = false;
        lastError = compositor.error();
        emit status(lastError);
    }
}

void Canvas::applySettings(const Settings& s) {
    if (exportLocked)
        return;
    const bool changeSelection = s.clipMin != config.clipMin || s.clipMax != config.clipMax ||
                                 s.skipStart != config.skipStart || s.skipEnd != config.skipEnd ||
                                 s.skipPercent != config.skipPercent || s.duplicates != config.duplicates;
    const bool changeLayout = s.minSlots != config.minSlots || s.maxSlots != config.maxSlots ||
                              s.modes != config.modes || s.weights != config.weights ||
                              s.reducedMotion != config.reducedMotion || s.duplicates != config.duplicates;
    config = s;
    config.normalize();
    nextTickAt = wallSeconds() + 1.0 / config.fps;
    timer.setInterval(std::chrono::nanoseconds(1000000000 / config.fps));
    timer.start();
    library->setValue("settings", config.json());
    for (auto& slot : players) {
        slot->decoder->set("hwdec", config.hwdec ? "auto-safe" : "no");
        slot->decoder->set("demuxer-max-bytes", QString::number(qint64(config.bufferMiB) * 1024 * 1024));
    }
    for (auto& slot : spares) {
        // A prepared seek may no longer satisfy the new duration/exclusions.
        if (changeSelection && (slot->loading || slot->ready))
            recycle(*slot);
        slot->decoder->set("hwdec", config.hwdec ? "auto-safe" : "no");
        slot->decoder->set("demuxer-max-bytes", QString::number(qint64(config.bufferMiB) * 1024 * 1024));
    }
    reloadLibrary();
    routeAudio();
    if (running && changeLayout)
        nextLayout();
    emit settingsChanged();
    update();
}
void Canvas::setAudio(bool muted, int volume) {
    if (exportLocked)
        return;
    config.muted = muted;
    config.volume = std::clamp(volume, 0, 100);
    routeAudio();
    library->setValue("settings", config.json());
}
void Canvas::reloadLibrary() {
    videos.clear();
    QStringList ids;
    for (const auto& v : library->videos())
        if (eligibilityReason(v, config).isEmpty() && !failed.contains(v.id)) {
            videos[v.id] = v;
            ids << v.id;
        }
    bag.reconcile(ids);
    persistShuffle();
    if (running && initialized) {
        for (auto& s : spares)
            if ((s->loading || s->ready) &&
                (!videos.contains(s->video.id) || videos.value(s->video.id).skipStart != s->video.skipStart ||
                 videos.value(s->video.id).skipEnd != s->video.skipEnd))
                recycle(*s);
        for (auto& s : players)
            if (!s->retiring && !videos.contains(s->video.id))
                s->cutRequested = true;
    }
    update();
}
void Canvas::persistShuffle() {
    library->setValue("shuffle", bag.json());
}
void Canvas::playPause() {
    if (exportLocked)
        return;
    if (!running) {
        if (!available) {
            emit status(lastError.isEmpty() ? "Video renderer is not ready." : lastError);
            return;
        }
        if (videos.empty()) {
            emit status("No eligible videos. Add a folder in Library, scan it, or adjust exclusions.");
            return;
        }
        running = true;
        paused = false;
        sessionTime = 0;
        nextLayout();
    } else {
        paused = !paused;
        for (auto& s : players)
            s->decoder->set("pause", paused ? "yes" : "no");
    }
    emit playbackChanged();
    update();
}
void Canvas::clearSlots() {
    for (auto* collection : {&players, &spares})
        for (auto& s : *collection)
            if (!s->reserved.isEmpty())
                bag.cancel(s->reserved);
    players.clear();
    spares.clear();
    audioSerial = -1;
}
void Canvas::stop() {
    if (exportLocked)
        return;
    makeCurrent();
    clearSlots();
    doneCurrent();
    running = false;
    paused = false;
    persistShuffle();
    emit playbackChanged();
    emit status("Stopped");
    update();
}
double Canvas::progress() const {
    return easedProgress(sessionTime, transitionStart, transitionDuration);
}
QRectF Canvas::rectangle(const Slot& s) const {
    return interpolateRect(s.from, s.target, progress());
}

void Canvas::finishTransition() {
    QElapsedTimer measure;
    measure.start();
    auto measured = qScopeGuard([&] { recordTiming("retire", measure.nsecsElapsed() / 1e6); });
    if (progress() < 1)
        return;
    makeCurrent();
    for (auto it = players.begin(); it != players.end();) {
        if (!(*it)->retiring) {
            ++it;
            continue;
        }
        auto slot = std::move(*it);
        it = players.erase(it);
        recycle(*slot);
        // Stop the file asynchronously, retaining the mpv/render context and
        // texture allocation for the next layout instead of destroying them.
        spares.push_back(std::move(slot));
    }
    doneCurrent();
    routeAudio();
}
void Canvas::warmPool(int count) {
    if (int(players.size() + spares.size()) >= count)
        return;
    makeCurrent();
    while (int(players.size() + spares.size()) < count) {
        auto slot = std::make_unique<Slot>();
        slot->serial = nextSerial++;
        slot->decoder = std::make_unique<Decoder>(api);
        mpv_opengl_init_params gl{};
        gl.get_proc_address = [](void*, const char* name) -> void* {
            return reinterpret_cast<void*>(QOpenGLContext::currentContext()->getProcAddress(name));
        };
        if (!slot->decoder->init(config.hwdec, config.bufferMiB, gl)) {
            lastError = slot->decoder->error;
            emit status(lastError);
            break;
        }
        spares.push_back(std::move(slot));
    }
    doneCurrent();
}
void Canvas::nextLayout() {
    if (exportLocked)
        return;
    QElapsedTimer measure;
    measure.start();
    auto measured = qScopeGuard([&] { recordTiming("layout", measure.nsecsElapsed() / 1e6); });
    if (!running || !initialized)
        return;
    // Complete an outstanding transition before choosing a new topology.
    transitionDuration = 0;
    finishTransition();
    int cap = config.duplicates ? config.maxSlots : std::min(config.maxSlots, int(videos.size()));
    if (cap <= 0) {
        stop();
        emit status("No eligible videos remain.");
        return;
    }
    // One on-screen decoder and one hidden preparation decoder per maximum
    // segment. Contexts are created here, never on the normal clip-cut path.
    warmPool(cap * 2);
    int low = std::min(config.minSlots, cap), count = std::uniform_int_distribution<int>(low, cap)(rng);
    // Rapid manual changes can arrive before an asynchronous stop completes.
    // Lay out the available players, rather than leaving holes for pending ones.
    const int readySpareCount = int(std::count_if(spares.begin(), spares.end(), [](const auto& s) {
        return !s->decoder->stopPending && !s->loading && !s->ready;
    }));
    count = std::min(count, int(players.size()) + readySpareCount);
    if (count == 0)
        return;
    if (cap < config.minSlots)
        emit status(QString("Using %1 players: only %2 eligible videos.").arg(cap).arg(videos.size()));
    fromMask = targetMask;
    mode = pickMode(config, rng, mode);
    layoutMode = resolveLayoutMode(mode, count, rng);
    targetMask = count == 1 && mode != "Inset" ? 0 : maskKind(layoutMode);
    auto layout = makeLayout(layoutMode, count, double(width()) / std::max(1, height()), rng);
    for (auto& s : players) {
        s->from = s->target;
        s->opacityFrom = 1;
    }
    for (int i = int(players.size()); i < count; ++i) {
        auto ready = std::find_if(spares.begin(), spares.end(), [](const auto& s) {
            return !s->decoder->stopPending && !s->loading && !s->ready;
        });
        if (ready == spares.end())
            break;
        auto slot = std::move(*ready);
        spares.erase(ready);
        while (auto* event = api.wait_event(slot->decoder->handle, 0)) {
            if (event->event_id == MPV_EVENT_NONE)
                break;
            slot->decoder->handleEvent(*event);
        }
        slot->retryAt = 0;
        slot->hasFrame = false;
        slot->cutRequested = true;
        slot->from = layout[i];
        slot->target = layout[i];
        slot->opacityFrom = 0;
        slot->opacityTarget = 1;
        slot->decoder->set("pause", paused ? "yes" : "no");
        players.push_back(std::move(slot));
    }
    for (int i = 0; i < int(players.size()); ++i) {
        auto& s = *players[i];
        s.retiring = i >= count;
        if (s.retiring) {
            s.opacityTarget = 0;
        } else {
            s.target = layout[i];
            s.opacityTarget = 1;
        }
    }
    transitionStart = sessionTime;
    transitionDuration = config.reducedMotion ? 0 : config.transition;
    nextLayoutAt = sessionTime + randomRange(rng, config.layoutMin, config.layoutMax);
    prepareClips();
    routeAudio();
    emit playbackChanged();
    update();
}
bool Canvas::loadNext(Slot& slot) {
    QElapsedTimer measure;
    measure.start();
    auto measured = qScopeGuard([&] { recordTiming("load", measure.nsecsElapsed() / 1e6); });
    if (slot.loading)
        return false;
    if (!slot.reserved.isEmpty()) {
        bag.cancel(slot.reserved);
        slot.reserved.clear();
    }
    // Hidden reservations can include an on-screen video from the previous
    // cycle. Simultaneous uniqueness is checked again at the visible cut.
    auto id = bag.reserve({});
    if (!id)
        return false;
    auto clip = chooseClip(videos.value(*id), config, rng);
    if (!clip) {
        bag.cancel(*id);
        slot.retryAt = sessionTime + 1;
        return false;
    }
    slot.reserved = *id;
    slot.video = videos.value(*id);
    slot.clip = *clip;
    slot.loading = true;
    slot.ready = false;
    slot.hasFrame = false;
    slot.loadStarted = wallSeconds();
    slot.decoder->set("mute", "yes");
    slot.decoder->set("pause", "yes");
    // The compositor owns the cut deadline. Leave a safe tail available if a
    // slow seek or shuffle boundary delays the handoff; never enter skip-end.
    const double skipEnd = slot.video.skipEnd >= 0
                               ? slot.video.skipEnd
                               : config.skipEnd * (config.skipPercent ? slot.video.duration / 100.0 : 1.0);
    if (!slot.decoder->loadFile(slot.video.path, clip->start, slot.video.duration - skipEnd - clip->start)) {
        bag.cancel(*id);
        slot.reserved.clear();
        slot.loading = false;
        failed.insert(*id);
        lastError = slot.decoder->error;
        emit status(lastError);
        videos.remove(*id);
        bag.reconcile(videos.keys());
        return false;
    }
    persistShuffle();
    return true;
}
void Canvas::recycle(Slot& slot) {
    if (!slot.reserved.isEmpty())
        bag.cancel(slot.reserved);
    slot.reserved.clear();
    slot.decoder->stopPlayback();
    slot.loading = slot.ready = slot.retiring = slot.hasFrame = slot.cutRequested = false;
    slot.awaitingMotion = false;
    slot.video = {};
    slot.retryAt = 0;
}
void Canvas::prepareClips() {
    if (!running)
        return;
    const int target =
        int(std::count_if(players.begin(), players.end(), [](const auto& s) { return !s->retiring; }));
    int pending = int(
        std::count_if(spares.begin(), spares.end(), [](const auto& s) { return s->loading || s->ready; }));
    // Drop excess reservations after shrinking the layout, so they cannot
    // prevent the remaining player from completing its shuffle cycle.
    for (auto it = spares.rbegin(); pending > target && it != spares.rend(); ++it)
        if ((*it)->loading || (*it)->ready) {
            recycle(**it);
            --pending;
        }
    for (auto& s : spares) {
        if (pending >= target)
            break;
        if (s->loading || s->ready || s->decoder->stopPending || sessionTime < s->retryAt)
            continue;
        if (loadNext(*s))
            ++pending;
    }
}
void Canvas::cutPreparedClips() {
    if (!running || paused)
        return;
    bool changed = false;
    for (auto& s : players) {
        if (s->retiring || (s->hasFrame && !s->cutRequested && sessionTime < s->cutAt))
            continue;
        auto next = std::find_if(spares.begin(), spares.end(), [&](const auto& candidate) {
            if (!candidate->ready || !candidate->hasFrame || candidate->reserved.isEmpty())
                return false;
            return config.duplicates || std::none_of(players.begin(), players.end(), [&](const auto& p) {
                       return p.get() != s.get() && p->hasFrame && p->video.id == candidate->video.id;
                   });
        });
        if (next == spares.end())
            continue;
        auto& incoming = **next;
        const bool replacement = s->hasFrame;
        lastCutDelay = replacement && !s->cutRequested ? std::max(0.0, sessionTime - s->cutAt) : 0;
        if (replacement) {
            ++cleanCuts;
            if (lastCutDelay > 2.0 / config.fps)
                ++delayedCuts;
        }
        if (profiling)
            profileEvents.append(QJsonObject{{"stage", "cut"},
                                             {"ms", lastCutDelay * 1000},
                                             {"at", wallSeconds()},
                                             {"slot", s->serial},
                                             {"replacement", replacement},
                                             {"from", s->video.id},
                                             {"to", incoming.video.id},
                                             {"start", incoming.clip.start},
                                             {"cycle", bag.cycle()},
                                             {"outgoingEof", s->decoder->string("eof-reached") == "yes"},
                                             {"outgoingFrameAgeMs", (wallSeconds() - s->lastFrameAt) * 1000},
                                             {"preparedMs", (wallSeconds() - incoming.loadStarted) * 1000}});
        // Exchange only media resources. The segment identity, geometry and
        // layout animation continue independently of the cut.
        bag.commit(incoming.reserved);
        incoming.reserved.clear();
        std::swap(s->decoder, incoming.decoder);
        std::swap(s->fbo, incoming.fbo);
        std::swap(s->video, incoming.video);
        std::swap(s->clip, incoming.clip);
        std::swap(s->displayWidth, incoming.displayWidth);
        std::swap(s->displayHeight, incoming.displayHeight);
        s->loading = false;
        s->ready = s->hasFrame = true;
        s->cutRequested = false;
        s->cutAt = sessionTime + s->clip.length;
        s->visibleAt = wallSeconds();
        s->lastFrameAt = incoming.lastFrameAt;
        s->awaitingMotion = true;
        s->decoder->set("pause", "no");
        recycle(incoming); // Async mute/stop; keep its context and texture.
        changed = true;
    }
    if (changed) {
        persistShuffle();
        routeAudio();
    }
}
void Canvas::nextClips() {
    if (exportLocked)
        return;
    if (!running)
        return;
    for (auto& s : players)
        if (!s->retiring)
            s->cutRequested = true;
    prepareClips();
    update();
}
void Canvas::nextAudio() {
    if (exportLocked)
        return;
    audioSerial = -1;
    QVector<int> options;
    for (auto& s : players)
        if (!s->retiring && s->ready && s->video.audio)
            options << s->serial;
    if (!options.empty())
        audioSerial = options[std::uniform_int_distribution<int>(0, int(options.size() - 1))(rng)];
    routeAudio();
}
void Canvas::routeAudio() {
    QElapsedTimer measure;
    measure.start();
    auto measured = qScopeGuard([&] { recordTiming("audio", measure.nsecsElapsed() / 1e6); });
    bool valid = false;
    for (auto& s : players)
        if (s->serial == audioSerial && !s->retiring && s->ready && s->video.audio)
            valid = true;
    if (!valid) {
        audioSerial = -1;
        QVector<int> ids;
        for (auto& s : players)
            if (!s->retiring && s->ready && s->video.audio)
                ids << s->serial;
        if (!ids.empty())
            audioSerial = ids[std::uniform_int_distribution<int>(0, int(ids.size() - 1))(rng)];
    }
    // Mute all other instances before enabling the chosen source.
    for (auto& s : players)
        if (config.muted || s->serial != audioSerial)
            s->decoder->set("mute", "yes");
    for (auto& s : players) {
        s->decoder->set("volume", QString::number(config.volume));
    }
    for (auto& s : spares)
        s->decoder->set("mute", "yes");
    const bool othersMuted =
        std::all_of(
            players.begin(), players.end(),
            [this](const auto& s) { return s->serial == audioSerial || s->decoder->muteConfirmed; }) &&
        std::all_of(spares.begin(), spares.end(), [](const auto& s) { return s->decoder->muteConfirmed; });
    if (!config.muted && othersMuted)
        for (auto& s : players)
            if (s->serial == audioSerial)
                s->decoder->set("mute", "no");
}
void Canvas::tick() {
    QElapsedTimer measure;
    measure.start();
    auto measured = qScopeGuard([&] {
        recordTiming("tick", measure.nsecsElapsed() / 1e6);
        scheduleTick();
    });
    double now = wallSeconds(), dt = now - lastTick;
    lastTick = now;
    if (running && !paused) {
        sessionTime += std::min(dt, 0.25);
        if (sessionTime >= nextLayoutAt)
            nextLayout();
        if (progress() >= 1 &&
            std::any_of(players.begin(), players.end(), [](const auto& s) { return s->retiring; }))
            finishTransition();
    }
    bool audioDirty = false;
    for (auto* collection : {&spares, &players}) {
        const bool hidden = collection == &spares;
        for (auto& s : *collection) {
            bool bad = false;
            while (auto* event = api.wait_event(s->decoder->handle, 0)) {
                if (event->event_id == MPV_EVENT_NONE)
                    break;
                s->decoder->handleEvent(*event);
                if (event->event_id == MPV_EVENT_SET_PROPERTY_REPLY)
                    audioDirty = true;
                if (event->event_id == MPV_EVENT_PLAYBACK_RESTART && s->loading) {
                    s->loading = false;
                    s->ready = true;
                    lastPreloadMs = (now - s->loadStarted) * 1000;
                    recordTiming("preload", lastPreloadMs);
                    // Reservation stays unconsumed until its frame is visible.
                }
                if (event->event_id == MPV_EVENT_END_FILE && (s->loading || s->ready)) {
                    const auto* endedFile = static_cast<mpv_event_end_file*>(event->data);
                    if (endedFile->reason == MPV_END_FILE_REASON_ERROR)
                        bad = true;
                    else if (!hidden && endedFile->reason == MPV_END_FILE_REASON_EOF)
                        s->cutRequested = true;
                }
                if (event->event_id == MPV_EVENT_COMMAND_REPLY && event->reply_userdata == 1 &&
                    event->error < 0 && s->loading)
                    bad = true;
            }
            if (s->loading && now - s->loadStarted > 20)
                bad = true;
            if (bad) {
                const auto id = s->video.id;
                failed.insert(id);
                lastError = "Skipped playback failure: " + s->video.title;
                emit status(lastError);
                if (hidden)
                    recycle(*s);
                else {
                    s->ready = false;
                    s->cutRequested = true;
                }
                videos.remove(id);
                bag.reconcile(videos.keys());
                persistShuffle();
                audioDirty = true;
            }
        }
    }
    prepareClips();
    if (audioDirty)
        routeAudio();
    if (running && videos.isEmpty()) {
        stop();
        emit status(
            "All eligible files failed playback. Check the performance panel, then restart to retry.");
    }
    if (now - lastStats >= 1) {
        fpsMeasured = frames / (now - lastStats);
        std::sort(frameGaps.begin(), frameGaps.end());
        frameGapMax = frameGaps.isEmpty() ? 0 : frameGaps.back();
        frameGapP95 = frameGaps.isEmpty()
                          ? 0
                          : frameGaps[std::min(int(frameGaps.size() - 1), int(frameGaps.size() * .95))];
        frameGaps.clear();
        schedulerMax = schedulerPeak;
        schedulerPeak = 0;
        textureUpdatesPerSecond = qRound(textureUpdates / (now - lastStats));
        textureUpdates = 0;
        frames = 0;
        lastStats = now;
    }
    if (isVisible())
        update();
}
void Canvas::scheduleTick() {
    // Alternate fractional frame intervals against a monotonic deadline. A
    // repeating 16.667 ms timer can round every tick to 17 ms on Windows.
    const double period = 1.0 / config.fps, now = wallSeconds();
    nextTickAt += period;
    if (nextTickAt <= now)
        nextTickAt = now + period; // No burst of catch-up frames.
    timer.setInterval(std::chrono::nanoseconds(qint64(std::ceil((nextTickAt - now) * 1e9))));
    timer.start();
}
void Canvas::resizeGL(int w, int h) {
    if (!layoutMode.endsWith("Honeycomb") || w <= 0 || h <= 0)
        return;
    const int count = int(std::count_if(players.begin(), players.end(),
                                        [](const auto& s) { return !s->retiring; }));
    const auto layout = makeLayout(layoutMode, count, double(w) / h, rng);
    int i = 0;
    for (auto& s : players)
        if (!s->retiring)
            s->target = layout[i++];
}
void Canvas::paintGL() {
    if (!initialized)
        return;
    QElapsedTimer elapsed;
    elapsed.start();
    const double paintAt = clock.nsecsElapsed() / 1e6;
    if (running && lastPaintAt > 0)
        recordTiming("frameGap", paintAt - lastPaintAt);
    lastPaintAt = paintAt;
    const int w = qRound(width() * devicePixelRatioF()), h = qRound(height() * devicePixelRatioF());
    for (auto* collection : {&players, &spares}) {
        for (auto& s : *collection) {
            if (!s->ready)
                continue;
            int tw = std::clamp(s->video.width, 16, config.textureLimit),
                th = std::max(
                    16, qRound(double(tw) * std::max(1, s->video.height) / std::max(1, s->video.width)));
            if (th > config.textureLimit) {
                tw = qRound(double(tw) * config.textureLimit / th);
                th = config.textureLimit;
            }
            const bool resized = !s->fbo || s->fbo->size() != QSize(tw, th);
            if (resized)
                s->fbo = std::make_unique<QOpenGLFramebufferObject>(tw, th);
            if (s->decoder->render(int(s->fbo->handle()), tw, th, resized || !s->hasFrame)) {
                ++textureUpdates;
                s->hasFrame = true;
                s->lastFrameAt = wallSeconds();
                if (s->awaitingMotion && !paused) {
                    recordTiming("cutFirstUpdate", (wallSeconds() - s->visibleAt) * 1000);
                    s->awaitingMotion = false;
                }
            }
            s->displayWidth = s->video.width;
            s->displayHeight = s->video.height;
        }
    }
    cutPreparedClips();
    const QColor background(config.backgroundColor);
    std::vector<DrawTile> tiles;
    const double t = progress();
    for (const auto& s : players)
        if (s->fbo && s->hasFrame)
            tiles.push_back({s->fbo->texture(),
                             QSize(std::max(1, s->displayWidth), std::max(1, s->displayHeight)),
                             rectangle(*s), float(s->opacityFrom * (1 - t) + s->opacityTarget * t),
                             0, s.get() == players.front().get()});
    if (available)
        compositor.draw(defaultFramebufferObject(), QSize(w, h), size(), background, config.crop, fromMask,
                        targetMask, float(t), tiles);
    else {
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        glClearColor(background.redF(), background.greenF(), background.blueF(), 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (players.empty()) {
        painter.fillRect(rect(), background);
        const bool lightBackground =
            background.redF() * .2126 + background.greenF() * .7152 + background.blueF() * .0722 > .5;
        painter.setPen(QPen(QColor(80, 113, 146, 32), 1));
        for (int x = 0; x < width(); x += 64)
            painter.drawLine(x, 0, x, height());
        for (int y = 0; y < height(); y += 64)
            painter.drawLine(0, y, width(), y);
        QRectF art(width() / 2.0 - 105, height() / 2.0 - 115, 210, 105);
        const QColor colors[] = {QColor("#284d56"), QColor("#303e60"), QColor("#3e345c"), QColor("#24454d")};
        auto preview = makeLayout("Hero", 5, 2, rng);
        int i = 0;
        for (auto r : preview) {
            QRectF tile(art.x() + r.x() * art.width(), art.y() + r.y() * art.height(),
                        r.width() * art.width() - 5, r.height() * art.height() - 5);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors[i++ % 4]);
            painter.drawRoundedRect(tile, 7, 7);
        }
        painter.setPen(QColor(lightBackground ? "#172033" : "#edf5ff"));
        painter.setFont(QFont("Segoe UI", 24, QFont::DemiBold));
        painter.drawText(QRect(0, height() / 2 + 10, width(), 45), Qt::AlignCenter,
                         "Your library. A different view.");
        painter.setPen(QColor(lightBackground ? "#34445b" : "#8494ad"));
        painter.setFont(QFont("Segoe UI", 11));
        painter.drawText(QRect(20, height() / 2 + 62, width() - 40, 60), Qt::AlignHCenter | Qt::TextWordWrap,
                         available ? "Add a video folder in Library, then press Play.\nClips and layouts "
                                     "change on their own schedules."
                                   : "Playback runtime unavailable. Check the status bar.");
    }
    if (paused) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(8, 12, 20, 180));
        painter.drawRoundedRect(QRect(width() / 2 - 60, 24, 120, 36), 18, 18);
        painter.setPen(Qt::white);
        painter.drawText(QRect(width() / 2 - 60, 24, 120, 36), Qt::AlignCenter, "PAUSED");
    }
    painter.end();
    paintMs = elapsed.nsecsElapsed() / 1e6;
    recordTiming("paint", paintMs);
    ++frames;
}
void Canvas::recordTiming(const QString& stage, double ms) {
    if (stage == "tick")
        schedulerPeak = std::max(schedulerPeak, ms);
    if (stage == "frameGap")
        frameGaps.append(ms);
    if (profiling)
        profileEvents.append(QJsonObject{{"stage", stage},
                                         {"ms", ms},
                                         {"at", wallSeconds()},
                                         {"transition", progress() < 1},
                                         {"slots", int(players.size())}});
}
QJsonObject Canvas::diagnostics() const {
    QJsonArray items;
    QJsonArray idle;
    for (const auto& s : spares)
        idle.append(QJsonObject{{"slot", s->serial},
                                {"title", s->video.title},
                                {"loading", s->loading},
                                {"prepared", s->ready && s->hasFrame},
                                {"reserved", s->reserved},
                                {"paused", s->decoder->string("pause")},
                                {"muted", s->decoder->string("mute")},
                                {"stopPending", s->decoder->stopPending}});
    for (auto& s : players)
        items.append(QJsonObject{{"slot", s->serial},
                                 {"title", s->video.title},
                                 {"ready", s->ready},
                                 {"loading", s->loading},
                                 {"retiring", s->retiring},
                                 {"start", s->clip.start},
                                 {"length", s->clip.length},
                                 {"cutAt", s->cutAt},
                                 {"frameAgeMs", (wallSeconds() - s->lastFrameAt) * 1000},
                                 {"time", s->decoder->number("time-pos")},
                                 {"hwdec", s->decoder->string("hwdec-current")},
                                 {"dropped", s->decoder->number("decoder-frame-drop-count")},
                                 {"muted", s->decoder->string("mute")},
                                 {"width", s->video.width},
                                 {"height", s->video.height}});
    return {{"running", running},
            {"paused", paused},
            {"renderer", rendererName},
            {"layout", mode},
            {"fps", fpsMeasured},
            {"paintMs", paintMs},
            {"slots", items},
            {"idleSlots", idle},
            {"cleanCuts", cleanCuts},
            {"delayedCuts", delayedCuts},
            {"lastCutDelayMs", lastCutDelay * 1000},
            {"lastPreloadMs", lastPreloadMs},
            {"frameGapP95", frameGapP95},
            {"frameGapMax", frameGapMax},
            {"schedulerMax", schedulerMax},
            {"eligible", videos.size()},
            {"shuffleRemaining", bag.remainingCount()},
            {"cycle", bag.cycle()},
            {"audioSlot", config.muted ? -1 : audioSerial},
            {"error", lastError},
            {"sessionTime", sessionTime},
            {"transitionProgress", progress()}};
}
QString Canvas::performanceText() const {
    QString out = QString("GPU\n%1\n\nCOMPOSITOR\n%2 fps / %3 target\n%4 ms last paint (CPU wall time)\n%5 "
                          "layout · %6 visible segments\n\nSHUFFLE\n%7 eligible · %8 remaining · cycle "
                          "%9\n\nMEMORY BUDGET\n%10 MiB demuxer cache per instance\n%11 MiB total cache "
                          "ceiling\nTextures capped at %12 px; decoder memory is additional.\n")
                      .arg(rendererName)
                      .arg(fpsMeasured, 0, 'f', 1)
                      .arg(config.fps)
                      .arg(paintMs, 0, 'f', 2)
                      .arg(mode)
                      .arg(players.size())
                      .arg(videos.size())
                      .arg(bag.remainingCount())
                      .arg(bag.cycle())
                      .arg(config.bufferMiB)
                      .arg(config.bufferMiB * (players.size() + spares.size()))
                      .arg(config.textureLimit);
    qint64 textureBytes = 0;
    for (const auto& collection : {&players, &spares})
        for (const auto& s : *collection)
            if (s->fbo)
                textureBytes += qint64(s->fbo->width()) * s->fbo->height() * 4;
    out += QString(
               "%1 hidden players (preloading + idle)\n%2 MiB allocated video textures (visible + hidden)\n\n"
               "FRAME PACING · LAST SECOND\n%3 ms p95 frame interval\n%4 ms longest frame interval\n"
               "%5 ms longest scheduler pass\n%6 new video texture updates / second\n"
               "Paint time excludes scheduling, window composition and presentation waits.\n\nDECODERS\n")
               .arg(spares.size())
               .arg(textureBytes / (1024.0 * 1024), 0, 'f', 1)
               .arg(frameGapP95, 0, 'f', 2)
               .arg(frameGapMax, 0, 'f', 2)
               .arg(schedulerMax, 0, 'f', 2)
               .arg(textureUpdatesPerSecond);
    out += QString("Prepared cuts: %1 · delayed cuts: %2\nLast preload: %3 ms · last cut delay: %4 ms\n")
               .arg(cleanCuts)
               .arg(delayedCuts)
               .arg(lastPreloadMs, 0, 'f', 1)
               .arg(lastCutDelay * 1000, 0, 'f', 1);
    for (auto& s : players)
        out += QString("\n#%1 %2\n%3×%4 · %5 · %6\nDropped frames: %7\n")
                   .arg(s->serial)
                   .arg(s->video.title)
                   .arg(s->video.width)
                   .arg(s->video.height)
                   .arg(s->decoder->string("hwdec-current"))
                   .arg(s->serial == audioSerial && !config.muted ? "AUDIO" : "muted")
                   .arg(s->decoder->number("decoder-frame-drop-count"));
    if (!lastError.isEmpty())
        out += "\nLAST ERROR\n" + lastError;
    return out;
}
void Canvas::mouseMoveEvent(QMouseEvent* e) {
    emit interaction();
    QOpenGLWidget::mouseMoveEvent(e);
}
void Canvas::mouseDoubleClickEvent(QMouseEvent*) {
    emit fullscreenRequested();
}
} // namespace kaleido
