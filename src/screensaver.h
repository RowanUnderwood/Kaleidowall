#pragma once
#include "canvas.h"
#include <QPointer>

namespace kaleido {
enum class SaverMode { Configure, Run, Preview, Invalid };
struct SaverArguments {
    SaverMode mode = SaverMode::Configure;
    quintptr parent = 0;
    QString dataDir, report;
    int testSeconds = 0, testMirrors = 0;
    bool testWindowed = false;
};
SaverArguments parseSaverArguments(const QStringList& arguments);
CanvasOptions saverCanvasOptions(bool preview, bool mute);

// Each mirror shares decoder textures with the sole Canvas. No decoder or audio pool lives here.
class SaverMirror : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
  public:
    explicit SaverMirror(Canvas* source, QWidget* parent = nullptr);
    ~SaverMirror() override;
  protected:
    void initializeGL() override;
    void paintGL() override;
  private:
    QPointer<Canvas> source;
    Compositor compositor;
    bool initialized = false;
};
int runScreensaver(const SaverArguments& arguments);
} // namespace kaleido
