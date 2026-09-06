#include "exporter.h"
#include "compositor.h"
#include "export_timeline.h"
#include "gpu.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace kaleido {
qint64 ExportOptions::frameCount() const {
    return qint64(std::ceil(duration * fps - 1e-8));
}
double ExportOptions::outputDuration() const {
    return double(frameCount()) / fps;
}
QString ExportOptions::validate() const {
    if (!std::isfinite(duration) || duration <= 0 || duration > 86400)
        return "Choose a duration between 0 and 24 hours.";
    if (fps != 30 && fps != 60)
        return "Choose 30 or 60 fps.";
    if (size != QSize(1280, 720) && size != QSize(1920, 1080) && size != QSize(3840, 2160))
        return "Choose 720p, 1080p or 4K UHD.";
    if (quality < 0 || quality > 2 || volume < 0 || volume > 100 || audioMode < 0 || audioMode > 2)
        return "Invalid export settings.";
    if (frameCount() < 1)
        return "The export must contain at least one frame.";
    if (destination.isEmpty() || QFileInfo(destination).suffix().compare("mp4", Qt::CaseInsensitive))
        return "Choose an MP4 destination filename.";
    if (!QFileInfo(QFileInfo(destination).absolutePath()).isDir())
        return "The destination folder does not exist.";
    if (QFileInfo::exists(destination) && !overwrite)
        return "The destination exists. Confirm replacement or choose a new filename.";
    if (audioMode == 2 && !QFileInfo(audioFile).isFile())
        return "Select an existing MP3 or WAV soundtrack.";
    if (audioMode == 2 && QFileInfo(audioFile).absoluteFilePath().compare(
                              QFileInfo(destination).absoluteFilePath(), Qt::CaseInsensitive) == 0)
        return "The output cannot replace its soundtrack.";
    return {};
}
QString mediaTool(const QString& name) {
    const QString local = QCoreApplication::applicationDirPath() + "/" + name + ".exe";
    return QFileInfo::exists(local) ? local : QStandardPaths::findExecutable(name);
}
namespace {
struct Failure : std::runtime_error {
    explicit Failure(const QString& message) : std::runtime_error(message.toStdString()) {}
};
QString seconds(double n) {
    return QString::number(n, 'f', 9);
}
void checkCancel(const std::atomic_bool& cancel) {
    if (cancel.load())
        throw Failure("Export canceled.");
}
QString errorText(QProcess& process) {
    return QString::fromUtf8(process.readAllStandardError()).right(6000);
}
void startProcess(QProcess& process, const QString& exe, const QStringList& args,
                  const std::atomic_bool& cancel) {
    checkCancel(cancel);
    process.setProgram(exe);
    process.setArguments(args);
    // NVENC ordinals otherwise follow CUDA's "fastest first" heuristic, which on a mixed machine
    // is neither PCI order nor DXGI order. Pinning the ordering here — the one place every FFmpeg
    // and FFprobe child is launched — makes -gpu mean the same card as the probe that resolved it.
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("CUDA_DEVICE_ORDER", "PCI_BUS_ID");
    process.setProcessEnvironment(environment);
    process.start();
    QElapsedTimer timeout;
    timeout.start();
    while (process.state() == QProcess::Starting && timeout.elapsed() < 10000) {
        checkCancel(cancel);
        process.waitForStarted(50);
    }
    if (process.state() == QProcess::NotRunning && process.error() == QProcess::FailedToStart)
        throw Failure("Cannot start " + exe + ": " + process.errorString());
    if (process.state() == QProcess::Starting)
        throw Failure("Timed out starting " + exe);
}
void finishProcess(QProcess& process, const std::atomic_bool& cancel, int timeoutMs = 120000) {
    QElapsedTimer timeout;
    timeout.start();
    QString errors;
    while (process.state() != QProcess::NotRunning) {
        checkCancel(cancel);
        process.waitForFinished(50);
        errors = (errors + errorText(process)).right(6000);
        if (timeout.elapsed() > timeoutMs)
            throw Failure("Media process timed out. " + errors);
    }
    errors += errorText(process);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        throw Failure("Media process failed: " + errors);
}
void writeProcess(QProcess& process, const char* bytes, qint64 count, const std::atomic_bool& cancel) {
    qint64 offset = 0;
    QElapsedTimer timeout;
    timeout.start();
    while (offset < count || process.bytesToWrite()) {
        checkCancel(cancel);
        if (process.state() == QProcess::NotRunning)
            throw Failure("Encoder stopped: " + errorText(process));
        if (offset < count && process.bytesToWrite() < 1024 * 1024) {
            const qint64 n = process.write(bytes + offset, std::min(qint64(1024 * 1024), count - offset));
            if (n < 0)
                throw Failure("Cannot write encoder input: " + process.errorString());
            offset += n;
        }
        if (process.waitForBytesWritten(50))
            timeout.restart();
        if (timeout.elapsed() > 60000)
            throw Failure("Encoder is not accepting frames. " + errorText(process));
    }
}
QByteArray readBytes(QProcess& process, qint64 size, const std::atomic_bool& cancel) {
    QByteArray bytes;
    bytes.reserve(size);
    QElapsedTimer timeout;
    timeout.start();
    while (bytes.size() < size) {
        checkCancel(cancel);
        const auto chunk = process.read(size - bytes.size());
        if (!chunk.isEmpty()) {
            bytes += chunk;
            timeout.restart();
            continue;
        }
        if (process.state() == QProcess::NotRunning)
            break;
        process.waitForReadyRead(50);
        if (timeout.elapsed() > 60000)
            throw Failure("Decoder stalled. " + errorText(process));
    }
    return bytes;
}
// QProcess's Windows reader drains stdout on a background thread even without an event loop.
// Raw media must bypass it: otherwise a fast decoder can buffer an entire clip in application RAM.
// The native pipe holds at most a few MiB and blocks the child until the compositor consumes data.
class MediaPipe {
  public:
    ~MediaPipe() {
        close();
    }
    void attach(QProcess& process) {
#ifdef Q_OS_WIN
        close();
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&reader, &writer, &security, 4 * 1024 * 1024))
            throw Failure("Cannot create the bounded media pipe.");
        if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0))
            throw Failure("Cannot configure the media pipe.");
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments* args) {
            args->startupInfo->hStdOutput = writer;
            args->startupInfo->dwFlags |= STARTF_USESTDHANDLES;
            args->inheritHandles = true;
            args->flags |= CREATE_NO_WINDOW;
        });
