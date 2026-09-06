#include "export_dialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <memory>

namespace kaleido {
ExportDialog::ExportDialog(const Settings& settings, const QJsonObject& saved, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Export video");
    setMinimumWidth(540);
    auto* body = new QVBoxLayout(this);
    auto* intro = new QLabel("Create a fresh sequence with your current applied settings.");
    intro->setWordWrap(true);
    body->addWidget(intro);
    auto* form = new QFormLayout;
    body->addLayout(form);
    auto* duration = new QDoubleSpinBox;
    duration->setRange(.001, 86400);
    duration->setDecimals(3);
    duration->setSuffix(" seconds");
    duration->setValue(saved["duration"].toDouble(60));
    form->addRow("Duration", duration);
    auto* size = new QComboBox;
    size->addItems({"720p · 1280 × 720", "1080p · 1920 × 1080", "4K UHD · 3840 × 2160"});
    size->setCurrentIndex(std::clamp(saved["size"].toInt(1), 0, 2));
    form->addRow("Canvas · 16:9", size);
    auto* fps = new QComboBox;
    fps->addItems({"30 fps", "60 fps"});
    fps->setCurrentIndex(saved["fps"].toInt(60) == 30 ? 0 : 1);
    form->addRow("Frame rate", fps);
    auto* quality = new QComboBox;
    quality->addItems({"Low · smaller drafts", "Medium · balanced", "High · detailed"});
    quality->setCurrentIndex(std::clamp(saved["quality"].toInt(2), 0, 2));
    form->addRow("Quality", quality);
    auto* encoder = new QComboBox;
    encoder->addItems({"NVIDIA hardware · H.264 MP4", "CPU fallback · H.264 MP4"});
    form->addRow("Encoder", encoder);
    auto* filename = new QLineEdit;
    QString folder =
        saved["folder"].toString(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    if (!QFileInfo(folder).isDir())
        folder = QDir::homePath();
    filename->setText(QDir(folder).filePath(
        "Kaleidowall-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".mp4"));
    auto* browse = new QPushButton("Browse…");
    auto* destinationRow = new QHBoxLayout;
    destinationRow->addWidget(filename);
    destinationRow->addWidget(browse);
    form->addRow("Destination", destinationRow);
    connect(browse, &QPushButton::clicked, this, [this, filename] {
        const auto path =
            QFileDialog::getSaveFileName(this, "Export destination", filename->text(), "MP4 video (*.mp4)",
                                         nullptr, QFileDialog::DontConfirmOverwrite);
        if (!path.isEmpty())
            filename->setText(path.endsWith(".mp4", Qt::CaseInsensitive) ? path : path + ".mp4");
    });
    auto* audio = new QComboBox;
    audio->addItems({"None", "Follow clip audio", "Import MP3 / WAV"});
    audio->setCurrentIndex(settings.muted ? 0 : 1);
    form->addRow("Audio", audio);
    auto* track = new QLineEdit;
    track->setReadOnly(true);
    track->setPlaceholderText("Choose a soundtrack");
    auto* import = new QPushButton("Import…");
    auto* trackRow = new QHBoxLayout;
    trackRow->addWidget(track);
    trackRow->addWidget(import);
    form->addRow("Soundtrack", trackRow);
    auto* match = new QCheckBox("Match audio duration");
    match->setChecked(true);
    form->addRow("", match);
    auto* volume = new QSpinBox;
    volume->setRange(0, 100);
    volume->setSuffix(" %");
    volume->setValue(settings.volume);
    form->addRow("Export volume", volume);
    auto* hint = new QLabel;
    hint->setWordWrap(true);
    body->addWidget(hint);
    auto* note = new QLabel("Imported audio replaces clip audio. With duration matching off, longer audio is "
                            "trimmed and shorter audio ends in silence. Playback pauses during export.");
    note->setWordWrap(true);
    body->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* start = buttons->addButton("Start export", QDialogButtonBox::AcceptRole);
    start->setObjectName("primary");
    body->addWidget(buttons);
    auto* probe = new QProcess(this);
    auto* probeTimeout = new QTimer(this);
    probeTimeout->setSingleShot(true);
    auto audibleDuration = std::make_shared<double>(0);
    auto probeOutput = std::make_shared<QByteArray>();
    auto update = [=] {
        const bool imported = audio->currentIndex() == 2;
        const bool probing = probe->state() != QProcess::NotRunning;
        track->setEnabled(imported);
        import->setEnabled(imported && !probing);
        match->setEnabled(imported && !probing);
        volume->setEnabled(audio->currentIndex() != 0);
        duration->setEnabled(!imported || !match->isChecked());
        start->setEnabled(!probing && (!imported || *audibleDuration > 0));
        if (imported && match->isChecked() && *audibleDuration > 0)
            duration->setValue(*audibleDuration);
        const int rate = fps->currentIndex() ? 60 : 30;
        const double requested =
            imported && match->isChecked() && *audibleDuration > 0 ? *audibleDuration : duration->value();
        hint->setText(
            probing ? "Measuring the decoded soundtrack length…"
                    : QString("Output: %1 seconds · %2 frames. Duration rounds up to a complete video frame.")
                          .arg(std::ceil(requested * rate - 1e-8) / rate, 0, 'f', 3)
                          .arg(qint64(std::ceil(requested * rate - 1e-8))));
    };
    connect(audio, &QComboBox::currentIndexChanged, this, update);
    connect(match, &QCheckBox::toggled, this, update);
    connect(duration, &QDoubleSpinBox::valueChanged, this, update);
    connect(fps, &QComboBox::currentIndexChanged, this, update);
    connect(probe, &QProcess::readyReadStandardOutput, this,
            [=] { *probeOutput = (*probeOutput + probe->readAllStandardOutput()).right(32768); });
    connect(probeTimeout, &QTimer::timeout, this, [=] { probe->kill(); });
    connect(probe, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            probeTimeout->stop();
            update();
            hint->setText("FFmpeg could not start. Install it or place it beside Kaleidowall.exe.");
        }
    });
    connect(probe, &QProcess::finished, this, [=](int code, QProcess::ExitStatus status) {
        probeTimeout->stop();
        *probeOutput += probe->readAllStandardOutput();
        const QRegularExpression pattern("out_time_us=(\\d+)");
        auto matches = pattern.globalMatch(QString::fromUtf8(*probeOutput));
        double length = 0;
        while (matches.hasNext())
            length = matches.next().captured(1).toDouble() / 1e6;
        if (code == 0 && status == QProcess::NormalExit && length > 0 && length <= 86400)
            *audibleDuration = length;
        update();
        if (*audibleDuration <= 0)
            hint->setText("Cannot read this soundtrack, or it exceeds 24 hours. Choose another MP3/WAV. " +
                          QString::fromUtf8(probe->readAllStandardError()).right(500));
    });
    importSoundtrack = [=](const QString& path) {
        if (path.isEmpty())
            return;
        audio->setCurrentIndex(2);
        *audibleDuration = 0;
        probeOutput->clear();
        track->setText(path);
        match->setChecked(true);
        // Decode to a null sink: final PCM timestamps exclude MP3 encoder padding, unlike container duration.
        probe->start(mediaTool("ffmpeg"),
                     {"-hide_banner", "-loglevel", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-vn",
                      "-ac", "2", "-ar", "48000", "-progress", "pipe:1", "-f", "null", "-"});
        probe->closeWriteChannel();
        probeTimeout->start(120000);
        update();
    };
    connect(import, &QPushButton::clicked, this, [=, this] {
        loadSoundtrack(
            QFileDialog::getOpenFileName(this, "Import soundtrack", track->text(), "Audio (*.mp3 *.wav)"));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [=] {
        probeTimeout->stop();
        if (probe->state() != QProcess::NotRunning)
            probe->kill();
    });
    connect(start, &QPushButton::clicked, this, [=, this] {
        options.size = size->currentIndex() == 0   ? QSize(1280, 720)
                       : size->currentIndex() == 1 ? QSize(1920, 1080)
                                                   : QSize(3840, 2160);
        options.duration =
            audio->currentIndex() == 2 && match->isChecked() ? *audibleDuration : duration->value();
        options.fps = fps->currentIndex() ? 60 : 30;
        options.quality = quality->currentIndex();
        options.softwareEncoder = encoder->currentIndex() == 1;
        options.destination = QDir::cleanPath(filename->text().trimmed());
        options.audioMode = audio->currentIndex();
        options.audioFile = track->text();
        options.volume = volume->value();
        options.seed = QRandomGenerator::global()->generate();
        options.overwrite = false;
        if (QFileInfo::exists(options.destination)) {
            if (QMessageBox::question(
                    this, "Replace video?",
                    "Replace this file after the export finishes successfully?\n" + options.destination,
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
            options.overwrite = true;
        }
        const auto invalid = options.validate();
        if (!invalid.isEmpty()) {
            QMessageBox::warning(this, "Check export settings", invalid);
            return;
        }
        accept();
    });
    update();
}
QJsonObject ExportDialog::preferences() const {
    return {{"duration", options.duration},
            {"size", options.size.height() == 720    ? 0
                     : options.size.height() == 1080 ? 1
                                                     : 2},
            {"fps", options.fps},
            {"quality", options.quality},
            {"folder", QFileInfo(options.destination).absolutePath()}};
}
} // namespace kaleido
