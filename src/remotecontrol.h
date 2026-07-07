/*
 *  Copyright (C) 2026
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

#include <QObject>
#include <QHash>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>

class QLocalServer;
class QLocalSocket;
class MainWindow;
class PlotView;

// A JSON-RPC 2.0 control server exposed over a local (Unix domain) socket.
// It speaks the standard JSON-RPC 2.0 envelope (newline-delimited compact
// JSON, one request/response per line) and implements the OpenRPC
// `rpc.discover` method so callers can introspect the available API.
//
// Runs entirely on the GUI thread: QLocalServer/QLocalSocket are event-driven,
// so handlers touch PlotView/MainWindow directly with no cross-thread work.
class RemoteControl : public QObject
{
    Q_OBJECT

public:
    RemoteControl(MainWindow *mainWindow, PlotView *plotView, QObject *parent = nullptr);

    // Begin listening. Returns the concrete socket path (for clients to
    // connect to), or an empty string if listening failed.
    QString start();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    // Returns the full JSON-RPC response object, or an empty object for a
    // notification (a request with no "id"), which must not be answered.
    QJsonObject dispatch(const QJsonObject &request);

    QJsonValue handleOpen(const QJsonValue &params, QJsonObject &errorOut);
    QJsonValue handleSeek(const QJsonValue &params, QJsonObject &errorOut);
    QJsonValue handleGetState();
    QJsonObject buildDiscoverDoc();
    QJsonObject stateObject();

    static QJsonObject makeError(int code, const QString &message);

    QLocalServer *server = nullptr;
    MainWindow *mainWindow;
    PlotView *plotView;
    QString socketPath;
    QHash<QLocalSocket *, QByteArray> buffers;  // per-connection read buffer
};
