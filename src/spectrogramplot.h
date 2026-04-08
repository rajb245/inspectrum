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

#pragma once

#include <QCache>
#include <QMutex>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QString>
#include <QWidget>
#include <QtConcurrent>
#include "fft.h"
#include "inputsource.h"
#include "plot.h"
#include "tuner.h"
#include "tunertransform.h"

#include <memory>
#include <array>
#include <map>
#include <math.h>
#include <set>
#include <vector>

class AnnotationLocation;

// RAII wrapper for a batched FFTW plan (N FFTs in a single call).
// The plan is read-only after construction, so multiple threads can
// execute it concurrently via fftwf_execute_dft with private buffers.
struct BatchFFTPlan {
    fftwf_plan plan = nullptr;
    fftwf_complex *bufIn = nullptr;   // also used as sync-path scratch
    fftwf_complex *bufOut = nullptr;
    int fftSize;
    int batchCount;

    BatchFFTPlan(int fftSz, int batch)
        : fftSize(fftSz), batchCount(batch)
    {
        int total = fftSz * batch;
        bufIn  = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * total);
        bufOut = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * total);
        int n[] = {fftSz};
        plan = fftwf_plan_many_dft(1, n, batch,
                   bufIn,  NULL, 1, fftSz,
                   bufOut, NULL, 1, fftSz,
                   FFTW_FORWARD, FFTW_MEASURE);
    }
    ~BatchFFTPlan() {
        if (plan)   fftwf_destroy_plan(plan);
        if (bufIn)  fftwf_free(bufIn);
        if (bufOut) fftwf_free(bufOut);
    }
    BatchFFTPlan(const BatchFFTPlan&) = delete;
    BatchFFTPlan& operator=(const BatchFFTPlan&) = delete;
};


class TileCacheKey
{

public:
    TileCacheKey(int fftSize, int zoomLevel, int nfftSkip, size_t sample) {
        this->fftSize = fftSize;
        this->zoomLevel = zoomLevel;
        this->nfftSkip = nfftSkip;
        this->sample = sample;
    }

    bool operator==(const TileCacheKey &k2) const {
        return (this->fftSize == k2.fftSize) &&
               (this->zoomLevel == k2.zoomLevel) &&
               (this->nfftSkip == k2.nfftSkip) &&
               (this->sample == k2.sample);
    }

    int fftSize;
    int zoomLevel;
    int nfftSkip;
    size_t sample;
};

class SpectrogramPlot : public Plot
{
    Q_OBJECT

public:
    SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>> src);
    void invalidateEvent() override;
    std::shared_ptr<AbstractSampleSource> output() override;
    void paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    bool mouseEvent(QEvent::Type type, QMouseEvent *event) override;
    void leaveEvent();
    std::shared_ptr<SampleSource<std::complex<float>>> input() { return inputSource; };
    void setSampleRate(double sampleRate);
    bool tunerEnabled();
    void enableScales(bool enabled);
    void enableAnnotations(bool enabled);
    void enableAnnoLabels(bool enabled);
    bool isAnnotationsEnabled();
    void enableAnnoColors(bool enabled);
    QString *mouseAnnotationComment(const QMouseEvent *event);

public slots:
    void setFFTSize(int size);
    void setPowerMax(int power);
    void setPowerMin(int power);
    void setZoomLevel(int zoom);
    void setSkip(int skip);
    void tunerMoved();

private:
    const int linesPerGraduation = 50;
    static const int tileSize = 65536; // This must be a multiple of the maximum FFT size

    std::shared_ptr<SampleSource<std::complex<float>>> inputSource;
    std::vector<AnnotationLocation> visibleAnnotationLocations;
    std::shared_ptr<FFT> fft;
    std::shared_ptr<BatchFFTPlan> batchFFT;
    bool useBatchFFT = false;  // set by auto-tune in setFFTSize
    std::shared_ptr<std::vector<float>> window;
    QCache<TileCacheKey, QPixmap> pixmapCache;
    QCache<TileCacheKey, std::array<float, tileSize>> fftCache;
    uint colormap[256];

    int fftSize;
    int zoomLevel;
    int nfftSkip;
    float powerMax;
    float powerMin;
    double sampleRate;
    bool frequencyScaleEnabled;
    bool sigmfAnnotationsEnabled;
    bool sigmfAnnotationLabels;
    bool sigmfAnnotationColors;

    Tuner tuner;
    std::shared_ptr<TunerTransform> tunerTransform;

    QPixmap* getPixmapTile(size_t tile);
    float* getFFTTile(size_t tile);
    void getLine(float *dest, size_t sample);
    int getStride();
    float getTunerPhaseInc();
    std::vector<float> getTunerTaps();
    int linesPerTile();
    void paintFrequencyScale(QPainter &painter, QRect &rect);
    void paintAnnotations(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);

    // OpenGL accelerated spectrogram rendering
    bool glInitialized = false;
    bool glFailed = false;
    QOpenGLShaderProgram *glShader = nullptr;
    GLuint glColormapTex = 0;
    GLuint glQuadVBO = 0;
    std::map<size_t, GLuint> glTileTextures;
    int glCacheFftSize = 0;
    int glCacheZoomLevel = 0;
    int glCacheNfftSkip = 0;
    bool glTileCacheDirty = false;

    bool initGL(QOpenGLFunctions *f);
    void paintMidGL(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);
    void paintMidCPU(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);
    GLuint uploadFFTToGL(QOpenGLFunctions *f, const float *data);
    void clearGLTileCache(QOpenGLFunctions *f);

    // Async tile computation (off the UI thread)
    QMutex asyncMutex;
    std::map<size_t, std::array<float, tileSize>*> asyncCompleted;
    std::set<size_t> asyncPending;
    int asyncGeneration = 0;

    void consumeAsyncTiles();
    void launchAsyncTile(size_t tile);
};

class AnnotationLocation
{
public:
    Annotation annotation;

    AnnotationLocation(Annotation annotation, int x, int y, int width, int height)
        : annotation(annotation), x(x), y(y), width(width), height(height) {}

    bool isInside(int pos_x, int pos_y) {
        return (x <= pos_x) && (pos_x <= x + width)
            && (y <= pos_y) && (pos_y <= y + height);
    }

private:
    int x;
    int y;
    int width;
    int height;
};
