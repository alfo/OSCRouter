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

#include "RouterController.h"

#include "Version.h"

#include <map>
#include <unordered_set>

#include <stdio.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonValue>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkInterface>

#define TICK_INTERVAL_MS 30

// Enough to cover what the engine says while starting up, plus a little recent
// traffic, without letting a busy show grow this without bound.
#define MAX_LOG_HISTORY 500

////////////////////////////////////////////////////////////////////////////////

namespace
{
// The routing engine reports state per item; the browser renders it, so the
// daemon only needs a stable name for each value.
const char* StateName(ItemState::EnumState state)
{
  switch (state)
  {
    case ItemState::STATE_CONNECTING: return "connecting";
    case ItemState::STATE_CONNECTED: return "connected";
    case ItemState::STATE_NOT_CONNECTED: return "notConnected";
    default: break;
  }
  return "uninitialized";
}

const char* LogTypeName(EosLog::EnumLogMsgType type)
{
  switch (type)
  {
    case EosLog::LOG_MSG_TYPE_DEBUG: return "debug";
    case EosLog::LOG_MSG_TYPE_INFO: return "info";
    case EosLog::LOG_MSG_TYPE_WARNING: return "warning";
    case EosLog::LOG_MSG_TYPE_ERROR: return "error";
    case EosLog::LOG_MSG_TYPE_RECV: return "recv";
    case EosLog::LOG_MSG_TYPE_SEND: return "send";
  }
  return "info";
}

QJsonObject TransformToJson(const EosRouteDst::sTransform& transform)
{
  // Serialised the same way it is stored in the file: an empty string means the
  // transform is disabled.
  QString str;
  ConfigFile::TransformToString(transform, str);
  return QJsonObject{{"value", str}};
}

void TransformFromJson(const QJsonValue& value, EosRouteDst::sTransform& transform)
{
  QString str;
  if (value.isObject())
    str = value.toObject().value("value").toString();
  else if (value.isString())
    str = value.toString();

  ConfigFile::StringToTransform(str, transform);
}
}  // namespace

////////////////////////////////////////////////////////////////////////////////

RouterController::RouterController(const QString& configPath, unsigned int reconnectDelayMS, QObject* parent /*= nullptr*/)
  : QObject(parent)
  , m_ConfigPath(configPath)
  , m_ReconnectDelay(reconnectDelayMS)
{
  m_Timer = new QTimer(this);
  connect(m_Timer, &QTimer::timeout, this, &RouterController::onTick);
  m_Timer->start(TICK_INTERVAL_MS);
}

////////////////////////////////////////////////////////////////////////////////

RouterController::~RouterController()
{
  Stop();
}

////////////////////////////////////////////////////////////////////////////////

bool RouterController::LoadConfigFile(QString& error)
{
  ConfigFile::Contents contents;
  if (!ConfigFile::LoadFile(m_ConfigPath, contents))
  {
    error = tr("unable to open \"%1\"").arg(m_ConfigPath);
    return false;
  }

  m_Contents = contents;
  return true;
}

////////////////////////////////////////////////////////////////////////////////

