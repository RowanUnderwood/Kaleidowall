#include "mpv_backend.h"
#include <QCoreApplication>
namespace kaleido {
bool MpvApi::load() {
    library.setFileName(QCoreApplication::applicationDirPath() + "/libmpv-2.dll");
    if (!library.load()) {
        error = library.errorString();
        return false;
    }
#define LOAD(name)                                                                                           \
    name = reinterpret_cast<decltype(name)>(library.resolve("mpv_" #name));                                  \
    if (!name) {                                                                                             \
        error = "libmpv API missing: " #name;                                                                \
        return false;                                                                                        \
    }
    LOAD(create);
    LOAD(initialize);
    LOAD(terminate_destroy);
    LOAD(set_option_string);
    LOAD(command_async);
    LOAD(set_property_string);
    LOAD(set_property_async);
    LOAD(observe_property);
    LOAD(get_property);
    LOAD(get_property_string);
    LOAD(free);
    LOAD(wait_event);
    LOAD(error_string);
    LOAD(render_context_create);
    LOAD(render_context_free);
    LOAD(render_context_render);
    LOAD(render_context_update);
    LOAD(render_context_report_swap);
#undef LOAD
    return true;
}
Decoder::~Decoder() {
    if (renderer)
        api.render_context_free(renderer);
    if (handle)
        api.terminate_destroy(handle);
}
bool Decoder::init(bool hwdec, int bufferMiB, mpv_opengl_init_params& gl) {
    handle = api.create();
    if (!handle) {
        error = "Cannot create libmpv instance";
        return false;
    }
    auto option = [&](const char* key, const QByteArray& val) {
        api.set_option_string(handle, key, val.constData());
    };
    option("config", "no");
    option("terminal", "no");
    option("input-default-bindings", "no");
    option("input-vo-keyboard", "no");
    option("vo", "libmpv");
    option("hwdec", hwdec ? "auto-safe" : "no");
    option("keep-open", "yes");
    option("idle", "yes");
    option("keep-open-pause", "no");
    option("mute", "yes");
    option("audio-client-name", "Kaleidowall");
    option("sub-visibility", "no");
    option("demuxer-max-bytes", QByteArray::number(qint64(bufferMiB) * 1024 * 1024));
    option("demuxer-max-back-bytes", "0");
    option("cache-secs", "3");
    option("video-sync", "audio");
    int e = api.initialize(handle);
    if (e < 0) {
        error = QString::fromUtf8(api.error_string(e));
        return false;
    }
    // Cache asynchronous observations, so neither scheduling nor diagnostics
    // waits for the playback core to answer a property read on the GUI thread.
    for (const char* name : {"time-pos", "decoder-frame-drop-count"})
        api.observe_property(handle, 0, name, MPV_FORMAT_DOUBLE);
    for (const char* name : {"mute", "pause", "eof-reached", "idle-active"})
        api.observe_property(handle, 0, name, MPV_FORMAT_FLAG);
    api.observe_property(handle, 0, "hwdec-current", MPV_FORMAT_STRING);
    properties["mute"] = true;
    requested["mute"] = "yes";
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
                                 {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    e = api.render_context_create(&renderer, handle, params);
    if (e < 0) {
        error = QString::fromUtf8(api.error_string(e));
        return false;
    }
    return true;
}
bool Decoder::loadFile(const QString& path, double start, double length) {
    properties["eof-reached"] = false;
    auto bytes = path.toUtf8();
    auto opts = QString("start=%1,end=%2").arg(start, 0, 'f', 6).arg(start + length, 0, 'f', 6).toUtf8();
    const char* args[] = {"loadfile", bytes.constData(), "replace", "-1", opts.constData(), nullptr};
    int e = api.command_async(handle, 1, args);
    if (e < 0)
        error = QString::fromUtf8(api.error_string(e));
    return e >= 0;
}
void Decoder::set(const char* key, const QString& val) {
    const QString name = QString::fromLatin1(key);
    if (requested.contains(name) && requested.value(name) == val)
        return;
    auto b = val.toUtf8();
    const char* value = b.constData();
    const auto request = nextRequest++;
    if (api.set_property_async(handle, request, key, MPV_FORMAT_STRING, &value) < 0) {
        if (name == "mute")
            muteConfirmed = false;
        return;
    }
    requested[name] = val;
    if (name == "mute") {
        muteRequest = request;
        muteConfirmed = false;
    }
}
void Decoder::stopPlayback() {
    set("mute", "yes");
    const char* command[] = {"stop", nullptr};
    stopPending = true;
    if (api.command_async(handle, 2, command) < 0)
        error = "Could not stop the idle player";
}
void Decoder::handleEvent(const mpv_event& event) {
    if (event.event_id == MPV_EVENT_PROPERTY_CHANGE) {
        const auto* p = static_cast<mpv_event_property*>(event.data);
        const QString name = QString::fromUtf8(p->name);
        if (!p->data) {
            properties.remove(name);
            return;
        }
        switch (p->format) {
        case MPV_FORMAT_DOUBLE:
            properties[name] = *static_cast<double*>(p->data);
            break;
        case MPV_FORMAT_FLAG:
            properties[name] = bool(*static_cast<int*>(p->data));
            break;
        case MPV_FORMAT_STRING:
            properties[name] = QString::fromUtf8(*static_cast<char**>(p->data));
            break;
        default:
            break;
        }
    } else if (event.event_id == MPV_EVENT_SET_PROPERTY_REPLY && event.reply_userdata == muteRequest) {
        // A stale observed mute flag is insufficient for a cross-player handoff:
        // wait for the latest mute command to finish before enabling a new source.
        muteConfirmed = event.error >= 0 && requested.value("mute") == "yes";
        if (event.error < 0)
            requested.remove("mute");
    } else if (event.event_id == MPV_EVENT_COMMAND_REPLY && event.reply_userdata == 2) {
        stopPending = event.error < 0;
    } else if (event.event_id == MPV_EVENT_START_FILE) {
        properties["eof-reached"] = false;
    }
}
double Decoder::number(const char* key, double fallback) const {
    return properties.value(QString::fromLatin1(key), fallback).toDouble();
}
QString Decoder::string(const char* key) const {
    const auto value = properties.value(QString::fromLatin1(key));
    if (value.metaType().id() == QMetaType::Bool)
        return value.toBool() ? "yes" : "no";
    return value.toString();
}
bool Decoder::render(int fbo, int w, int h, bool force) {
    const auto flags = api.render_context_update(renderer);
    if (!force && !(flags & MPV_RENDER_UPDATE_FRAME))
        return false;
    mpv_opengl_fbo target{fbo, w, h, 0};
    // The compositor maps texture v=0 to the top of each viewport. This is an
    // offscreen texture, so applying the default-framebuffer flip inverts it.
    int flip = 0, block = 0;
    mpv_render_param params[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &target},
                                 {MPV_RENDER_PARAM_FLIP_Y, &flip},
                                 {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    const int result = api.render_context_render(renderer, params);
    if (result < 0)
        error = QString::fromUtf8(api.error_string(result));
    return result >= 0;
}
} // namespace kaleido
