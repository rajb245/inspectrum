/*
 *  Copyright (C) 2015, Mike Walters <mike@flomp.net>
 *
 *  This file is part of inspectrum.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "spectrogramplot.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmapCache>
#include <QRect>
#include <liquid/liquid.h>
#include <algorithm>
#include <functional>
#include <cstdlib>
#include <limits>
#include "util.h"

// GL format constants (may not be in GL 1.1 headers)
#ifndef GL_R32F
#define GL_R32F 0x822E
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif

// ---------------------------------------------------------------------------
// GLSL shaders for GPU-accelerated colormap rendering
// ---------------------------------------------------------------------------
static const char *glVertSrc = R"(
    attribute vec2 aPos;
    uniform vec2 uDstPos;
    uniform vec2 uDstSize;
    uniform vec2 uViewport;
    uniform vec2 uTexMin;
    uniform vec2 uTexMax;
    varying vec2 vTexCoord;
    void main() {
        vec2 pixel = uDstPos + aPos * uDstSize;
        gl_Position = vec4(
            2.0 * pixel.x / uViewport.x - 1.0,
            1.0 - 2.0 * pixel.y / uViewport.y,
            0.0, 1.0
        );
        vTexCoord = uTexMin + aPos * (uTexMax - uTexMin);
    }
)";

static const char *glFragSrc = R"(
    varying highp vec2 vTexCoord;
    uniform sampler2D uFFTData;
    uniform sampler2D uColormap;
    uniform highp float uPowerMax;
    uniform highp float uPowerRange;
    void main() {
        // Texture is width=fftSize, height=linesPerTile.
        // vTexCoord.x = time fraction, vTexCoord.y = frequency fraction.
        // Swap to (freq, time) to match the transposed memory layout.
        highp float power = texture2D(uFFTData, vec2(vTexCoord.y, vTexCoord.x)).r;
        highp float norm = clamp((power - uPowerMax) * uPowerRange, 0.0, 1.0);
        gl_FragColor = texture2D(uColormap, vec2(norm, 0.5));
    }
)";


SpectrogramPlot::SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>> src) : Plot(src), inputSource(src), fftSize(512), tuner(fftSize, this)
{
    setFFTSize(fftSize);
    zoomLevel = 1;
    nfftSkip = 1;
    powerMax = 0.0f;
    powerMin = -50.0f;
    sampleRate = 0;
    frequencyScaleEnabled = false;
    sigmfAnnotationsEnabled = true;
    sigmfAnnotationLabels = true;
    sigmfAnnotationColors = true;

    for (int i = 0; i < 256; i++) {
        float p = (float)i / 256;
        colormap[i] = QColor::fromHsvF(p * 0.83f, 1.0, 1.0 - p).rgba();
    }

    tunerTransform = std::make_shared<TunerTransform>(src);
    connect(&tuner, &Tuner::tunerMoved, this, &SpectrogramPlot::tunerMoved);
}

void SpectrogramPlot::invalidateEvent()
{
    // HACK: this makes sure we update the height for real signals (as InputSource is passed here before the file is opened)
    setFFTSize(fftSize);

    pixmapCache.clear();
    fftCache.clear();
    glTileCacheDirty = true;
    emit repaint();
}

void SpectrogramPlot::paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (tunerEnabled())
        tuner.paintFront(painter, rect, sampleRange);

    if (frequencyScaleEnabled)
        paintFrequencyScale(painter, rect);

    if (sigmfAnnotationsEnabled)
        paintAnnotations(painter, rect, sampleRange);
}

void SpectrogramPlot::paintFrequencyScale(QPainter &painter, QRect &rect)
{
    if (sampleRate == 0) {
        return;
    }

    if (sampleRate / 2 > UINT64_MAX) {
        return;
    }

    // At which pixel is F_+sampleRate/2
    int y = rect.y();

    int plotHeight = rect.height();
    if (inputSource->realSignal())
        plotHeight *= 2;

    double bwPerPixel = (double)sampleRate / plotHeight;
    int tickHeight = 50;

    uint64_t bwPerTick = 10 * pow(10, floor(log(bwPerPixel * tickHeight) / log(10)));

    if (bwPerTick < 1) {
        return;
    }

    painter.save();

    QPen pen(Qt::white, 1, Qt::SolidLine);
    painter.setPen(pen);
    QFontMetrics fm(painter.font());


    uint64_t tick = 0;

    while (tick <= sampleRate / 2) {

        int tickpy = plotHeight / 2 - tick / bwPerPixel + y;
        int tickny = plotHeight / 2 + tick / bwPerPixel + y;

        if (!inputSource->realSignal())
            painter.drawLine(0, tickny, 30, tickny);
        painter.drawLine(0, tickpy, 30, tickpy);

        if (tick != 0) {
            char buf[128];

            if (bwPerTick % 1000000000 == 0) {
                snprintf(buf, sizeof(buf), "-%lu GHz", tick / 1000000000);
            } else if (bwPerTick % 1000000 == 0) {
                snprintf(buf, sizeof(buf), "-%lu MHz", tick / 1000000);
            } else if(bwPerTick % 1000 == 0) {
                snprintf(buf, sizeof(buf), "-%lu kHz", tick / 1000);
            } else {
                snprintf(buf, sizeof(buf), "-%lu Hz", tick);
            }

            if (!inputSource->realSignal())
                painter.drawText(5, tickny - 5, buf);

            buf[0] = ' ';
            painter.drawText(5, tickpy + 15, buf);
        }

        tick += bwPerTick;
    }

    // Draw small ticks
    bwPerTick /= 10;

    if (bwPerTick >= 1 ) {
        tick = 0;
        while (tick <= sampleRate / 2) {

            int tickpy = plotHeight / 2 - tick / bwPerPixel + y;
            int tickny = plotHeight / 2 + tick / bwPerPixel + y;

            if (!inputSource->realSignal())
                painter.drawLine(0, tickny, 3, tickny);
            painter.drawLine(0, tickpy, 3, tickpy);

            tick += bwPerTick;
        }
    }
    painter.restore();
}

void SpectrogramPlot::paintAnnotations(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    // Pixel (from the top) at which 0 Hz sits
    int zero = rect.y() + rect.height() / 2;

    painter.save();
    QPen pen(Qt::white, 1, Qt::SolidLine);
    painter.setPen(pen);
    QFontMetrics fm(painter.font());

    visibleAnnotationLocations.clear();

    for (int i = 0; i < inputSource->annotationList.size(); i++) {
        Annotation a = inputSource->annotationList.at(i);

        size_t labelLength = fm.boundingRect(a.label).width() * getStride();

        // Check if:
        //  (1) End of annotation (might be maximum, or end of label text) is still visible in time
        //  (2) Part of the annotation is already visible in time
        //
        // Currently there is no check if the annotation is visible in frequency. This is a
        // possible performance improvement
        //
        size_t start = a.sampleRange.minimum;
        size_t end = std::max(a.sampleRange.minimum + labelLength, a.sampleRange.maximum);

        if(start <= sampleRange.maximum && end >= sampleRange.minimum) {

            double frequency = a.frequencyRange.maximum - inputSource->getFrequency();
            int x = (a.sampleRange.minimum - sampleRange.minimum) / getStride();
            int y = zero - frequency / sampleRate * rect.height();
            int height = (a.frequencyRange.maximum - a.frequencyRange.minimum) / sampleRate * rect.height();
            int width = (a.sampleRange.maximum - a.sampleRange.minimum) / getStride();

            if (sigmfAnnotationColors) {
                painter.setPen(a.boxColor);
            }
            if (sigmfAnnotationLabels) {
                // Draw the label 2 pixels above the box
                painter.drawText(x, y - 2, a.label);
            }
            painter.drawRect(x, y, width, height);

            visibleAnnotationLocations.emplace_back(a, x, y, width, height);
        }
    }

    painter.restore();
}

QString *SpectrogramPlot::mouseAnnotationComment(const QMouseEvent *event) {
    auto pos = event->pos();
    int mouse_x = pos.x();
    int mouse_y = pos.y();

    for (auto& a : visibleAnnotationLocations) {
        if (!a.annotation.comment.isEmpty() && a.isInside(mouse_x, mouse_y)) {
            return &a.annotation.comment;
        }
    }
    return nullptr;
}

void SpectrogramPlot::paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    if (!inputSource || inputSource->count() == 0)
        return;

    // Use OpenGL accelerated path if a GL context is available
    if (QOpenGLContext::currentContext() && !glFailed) {
        paintMidGL(painter, rect, sampleRange);
        return;
    }

    // CPU fallback
    paintMidCPU(painter, rect, sampleRange);
}

void SpectrogramPlot::paintMidCPU(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    size_t sampleOffset = sampleRange.minimum % (getStride() * linesPerTile());
    size_t tileID = sampleRange.minimum - sampleOffset;
    int xoffset = sampleOffset / getStride();

    // Paint first (possibly partial) tile
    painter.drawPixmap(QRect(rect.left(), rect.y(), linesPerTile() - xoffset, height()), *getPixmapTile(tileID), QRect(xoffset, 0, linesPerTile() - xoffset, height()));
    tileID += getStride() * linesPerTile();

    // Paint remaining tiles
    for (int x = linesPerTile() - xoffset; x < rect.right(); x += linesPerTile()) {
        painter.drawPixmap(QRect(x, rect.y(), linesPerTile(), height()), *getPixmapTile(tileID), QRect(0, 0, linesPerTile(), height()));
        tileID += getStride() * linesPerTile();
    }
}

// ---------------------------------------------------------------------------
// OpenGL accelerated rendering
// ---------------------------------------------------------------------------

bool SpectrogramPlot::initGL(QOpenGLFunctions *f)
{
    // Compile shader
    glShader = new QOpenGLShaderProgram();
    if (!glShader->addShaderFromSourceCode(QOpenGLShader::Vertex, glVertSrc) ||
        !glShader->addShaderFromSourceCode(QOpenGLShader::Fragment, glFragSrc) ||
        !glShader->link()) {
        fprintf(stderr, "SpectrogramPlot: GL shader failed: %s\n",
                glShader->log().toUtf8().constData());
        fflush(stderr);
        delete glShader;
        glShader = nullptr;
        return false;
    }

    // Unit quad VBO (triangle strip: TL, TR, BL, BR)
    float quadVerts[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };
    f->glGenBuffers(1, &glQuadVBO);
    f->glBindBuffer(GL_ARRAY_BUFFER, glQuadVBO);
    f->glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    f->glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Colormap texture (256x1 RGBA)
    uint8_t cmapData[256 * 4];
    for (int i = 0; i < 256; i++) {
        QRgb c = colormap[i];
        cmapData[i * 4 + 0] = qRed(c);
        cmapData[i * 4 + 1] = qGreen(c);
        cmapData[i * 4 + 2] = qBlue(c);
        cmapData[i * 4 + 3] = 255;
    }
    f->glGenTextures(1, &glColormapTex);
    f->glBindTexture(GL_TEXTURE_2D, glColormapTex);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, cmapData);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glBindTexture(GL_TEXTURE_2D, 0);

    fprintf(stderr, "SpectrogramPlot: OpenGL colormap shader initialized\n");
    fflush(stderr);
    return true;
}

void SpectrogramPlot::clearGLTileCache(QOpenGLFunctions *f)
{
    for (auto &kv : glTileTextures)
        f->glDeleteTextures(1, &kv.second);
    glTileTextures.clear();
}

GLuint SpectrogramPlot::getOrCreateGLTile(QOpenGLFunctions *f, size_t tile)
{
    auto it = glTileTextures.find(tile);
    if (it != glTileTextures.end())
        return it->second;

    float *fftTile = getFFTTile(tile);

    GLuint tex;
    f->glGenTextures(1, &tex);
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F,
                    fftSize, linesPerTile(), 0,
                    GL_RED, GL_FLOAT, fftTile);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glBindTexture(GL_TEXTURE_2D, 0);

    glTileTextures[tile] = tex;
    return tex;
}

void SpectrogramPlot::paintMidGL(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    auto *ctx = QOpenGLContext::currentContext();
    auto *f = ctx->functions();

    // Lazy GL init
    if (!glInitialized) {
        glInitialized = initGL(f);
        glFailed = !glInitialized;
        if (glFailed) {
            paintMidCPU(painter, rect, sampleRange);
            return;
        }
    }

    // Invalidate tile cache if FFT parameters changed
    if (fftSize != glCacheFftSize || zoomLevel != glCacheZoomLevel ||
        nfftSkip != glCacheNfftSkip || glTileCacheDirty) {
        clearGLTileCache(f);
        glCacheFftSize = fftSize;
        glCacheZoomLevel = zoomLevel;
        glCacheNfftSkip = nfftSkip;
        glTileCacheDirty = false;
    }

    painter.beginNativePainting();

    // Viewport dimensions for NDC transform
    GLint vp[4];
    f->glGetIntegerv(GL_VIEWPORT, vp);
    float vpW = vp[2], vpH = vp[3];

    // Bind shader and set shared uniforms
    glShader->bind();
    glShader->setUniformValue("uViewport", QVector2D(vpW, vpH));
    glShader->setUniformValue("uPowerMax", powerMax);
    glShader->setUniformValue("uPowerRange",
        -1.0f / std::abs(static_cast<int>(powerMin - powerMax)));

    // Colormap on texture unit 1
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, glColormapTex);
    glShader->setUniformValue("uColormap", 1);

    // FFT data will go on texture unit 0
    f->glActiveTexture(GL_TEXTURE0);
    glShader->setUniformValue("uFFTData", 0);

    // Bind quad geometry
    f->glBindBuffer(GL_ARRAY_BUFFER, glQuadVBO);
    int aPosLoc = glShader->attributeLocation("aPos");
    f->glEnableVertexAttribArray(aPosLoc);
    f->glVertexAttribPointer(aPosLoc, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Frequency range: for real signals show only positive half
    float freqTexMax = inputSource->realSignal() ? 0.5f : 0.0f;

    // Tile iteration (same logic as the CPU path)
    size_t sampleOffset = sampleRange.minimum % (getStride() * linesPerTile());
    size_t tileID = sampleRange.minimum - sampleOffset;
    int xoffset = sampleOffset / getStride();

    auto drawTile = [&](int screenX, int screenW, float tTimeMin, float tTimeMax) {
        GLuint tex = getOrCreateGLTile(f, tileID);
        f->glBindTexture(GL_TEXTURE_2D, tex);

        glShader->setUniformValue("uDstPos",  QVector2D(screenX, rect.y()));
        glShader->setUniformValue("uDstSize", QVector2D(screenW, height()));
        // X axis of quad = time  → texture t (height axis)
        // Y axis of quad = freq  → texture s (width axis), high freq at top
        glShader->setUniformValue("uTexMin", QVector2D(tTimeMin, 1.0f));
        glShader->setUniformValue("uTexMax", QVector2D(tTimeMax, freqTexMax));

        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };

    // First (possibly partial) tile
    {
        int w = linesPerTile() - xoffset;
        float tMin = static_cast<float>(xoffset) / linesPerTile();
        drawTile(rect.left(), w, tMin, 1.0f);
        tileID += getStride() * linesPerTile();
    }

    // Remaining full tiles
    for (int x = linesPerTile() - xoffset; x < rect.right(); x += linesPerTile()) {
        drawTile(x, linesPerTile(), 0.0f, 1.0f);
        tileID += getStride() * linesPerTile();
    }

    // Restore GL state
    f->glDisableVertexAttribArray(aPosLoc);
    f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    glShader->release();

    painter.endNativePainting();
}

QPixmap* SpectrogramPlot::getPixmapTile(size_t tile)
{
    QPixmap *obj = pixmapCache.object(TileCacheKey(fftSize, zoomLevel, nfftSkip, tile));
    if (obj != 0)
        return obj;

    float *fftTile = getFFTTile(tile);
    obj = new QPixmap(linesPerTile(), fftSize);
    QImage image(linesPerTile(), fftSize, QImage::Format_RGB32);
    float powerRange = -1.0f / std::abs(int(powerMin - powerMax));
    for (int y = 0; y < fftSize; y++) {
        auto scanLine = (QRgb*)image.scanLine(fftSize - y - 1);
        for (int x = 0; x < linesPerTile(); x++) {
            float *fftLine = &fftTile[x * fftSize];
            float normPower = (fftLine[y] - powerMax) * powerRange;
            normPower = clamp(normPower, 0.0f, 1.0f);

            scanLine[x] = colormap[(uint8_t)(normPower * (256 - 1))];
        }
    }
    obj->convertFromImage(image);
    pixmapCache.insert(TileCacheKey(fftSize, zoomLevel, nfftSkip, tile), obj);
    return obj;
}

float* SpectrogramPlot::getFFTTile(size_t tile)
{
    std::array<float, tileSize>* obj = fftCache.object(TileCacheKey(fftSize, zoomLevel, nfftSkip, tile));
    if (obj != nullptr)
        return obj->data();

    std::array<float, tileSize>* destStorage = new std::array<float, tileSize>;
    float *ptr = destStorage->data();
    size_t sample = tile;
    while ((ptr - destStorage->data()) < tileSize) {
        getLine(ptr, sample);
        sample += getStride();
        ptr += fftSize;
    }
    fftCache.insert(TileCacheKey(fftSize, zoomLevel, nfftSkip, tile), destStorage);
    return destStorage->data();
}

void SpectrogramPlot::getLine(float *dest, size_t sample)
{
    if (inputSource && fft) {
        // Make sample be the midpoint of the FFT, unless this takes us
        // past the beginning of the inputSource (if we remove the
        // std::max(·, 0), then an ugly red bar appears at the beginning
        // of the spectrogram with large zooms and FFT sizes).
        const auto first_sample = std::max(static_cast<ssize_t>(sample) - fftSize / 2,
                        static_cast<ssize_t>(0));
        auto buffer = inputSource->getSamples(first_sample, fftSize);
        if (buffer == nullptr) {
            auto neg_infinity = -1 * std::numeric_limits<float>::infinity();
            for (int i = 0; i < fftSize; i++, dest++)
                *dest = neg_infinity;
            return;
        }

        for (int i = 0; i < fftSize; i++) {
            buffer[i] *= window[i];
        }

        fft->process(reinterpret_cast<fftwf_complex*>(buffer.get()), reinterpret_cast<fftwf_complex*>(buffer.get()));
        const float invFFTSize = 1.0f / fftSize;
        const float logMultiplier = 10.0f / log2f(10.0f);
        for (int i = 0; i < fftSize; i++) {
            // Start from the middle of the FFTW array and wrap
            // to rearrange the data
            int k = i ^ (fftSize >> 1);
            auto s = buffer[k] * invFFTSize;
            float power = s.real() * s.real() + s.imag() * s.imag();
            float logPower = log2f(power) * logMultiplier;
            *dest = logPower;
            dest++;
        }
    }
}

int SpectrogramPlot::getStride()
{
    return fftSize * nfftSkip / zoomLevel;
}

float SpectrogramPlot::getTunerPhaseInc()
{
    auto freq = 0.5f - tuner.centre() / (float)fftSize;
    return freq * Tau;
}

std::vector<float> SpectrogramPlot::getTunerTaps()
{
    float cutoff = tuner.deviation() / (float)fftSize;
    float gain = pow(10.0f, powerMax / -10.0f);
    auto atten = 60.0f;
    auto len = estimate_req_filter_len(std::min(cutoff, 0.05f), atten);
    auto taps = std::vector<float>(len);
    liquid_firdes_kaiser(len, cutoff, atten, 0.0f, taps.data());
    std::transform(taps.begin(), taps.end(), taps.begin(),
                   std::bind(std::multiplies<float>(), std::placeholders::_1, gain));
    return taps;
}

int SpectrogramPlot::linesPerTile()
{
    return tileSize / fftSize;
}

bool SpectrogramPlot::mouseEvent(QEvent::Type type, QMouseEvent *event)
{
    if (tunerEnabled())
        return tuner.mouseEvent(type, event);

    return false;
}

void SpectrogramPlot::leaveEvent()
{
    if (tunerEnabled())
        tuner.leaveEvent();
}

std::shared_ptr<AbstractSampleSource> SpectrogramPlot::output()
{
    return tunerTransform;
}

void SpectrogramPlot::setFFTSize(int size)
{
    float sizeScale = float(size) / float(fftSize);
    fftSize = size;
    fft.reset(new FFT(fftSize));

    window.reset(new float[fftSize]);
    for (int i = 0; i < fftSize; i++) {
        window[i] = 0.5f * (1.0f - cos(Tau * i / (fftSize - 1)));
    }

    if (inputSource->realSignal()) {
        setHeight(fftSize/2);
    } else {
        setHeight(fftSize);
    }
    auto dev = tuner.deviation();
    auto centre = tuner.centre();
    tuner.setHeight(height());
    tuner.setDeviation( dev * sizeScale );
    tuner.setCentre( centre * sizeScale );
}

void SpectrogramPlot::setPowerMax(int power)
{
    powerMax = power;
    pixmapCache.clear();
    tunerMoved();
}

void SpectrogramPlot::setPowerMin(int power)
{
    powerMin = power;
    pixmapCache.clear();
}

void SpectrogramPlot::setZoomLevel(int zoom)
{
    zoomLevel = zoom;
}

void SpectrogramPlot::setSkip(int skip)
{
    nfftSkip = skip;
}

void SpectrogramPlot::setSampleRate(double rate)
{
    sampleRate = rate;
}

void SpectrogramPlot::enableScales(bool enabled)
{
   frequencyScaleEnabled = enabled;
}

void SpectrogramPlot::enableAnnotations(bool enabled)
{
   sigmfAnnotationsEnabled = enabled;
}

bool SpectrogramPlot::isAnnotationsEnabled(void)
{
    return sigmfAnnotationsEnabled;
}

void SpectrogramPlot::enableAnnoLabels(bool enabled)
{
    sigmfAnnotationLabels = enabled;
}

void SpectrogramPlot::enableAnnoColors(bool enabled)
{
    sigmfAnnotationColors = enabled;
}

bool SpectrogramPlot::tunerEnabled()
{
    return (tunerTransform->subscriberCount() > 0);
}

void SpectrogramPlot::tunerMoved()
{
    tunerTransform->setFrequency(getTunerPhaseInc());
    tunerTransform->setTaps(getTunerTaps());
    tunerTransform->setRelativeBandwith(tuner.deviation() * 2.0 / height());

    // TODO: for invalidating traceplot cache, this shouldn't really go here
    QPixmapCache::clear();

    emit repaint();
}

uint qHash(const TileCacheKey &key, uint seed)
{
    return key.fftSize ^ key.zoomLevel ^ key.sample ^ seed;
}
