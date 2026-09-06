#include "compositor.h"
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <algorithm>

namespace kaleido {
int maskKind(const QString& mode) {
    return mode == "Circles" ? 1 : mode == "Hexagons" ? 2 : 0;
}
double easedProgress(double time, double start, double duration) {
    const double t = duration <= 0 ? 1 : std::clamp((time - start) / duration, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
QRectF interpolateRect(const QRectF& from, const QRectF& to, double p) {
    return {from.topLeft() * (1 - p) + to.topLeft() * p, from.size() * (1 - p) + to.size() * p};
}
bool Compositor::initialize() {
    if (!initializeOpenGLFunctions())
        return false;
    const char* vertex = R"(#version 330 core
        layout(location=0) in vec2 position;
        out vec2 uv;
        uniform vec4 rect;
        void main(){uv=position;vec2 p=rect.xy+position*rect.zw;gl_Position=vec4(p.x*2.-1.,1.-p.y*2.,0.,1.);}
    )";
    const char* fragment = R"(#version 330 core
        in vec2 uv;out vec4 color;
        uniform sampler2D frame,chroma;
        uniform int yuv;
        uniform vec2 videoSize,viewSize;
        uniform vec3 backgroundColor;
        uniform float alpha,maskProgress;
        uniform int crop,oldMask,newMask;
        float shape(int kind,vec2 p){
            if(kind==0)return max(abs(p.x),abs(p.y))-.5;
            vec2 q=(p*viewSize)/min(viewSize.x,viewSize.y);
            if(kind==1)return length(q)-.485;
            q=abs(q);return max(q.y,dot(q,vec2(.8660254,.5)))-.465;
        }
        void main(){
            vec2 p=uv-.5;
            float d=mix(shape(oldMask,p),shape(newMask,p),maskProgress);
            float edge=1.-smoothstep(-.002,.002,d);
            if(edge<=0.)discard;
            float va=videoSize.x/videoSize.y,da=viewSize.x/viewSize.y;
            vec2 scale=vec2(1.);
            if(crop==1){if(va>da)scale.x=da/va;else scale.y=va/da;}
            else {if(va>da)scale.y=va/da;else scale.x=da/va;}
            vec2 t=p*scale+.5;
            vec3 rgb=texture(frame,t).rgb;
            if(yuv==1){
                float y=(rgb.r*255.-16.)/219.;
                vec2 c=(texture(chroma,t).rg*255.-128.)/224.;
                rgb=vec3(y+1.5748*c.y,y-.187324*c.x-.468124*c.y,y+1.8556*c.x);
            }
            if(any(lessThan(t,vec2(0.)))||any(greaterThan(t,vec2(1.))))rgb=backgroundColor;
            color=vec4(rgb,alpha*edge);
        }
    )";
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
        !program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !program.link())
        return false;
    const float vertices[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
    return true;
}
void Compositor::release() {
    if (vbo)
        glDeleteBuffers(1, &vbo);
    if (vao)
        glDeleteVertexArrays(1, &vao);
    vbo = vao = 0;
    program.removeAllShaders();
}
void Compositor::draw(GLuint target, QSize pixels, QSize logicalSize, const QColor& background, bool crop,
                      int oldMask, int newMask, float progress, const std::vector<DrawTile>& tiles) {
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, pixels.width(), pixels.height());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(background.redF(), background.greenF(), background.blueF(), 1);
    glClear(GL_COLOR_BUFFER_BIT);
    program.bind();
    program.setUniformValue("backgroundColor",
                            QVector3D(background.redF(), background.greenF(), background.blueF()));
    program.setUniformValue("oldMask", oldMask);
    program.setUniformValue("newMask", newMask);
    program.setUniformValue("maskProgress", progress);
    program.setUniformValue("crop", crop ? 1 : 0);
    program.setUniformValue("frame", 0);
    program.setUniformValue("chroma", 1);
    glBindVertexArray(vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    for (const auto& tile : tiles) {
        QRectF r = tile.rect;
        r.adjust(2.0 / logicalSize.width(), 2.0 / logicalSize.height(), -2.0 / logicalSize.width(),
                 -2.0 / logicalSize.height());
        if (r.width() <= 0 || r.height() <= 0)
            continue;
        program.setUniformValue("rect",
                                QVector4D(float(r.x()), float(r.y()), float(r.width()), float(r.height())));
        program.setUniformValue(
            "viewSize", QVector2D(float(r.width() * pixels.width()), float(r.height() * pixels.height())));
        program.setUniformValue("videoSize",
                                QVector2D(float(tile.videoSize.width()), float(tile.videoSize.height())));
        program.setUniformValue("alpha", tile.opacity);
        program.setUniformValue("yuv", tile.chromaTexture ? 1 : 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tile.chromaTexture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tile.texture);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindVertexArray(0);
    program.release();
    glDisable(GL_BLEND);
}
bool Nv12Converter::initialize() {
    if (!initializeOpenGLFunctions())
        return false;
    const char* vertex = R"(#version 330 core
        void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0.,1.);}
    )";
    const char* fragment = R"(#version 330 core
        uniform sampler2D source;
        uniform vec2 size;
        out vec4 color;
        vec3 pixel(vec2 p){return texture(source,vec2((p.x+.5)/size.x,1.-(p.y+.5)/size.y)).rgb;}
        void main(){
            vec2 p=floor(gl_FragCoord.xy);
            float value;
            if(p.y<size.y){
                float y=dot(pixel(p),vec3(.2126,.7152,.0722));value=(16.+219.*y)/255.;
            }else{
                vec2 q=vec2(floor(p.x/2.)*2.,(p.y-size.y)*2.);
                vec3 rgb=(pixel(q)+pixel(q+vec2(1,0))+pixel(q+vec2(0,1))+pixel(q+vec2(1,1)))*.25;
                float y=dot(rgb,vec3(.2126,.7152,.0722));
                float c=mod(p.x,2.)<1.?(rgb.b-y)/1.8556:(rgb.r-y)/1.5748;
                value=(128.+224.*c)/255.;
            }
            color=vec4(value,0,0,1);
        }
    )";
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
        !program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !program.link())
        return false;
    glGenVertexArrays(1, &vao);
    return true;
}
void Nv12Converter::draw(GLuint rgbTexture, GLuint destination, QSize size) {
    glBindFramebuffer(GL_FRAMEBUFFER, destination);
    glViewport(0, 0, size.width(), size.height() * 3 / 2);
    glDisable(GL_BLEND);
    program.bind();
    program.setUniformValue("source", 0);
    program.setUniformValue("size", QVector2D(float(size.width()), float(size.height())));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rgbTexture);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    program.release();
}
void Nv12Converter::release() {
    if (vao)
        glDeleteVertexArrays(1, &vao);
    vao = 0;
    program.removeAllShaders();
}
} // namespace kaleido
