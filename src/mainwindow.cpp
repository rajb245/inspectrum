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

#include <QMessageBox>
#include <QtWidgets>
#include <QPixmapCache>
#include <QRubberBand>
#include <sstream>

#include "mainwindow.h"
#include "remotecontrol.h"
#include "util.h"

MainWindow::MainWindow()
{
    setWindowTitle(tr("inspectrum - jacobagilbert edition"));

    QPixmapCache::setCacheLimit(40960);

    dock = new SpectrogramControls(tr("Controls"), this);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, dock);

    input = new InputSource();
    input->subscribe(this);

    plots = new PlotView(input);
    setCentralWidget(plots);

    // Watch the open file(s) so on-disk edits (e.g. regenerated annotations)
    // can trigger an automatic reload.  A short debounce coalesces the burst
    // of events an editor emits and lets an atomic save (write-temp + rename)
    // settle before we re-read.
    fileWatcher = new QFileSystemWatcher(this);
    reloadDebounce = new QTimer(this);
    reloadDebounce->setSingleShot(true);
    reloadDebounce->setInterval(300);
    connect(fileWatcher, &QFileSystemWatcher::fileChanged, this, &MainWindow::onWatchedFileChanged);
    connect(reloadDebounce, &QTimer::timeout, this, &MainWindow::reloadFile);

    // Connect dock inputs
    connect(dock, &SpectrogramControls::openFile, this, &MainWindow::openFile);
    connect(dock, &SpectrogramControls::reloadFile, this, &MainWindow::reloadFile);
    connect(dock->autoReloadCheckBox, &QCheckBox::toggled, this, &MainWindow::setAutoReload);
    connect(dock->sampleRate, static_cast<void (QLineEdit::*)(const QString&)>(&QLineEdit::textChanged), this, static_cast<void (MainWindow::*)(QString)>(&MainWindow::setSampleRate));
    connect(dock, static_cast<void (SpectrogramControls::*)(int, int)>(&SpectrogramControls::fftOrZoomChanged), plots, &PlotView::setFFTAndZoom);
    connect(dock->powerMaxSlider, &QSlider::valueChanged, plots, &PlotView::setPowerMax);
    connect(dock->powerMinSlider, &QSlider::valueChanged, plots, &PlotView::setPowerMin);
    connect(dock->cursorsCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableCursors);
    connect(dock->scalesCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableScales);
    connect(dock->annosCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableAnnotations);
    connect(dock->annosCheckBox, &QCheckBox::stateChanged, dock, &SpectrogramControls::enableAnnotations);
    connect(dock->annoLabelCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableAnnoLabels);
    connect(dock->commentsCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableAnnotationCommentsTooltips);
    connect(dock->annoColorCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableAnnoColors);
    connect(dock->cursorSymbolsSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), plots, &PlotView::setCursorSegments);

    // Connect dock outputs
    connect(plots, &PlotView::timeSelectionChanged, dock, &SpectrogramControls::timeSelectionChanged);
    connect(plots, &PlotView::zoomIn, dock, &SpectrogramControls::zoomIn);
    connect(plots, &PlotView::zoomOut, dock, &SpectrogramControls::zoomOut);
    connect(plots, &PlotView::fftSizeUp, dock, &SpectrogramControls::fftSizeUp);
    connect(plots, &PlotView::fftSizeDown, dock, &SpectrogramControls::fftSizeDown);
    connect(plots, &PlotView::annotationSelected, dock, &SpectrogramControls::showAnnotation);

    // Set defaults after making connections so everything is in sync
    dock->setDefaults();

    // Start the JSON-RPC control server (local socket).
    remote = new RemoteControl(this, plots, this);
    QString remotePath = remote->start();
    if (!remotePath.isEmpty())
        qInfo().noquote() << "inspectrum: JSON-RPC control listening on" << remotePath;
    else
        qWarning() << "inspectrum: failed to start JSON-RPC control server";
}