#else
        Q_UNUSED(process);
#endif
    }
    void started() {
#ifdef Q_OS_WIN
        if (writer)
            CloseHandle(std::exchange(writer, nullptr));
#endif
    }
    QByteArray read(QProcess& process, qint64 count, const std::atomic_bool& cancel) {
#ifdef Q_OS_WIN
        Q_UNUSED(process);
        QByteArray bytes(count, Qt::Uninitialized);
        qint64 offset = 0;
        QElapsedTimer timeout;
        timeout.start();
        while (offset < count) {
            checkCancel(cancel);
            DWORD available = 0;
            if (!PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE)
                    break;
                throw Failure("Cannot read the media pipe.");
            }
            if (!available) {
                if (timeout.elapsed() > 60000)
                    throw Failure("Media decoder stalled for 60 seconds.");
                QThread::msleep(1);
                continue;
            }
            DWORD received = 0;
            if (!ReadFile(reader, bytes.data() + offset, DWORD(std::min(qint64(available), count - offset)),
                          &received, nullptr))
                throw Failure("Media pipe read failed.");
            offset += received;
            timeout.restart();
        }
        bytes.resize(offset);
        return bytes;
#else
        return readBytes(process, count, cancel);
#endif
    }

  private:
    void close() {
#ifdef Q_OS_WIN
        if (reader)
            CloseHandle(std::exchange(reader, nullptr));
        if (writer)
            CloseHandle(std::exchange(writer, nullptr));
#endif
    }
#ifdef Q_OS_WIN
    HANDLE reader = nullptr, writer = nullptr;
