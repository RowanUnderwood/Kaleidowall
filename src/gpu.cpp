#include "gpu.h"
#include "exporter.h"
#include <QProcess>
#include <QRegularExpression>
#include <cwchar>
#ifdef Q_OS_WIN
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace kaleido {
QVector<QString> parseNvencDevices(const QString& stderrText) {
    // Lines look like: [h264_nvenc @ 0000...] [ GPU #1 - < NVIDIA GeForce RTX 5090 > has Compute SM 12.0 ]
    static const QRegularExpression pattern(R"(\[\s*GPU\s+#(\d+)\s+-\s+<\s*(.*?)\s*>)");
    QVector<QString> out;
    auto it = pattern.globalMatch(stderrText);
    while (it.hasNext()) {
        auto match = it.next();
        const int index = match.captured(1).toInt();
        if (index < 0 || index > 63)
            continue;
        while (out.size() <= index)
            out << QString();
        out[index] = match.captured(2);
    }
    return out;
}
QString normalizeRendererName(const QString& glRenderer) {
    // NVIDIA reports "NVIDIA GeForce RTX 5090/PCIe/SSE2"; DXGI and NVENC report the bare model.
    return glRenderer.section('/', 0, 0).trimmed();
}
namespace {
QVector<QString> probeNvencDevices() {
    const QString ffmpeg = mediaTool("ffmpeg");
    if (ffmpeg.isEmpty())
        return {};
    QProcess probe;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("CUDA_DEVICE_ORDER", "PCI_BUS_ID");
    probe.setProcessEnvironment(environment);
    // An out-of-range -gpu makes nvenc print its whole device list before it fails. The non-zero
    // exit is expected; only the listing matters.
    probe.start(ffmpeg, {"-hide_banner", "-v", "verbose", "-nostdin", "-f", "lavfi", "-i",
                         "nullsrc=s=64x64:d=0.04", "-c:v", "h264_nvenc", "-gpu", "999", "-f", "null", "-"});
    if (!probe.waitForStarted(5000))
        return {};
    if (!probe.waitForFinished(15000)) {
        probe.kill();
        probe.waitForFinished(2000);
        return {};
    }
    return parseNvencDevices(QString::fromLocal8Bit(probe.readAllStandardError()));
}
QVector<QString> probeDxgiAdapters() {
#ifdef Q_OS_WIN
    // Enumeration order here is the order FFmpeg's d3d11va walks for -hwaccel_device, so software
    // adapters are kept in place rather than skipped: the positions have to line up.
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return {};
    QVector<QString> out;
    for (UINT i = 0; i < 64; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        out << (SUCCEEDED(adapter->GetDesc1(&desc))
                    ? QString::fromWCharArray(desc.Description, int(wcsnlen(desc.Description, 128)))
                    : QString());
    }
    return out;
#else
    return {};
#endif
}
int indexOfName(const QVector<QString>& names, const QString& want) {
    if (want.isEmpty())
        return -1;
    for (int i = 0; i < names.size(); ++i)
        if (!names[i].isEmpty() && names[i].compare(want, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}
} // namespace
GpuSelection selectExportGpu(const QString& glRenderer) {
    static bool probed = false;
    static QVector<QString> nvencDevices, dxgiAdapters;
    if (!probed) {
        probed = true;
        nvencDevices = probeNvencDevices();
        dxgiAdapters = probeDxgiAdapters();
    }
    const QString want = normalizeRendererName(glRenderer);
    GpuSelection selection;
    const int dxgi = indexOfName(dxgiAdapters, want);
    const int nvenc = indexOfName(nvencDevices, want);
    // Index 0 is the fallback in both spaces. With CUDA_DEVICE_ORDER=PCI_BUS_ID that is the first
    // card in PCI order rather than whichever one the driver rates fastest.
    selection.dxgi = dxgi >= 0 ? dxgi : 0;
    selection.nvenc = nvenc >= 0 ? nvenc : 0;
    selection.decodeMatched = dxgi >= 0;
    selection.encodeMatched = nvenc >= 0;
    selection.decodeName = selection.dxgi < dxgiAdapters.size() ? dxgiAdapters[selection.dxgi] : QString();
    selection.encodeName = selection.nvenc < nvencDevices.size() ? nvencDevices[selection.nvenc] : QString();
    return selection;
}
} // namespace kaleido
