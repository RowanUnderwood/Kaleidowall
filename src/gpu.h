#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace kaleido {
// Export decode, encode, and composition must land on one card, but FFmpeg addresses GPUs through
// three unrelated index spaces: DXGI adapter order for d3d11va, the NVENC/CUDA ordinal for
// h264_nvenc, and NVML/PCI order for nvidia-smi. None of them agree on a multi-GPU machine, so an
// index is only meaningful next to the enumeration it came from. Resolve by device name instead.
struct GpuSelection {
    int dxgi = 0;  // -hwaccel_device for d3d11va
    int nvenc = 0; // -gpu for h264_nvenc, under CUDA_DEVICE_ORDER=PCI_BUS_ID
    QString decodeName, encodeName;
    bool decodeMatched = false, encodeMatched = false;
};

// Pure helpers, exposed for tests: neither touches hardware.
QVector<QString> parseNvencDevices(const QString& ffmpegStderr);
QString normalizeRendererName(const QString& glRenderer);

// Enumerates DXGI adapters and NVENC devices, then matches both against the compositing GPU named
// by GL_RENDERER. Probing runs once per process; later calls reuse the first result.
GpuSelection selectExportGpu(const QString& glRenderer);
} // namespace kaleido
