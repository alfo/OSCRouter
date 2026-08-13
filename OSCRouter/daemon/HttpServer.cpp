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

#include "HttpServer.h"

#include "RouterController.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

// A request larger than this is refused outright. The largest thing the web
// interface sends is the configuration, which is far below this.
#define MAX_REQUEST_BYTES (4 * 1024 * 1024)

////////////////////////////////////////////////////////////////////////////////

namespace
{
const char* StatusText(int status)
{
  switch (status)
  {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
  }
  return "OK";
}

QByteArray ContentTypeForPath(const QString& path)
{
  if (path.endsWith(QLatin1String(".html"))) return "text/html; charset=utf-8";
  if (path.endsWith(QLatin1String(".js"))) return "application/javascript; charset=utf-8";
  if (path.endsWith(QLatin1String(".css"))) return "text/css; charset=utf-8";
  if (path.endsWith(QLatin1String(".svg"))) return "image/svg+xml";
  if (path.endsWith(QLatin1String(".json"))) return "application/json; charset=utf-8";
  return "application/octet-stream";
}
}  // namespace

////////////////////////////////////////////////////////////////////////////////

HttpServer::HttpServer(RouterController& controller, QObject* parent /*= nullptr*/)
  : QTcpServer(parent)
  , m_Controller(controller)
{
  connect(&m_Controller, &RouterController::logMessage, this, &HttpServer::onLogMessage);
  connect(&m_Controller, &RouterController::itemStatesChanged, this, &HttpServer::onItemStatesChanged);
  connect(&m_Controller, &RouterController::runStateChanged, this, &HttpServer::onRunStateChanged);
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::incomingConnection(qintptr socketDescriptor)
{
  QTcpSocket* socket = new QTcpSocket(this);
  if (!socket->setSocketDescriptor(socketDescriptor))
  {
    delete socket;
    return;
  }

  if (!m_AllowedPeer.isEmpty() && socket->peerAddress() != QHostAddress(m_AllowedPeer))
  {
    socket->disconnectFromHost();
    socket->deleteLater();
    return;
  }

  m_Connections.insert(socket, Connection());

  connect(socket, &QTcpSocket::readyRead, this, &HttpServer::onReadyRead);
  connect(socket, &QTcpSocket::disconnected, this, &HttpServer::onDisconnected);
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::onDisconnected()
{
  QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
  if (!socket)
    return;

  m_EventClients.remove(socket);
  m_Connections.remove(socket);
  socket->deleteLater();
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::onReadyRead()
{
  QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
  if (!socket)
    return;

  QHash<QTcpSocket*, Connection>::iterator i = m_Connections.find(socket);
  if (i == m_Connections.end())
    return;

  Connection& connection = i.value();
  connection.buffer.append(socket->readAll());

  if (connection.buffer.size() > MAX_REQUEST_BYTES)
  {
    SendError(socket, 413, QStringLiteral("request too large"));
    return;
  }

  if (!connection.headersComplete)
  {
    const int end = connection.buffer.indexOf("\r\n\r\n");
    if (end < 0)
      return;  // wait for the rest of the headers

    connection.headerLength = end + 4;
    connection.headersComplete = true;

    // Content-Length decides whether a body still has to arrive.
    const QByteArray head = connection.buffer.left(end).toLower();
    const int lengthPos = head.indexOf("content-length:");
    if (lengthPos >= 0)
    {
      const int lineEnd = head.indexOf("\r\n", lengthPos);
      const QByteArray value = head.mid(lengthPos + 15, (lineEnd < 0 ? -1 : lineEnd - lengthPos - 15)).trimmed();
      connection.contentLength = value.toLongLong();
    }
  }

  if (connection.buffer.size() < (connection.headerLength + connection.contentLength))
    return;  // wait for the rest of the body

  Request request;
  if (!ParseRequest(connection.buffer, connection.headerLength, request))
  {
    SendError(socket, 400, QStringLiteral("malformed request"));
    return;
  }

  request.body = connection.buffer.mid(connection.headerLength, static_cast<int>(connection.contentLength));

  // One request per connection keeps this simple; responses close the socket.
  connection.buffer.clear();
  connection.headersComplete = false;
  connection.contentLength = 0;
  connection.headerLength = 0;

  HandleRequest(socket, request);
}

////////////////////////////////////////////////////////////////////////////////

bool HttpServer::ParseRequest(const QByteArray& raw, int headerLength, Request& request) const
{
  const QList<QByteArray> lines = raw.left(headerLength - 4).split('\n');
  if (lines.isEmpty())
    return false;

  const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
  if (requestLine.size() < 2)
    return false;

  request.method = requestLine[0].trimmed();

  // Strip any query string; none of the endpoints use one.
  QString target = QString::fromUtf8(requestLine[1].trimmed());
  const int query = target.indexOf(QLatin1Char('?'));
  if (query >= 0)
    target = target.left(query);
  request.path = QUrl::fromPercentEncoding(target.toUtf8());

  for (int i = 1; i < lines.size(); i++)
  {
    const QByteArray line = lines[i].trimmed();
    const int colon = line.indexOf(':');
    if (colon > 0)
      request.headers.insert(line.left(colon).toLower(), line.mid(colon + 1).trimmed());
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::HandleRequest(QTcpSocket* socket, const Request& request)
{
  // Under Home Assistant ingress the application is served from a prefix such
  // as /api/hassio_ingress/<token>/. The browser only ever uses relative URLs,
  // so the prefix is stripped here and the rest of the routing is unaware of it.
  Request adjusted = request;
  const QByteArray ingressPath = request.headers.value("x-ingress-path");
  if (!ingressPath.isEmpty())
  {
    const QString prefix = QString::fromUtf8(ingressPath);
    if (adjusted.path.startsWith(prefix))
    {
      adjusted.path = adjusted.path.mid(prefix.length());
      if (!adjusted.path.startsWith(QLatin1Char('/')))
        adjusted.path.prepend(QLatin1Char('/'));
    }
  }

  if (adjusted.path.startsWith(QLatin1String("/api/")))
  {
    if (!HandleApiRequest(socket, adjusted))
      SendError(socket, 404, QStringLiteral("no such endpoint"));
    return;
  }

  HandleStaticRequest(socket, adjusted);
}

////////////////////////////////////////////////////////////////////////////////

bool HttpServer::HandleApiRequest(QTcpSocket* socket, const Request& request)
{
  const QString& path = request.path;
  const bool isGet = (request.method == "GET");
  const bool isPost = (request.method == "POST");
  const bool isPut = (request.method == "PUT");

  // Live updates: log lines, item states and run state.
  if (path == QLatin1String("/api/events") && isGet)
  {
    BeginEventStream(socket);
    return true;
  }

  if (path == QLatin1String("/api/status") && isGet)
  {
    SendJson(socket, 200, m_Controller.StatusToJson());
    return true;
  }

  if (path == QLatin1String("/api/interfaces") && isGet)
  {
    SendJson(socket, 200, RouterController::InterfacesToJson());
    return true;
  }

  if (path == QLatin1String("/api/config"))
  {
    if (isGet)
    {
      SendJson(socket, 200, m_Controller.ConfigToJson());
      return true;
    }

    if (isPut)
    {
      QJsonParseError parseError;
      const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
      if (parseError.error != QJsonParseError::NoError || !doc.isObject())
      {
        SendError(socket, 400, parseError.errorString());
        return true;
      }

      QString error;
      if (!m_Controller.ConfigFromJson(doc.object(), error) || !m_Controller.SaveConfigFile(error))
      {
        SendError(socket, 500, error);
        return true;
      }

      SendJson(socket, 200, m_Controller.ConfigToJson());
      return true;
    }

    SendError(socket, 405, QStringLiteral("method not allowed"));
    return true;
  }

  // Persist and restart the routing engine with the current configuration.
  if (path == QLatin1String("/api/config/apply") && isPost)
  {
    QString error;
    if (!m_Controller.SaveConfigFile(error))
    {
      SendError(socket, 500, error);
      return true;
    }

    m_Controller.Restart();
    SendJson(socket, 200, m_Controller.StatusToJson());
    return true;
  }

  // The raw ".osc.txt", for people who would rather edit the file directly.
  if (path == QLatin1String("/api/file"))
  {
    if (isGet)
    {
      SendResponse(socket, 200, "text/plain; charset=utf-8", m_Controller.GetConfigFileText().toUtf8());
      return true;
    }

    if (isPut)
    {
      QString error;
      if (!m_Controller.SetConfigFileText(QString::fromUtf8(request.body), error))
      {
        SendError(socket, 500, error);
        return true;
      }

      SendJson(socket, 200, m_Controller.ConfigToJson());
      return true;
    }

    SendError(socket, 405, QStringLiteral("method not allowed"));
    return true;
  }

  if (path == QLatin1String("/api/start") && isPost)
  {
    m_Controller.Start();
    SendJson(socket, 200, m_Controller.StatusToJson());
    return true;
  }

  if (path == QLatin1String("/api/stop") && isPost)
  {
    m_Controller.Stop();
    SendJson(socket, 200, m_Controller.StatusToJson());
    return true;
  }

  if (path == QLatin1String("/api/mute-all") && isPost)
  {
    const QJsonObject body = QJsonDocument::fromJson(request.body).object();
    m_Controller.SetMuteAll(body.value("incoming").toBool(), body.value("outgoing").toBool());
    SendJson(socket, 200, m_Controller.StatusToJson());
    return true;
  }

  // /api/routes/<index>/mute and /api/routes/<index>/enable
  if (path.startsWith(QLatin1String("/api/routes/")) && isPost)
  {
    const QStringList parts = path.mid(12).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() == 2)
    {
      bool ok = false;
      const int index = parts[0].toInt(&ok);
      const QJsonObject body = QJsonDocument::fromJson(request.body).object();
      const bool value = body.value("value").toBool();

      if (ok && parts[1] == QLatin1String("mute"))
      {
        if (!m_Controller.SetRouteMuted(index, value))
        {
          SendError(socket, 400, QStringLiteral("no such route"));
          return true;
        }

        // Muting applies live, but the file should reflect it too.
        QString error;
        m_Controller.SaveConfigFile(error);
        SendJson(socket, 200, m_Controller.StatusToJson());
        return true;
      }

      if (ok && parts[1] == QLatin1String("enable"))
      {
        if (!m_Controller.SetRouteEnabled(index, value))
        {
          SendError(socket, 400, QStringLiteral("no such route"));
          return true;
        }

        // Enabling and disabling changes which routes are built, so the engine
        // has to be rebuilt, exactly as the desktop application does.
        QString error;
        m_Controller.SaveConfigFile(error);
        m_Controller.Restart();
        SendJson(socket, 200, m_Controller.StatusToJson());
        return true;
      }
    }
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::HandleStaticRequest(QTcpSocket* socket, const Request& request)
{
  if (request.method != "GET")
  {
    SendError(socket, 405, QStringLiteral("method not allowed"));
    return;
  }

  QString path = request.path;
  if (path.isEmpty() || path == QLatin1String("/"))
    path = QStringLiteral("/index.html");

  // Everything is served from the compiled-in resources, so reject any attempt
  // to escape that tree rather than trying to normalise it.
  if (path.contains(QLatin1String("..")))
  {
    SendError(socket, 403, QStringLiteral("forbidden"));
    return;
  }

  QFile file(QStringLiteral(":/web%1").arg(path));
  if (!file.open(QFile::ReadOnly))
  {
    SendError(socket, 404, QStringLiteral("not found"));
    return;
  }

  SendResponse(socket, 200, ContentTypeForPath(path), file.readAll());
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::SendResponse(QTcpSocket* socket, int status, const QByteArray& contentType, const QByteArray& body)
{
  QByteArray response;
  response.append("HTTP/1.1 ");
  response.append(QByteArray::number(status));
  response.append(' ');
  response.append(StatusText(status));
  response.append("\r\nContent-Type: ");
  response.append(contentType);
  response.append("\r\nContent-Length: ");
  response.append(QByteArray::number(body.size()));
  response.append("\r\nCache-Control: no-store");
  response.append("\r\nConnection: close\r\n\r\n");
  response.append(body);

  socket->write(response);
  socket->flush();
  socket->disconnectFromHost();
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::SendJson(QTcpSocket* socket, int status, const QJsonObject& object)
{
  SendResponse(socket, status, "application/json; charset=utf-8", QJsonDocument(object).toJson(QJsonDocument::Compact));
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::SendJson(QTcpSocket* socket, int status, const QJsonArray& array)
{
  SendResponse(socket, status, "application/json; charset=utf-8", QJsonDocument(array).toJson(QJsonDocument::Compact));
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::SendError(QTcpSocket* socket, int status, const QString& message)
{
  SendJson(socket, status, QJsonObject{{"error", message}});
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::BeginEventStream(QTcpSocket* socket)
{
  QByteArray response;
  response.append("HTTP/1.1 200 OK\r\n");
  response.append("Content-Type: text/event-stream\r\n");
  response.append("Cache-Control: no-store\r\n");
  // Chunked encoding is not used, and no proxy should buffer the stream.
  response.append("X-Accel-Buffering: no\r\n");
  response.append("Connection: keep-alive\r\n\r\n");

  socket->write(response);
  socket->flush();

  m_EventClients.insert(socket);

  // Prime the browser with the current state so it does not have to poll first.
  SendEvent(socket, "status", QJsonDocument(m_Controller.StatusToJson()).toJson(QJsonDocument::Compact));
  SendEvent(socket, "itemStates", QJsonDocument(m_Controller.ItemStatesToJson()).toJson(QJsonDocument::Compact));

  // Replay recent history, so the log pane is not empty on arrival and whatever
  // the routing engine reported while starting up is still visible.
  const std::deque<QJsonObject>& history = m_Controller.GetLogHistory();
  for (std::deque<QJsonObject>::const_iterator i = history.begin(); i != history.end(); i++)
    SendEvent(socket, "log", QJsonDocument(*i).toJson(QJsonDocument::Compact));
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::SendEvent(QTcpSocket* socket, const QByteArray& name, const QByteArray& data)
{
  if (!socket || socket->state() != QAbstractSocket::ConnectedState)
    return;

  QByteArray event;
  event.append("event: ");
  event.append(name);
  event.append("\ndata: ");
  event.append(data);
  event.append("\n\n");

  socket->write(event);
  socket->flush();
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::BroadcastEvent(const QByteArray& name, const QJsonValue& data)
{
  if (m_EventClients.isEmpty())
    return;

  QByteArray payload;
  if (data.isArray())
    payload = QJsonDocument(data.toArray()).toJson(QJsonDocument::Compact);
  else
    payload = QJsonDocument(data.toObject()).toJson(QJsonDocument::Compact);

  const QSet<QTcpSocket*> clients = m_EventClients;
  for (QSet<QTcpSocket*>::const_iterator i = clients.begin(); i != clients.end(); i++)
    SendEvent(*i, name, payload);
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::onLogMessage(const QJsonObject& message)
{
  BroadcastEvent("log", message);
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::onItemStatesChanged()
{
  BroadcastEvent("itemStates", m_Controller.ItemStatesToJson());
}

////////////////////////////////////////////////////////////////////////////////

void HttpServer::onRunStateChanged()
{
  BroadcastEvent("status", m_Controller.StatusToJson());
}

////////////////////////////////////////////////////////////////////////////////
