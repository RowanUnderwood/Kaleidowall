#pragma once
#include <QHash>
#include <QLibrary>
#include <QString>
#include <QVariant>
#include <mpv/client.h>
#include <mpv/render_gl.h>

namespace kaleido {
class MpvApi {
  public:
    bool load();
    QString error;
#define API(name) decltype(&mpv_##name) name = nullptr
    API(create);
    API(initialize);
    API(terminate_destroy);
    API(set_option_string);
    API(command_async);
    API(set_property_string);
    API(set_property_async);
    API(observe_property);
    API(get_property);
    API(get_property_string);
    API(free);
    API(wait_event);
    API(error_string);
    API(render_context_create);
    API(render_context_free);
    API(render_context_render);
    API(render_context_update);
    API(render_context_report_swap);
#undef API
  private:
    QLibrary library;
};
class Decoder {
  public:
    explicit Decoder(MpvApi& a) : api(a) {}
    ~Decoder();
    bool init(bool hwdec, int bufferMiB, mpv_opengl_init_params& gl);
    bool loadFile(const QString& path, double start, double length);
    void stopPlayback();
    void handleEvent(const mpv_event& event);
    void set(const char* key, const QString& val);
    double number(const char* key, double fallback = 0) const;
    QString string(const char* key) const;
    bool render(int fbo, int width, int height, bool force = false);
    bool muteConfirmed = true;
    bool stopPending = false;
    MpvApi& api;
    mpv_handle* handle = nullptr;
    mpv_render_context* renderer = nullptr;
    QString error;

  private:
    QHash<QString, QVariant> properties;
    QHash<QString, QString> requested;
    quint64 nextRequest = 100, muteRequest = 0;
};
} // namespace kaleido
