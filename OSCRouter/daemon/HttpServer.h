// Copyright (c) 2018 Electronic Theatre Controls, Inc., http://www.etcconnect.com
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtNetwork/QTcpServer>

class QTcpSocket;
class RouterController;

////////////////////////////////////////////////////////////////////////////////

// A small HTTP/1.1 server for the web interface.
//
// Deliberately built directly on QTcpServer rather than Qt HttpServer: it keeps
// the daemon's dependencies to Qt Core, Network and Qml, which is what makes the
// container image small, and what it has to serve is modest -- a handful of JSON
// endpoints, a Server-Sent Events stream, and a few static files compiled into
// the binary as Qt resources.
//
// Live updates use Server-Sent Events rather than WebSockets. The traffic is
// one-directional (engine to browser), SSE needs no extra Qt module and no
// framing code, and browsers reconnect on their own.
class HttpServer : public QTcpServer
{
  Q_OBJECT

public:
  HttpServer(RouterController& controller, QObject* parent = nullptr);

  // When set, connections from any other peer are refused. Home Assistant's
  // ingress proxy is the only permitted client in an add-on install.
  void SetAllowedPeer(const QString& address) { m_AllowedPeer = address; }

protected:
  void incomingConnection(qintptr socketDescriptor) override;

private slots:
  void onReadyRead();
  void onDisconnected();
  void onLogMessage(const QJsonObject& message);
  void onItemStatesChanged();
  void onRunStateChanged();

private:
  struct Request
  {
    QByteArray method;
    QString path;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
  };

  // Per-connection parse state, since a request can arrive in several reads.
  struct Connection
  {
    QByteArray buffer;
    bool headersComplete = false;
    qint64 contentLength = 0;
    int headerLength = 0;
  };

  bool ParseRequest(const QByteArray& raw, int headerLength, Request& request) const;
  void HandleRequest(QTcpSocket* socket, const Request& request);
  bool HandleApiRequest(QTcpSocket* socket, const Request& request);
  void HandleStaticRequest(QTcpSocket* socket, const Request& request);

  void SendResponse(QTcpSocket* socket, int status, const QByteArray& contentType, const QByteArray& body);
  void SendJson(QTcpSocket* socket, int status, const QJsonObject& object);
  void SendJson(QTcpSocket* socket, int status, const QJsonArray& array);
  void SendError(QTcpSocket* socket, int status, const QString& message);

  void BeginEventStream(QTcpSocket* socket);
  void SendEvent(QTcpSocket* socket, const QByteArray& name, const QByteArray& data);
  void BroadcastEvent(const QByteArray& name, const QJsonValue& data);

  RouterController& m_Controller;
  QString m_AllowedPeer;
  QHash<QTcpSocket*, Connection> m_Connections;
  QSet<QTcpSocket*> m_EventClients;
};

////////////////////////////////////////////////////////////////////////////////

#endif
