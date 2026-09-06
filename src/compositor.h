#pragma once
#include <QColor>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QRectF>
#include <QSize>
#include <vector>

namespace kaleido {
int maskKind(const QString& mode);
double easedProgress(double time, double start, double duration);
QRectF interpolateRect(const QRectF& from, const QRectF& to, double progress);
struct DrawTile {
    GLuint texture = 0;
    QSize videoSize;
    QRectF rect;
    float opacity = 1;
    GLuint chromaTexture = 0; // Nonzero: texture is limited-range BT.709 NV12 luma.
};
// Context-owned; initialize, draw and release with its GL context current.
class Compositor : protected QOpenGLFunctions_3_3_Core {
  public:
    bool initialize();
    void release();
    QString error() const {
        return program.log();
    }
    void draw(GLuint target, QSize pixels, QSize logicalSize, const QColor& background, bool crop,
              int oldMask, int newMask, float progress, const std::vector<DrawTile>& tiles);

  private:
    QOpenGLShaderProgram program;
    GLuint vao = 0, vbo = 0;
};
// Packs an RGB framebuffer into an R8 target containing top-down NV12 bytes. Color conversion
// and 2x2 chroma downsampling execute on the GPU, avoiding CPU RGB conversion before NVENC.
class Nv12Converter : protected QOpenGLFunctions_3_3_Core {
  public:
    bool initialize();
    void draw(GLuint rgbTexture, GLuint destination, QSize size);
    void release();

  private:
    QOpenGLShaderProgram program;
    GLuint vao = 0;
};
} // namespace kaleido
