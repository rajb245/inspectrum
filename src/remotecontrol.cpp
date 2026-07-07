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

#include "remotecontrol.h"
#include "mainwindow.h"
#include "plotview.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include <QBuffer>
#include <QImage>

// JSON-RPC 2.0 standard error codes (https://www.jsonrpc.org/specification)
static const int kParseError     = -32700;
static const int kInvalidRequest = -32600;
static const int kMethodNotFound = -32601;
static const int kInvalidParams  = -32602;
static const int kInternalError  = -32603;

RemoteControl::RemoteControl(MainWindow *mainWindow, PlotView *plotView, QObject *parent)
    : QObject(parent), mainWindow(mainWindow), plotView(plotView)
{
}

QString RemoteControl::start()
{
    // Listen on a predictable temp-dir socket path so non-Qt clients can
    // connect via AF_UNIX without needing Qt's name-mangling rules.
    socketPath = QDir::tempPath() + "/inspectrum.sock";
    QLocalServer::removeServer(socketPath);  // clear a stale socket from a prior crash

    server = new QLocalServer(this);
    connect(server, &QLocalServer::newConnection, this, &RemoteControl::onNewConnection);
    if (!server->listen(socketPath)) {
        delete server;
        server = nullptr;
        socketPath.clear();
        return QString();
    }
    return server->fullServerName();
}

void RemoteControl::onNewConnection()
{
    while (QLocalSocket *sock = server->nextPendingConnection()) {
        buffers.insert(sock, QByteArray());
        connect(sock, &QLocalSocket::readyRead, this, &RemoteControl::onReadyRead);
        connect(sock, &QLocalSocket::disconnected, this, &RemoteControl::onDisconnected);
    }
}

void RemoteControl::onDisconnected()
{
    auto *sock = qobject_cast<QLocalSocket *>(sender());
    if (!sock)
        return;
    buffers.remove(sock);
    sock->deleteLater();
}

void RemoteControl::onReadyRead()
{
    auto *sock = qobject_cast<QLocalSocket *>(sender());
    if (!sock)
        return;

    QByteArray &buf = buffers[sock];
    buf.append(sock->readAll());

    // Requests are newline-delimited compact JSON, one per line.
    int nl;
    while ((nl = buf.indexOf('\n')) != -1) {
        QByteArray line = buf.left(nl);
        buf.remove(0, nl + 1);
        if (line.trimmed().isEmpty())
            continue;

        QJsonObject response;
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(line, &parseErr);
        if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
            response = QJsonObject{
                {"jsonrpc", "2.0"},
                {"error", makeError(kParseError, "Parse error")},
                {"id", QJsonValue()},  // null: we couldn't determine the id
            };
        } else {
            response = dispatch(doc.object());
        }

        if (!response.isEmpty()) {  // empty == notification, no reply
            sock->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
            sock->write("\n");
        }
    }
}

QJsonObject RemoteControl::dispatch(const QJsonObject &request)
{
    const bool isNotification = !request.contains("id");
    const QJsonValue id = request.value("id");
    const QString method = request.value("method").toString();
    const QJsonValue params = request.value("params");

    auto reply = [&](const QJsonValue &result) -> QJsonObject {
        if (isNotification)
            return QJsonObject();
        return QJsonObject{{"jsonrpc", "2.0"}, {"result", result}, {"id", id}};
    };
    auto fail = [&](const QJsonObject &error) -> QJsonObject {
        if (isNotification)
            return QJsonObject();
        return QJsonObject{{"jsonrpc", "2.0"}, {"error", error}, {"id", id}};
    };

    if (request.value("jsonrpc").toString() != "2.0" || method.isEmpty())
        return fail(makeError(kInvalidRequest, "Invalid Request"));

    QJsonObject error;
    if (method == "open") {
        QJsonValue result = handleOpen(params, error);
        return error.isEmpty() ? reply(result) : fail(error);
    } else if (method == "seek") {
        QJsonValue result = handleSeek(params, error);
        return error.isEmpty() ? reply(result) : fail(error);
    } else if (method == "snapshot") {
        QJsonValue result = handleSnapshot(params, error);
        return error.isEmpty() ? reply(result) : fail(error);
    } else if (method == "getState") {
        return reply(handleGetState());
    } else if (method == "rpc.discover") {
        return reply(buildDiscoverDoc());
    }

    return fail(makeError(kMethodNotFound, QString("Method not found: %1").arg(method)));
}

QJsonValue RemoteControl::handleOpen(const QJsonValue &params, QJsonObject &errorOut)
{
    if (!params.isObject()) {
        errorOut = makeError(kInvalidParams, "params must be an object with a 'path' string");
        return QJsonValue();
    }
    const QString path = params.toObject().value("path").toString();
    if (path.isEmpty()) {
        errorOut = makeError(kInvalidParams, "missing required string param 'path'");
        return QJsonValue();
    }
    if (!QFileInfo::exists(path)) {
        errorOut = makeError(kInvalidParams, QString("file does not exist: %1").arg(path));
        return QJsonValue();
    }
    mainWindow->openFile(path);
    return stateObject();
}

