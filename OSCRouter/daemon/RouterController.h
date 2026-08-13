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
#ifndef ROUTER_CONTROLLER_H
#define ROUTER_CONTROLLER_H

#ifndef CONFIG_FILE_H
#include "ConfigFile.h"
#endif

#include <deque>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>

class QTimer;
class RouterThread;

////////////////////////////////////////////////////////////////////////////////

// Owns the routing engine for the headless daemon.
//
// This is the non-graphical equivalent of what MainWindow does: it holds the
// configuration, builds and starts a RouterThread from it, and polls
// RouterThread::Sync on a timer to drain the log queue and pick up item state
// changes. Those arrive here as Qt signals instead of being written into
// widgets, and HttpServer forwards them to connected browsers.
class RouterController : public QObject
{
  Q_OBJECT

public:
  explicit RouterController(const QString& configPath, unsigned int reconnectDelayMS, QObject* parent = nullptr);
  ~RouterController() override;

  // Echo the routing engine's log to stdout, so it reaches "docker logs" and
  // the Home Assistant add-on log rather than only the browser. Per-packet
  // messages are excluded unless asked for, as they arrive at traffic rates.
  void SetEchoLogToStdout(bool b) { m_EchoLogToStdout = b; }
  void SetEchoPacketsToStdout(bool b) { m_EchoPacketsToStdout = b; }

  // Recent log messages, replayed to a browser when it connects. Without this
  // the log pane would sit empty until the next message happened to arrive,
  // hiding whatever the engine said while starting up.
  const std::deque<QJsonObject>& GetLogHistory() const { return m_LogHistory; }

  // Configuration file
  const QString& GetConfigPath() const { return m_ConfigPath; }
  bool LoadConfigFile(QString& error);
  bool SaveConfigFile(QString& error);
  QString GetConfigFileText() const;
  bool SetConfigFileText(const QString& text, QString& error);

  // Routing engine lifecycle
  void Start();
  void Stop();
  bool IsRunning() const { return m_RouterThread != nullptr; }
  // Applies the current configuration by restarting the routing engine, exactly
  // as the desktop application rebuilds its routes after an edit.
  void Restart();

  // JSON representations used by the web interface
  QJsonObject ConfigToJson() const;
  bool ConfigFromJson(const QJsonObject& json, QString& error);
  QJsonObject StatusToJson() const;
  QJsonArray ItemStatesToJson() const;
  static QJsonArray InterfacesToJson();

  // Live edits that do not require a restart. Muting is carried to the routing
  // engine through the shared item state table, as it is in the desktop UI.
  bool SetRouteMuted(int routeIndex, bool muted);
  void SetMuteAll(bool incoming, bool outgoing);

  // Live edit that does require a restart.
  bool SetRouteEnabled(int routeIndex, bool enabled);

signals:
  // One drained log message, already shaped for the browser.
  void logMessage(const QJsonObject& message);
  // Item states changed since the last tick.
  void itemStatesChanged();
  // The routing engine started or stopped.
  void runStateChanged();

private slots:
  void onTick();

private:
  // Turns the file representation into something RouterThread can run:
  // registers an item state table entry per unique source and destination
  // address, drops routes with an invalid port, and drops duplicates. This
  // mirrors RoutingWidget::SaveRoutes and TcpWidget::SaveConnections, which are
  // where the desktop application performs the same checks.
  void PrepareForRouting(Router::ROUTES& routes, Router::CONNECTIONS& connections);

  void Sync(bool logsOnly);

  QString m_ConfigPath;
  unsigned int m_ReconnectDelay = 5000;
  ConfigFile::Contents m_Contents;
  ItemStateTable m_ItemStateTable;
  RouterThread* m_RouterThread = nullptr;
  QTimer* m_Timer = nullptr;
  EosLog m_Log;
  EosLog::LOG_Q m_TempLogQ;
  std::deque<QJsonObject> m_LogHistory;
  bool m_EchoLogToStdout = true;
  bool m_EchoPacketsToStdout = false;

  // Maps a route index in m_Contents.routes to the item state table ids it was
  // given by PrepareForRouting, so the web interface can address routes by
  // their position in the configuration.
  struct RouteStateIds
  {
    ItemStateTable::ID src = ItemStateTable::sm_Invalid_Id;
    ItemStateTable::ID dst = ItemStateTable::sm_Invalid_Id;
  };
  std::vector<RouteStateIds> m_RouteStateIds;
};

////////////////////////////////////////////////////////////////////////////////

#endif
