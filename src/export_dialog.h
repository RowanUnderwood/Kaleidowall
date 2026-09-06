#pragma once
#include "exporter.h"
#include <QDialog>
#include <functional>

namespace kaleido {
class ExportDialog : public QDialog {
  public:
    ExportDialog(const Settings& settings, const QJsonObject& preferences, QWidget* parent = nullptr);
    ExportOptions options;
    QJsonObject preferences() const;
    void loadSoundtrack(const QString& path) {
        importSoundtrack(path);
    }

  private:
    std::function<void(const QString&)> importSoundtrack;
};
} // namespace kaleido