QJsonValue RemoteControl::handleSeek(const QJsonValue &params, QJsonObject &errorOut)
{
    if (!params.isObject()) {
        errorOut = makeError(kInvalidParams, "params must be an object with 'sample' or 'seconds'");
        return QJsonValue();
    }
    const QJsonObject obj = params.toObject();

    size_t sample;
    if (obj.contains("sample")) {
        double s = obj.value("sample").toDouble(-1);
        if (s < 0) {
            errorOut = makeError(kInvalidParams, "'sample' must be a non-negative number");
            return QJsonValue();
        }
        sample = static_cast<size_t>(s);
    } else if (obj.contains("seconds")) {
        double secs = obj.value("seconds").toDouble(-1);
        double rate = plotView->currentSampleRate();
        if (secs < 0 || rate <= 0) {
            errorOut = makeError(kInvalidParams, "'seconds' requires a valid sample rate and non-negative value");
            return QJsonValue();
        }
        sample = static_cast<size_t>(secs * rate);
    } else {
        errorOut = makeError(kInvalidParams, "provide either 'sample' or 'seconds'");
        return QJsonValue();
    }

    plotView->seekToSample(sample);
    return stateObject();
}

QJsonValue RemoteControl::handleSnapshot(const QJsonValue &params, QJsonObject &errorOut)
{
    QImage img = plotView->grabCanvas();
    if (img.isNull()) {
        errorOut = makeError(kInternalError, "failed to capture canvas");
        return QJsonValue();
    }

    const QJsonObject obj = params.isObject() ? params.toObject() : QJsonObject();

    // If a path is given, write the PNG to disk and return the path; otherwise
    // return the PNG bytes inline as base64.
    if (obj.contains("path")) {
        const QString path = obj.value("path").toString();
        if (path.isEmpty()) {
            errorOut = makeError(kInvalidParams, "'path' must be a non-empty string");
            return QJsonValue();
        }
        if (!img.save(path, "PNG")) {
            errorOut = makeError(kInternalError, QString("failed to write PNG to %1").arg(path));
            return QJsonValue();
        }
        return QJsonObject{{"path", path}, {"width", img.width()}, {"height", img.height()}};
    }

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!img.save(&buffer, "PNG")) {
        errorOut = makeError(kInternalError, "failed to encode PNG");
        return QJsonValue();
    }
    return QJsonObject{
        {"png_base64", QString::fromLatin1(bytes.toBase64())},
        {"width", img.width()},
        {"height", img.height()},
    };
}

QJsonValue RemoteControl::handleGetState()
{
    return stateObject();
}

QJsonObject RemoteControl::stateObject()
{
    return QJsonObject{
        {"file", mainWindow->currentFile()},
        {"sampleOffset", static_cast<double>(plotView->viewStartSample())},
        {"sampleCount", static_cast<double>(plotView->totalSamples())},
        {"sampleRate", plotView->currentSampleRate()},
        {"fftSize", plotView->currentFFTSize()},
        {"zoomLevel", plotView->currentZoomLevel()},
        {"powerMin", plotView->currentPowerMin()},
        {"powerMax", plotView->currentPowerMax()},
    };
}

QJsonObject RemoteControl::makeError(int code, const QString &message)
{
    return QJsonObject{{"code", code}, {"message", message}};
}

QJsonObject RemoteControl::buildDiscoverDoc()
{
    // A minimal OpenRPC 1.2.6 document describing the exposed methods, so a
    // caller can introspect names, params and result shapes.
    auto param = [](const QString &name, const QString &type, bool required) {
        return QJsonObject{
            {"name", name},
            {"required", required},
            {"schema", QJsonObject{{"type", type}}},
        };
    };

    QJsonObject stateSchema{
        {"type", "object"},
        {"properties", QJsonObject{
            {"file", QJsonObject{{"type", "string"}}},
            {"sampleOffset", QJsonObject{{"type", "integer"}}},
            {"sampleCount", QJsonObject{{"type", "integer"}}},
            {"sampleRate", QJsonObject{{"type", "number"}}},
            {"fftSize", QJsonObject{{"type", "integer"}}},
            {"zoomLevel", QJsonObject{{"type", "integer"}}},
            {"powerMin", QJsonObject{{"type", "integer"}}},
            {"powerMax", QJsonObject{{"type", "integer"}}},
        }},
    };
    QJsonObject stateResult{{"name", "state"}, {"schema", stateSchema}};

    QJsonArray methods;
    methods.append(QJsonObject{
        {"name", "open"},
        {"summary", "Open a signal/SigMF file and return the resulting state."},
        {"params", QJsonArray{param("path", "string", true)}},
        {"result", stateResult},
    });
    methods.append(QJsonObject{
        {"name", "seek"},
        {"summary", "Scroll the view so the given offset is at the left edge. "
                    "Provide 'sample' (index) or 'seconds'."},
        {"params", QJsonArray{param("sample", "integer", false), param("seconds", "number", false)}},
        {"result", stateResult},
    });
    methods.append(QJsonObject{
        {"name", "snapshot"},
        {"summary", "Capture the current canvas (spectrogram + frequency/time "
                    "axes + annotation boxes) as a PNG. With 'path', writes the "
                    "file and returns {path,width,height}; otherwise returns the "
                    "PNG inline as {png_base64,width,height}."},
        {"params", QJsonArray{param("path", "string", false)}},
        {"result", QJsonObject{{"name", "image"}, {"schema", QJsonObject{{"type", "object"}}}}},
    });
    methods.append(QJsonObject{
        {"name", "getState"},
        {"summary", "Return the current view/spectrogram state."},
        {"params", QJsonArray{}},
        {"result", stateResult},
    });
    methods.append(QJsonObject{
        {"name", "rpc.discover"},
        {"summary", "Return this OpenRPC service description."},
        {"params", QJsonArray{}},
        {"result", QJsonObject{{"name", "openrpc"}, {"schema", QJsonObject{{"type", "object"}}}}},
    });

    return QJsonObject{
        {"openrpc", "1.2.6"},
        {"info", QJsonObject{
            {"title", "inspectrum remote control"},
            {"version", "1.0.0"},
        }},
        {"methods", methods},
    };
}