#endif
};
class ClipReader {
  public:
    ClipReader(const ExportSlot& slot, const ExportOptions& options, const QString& ffmpeg,
               const std::atomic_bool& cancel, QHash<QString, QSize>& dimensions, int threadBudget)
        : slot(slot), options(options), ffmpeg(ffmpeg), cancel(cancel), threadBudget(threadBudget) {
        if (!dimensions.contains(slot.video.path)) {
            QProcess probe;
            startProcess(
                probe, mediaTool("ffprobe"),
                {"-v", "error", "-select_streams", "V:0", "-show_streams", "-of", "json", slot.video.path},
                cancel);
            probe.closeWriteChannel();
            finishProcess(probe, cancel, 30000);
            const auto streams =
                QJsonDocument::fromJson(probe.readAllStandardOutput()).object()["streams"].toArray();
            if (streams.isEmpty())
                throw Failure("No video stream in " + slot.video.path);
            const auto stream = streams.first().toObject();
            int w = stream["width"].toInt(), h = stream["height"].toInt();
            const auto sar = stream["sample_aspect_ratio"].toString().split(':');
            if (sar.size() == 2 && sar[0].toDouble() > 0 && sar[1].toDouble() > 0)
                w = qRound(w * sar[0].toDouble() / sar[1].toDouble());
            double rotation = stream["tags"].toObject()["rotate"].toString().toDouble();
            for (const auto& side : stream["side_data_list"].toArray())
                if (side.toObject().contains("rotation"))
                    rotation = side.toObject()["rotation"].toDouble();
            if (std::abs(std::sin(rotation * 3.141592653589793 / 180.0)) > .5)
                std::swap(w, h);
            if (w < 1 || h < 1)
                throw Failure("Invalid video dimensions in " + slot.video.path);
            dimensions[slot.video.path] = QSize(w, h);
        }
        this->slot.video.width = dimensions[slot.video.path].width();
        this->slot.video.height = dimensions[slot.video.path].height();
        start(true);
    }
    ~ClipReader() {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
    }
    QByteArray next() {
        if (ended)
            return {};
        auto bytes = pipe.read(process, qint64(size.width()) * size.height() * 3 / 2, cancel);
        if (bytes.isEmpty() && !hadFrame && hardware) {
            process.kill();
            process.waitForFinished(3000);
            process.readAllStandardError();
            start(false);
            bytes = pipe.read(process, qint64(size.width()) * size.height() * 3 / 2, cancel);
        }
        if (bytes.isEmpty()) {
            finishProcess(process, cancel);
            if (!hadFrame)
                throw Failure("No decoded frames in " + slot.video.path);
            ended = true;
            return {};
        }
        if (bytes.size() != qint64(size.width()) * size.height() * 3 / 2)
            throw Failure("Incomplete video frame in " + slot.video.path + ": " + errorText(process));
        hadFrame = true;
        return bytes;
    }
    QSize size;
    bool hardware = true;

