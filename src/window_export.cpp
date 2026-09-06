#include "export_dialog.h"
#include "window.h"
#include <QAction>
#include <QDesktopServices>
#include <QFileInfo>
#include <QMessageBox>
#include <QPixmap>
#include <QStatusBar>
#include <QUrl>

namespace kaleido {
void Window::showExportDialog() {
    if (exportWorker)
        return;
    ExportDialog dialog(player->settings(), media->value("exportPreferences"), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    media->setValue("exportPreferences", dialog.preferences());
    startExport(dialog.options);
}
void Window::cancelExport() {
    if (!exportWorker)
        return;
    exportWorker->cancel();
    exportCancel->setEnabled(false);
    exportStatus->setText("Canceling export…");
}
void Window::startExport(const ExportOptions& options) {
    if (exportWorker)
        return;
    if (!exportBar) {
        exportBar = new QToolBar("Export progress", this);
        exportBar->setMovable(false);
        addToolBarBreak(Qt::BottomToolBarArea);
        addToolBar(Qt::BottomToolBarArea, exportBar);
        exportStatus = new QLabel;
        exportStatus->setMinimumWidth(260);
        exportStatus->setMaximumWidth(720);
        exportStatus->setWordWrap(true);
        exportStatus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        exportBar->addWidget(exportStatus);
        exportProgress = new QProgressBar;
        exportProgress->setRange(0, 1000);
        exportProgress->setMaximumWidth(220);
        exportBar->addWidget(exportProgress);
        exportCancel = new QPushButton("Cancel");
        exportCancelAction = exportBar->addWidget(exportCancel);
        exportOpen = new QPushButton("Open file");
        exportOpenAction = exportBar->addWidget(exportOpen);
        exportFolder = new QPushButton("Show in folder");
        exportFolderAction = exportBar->addWidget(exportFolder);
        connect(exportCancel, &QPushButton::clicked, this, &Window::cancelExport);
        connect(exportOpen, &QPushButton::clicked, this,
                [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(completedExport)); });
        connect(exportFolder, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(completedExport).absolutePath()));
        });
        connect(&exportPreviewTimer, &QTimer::timeout, this, [this] {
            if (!exportWorker)
                return;
            auto frame = exportWorker->takePreview();
            if (!frame.isNull() && !isMinimized())
                exportPreview->setPixmap(QPixmap::fromImage(frame).scaled(
                    exportPreview->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
        });
    }
    exportBar->show();
    exportCancelAction->setVisible(true);
    exportCancel->setEnabled(true);
    exportOpenAction->setVisible(false);
    exportFolderAction->setVisible(false);
    exportProgress->setValue(0);
    exportProgress->setFormat("Preparing…");
    exportStatus->setText("Preparing export…");
    exportPreview->setText("Preparing export…");
    resumeAfterExport = player->playing() && !player->isPaused();
    if (resumeAfterExport)
        player->playPause();
    player->setExportLocked(true);
    header->setEnabled(false);
    controls->setEnabled(false);
    dock->setEnabled(false);
    restoreDockAfterExport = dock->isVisible();
    dock->hide();
    canvasStack->setCurrentWidget(exportPreview);
    exportSurface = new QOffscreenSurface;
    exportSurface->setFormat(QSurfaceFormat::defaultFormat());
    exportSurface->create();
    exportWorker = new ExportWorker(options, player->settings(), media->videos(), exportSurface, this);
    auto outcome = std::make_shared<QJsonObject>();
    connect(exportWorker, &ExportWorker::progress, this,
            [this, targetFps = options.fps](const QString& stage, qint64 completed, qint64 total,
                                            double elapsed) {
                const int percent = stage == "Completed"
                                        ? 100
                                        : std::min(99, int(completed * 100 / std::max(qint64(1), total)));
                exportProgress->setValue(percent * 10);
                exportProgress->setFormat(QString::number(percent) + "%");
                QString text = QString("%1 · %2 / %3 frames · %4 s elapsed")
                                   .arg(stage)
                                   .arg(completed)
                                   .arg(total)
                                   .arg(elapsed, 0, 'f', 0);
                if (stage == "Rendering" && completed > 0 && elapsed >= 3) {
                    const double fps = completed / elapsed;
                    text += QString(" · %1× speed · ~%2 s rendering remaining")
                                .arg(fps / targetFps, 0, 'f', 2)
                                .arg((total - completed) / fps, 0, 'f', 0);
                }
                exportStatus->setText(text);
            });
    connect(exportWorker, &ExportWorker::result, this,
            [outcome](bool success, bool canceled, const QString& message) {
                *outcome = {{"success", success}, {"canceled", canceled}, {"message", message}};
            });
    connect(exportWorker, &QThread::finished, this, [this, outcome, options] {
        exportPreviewTimer.stop();
        auto* worker = exportWorker;
        const auto report = worker->report;
        exportWorker = nullptr;
        worker->deleteLater();
        delete exportSurface;
        exportSurface = nullptr;
        player->setExportLocked(false);
        header->setEnabled(true);
        controls->setEnabled(true);
        dock->setEnabled(true);
        dock->setVisible(restoreDockAfterExport);
        canvasStack->setCurrentWidget(player);
        if (resumeAfterExport && !closeAfterExport)
            player->playPause();
        exportCancelAction->setVisible(false);
        const bool success = (*outcome)["success"].toBool(), canceled = (*outcome)["canceled"].toBool();
        const QString message = (*outcome)["message"].toString("Export stopped unexpectedly.");
        if (success) {
            completedExport = options.destination;
            exportStatus->setText(QString("Export complete · %1 · %2 seconds")
                                      .arg(QFileInfo(completedExport).fileName())
                                      .arg(report["elapsedSeconds"].toDouble(), 0, 'f', 1));
            exportOpenAction->setVisible(true);
            exportFolderAction->setVisible(true);
        } else {
            exportStatus->setText(canceled ? "Export canceled. Destination unchanged."
                                           : "Export failed. " + message);
            exportStatus->setToolTip(message);
            exportProgress->setFormat(canceled ? "Canceled" : "Failed");
        }
        emit exportFinished(success, canceled, message, report);
        if (closeAfterExport)
            close();
    });
    exportPreviewTimer.start(333);
    exportWorker->start();
}
} // namespace kaleido
