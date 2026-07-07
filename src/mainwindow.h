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

#include <QMainWindow>
#include <QScrollArea>
#include <QFileSystemWatcher>
#include <QTimer>
#include "spectrogramcontrols.h"
#include "plotview.h"

class RemoteControl;

class MainWindow : public QMainWindow, Subscriber
{
    Q_OBJECT

public:
    MainWindow();
    void changeSampleRate(double rate);
    QString currentFile() const { return currentFileName; }

public slots:
    void openFile(QString fileName);
    void reloadFile();
    void setSampleRate(QString rate);
    void setSampleRate(double rate);
    void setFormat(QString fmt);
    void invalidateEvent() override;

private slots:
    void onWatchedFileChanged(const QString &path);
    void setAutoReload(bool enabled);

protected:
    // Accept files dragged from Finder (or any file manager) onto the window.
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    // Persist the window geometry on close.
    void closeEvent(QCloseEvent *event) override;

private:
    void updateFileWatcher();
    // Window-geometry persistence in an INI file next to the executable.
    static QString settingsFilePath();
    void restoreWindowGeometry();

    SpectrogramControls *dock;
    PlotView *plots;
    InputSource *input;
    RemoteControl *remote;
    QString currentFileName;
    QFileSystemWatcher *fileWatcher;
    QTimer *reloadDebounce;
    bool autoReloadEnabled = true;
};