  private:
    void start(bool hw) {
        hardware = hw;
        // Video dimensions are normalized to display aspect during preflight.
        const double aspect = double(std::max(1, slot.video.width)) / std::max(1, slot.video.height);
        int w = std::min(options.size.width(), std::max(2, slot.video.width));
        int h = qRound(w / aspect);
        if (h > options.size.height()) {
            h = options.size.height();
            w = qRound(h * aspect);
        }
        size = QSize(std::max(2, w / 2 * 2), std::max(2, h / 2 * 2));
        QStringList args{"-hide_banner",
                         "-loglevel",
                         "error",
                         "-xerror",
                         "-nostdin",
                         "-threads",
                         QString::number(threadBudget)};
        if (hw)
            args << "-hwaccel" << "d3d11va" << "-hwaccel_device"
                 << QString::number(options.decodeAdapter);
        QString filter = QString("setpts=PTS-STARTPTS,fps=fps=%1:start_time=0,").arg(options.fps);
        filter +=
            QString("scale=%1:%2:flags=bilinear:out_color_matrix=bt709:out_range=tv,setsar=1,format=nv12")
                .arg(size.width())
                .arg(size.height());
        args << "-ss" << seconds(slot.clip.start) << "-i" << slot.video.path << "-t"
             << seconds(std::min(slot.usableEnd - slot.clip.start, options.outputDuration() - slot.begins))
             << "-map" << "0:V:0" << "-an" << "-sn" << "-dn" << "-filter_threads" << "1"
             << "-vf" << filter << "-threads" << "1" << "-f" << "rawvideo" << "-pix_fmt" << "nv12"
             << "pipe:1";
        pipe.attach(process);
        startProcess(process, ffmpeg, args, cancel);
        pipe.started();
        process.closeWriteChannel();
    }
    ExportSlot slot;
    ExportOptions options;
    QString ffmpeg;
    const std::atomic_bool& cancel;
    QProcess process;
    MediaPipe pipe;
    bool hadFrame = false, ended = false;
    int threadBudget;
};
void appendSilence(QFile& file, qint64 bytes, const std::atomic_bool& cancel) {
    const QByteArray zero(65536, '\0');
    while (bytes > 0) {
        checkCancel(cancel);
        const qint64 n = std::min(bytes, qint64(zero.size()));
        if (file.write(zero.constData(), n) != n)
            throw Failure("Cannot write audio: " + file.errorString());
        bytes -= n;
    }
}
void buildAudio(const QString& path, const std::vector<AudioSpan>& spans, const ExportOptions& options,
                const QString& ffmpeg, const std::atomic_bool& cancel) {
    QFile audio(path);
    if (!audio.open(QIODevice::WriteOnly))
        throw Failure("Cannot create audio staging file: " + audio.errorString());
    const qint64 totalSamples = qRound64(options.outputDuration() * 48000);
    qint64 position = 0;
    QHash<QString, int> audioTracks;
    for (const auto& span : spans) {
        checkCancel(cancel);
        const qint64 begins = std::clamp(qRound64(span.start * 48000), position, totalSamples);
        const qint64 ends = std::clamp(qRound64(span.end * 48000), begins, totalSamples);
        appendSilence(audio, (begins - position) * 4, cancel);
        position = begins;
        if (ends == begins)
            continue;
        if (!audioTracks.contains(span.path)) {
            QProcess probe;
            startProcess(probe, mediaTool("ffprobe"),
                         {"-v", "error", "-select_streams", "a", "-show_streams", "-of", "json", span.path},
                         cancel);
            probe.closeWriteChannel();
            finishProcess(probe, cancel, 30000);
            const auto tracks =
                QJsonDocument::fromJson(probe.readAllStandardOutput()).object()["streams"].toArray();
            if (tracks.isEmpty())
                throw Failure("No audio stream in " + span.path);
            int index = tracks.first().toObject()["index"].toInt();
            for (const auto& value : tracks) {
                const auto track = value.toObject();
                if (track["disposition"].toObject()["default"].toInt()) {
                    index = track["index"].toInt();
                    break;
                }
            }
            audioTracks[span.path] = index;
        }
        QProcess decoder;
        MediaPipe pipe;
        pipe.attach(decoder);
        startProcess(decoder, ffmpeg,
                     {"-hide_banner",
                      "-loglevel",
                      "error",
                      "-nostdin",
                      "-threads",
                      "2",
                      "-ss",
                      seconds(span.sourceStart),
                      "-i",
                      span.path,
                      "-t",
                      seconds(double(ends - begins) / 48000),
                      "-map",
                      QString("0:%1").arg(audioTracks[span.path]),
                      "-vn",
                      "-sn",
                      "-dn",
                      "-af",
                      "aresample=48000:async=1:first_pts=0,volume=" + seconds(options.volume / 100.0),
                      "-ac",
                      "2",
                      "-ar",
                      "48000",
                      "-f",
                      "s16le",
                      "pipe:1"},
                     cancel);
        pipe.started();
        decoder.closeWriteChannel();
        qint64 remaining = (ends - begins) * 4;
        while (remaining) {
            const auto data = pipe.read(decoder, std::min(remaining, qint64(65536)), cancel);
            if (data.isEmpty())
                break;
            if (audio.write(data) != data.size())
                throw Failure("Cannot write soundtrack: " + audio.errorString());
            remaining -= data.size();
        }
        // Drain the small rounding tail so the process cannot block on its output pipe.
        while (!pipe.read(decoder, 65536, cancel).isEmpty()) {
        }
        finishProcess(decoder, cancel);
        appendSilence(audio, remaining, cancel);
        position = ends;
    }
    appendSilence(audio, (totalSamples - position) * 4, cancel);
    if (!audio.flush())
        throw Failure("Cannot flush soundtrack: " + audio.errorString());
}
bool publishFile(const QString& temporary, const QString& destination, bool overwrite) {
#ifdef Q_OS_WIN
    return MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(temporary).utf16()),
                       reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(destination).utf16()),
                       MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0));
#else
    if (QFileInfo::exists(destination))
        return false;
    return QFile::rename(temporary, destination);
#endif
}
} // namespace
ExportWorker::ExportWorker(ExportOptions options, Settings settings, QVector<Video> media,
                           QOffscreenSurface* surface, QObject* parent)
    : QThread(parent), options(options), settings(settings), media(media), surface(surface) {}