void MainWindow::openFile(QString fileName)
{
    QString title="%1 jacobagilbert edition: %2";
    this->setWindowTitle(title.arg(QApplication::applicationName(),fileName.section('/',-1,-1)));

    // Reset the annotation inspector; the previous file's selection is stale.
    dock->clearAnnotation();

    // Try to parse osmocom_fft filenames and extract the sample rate and center frequency.
    // Example file name: "name-f2.411200e+09-s5.000000e+06-t20160807180210.cfile"
    QRegularExpression rx(QRegularExpression::anchoredPattern("(.*)-f(.*)-s(.*)-.*\\.cfile"));
    QString basename = fileName.section('/',-1,-1);

    auto match = rx.match(basename);
    if (match.hasMatch()) {
        QString centerfreq = match.captured(2);
        QString samplerate = match.captured(3);

        std::stringstream ss(samplerate.toUtf8().constData());

        // Needs to be a double as the number is in scientific format
        double rate;
        ss >> rate;
        if (!ss.fail()) {
            setSampleRate(rate);
        }
    }

    try
    {
        input->openFile(fileName.toUtf8().constData());
        currentFileName = fileName;
        updateFileWatcher();
    }
    catch (const std::exception &ex)
    {
        QMessageBox msgBox(QMessageBox::Critical, "Inspectrum openFile error", QString("%1: %2").arg(fileName).arg(ex.what()));
        msgBox.exec();
    }
}

void MainWindow::reloadFile()
{
    if (!currentFileName.isEmpty())
        openFile(currentFileName);
}

void MainWindow::setAutoReload(bool enabled)
{
    autoReloadEnabled = enabled;
}

void MainWindow::onWatchedFileChanged(const QString & /*path*/)
{
    if (autoReloadEnabled) {
        // Debounce: an editor may emit several change events, and an atomic
        // save briefly removes the file before the replacement appears.
        reloadDebounce->start();
    } else {
        // Still re-arm the watcher so a later change is detected even though
        // we're not reloading now (an atomic save drops the watched path).
        updateFileWatcher();
    }
}

void MainWindow::updateFileWatcher()
{
    // Reset the watch list to the file(s) backing the current recording.
    if (!fileWatcher->files().isEmpty())
        fileWatcher->removePaths(fileWatcher->files());

    if (currentFileName.isEmpty())
        return;

    // For SigMF, watch both the metadata and data siblings; the annotations
    // live in the .sigmf-meta, but either may be regenerated.  For other
    // formats just watch the opened file.
    QFileInfo info(currentFileName);
    QStringList candidates;
    const QString suffix = info.suffix();
    if (suffix == "sigmf-meta" || suffix == "sigmf-data" || suffix == "sigmf-" || suffix == "sigmf") {
        const QString base = info.path() + "/" + info.completeBaseName();
        candidates << base + ".sigmf-meta" << base + ".sigmf-data";
    } else {
        candidates << currentFileName;
    }

    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            fileWatcher->addPath(path);
    }
}

void MainWindow::invalidateEvent()
{
    plots->setSampleRate(input->rate());

    // Only update the text box if it is not already representing
    // the current value. Otherwise the cursor might jump or the
    // representation might change (e.g. to scientific).
    double currentValue = dock->sampleRate->text().toDouble();
    if(QString::number(input->rate()) != QString::number(currentValue)) {
        setSampleRate(input->rate());
    }
}

void MainWindow::setSampleRate(QString rate)
{
    auto sampleRate = rate.toDouble();
    input->setSampleRate(sampleRate);
    plots->setSampleRate(sampleRate);

    // Save the sample rate in settings as we're likely to be opening the same file across multiple runs
    QSettings settings;
    settings.setValue("SampleRate", sampleRate);
}

void MainWindow::setSampleRate(double rate)
{
    dock->sampleRate->setText(QString::number(rate));
}

void MainWindow::setFormat(QString fmt)
{
    input->setFormat(fmt.toUtf8().constData());
}
