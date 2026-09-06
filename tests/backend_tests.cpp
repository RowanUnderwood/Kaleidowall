#include "mpv_backend.h"
#include <QtTest>
using namespace kaleido;

namespace {
quint64 requestId;
int writes, rendered;
uint64_t renderFlags;
QString sentValue;
Qt::HANDLE destroyingThread = nullptr;
int fakeSetProperty(mpv_handle*, uint64_t id, const char*, mpv_format format, void* data) {
    if (format != MPV_FORMAT_STRING)
        return MPV_ERROR_PROPERTY_FORMAT;
    requestId = id;
    ++writes;
    sentValue = QString::fromUtf8(*static_cast<const char**>(data));
    return 0;
}
uint64_t updateFrame(mpv_render_context*) {
    return renderFlags;
}
int renderFrame(mpv_render_context*, mpv_render_param*) {
    ++rendered;
    return 0;
}
} // namespace
class BackendTests : public QObject {
    Q_OBJECT
  private slots:
    void staleMuteAcknowledgmentCannotEnableHandoff() {
        MpvApi api;
        api.set_property_async = &fakeSetProperty;
        Decoder decoder(api);
        decoder.set("mute", "no");
        const auto unmuteId = requestId;
        decoder.set("mute", "yes");
        const auto muteId = requestId;
        QVERIFY(!decoder.muteConfirmed);
        mpv_event reply{};
        reply.event_id = MPV_EVENT_SET_PROPERTY_REPLY;
        reply.reply_userdata = unmuteId;
        decoder.handleEvent(reply);
        QVERIFY(!decoder.muteConfirmed);
        reply.reply_userdata = muteId;
        decoder.handleEvent(reply);
        QVERIFY(decoder.muteConfirmed);
    }
    void failedMuteDoesNotClaimSilenceAndCanRetry() {
        MpvApi api;
        api.set_property_async = &fakeSetProperty;
        Decoder decoder(api);
        decoder.set("mute", "yes");
        const auto failedId = requestId;
        mpv_event reply{};
        reply.event_id = MPV_EVENT_SET_PROPERTY_REPLY;
        reply.reply_userdata = failedId;
        reply.error = MPV_ERROR_PROPERTY_ERROR;
        decoder.handleEvent(reply);
        QVERIFY(!decoder.muteConfirmed);
        decoder.set("mute", "yes");
        QVERIFY(requestId != failedId);
    }
    void duplicateWritesAreNotQueuedEveryFrame() {
        MpvApi api;
        api.set_property_async = &fakeSetProperty;
        Decoder decoder(api);
        writes = 0;
        for (int i = 0; i < 120; ++i)
            decoder.set("volume", "45");
        QCOMPARE(writes, 1);
        QCOMPARE(sentValue, QString("45"));
    }
    void observedPropertiesAreCachedWithoutCoreCalls() {
        MpvApi api;
        Decoder decoder(api);
        double pos = 12.5;
        mpv_event_property prop{"time-pos", MPV_FORMAT_DOUBLE, &pos};
        mpv_event event{};
        event.event_id = MPV_EVENT_PROPERTY_CHANGE;
        event.data = &prop;
        decoder.handleEvent(event);
        QCOMPARE(decoder.number("time-pos"), 12.5);
        int muted = 1;
        prop = {"mute", MPV_FORMAT_FLAG, &muted};
        decoder.handleEvent(event);
        QCOMPARE(decoder.string("mute"), QString("yes"));
        prop = {"time-pos", MPV_FORMAT_NONE, nullptr};
        decoder.handleEvent(event);
        QCOMPARE(decoder.number("time-pos", -1), -1.);
    }
    void textureIsReusedUnlessNewFrameOrResize() {
        MpvApi api;
        api.render_context_update = &updateFrame;
        api.render_context_render = &renderFrame;
        Decoder decoder(api);
        rendered = 0;
        renderFlags = 0;
        for (int i = 0; i < 60; ++i)
            QVERIFY(!decoder.render(1, 640, 360));
        QCOMPARE(rendered, 0);
        renderFlags = MPV_RENDER_UPDATE_FRAME;
        QVERIFY(decoder.render(1, 640, 360));
        QCOMPARE(rendered, 1);
        renderFlags = 0;
        QVERIFY(decoder.render(2, 1280, 720, true));
        QCOMPARE(rendered, 2);
    }
    void playerIsNotReusableUntilStopAcknowledgment() {
        MpvApi api;
        api.set_property_async = &fakeSetProperty;
        api.command_async = [](mpv_handle*, uint64_t, const char**) { return 0; };
        Decoder decoder(api);
        decoder.stopPlayback();
        QVERIFY(decoder.stopPending);
        mpv_event event{};
        event.event_id = MPV_EVENT_COMMAND_REPLY;
        event.reply_userdata = 1;
        decoder.handleEvent(event);
        QVERIFY(decoder.stopPending);
        event.reply_userdata = 2;
        decoder.handleEvent(event);
        QVERIFY(!decoder.stopPending);
    }
    void handleIsDestroyedOffTheCallingThread() {
        // libmpv leaves a stray CoUninitialize() on whichever thread destroys a
        // player, which would otherwise dismantle the COM apartment Qt owns on
        // the GUI thread and fault QGuiApplication's own shutdown.
        MpvApi api;
        api.terminate_destroy = [](mpv_handle*) { destroyingThread = QThread::currentThreadId(); };
        destroyingThread = QThread::currentThreadId();
        {
            Decoder decoder(api);
            decoder.handle = reinterpret_cast<mpv_handle*>(1);
        }
        QVERIFY(destroyingThread != QThread::currentThreadId());
    }
};
QTEST_GUILESS_MAIN(BackendTests)
#include "backend_tests.moc"