ExportWorker::~ExportWorker() {
    cancel();
    wait();
}
QImage ExportWorker::takePreview() {
    QMutexLocker lock(&previewMutex);
    return std::exchange(preview, {});
}
void ExportWorker::run() {
    QElapsedTimer elapsed;
    elapsed.start();
    GpuSelection device;
    try {
        const auto invalid = options.validate();
        if (!invalid.isEmpty())
            throw Failure(invalid);
        const QString ffmpeg = mediaTool("ffmpeg"), ffprobe = mediaTool("ffprobe");
        if (ffmpeg.isEmpty() || ffprobe.isEmpty())
            throw Failure("FFmpeg and FFprobe are required beside Kaleidowall.exe or on PATH.");
        emit progress("Preparing", 0, options.frameCount(), 0);
        const QStorageInfo storage(QFileInfo(options.destination).absolutePath());
        const qint64 audioSpace =
            options.audioMode ? qint64(std::ceil(options.outputDuration() * 192000)) : 0;
        if (storage.isValid() && storage.isReady() &&
            storage.bytesAvailable() < audioSpace + 64 * 1024 * 1024)
            throw Failure("Not enough free space for the soundtrack and export staging files.");
        QHash<QString, QPair<qint64, qint64>> fingerprints;
        for (const auto& v : media)
            if (eligibilityReason(v, settings).isEmpty()) {
                const QFileInfo info(v.path);
                fingerprints[v.path] = {info.size(), info.lastModified().toMSecsSinceEpoch()};
            }
        for (const auto& v : media)
            if (QFileInfo(v.path).absoluteFilePath().compare(
                    QFileInfo(options.destination).absoluteFilePath(), Qt::CaseInsensitive) == 0)
                throw Failure("Choose a destination outside your source videos; export cannot replace a "
                              "library source.");
        ExportTimeline timeline(settings, media, options.seed,
                                double(options.size.width()) / options.size.height());
        if (!timeline.error.isEmpty())
            throw Failure(timeline.error);
        emit progress("Preparing", 0, options.frameCount(), 0);
        QTemporaryDir staging(QFileInfo(options.destination).absolutePath() + "/.kaleidowall-XXXXXX");
        if (!staging.isValid())
            throw Failure("Cannot create temporary files in the destination folder.");
        const QString videoPath = staging.filePath("video.mp4"),
                      outputPath = staging.filePath("finished.mp4");
        QOpenGLContext context;
        context.setFormat(surface->format());
        if (!context.create() || !context.makeCurrent(surface))
            throw Failure("Cannot create the offscreen OpenGL export context.");
        QOpenGLFunctions_3_3_Core gl;
        if (!gl.initializeOpenGLFunctions())
            throw Failure("OpenGL 3.3 is unavailable for export.");
        const QString gpu = QString::fromLatin1(reinterpret_cast<const char*>(gl.glGetString(GL_RENDERER)));
        // Decode and encode follow the card that composites, so a multi-GPU machine does not split
        // the pipeline across adapters. Resolved before any child process is launched.
        device = selectExportGpu(gpu);
        options.decodeAdapter = device.dxgi;
        options.encodeGpu = device.nvenc;
        Compositor compositor;
        if (!compositor.initialize())
            throw Failure("Export compositor: " + compositor.error());
        auto release = qScopeGuard([&] { compositor.release(); });
        QOpenGLFramebufferObject target(options.size, QOpenGLFramebufferObject::NoAttachment);
        if (!target.isValid())
            throw Failure("Cannot allocate the output framebuffer.");
        Nv12Converter converter;
        if (!converter.initialize())
            throw Failure("Cannot initialize GPU color conversion.");
        auto releaseConverter = qScopeGuard([&] { converter.release(); });
        QOpenGLFramebufferObjectFormat nv12Format;
        nv12Format.setInternalTextureFormat(GL_R8);
        QOpenGLFramebufferObject nv12(QSize(options.size.width(), options.size.height() * 3 / 2), nv12Format);
        QOpenGLFramebufferObject previewTarget(options.size.scaled(QSize(1280, 720), Qt::KeepAspectRatio));
        if (!nv12.isValid() || !previewTarget.isValid())
            throw Failure("Cannot allocate color/preview buffers.");
        const qint64 frameBytes = qint64(options.size.width()) * options.size.height() * 3 / 2;
        GLuint pbo[2]{};
        gl.glGenBuffers(2, pbo);
        auto releaseBuffers = qScopeGuard([&] {
            gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            gl.glDeleteBuffers(2, pbo);
        });
        for (auto buffer : pbo) {
            gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
            gl.glBufferData(GL_PIXEL_PACK_BUFFER, frameBytes, nullptr, GL_STREAM_READ);
        }
        gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        QProcess encoder;
        const int quality = options.quality;
        QStringList encodeArgs{"-hide_banner",
                               "-loglevel",
                               "error",
                               "-nostdin",
                               "-y",
                               "-f",
                               "rawvideo",
                               "-pix_fmt",
                               "nv12",
                               "-video_size",
                               QString("%1x%2").arg(options.size.width()).arg(options.size.height()),
                               "-framerate",
                               QString::number(options.fps),
                               "-i",
                               "pipe:0",
                               "-an"};
        if (options.softwareEncoder)
            encodeArgs << "-c:v" << "libx264" << "-preset" << "fast" << "-crf"
                       << QString::number(quality == 0   ? 28
                                          : quality == 1 ? 23
                                                         : 19)
                       << "-threads" << "8";
        else
            encodeArgs << "-c:v" << "h264_nvenc" << "-gpu" << QString::number(options.encodeGpu) << "-preset"
                       << (quality == 0   ? "p4"
                           : quality == 1 ? "p5"
                                          : "p6")
                       << "-tune" << "hq" << "-rc" << "vbr" << "-cq"
                       << QString::number(quality == 0   ? 28
                                          : quality == 1 ? 23
                                                         : 19)
                       << "-b:v" << "0";
        encodeArgs << "-profile:v" << "high" << "-g" << QString::number(options.fps * 2) << "-color_primaries"
                   << "bt709" << "-color_trc" << "bt709" << "-colorspace" << "bt709"
                   << "-color_range" << "tv" << videoPath;
        startProcess(encoder, ffmpeg, encodeArgs, canceled);
        struct Stream {
            std::unique_ptr<ClipReader> reader;
            GLuint texture = 0, chroma = 0;
        };
        std::map<int, Stream> streams;
        QHash<QString, QSize> displayDimensions;
        auto clearTextures = qScopeGuard([&] {
            for (auto& [id, stream] : streams) {
                gl.glDeleteTextures(1, &stream.texture);
                gl.glDeleteTextures(1, &stream.chroma);
            }
        });
        std::vector<AudioSpan> audioSpans;
        QJsonArray events;
        double decodeMs = 0, renderMs = 0, encodeMs = 0;
        qint64 lastPreview = -1000, lastProgress = -1000;
        int softwareDecoders = 0;
        auto consume = [&](int index) {
            gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[index]);
            const auto* data = static_cast<const char*>(
                gl.glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, frameBytes, GL_MAP_READ_BIT));
            if (!data)
                throw Failure("Cannot read the export framebuffer.");
            auto unmap = qScopeGuard([&] {
                gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[index]);
                gl.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            });
            QElapsedTimer timing;
            timing.start();
            writeProcess(encoder, data, frameBytes, canceled);
            encodeMs += timing.nsecsElapsed() / 1e6;
            if (elapsed.elapsed() - lastPreview >= 333) {
                gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                QOpenGLFramebufferObject::blitFramebuffer(
                    &previewTarget, QRect(QPoint(0, 0), previewTarget.size()), &target,
                    QRect(QPoint(0, 0), options.size), GL_COLOR_BUFFER_BIT, GL_LINEAR);
                QImage image = previewTarget.toImage();
                QMutexLocker lock(&previewMutex);
                preview = std::move(image);
                lastPreview = elapsed.elapsed();
            }
        };
        for (qint64 frame = 0; frame < options.frameCount(); ++frame) {
            checkCancel(canceled);
            const double time = double(frame) / options.fps;
            timeline.advance(time);
            QSet<int> needed;
            for (const auto& slot : timeline.segments())
                if (slot.stream)
                    needed.insert(slot.stream);
            for (auto it = streams.begin(); it != streams.end();) {
                if (needed.contains(it->first)) {
                    ++it;
                    continue;
                }
                gl.glDeleteTextures(1, &it->second.texture);
                gl.glDeleteTextures(1, &it->second.chroma);
                it = streams.erase(it);
            }
            // Start every newly needed decoder before reading any, allowing source decode to overlap.
            for (const auto& slot : timeline.segments())
                if (slot.stream && !streams.contains(slot.stream)) {
                    const QFileInfo info(slot.video.path);
                    if (!info.isFile() || fingerprints.value(slot.video.path) !=
                                              qMakePair(info.size(), info.lastModified().toMSecsSinceEpoch()))
                        throw Failure("A source file changed or disappeared during export: " +
                                      slot.video.path);
                    Stream stream;
                    stream.reader = std::make_unique<ClipReader>(
                        slot, options, ffmpeg, canceled, displayDimensions,
                        std::clamp(QThread::idealThreadCount() / std::max(1, settings.maxSlots), 1, 4));
                    gl.glGenTextures(1, &stream.texture);
                    gl.glBindTexture(GL_TEXTURE_2D, stream.texture);
                    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    const QSize size = stream.reader->size;
                    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, size.width(), size.height(), 0, GL_RED,
                                    GL_UNSIGNED_BYTE, nullptr);
                    gl.glGenTextures(1, &stream.chroma);
                    gl.glBindTexture(GL_TEXTURE_2D, stream.chroma);
                    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, size.width() / 2, size.height() / 2, 0, GL_RG,
                                    GL_UNSIGNED_BYTE, nullptr);
                    if (gl.glGetError() != GL_NO_ERROR) {
                        gl.glDeleteTextures(1, &stream.texture);
                        gl.glDeleteTextures(1, &stream.chroma);
                        throw Failure(
                            "Cannot allocate source textures. Reduce the number of tiles or output size.");
                    }
                    streams.emplace(slot.stream, std::move(stream));
                    events.append(QJsonObject{{"time", time},
                                              {"stream", slot.stream},
                                              {"slot", slot.serial},
                                              {"source", slot.video.path},
                                              {"sourceStart", slot.clip.start},
                                              {"clipDuration", slot.clip.length},
                                              {"usableEnd", slot.usableEnd}});
                }
            const double p = timeline.progress();
            std::vector<DrawTile> tiles;
            QElapsedTimer stage;
            stage.start();
            for (const auto& slot : timeline.segments())
                if (slot.stream) {
                    auto& stream = streams.at(slot.stream);
                    const bool wasHardware = stream.reader->hardware;
                    const auto bytes = stream.reader->next();
                    if (wasHardware && !stream.reader->hardware)
                        ++softwareDecoders;
                    if (!bytes.isEmpty()) {
                        gl.glBindTexture(GL_TEXTURE_2D, stream.texture);
                        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, stream.reader->size.width(),
                                           stream.reader->size.height(), GL_RED, GL_UNSIGNED_BYTE,
                                           bytes.constData());
                        gl.glBindTexture(GL_TEXTURE_2D, stream.chroma);
                        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, stream.reader->size.width() / 2,
                                           stream.reader->size.height() / 2, GL_RG, GL_UNSIGNED_BYTE,
                                           bytes.constData() + qint64(stream.reader->size.width()) *
                                                                   stream.reader->size.height());
                        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    }
                    tiles.push_back(
                        {stream.texture, stream.reader->size, interpolateRect(slot.from, slot.target, p),
                         float(slot.opacityFrom * (1 - p) + slot.opacityTarget * p), stream.chroma});
                }
            decodeMs += stage.nsecsElapsed() / 1e6;
            stage.restart();
            compositor.draw(target.handle(), options.size, options.size, QColor(settings.backgroundColor),
                            settings.crop, timeline.oldMask, timeline.newMask, float(p), tiles);
            converter.draw(target.texture(), nv12.handle(), options.size);
            gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[frame % 2]);
            gl.glReadPixels(0, 0, options.size.width(), options.size.height() * 3 / 2, GL_RED,
                            GL_UNSIGNED_BYTE, nullptr);
            gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            renderMs += stage.nsecsElapsed() / 1e6;
            if (frame > 0)
                consume(int((frame - 1) % 2));
            if (options.audioMode == 1)
                if (const auto* slot = timeline.audioSlot()) {
                    const double sourceTime = slot->clip.start + time - slot->begins;
                    if (sourceTime < slot->usableEnd) {
                        const double end =
                            std::min(time + 1.0 / options.fps, time + slot->usableEnd - sourceTime);
                        if (!audioSpans.empty() && audioSpans.back().stream == slot->stream &&
                            std::abs(audioSpans.back().end - time) < 1e-7)
                            audioSpans.back().end = end;
                        else
                            audioSpans.push_back({slot->video.path, sourceTime, time, end, slot->stream});
                    }
                }
            if (elapsed.elapsed() - lastProgress >= 200) {
                emit progress("Rendering", std::max(qint64(0), frame), options.frameCount(),
                              elapsed.elapsed() / 1000.0);
                lastProgress = elapsed.elapsed();
            }
        }
        consume(int((options.frameCount() - 1) % 2));
        encoder.closeWriteChannel();
        emit progress("Finishing video", options.frameCount(), options.frameCount(),
                      elapsed.elapsed() / 1000.0);
        finishProcess(encoder, canceled);
        // Release decoder processes before audio finalization to free their decode surfaces and CPU threads.
        for (auto& [id, stream] : streams) {
            gl.glDeleteTextures(1, &stream.texture);
            gl.glDeleteTextures(1, &stream.chroma);
        }
        streams.clear();
        if (options.audioMode == 2)
            audioSpans = {{options.audioFile, 0, 0, options.outputDuration(), 0}};
        if (options.audioMode) {
            emit progress("Building soundtrack", options.frameCount(), options.frameCount(),
                          elapsed.elapsed() / 1000.0);
            buildAudio(staging.filePath("audio.pcm"), audioSpans, options, ffmpeg, canceled);
        }
        emit progress("Finalizing MP4", options.frameCount(), options.frameCount(),
                      elapsed.elapsed() / 1000.0);
        QStringList muxArgs{"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", videoPath};
        if (options.audioMode)
            muxArgs << "-f" << "s16le" << "-ar" << "48000" << "-ac" << "2" << "-i"
                    << staging.filePath("audio.pcm");
        muxArgs << "-map" << "0:v:0" << "-c:v" << "copy";
        if (options.audioMode)
            muxArgs << "-map" << "1:a:0" << "-c:a" << "aac" << "-b:a" << "192k";
        muxArgs << "-t" << seconds(options.outputDuration()) << "-movflags" << "+faststart" << outputPath;
        QProcess muxer;
        startProcess(muxer, ffmpeg, muxArgs, canceled);
        muxer.closeWriteChannel();
        finishProcess(muxer, canceled, 1800000);
        QProcess probe;
        startProcess(probe, ffprobe,
                     {"-v", "error", "-show_streams", "-show_format", "-of", "json", outputPath}, canceled);
        probe.closeWriteChannel();
        finishProcess(probe, canceled);
        const auto metadata = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
        bool validVideo = false, validAudio = options.audioMode == 0;
        for (const auto& value : metadata["streams"].toArray()) {
            const auto stream = value.toObject();
            if (stream["codec_type"] == "video")
                validVideo = stream["width"].toInt() == options.size.width() &&
                             stream["height"].toInt() == options.size.height() &&
                             stream["nb_frames"].toString().toLongLong() == options.frameCount();
            if (stream["codec_type"] == "audio")
                validAudio = true;
        }
        if (!validVideo || !validAudio)
            throw Failure("The output failed stream/frame-count validation; destination was not changed.");
        checkCancel(canceled);
        if (!publishFile(outputPath, QFileInfo(options.destination).absoluteFilePath(), options.overwrite))
            throw Failure("Cannot publish the finished video. The destination may be open, read-only, or "
                          "already exist.");
        QJsonArray audioEvents;
        for (const auto& span : audioSpans)
            audioEvents.append(QJsonObject{{"source", span.path},
                                           {"start", span.start},
                                           {"end", span.end},
                                           {"sourceStart", span.sourceStart},
                                           {"stream", span.stream}});
        report = {{"gpu", gpu},
                  {"gpuDecode", device.decodeName},
                  {"gpuEncode", device.encodeName},
                  {"decodeAdapter", options.decodeAdapter},
                  {"encodeGpu", options.encodeGpu},
                  {"encoder", options.softwareEncoder ? "libx264" : "h264_nvenc"},
                  {"seed", double(options.seed)},
                  {"frames", double(options.frameCount())},
                  {"duration", options.outputDuration()},
                  {"elapsedSeconds", elapsed.elapsed() / 1000.0},
                  {"decodeUploadMs", decodeMs},
                  {"renderSubmitMs", renderMs},
                  {"encoderWriteMs", encodeMs},
                  {"softwareDecoderRetries", softwareDecoders},
                  {"clips", events},
                  {"audio", audioEvents},
                  {"settings", settings.json()},
                  {"metadata", metadata}};
        emit progress("Completed", options.frameCount(), options.frameCount(), elapsed.elapsed() / 1000.0);
        emit result(true, false, options.destination);
    } catch (const std::exception& e) {
        QString message = QString::fromUtf8(e.what());
        if (!options.softwareEncoder && !canceled.load() && message.contains("nvenc", Qt::CaseInsensitive)) {
            if (!device.encodeMatched)
                message += "\nNVENC could not be matched to the compositing GPU, so it fell back to "
                           "device " +
                           QString::number(options.encodeGpu) + ".";
            message += "\nYou can retry using CPU fallback in the Export dialog.";
        }
        emit result(false, canceled.load(), message);
    }
}
} // namespace kaleido