bool RouterController::SaveConfigFile(QString& error)
{
  if (!ConfigFile::SaveFile(m_ConfigPath, m_Contents))
  {
    error = tr("unable to write \"%1\"").arg(m_ConfigPath);
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////

QString RouterController::GetConfigFileText() const
{
  QFile file(m_ConfigPath);
  if (!file.open(QFile::ReadOnly | QFile::Text))
    return QString();

  QTextStream stream(&file);
  stream.setEncoding(QStringConverter::Utf8);
  return stream.readAll();
}

////////////////////////////////////////////////////////////////////////////////

bool RouterController::SetConfigFileText(const QString& text, QString& error)
{
  // Written through verbatim rather than parsed and re-serialised. Someone
  // editing the file directly expects to get back what they typed, and
  // round-tripping it would quietly rewrite anything the parser normalises --
  // or drop it outright.
  QDir().mkpath(QFileInfo(m_ConfigPath).absolutePath());

  QFile file(m_ConfigPath);
  if (!file.open(QFile::WriteOnly | QFile::Truncate))
  {
    error = tr("unable to write \"%1\"").arg(m_ConfigPath);
    return false;
  }

  QTextStream stream(&file);
  stream.setEncoding(QStringConverter::Utf8);
  stream << text;
  stream.flush();
  file.close();

  ConfigFile::Contents contents;
  ConfigFile::LoadLines(ConfigFile::SplitLines(text), contents);
  m_Contents = contents;
  return true;
}

////////////////////////////////////////////////////////////////////////////////

void RouterController::PrepareForRouting(Router::ROUTES& routes, Router::CONNECTIONS& connections)
{
  routes.clear();
  connections.clear();

  m_ItemStateTable.Clear();
  m_ItemStateTable.SetMuteAllIncoming(m_Contents.itemStateTable.GetMuteAllIncoming());
  m_ItemStateTable.SetMuteAllOutgoing(m_Contents.itemStateTable.GetMuteAllOutgoing());

  m_RouteStateIds.assign(m_Contents.routes.size(), RouteStateIds());

  // One item state per unique address, shared by every route using it.
  typedef std::map<EosAddr, ItemStateTable::ID> AddrStates;
  AddrStates srcAddrStates;
  AddrStates dstAddrStates;

  for (size_t i = 0; i < m_Contents.routes.size(); i++)
  {
    Router::sRoute route = m_Contents.routes[i];

    if (!ValidPort(route.src.protocol, route.src.addr.port))
      continue;  // port required

    bool duplicate = false;
    for (Router::ROUTES::const_iterator j = routes.begin(); j != routes.end(); j++)
    {
      if (j->src == route.src && j->dst == route.dst)
      {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;

    AddrStates::const_iterator j = srcAddrStates.find(route.src.addr);
    if (j == srcAddrStates.end())
      srcAddrStates[route.src.addr] = route.srcItemStateTableId = m_ItemStateTable.Register(/*mute*/ false);
    else
      route.srcItemStateTableId = j->second;

    j = dstAddrStates.find(route.dst.addr);
    if (j == dstAddrStates.end())
      dstAddrStates[route.dst.addr] = route.dstItemStateTableId = m_ItemStateTable.Register(route.mute);
    else
      route.dstItemStateTableId = j->second;

    m_RouteStateIds[i].src = route.srcItemStateTableId;
    m_RouteStateIds[i].dst = route.dstItemStateTableId;

    routes.push_back(route);
  }

  // TCP connections are registered after the routes, matching the order the
  // desktop application uses in MainWindow::BuildRoutes.
  for (size_t i = 0; i < m_Contents.connections.size(); i++)
  {
    Router::sConnection connection = m_Contents.connections[i];

    if (connection.addr.port == 0)
      continue;  // port required

    if (connection.addr.ip == QLatin1String("0.0.0.0"))
      connection.addr.ip.clear();

    bool duplicate = false;
    for (Router::CONNECTIONS::const_iterator j = connections.begin(); j != connections.end(); j++)
    {
      if (j->addr == connection.addr)
      {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;

    connection.itemStateTableId = m_ItemStateTable.Register(/*mute*/ false);
    connections.push_back(connection);
  }
}

////////////////////////////////////////////////////////////////////////////////

void RouterController::Start()
{
  Stop();

  Router::ROUTES routes;
  Router::CONNECTIONS connections;
  PrepareForRouting(routes, connections);

  if (routes.empty())
  {
    m_Log.AddWarning("no routes configured, not starting");
    emit runStateChanged();
    return;
  }

  m_RouterThread = new RouterThread(routes, connections, m_Contents.settings, m_ItemStateTable, m_ReconnectDelay);
  m_RouterThread->start();

  emit runStateChanged();
}

////////////////////////////////////////////////////////////////////////////////

void RouterController::Stop()
{
  if (m_RouterThread)
  {
    m_RouterThread->Stop();
    Sync(/*logsOnly*/ true);
    delete m_RouterThread;
    m_RouterThread = nullptr;

    emit runStateChanged();
  }
}

////////////////////////////////////////////////////////////////////////////////

void RouterController::Restart()
{
  Start();
}

////////////////////////////////////////////////////////////////////////////////

void RouterController::Sync(bool logsOnly)
{
  if (m_RouterThread)
  {
    m_RouterThread->Sync(m_TempLogQ, m_ItemStateTable);
    m_Log.AddQ(m_TempLogQ);
  }

  m_Log.Flush(m_TempLogQ);

  for (EosLog::LOG_Q::const_iterator i = m_TempLogQ.begin(); i != m_TempLogQ.end(); i++)
  {
    const char* typeName = LogTypeName(i->type);
    const bool isPacket = (i->type == EosLog::LOG_MSG_TYPE_RECV || i->type == EosLog::LOG_MSG_TYPE_SEND);

    if (m_EchoLogToStdout && (m_EchoPacketsToStdout || !isPacket))
    {
      printf("[%s] %s\n", typeName, i->text.c_str());
      fflush(stdout);
    }

    const QJsonObject message{
      {"type", QString::fromLatin1(typeName)}, {"timestamp", static_cast<qint64>(i->timestamp)}, {"text", QString::fromUtf8(i->text.c_str())}};

    m_LogHistory.push_back(message);
    while (m_LogHistory.size() > MAX_LOG_HISTORY)
      m_LogHistory.pop_front();

    emit logMessage(message);
  }

  m_TempLogQ.clear();

  if (!logsOnly)
  {
    if (m_ItemStateTable.GetDirty())
    {
      emit itemStatesChanged();
      m_ItemStateTable.Reset();
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

void RouterController::onTick()
{
  Sync(/*logsOnly*/ false);
}

////////////////////////////////////////////////////////////////////////////////

bool RouterController::SetRouteMuted(int routeIndex, bool muted)
{
  if (routeIndex < 0 || routeIndex >= static_cast<int>(m_Contents.routes.size()))
    return false;

  m_Contents.routes[routeIndex].mute = muted;

  // Carried to the routing engine through the shared item state table on the
  // next tick, so muting takes effect without restarting.
  const ItemStateTable::ID id = m_RouteStateIds[routeIndex].dst;
  if (id != ItemStateTable::sm_Invalid_Id)
    m_ItemStateTable.Mute(id, muted);

  return true;
}

////////////////////////////////////////////////////////////////////////////////

void RouterController::SetMuteAll(bool incoming, bool outgoing)
{
  m_Contents.itemStateTable.SetMuteAllIncoming(incoming);
  m_Contents.itemStateTable.SetMuteAllOutgoing(outgoing);
  m_ItemStateTable.SetMuteAllIncoming(incoming);
  m_ItemStateTable.SetMuteAllOutgoing(outgoing);
}

////////////////////////////////////////////////////////////////////////////////

bool RouterController::SetRouteEnabled(int routeIndex, bool enabled)
{
  if (routeIndex < 0 || routeIndex >= static_cast<int>(m_Contents.routes.size()))
    return false;

  m_Contents.routes[routeIndex].enable = enabled;
  return true;
}

////////////////////////////////////////////////////////////////////////////////

QJsonObject RouterController::ConfigToJson() const
{
  QJsonArray routes;
  for (size_t i = 0; i < m_Contents.routes.size(); i++)
  {
    const Router::sRoute& route = m_Contents.routes[i];

    routes.append(QJsonObject{
      {"index", static_cast<int>(i)},
      {"label", route.label},
      {"enable", route.enable},
      {"mute", route.mute},
      {"src", QJsonObject{{"ip", route.src.addr.ip},
                          {"port", static_cast<int>(route.src.addr.port)},
                          {"path", route.src.path},
                          {"protocol", static_cast<int>(route.src.protocol)},
                          {"multicastInterfaceIP", route.src.multicastInterfaceIP}}},
      {"dst", QJsonObject{{"ip", route.dst.addr.ip},
                          {"port", static_cast<int>(route.dst.addr.port)},
                          {"path", route.dst.path},
                          {"protocol", static_cast<int>(route.dst.protocol)},
                          {"multicastInterfaceIP", route.dst.multicastInterfaceIP},
                          {"script", route.dst.script},
                          {"scriptText", route.dst.scriptText}}},
      {"inMin", TransformToJson(route.dst.inMin)},
      {"inMax", TransformToJson(route.dst.inMax)},
      {"outMin", TransformToJson(route.dst.outMin)},
      {"outMax", TransformToJson(route.dst.outMax)},
    });
  }

  QJsonArray connections;
  for (size_t i = 0; i < m_Contents.connections.size(); i++)
  {
    const Router::sConnection& connection = m_Contents.connections[i];

    connections.append(QJsonObject{{"index", static_cast<int>(i)},
                                   {"label", connection.label},
                                   {"server", connection.server},
                                   {"frameMode", static_cast<int>(connection.frameMode)},
                                   {"ip", connection.addr.ip},
                                   {"port", static_cast<int>(connection.addr.port)}});
  }

  QJsonArray otpModules;
  for (size_t i = 0; i < static_cast<size_t>(otp::ModuleType::kCount); i++)
    otpModules.append(m_Contents.settings.otpModuleTypes.find(static_cast<otp::ModuleType>(i)) != m_Contents.settings.otpModuleTypes.end());

  return QJsonObject{{"routes", routes},
                     {"connections", connections},
                     {"settings", QJsonObject{{"sACNIP", m_Contents.settings.sACNIP},
                                              {"artNetIP", m_Contents.settings.artNetIP},
                                              {"otpIP", m_Contents.settings.otpIP},
                                              {"levelChangesOnly", m_Contents.settings.levelChangesOnly},
                                              {"script", m_Contents.settings.script},
                                              {"otpModules", otpModules}}},
                     {"muteAllIncoming", m_Contents.itemStateTable.GetMuteAllIncoming()},
                     {"muteAllOutgoing", m_Contents.itemStateTable.GetMuteAllOutgoing()}};
}

////////////////////////////////////////////////////////////////////////////////

bool RouterController::ConfigFromJson(const QJsonObject& json, QString& error)
{
  ConfigFile::Contents contents;

  const QJsonArray routes = json.value("routes").toArray();
  for (QJsonArray::const_iterator i = routes.begin(); i != routes.end(); i++)
  {
    const QJsonObject obj = i->toObject();
    const QJsonObject src = obj.value("src").toObject();
    const QJsonObject dst = obj.value("dst").toObject();

    Router::sRoute route;
    route.label = obj.value("label").toString();
    route.enable = obj.value("enable").toBool(true);
    route.mute = obj.value("mute").toBool(false);

    route.src.addr.ip = src.value("ip").toString();
    route.src.addr.port = static_cast<unsigned short>(src.value("port").toInt());
    route.src.path = src.value("path").toString();
    route.src.protocol = ConfigFile::SanitizeProtocol(src.value("protocol").toInt());
    route.src.multicastInterfaceIP = src.value("multicastInterfaceIP").toString();

    route.dst.addr.ip = dst.value("ip").toString();
    route.dst.addr.port = static_cast<unsigned short>(dst.value("port").toInt());
    route.dst.path = dst.value("path").toString();
    route.dst.protocol = ConfigFile::SanitizeProtocol(dst.value("protocol").toInt());
    route.dst.multicastInterfaceIP = dst.value("multicastInterfaceIP").toString();
    route.dst.scriptText = dst.value("scriptText").toString();
    route.dst.script = !route.dst.scriptText.isEmpty();

    TransformFromJson(obj.value("inMin"), route.dst.inMin);
    TransformFromJson(obj.value("inMax"), route.dst.inMax);
    TransformFromJson(obj.value("outMin"), route.dst.outMin);
    TransformFromJson(obj.value("outMax"), route.dst.outMax);

    contents.routes.push_back(route);
  }

  const QJsonArray connections = json.value("connections").toArray();
  for (QJsonArray::const_iterator i = connections.begin(); i != connections.end(); i++)
  {
    const QJsonObject obj = i->toObject();

    Router::sConnection connection;
    connection.label = obj.value("label").toString();
    connection.server = obj.value("server").toBool();

    const int frameMode = obj.value("frameMode").toInt();
    connection.frameMode =
      ((frameMode >= 0 && frameMode < OSCStream::FRAME_MODE_COUNT) ? static_cast<OSCStream::EnumFrameMode>(frameMode) : OSCStream::FRAME_MODE_DEFAULT);

    connection.addr.ip = obj.value("ip").toString();
    connection.addr.port = static_cast<unsigned short>(obj.value("port").toInt());

    contents.connections.push_back(connection);
  }

  const QJsonObject settings = json.value("settings").toObject();
  contents.settings.sACNIP = settings.value("sACNIP").toString();
  contents.settings.artNetIP = settings.value("artNetIP").toString();
  contents.settings.otpIP = settings.value("otpIP").toString();
  contents.settings.levelChangesOnly = settings.value("levelChangesOnly").toBool();
  contents.settings.script = settings.value("script").toString();

  const QJsonArray otpModules = settings.value("otpModules").toArray();
  contents.settings.otpModuleTypes.clear();
  for (qsizetype i = 0; i < otpModules.size() && i < static_cast<qsizetype>(otp::ModuleType::kCount); i++)
  {
    if (otpModules.at(i).toBool())
      contents.settings.otpModuleTypes.insert(static_cast<otp::ModuleType>(i));
  }

  contents.itemStateTable.SetMuteAllIncoming(json.value("muteAllIncoming").toBool());
  contents.itemStateTable.SetMuteAllOutgoing(json.value("muteAllOutgoing").toBool());

  m_Contents = contents;

  Q_UNUSED(error);
  return true;
}

////////////////////////////////////////////////////////////////////////////////

QJsonObject RouterController::StatusToJson() const
{
  return QJsonObject{{"running", IsRunning()},
                     // Reported so a running instance can be identified without
                     // having to guess from the shape of the interface.
                     {"version", QStringLiteral("%1.%2.%3").arg(OSCROUTER_VERSION_MAJOR).arg(OSCROUTER_VERSION_MINOR).arg(OSCROUTER_VERSION_PATCH)},
                     {"configPath", m_ConfigPath},
                     {"routeCount", static_cast<int>(m_Contents.routes.size())},
                     {"connectionCount", static_cast<int>(m_Contents.connections.size())},
                     {"muteAllIncoming", m_ItemStateTable.GetMuteAllIncoming()},
                     {"muteAllOutgoing", m_ItemStateTable.GetMuteAllOutgoing()},
                     {"issues", IssuesToJson()}};
}

////////////////////////////////////////////////////////////////////////////////

QJsonArray RouterController::IssuesToJson() const
{
  QJsonArray result;

  const ConfigFile::ISSUES issues = ConfigFile::Diagnose(m_Contents);
  for (ConfigFile::ISSUES::const_iterator i = issues.begin(); i != issues.end(); i++)
  {
    result.append(QJsonObject{{"level", QString::fromLatin1(i->level == ConfigFile::Issue::Level::kError ? "error" : "warning")},
                              {"routeIndex", i->routeIndex},
                              {"message", i->message}});
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////

QJsonArray RouterController::ItemStatesToJson() const
{
  // Reported per route, since that is how the web interface lays them out.
  QJsonArray states;

  for (size_t i = 0; i < m_RouteStateIds.size(); i++)
  {
    const ItemState* src = m_ItemStateTable.GetItemState(m_RouteStateIds[i].src);
    const ItemState* dst = m_ItemStateTable.GetItemState(m_RouteStateIds[i].dst);

    QJsonObject entry{{"index", static_cast<int>(i)}};

    if (src)
    {
      entry["srcState"] = QString::fromLatin1(StateName(src->state));
      entry["srcActivity"] = src->activity;
    }

    if (dst)
    {
      entry["dstState"] = QString::fromLatin1(StateName(dst->state));
      entry["dstActivity"] = dst->activity;
      entry["dstMute"] = dst->mute;
    }

    states.append(entry);
  }

  return states;
}

////////////////////////////////////////////////////////////////////////////////

QJsonArray RouterController::InterfacesToJson()
{
  // Mirrors the interface pickers in SettingsWidget: running interfaces with an
  // IPv4 address, de-duplicated.
  QJsonArray result;
  std::unordered_set<quint32> ips;

  const QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
  for (QList<QNetworkInterface>::const_iterator ifaceIter = ifaces.begin(); ifaceIter != ifaces.end(); ++ifaceIter)
  {
    const QNetworkInterface& iface = *ifaceIter;
    if (!iface.flags().testFlag(QNetworkInterface::IsRunning))
      continue;

    const QList<QNetworkAddressEntry> addrs = iface.addressEntries();
    for (QList<QNetworkAddressEntry>::const_iterator addrIter = addrs.begin(); addrIter != addrs.end(); ++addrIter)
    {
      const QHostAddress addr = addrIter->ip();
      if (addr.protocol() != QAbstractSocket::IPv4Protocol)
        continue;

      const quint32 ip = addr.toIPv4Address();
      if (ips.find(ip) != ips.end())
        continue;  // already added

      ips.insert(ip);
      result.append(QJsonObject{{"ip", addr.toString()}, {"name", iface.humanReadableName()}});
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
