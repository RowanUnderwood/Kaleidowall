#pragma once
#include "canvas.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QToolBar>

namespace prism {
class Window : public QMainWindow {
    Q_OBJECT
  public:
    explicit Window(const QString& dataDir);
    ~Window() override;
    Canvas* canvas() const {
        return player;
    }
    Library* library() const {
        return media;
    }
    void showPanel(int);
    void toggleFullscreen();

  protected:
    void closeEvent(QCloseEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

  private:
    QWidget* settingsPage();
    QWidget* libraryPage();
    QWidget* performancePage();
    void syncSettings();
    void refreshBackgroundColor();
    void applySettings();
    void refreshLibrary();
    void editVideo(int row);
    void refreshPresets();
    void revealControls();
    Library* media;
    Canvas* player;
    QDockWidget* dock;
    QStackedWidget* panels;
    QToolBar *header, *controls;
    QPushButton *playButton, *muteButton;
    QPushButton* backgroundColorButton;
    QString selectedBackgroundColor;
    QSlider* volume;
    QLabel* statusLabel;
    QComboBox *presets, *audioMode;
    QSpinBox *minSlots, *maxSlots, *fps, *buffer, *texture;
    QDoubleSpinBox *clipMin, *clipMax, *layoutMin, *layoutMax, *transition, *skipStart, *skipEnd;
    QCheckBox *percent, *reduced, *duplicates, *crop, *hwdec;
    QMap<QString, QSpinBox*> modeWeights;
    QListWidget* folderList;
    QTableWidget* videoTable;
    QLineEdit* search;
    QLabel *librarySummary, *scanLabel;
    QPushButton *scanButton, *removeFolderButton, *addFolderButton;
    QTextEdit* performance;
    QTimer hideTimer, statsTimer;
    QVector<Video> rows;
    bool syncing = false;
};
} // namespace prism
