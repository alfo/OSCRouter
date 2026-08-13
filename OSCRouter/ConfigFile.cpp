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

#include "ConfigFile.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>

////////////////////////////////////////////////////////////////////////////////

QString FileUtils::QuotedString(const QString& str)
{
  // "test" -> """test"""
  // test,  -> "test,"

  QString quoted(str);
  quoted.replace("\"", "\"\"");
  if (quoted.contains('\"') || quoted.contains(','))
  {
    quoted.prepend("\"");
    quoted.append("\"");
  }

  quoted.replace("\n", "\\n");

  return quoted;
}

////////////////////////////////////////////////////////////////////////////////

void FileUtils::GetItemsFromQuotedString(const QString& str, QStringList& items)
{
  items.clear();

  int len = str.size();
  int index = 0;
  bool quoted = false;
  for (int i = 0; i <= len; i++)
  {
    if (i >= len || (str[i] == QChar(',') && !quoted))
    {
      int itemLen = (i - index);
      if (itemLen > 0)
      {
        QString item(str.mid(index, itemLen).trimmed());

        // remove quotes
        if (item.startsWith('\"') && item.endsWith('\"'))
        {
          itemLen = (item.size() - 2);
          if (itemLen > 0)
            item = item.mid(1, itemLen);
          else
            item.clear();
        }

        // fix quoted quotes
        item.replace("\"\"", "\"");

        // replace newlines
        item.replace("\\n", "\n");

        items.push_back(item);
      }
      else
        items.push_back(QString());

      index = (i + 1);
    }
    else if (str[i] == QChar('\"'))
    {
      if (!quoted)
        quoted = true;
      else if ((i + 1) >= len || str[i + 1] != QChar('\"'))
        quoted = false;
      else
        ++i;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::StringToTransform(const QString& str, EosRouteDst::sTransform& transform)
{
  if (str.isEmpty())
  {
    transform.enabled = false;
    transform.value = 0;
  }
  else
  {
    transform.value = str.toFloat(&transform.enabled);
    if (!transform.enabled)
      transform.value = 0;
  }
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::TransformToString(const EosRouteDst::sTransform& transform, QString& str)
{
  str = (transform.enabled ? QString::number(transform.value) : QString());
}

////////////////////////////////////////////////////////////////////////////////

Protocol ConfigFile::SanitizeProtocol(int protocol)
{
  if (protocol < 0 || protocol >= static_cast<int>(Protocol::kCount))
    return Protocol::kDefault;

  return static_cast<Protocol>(protocol);
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::LoadSettingsLine(const QString& line, Router::Settings& settings)
{
  QStringList items;
  FileUtils::GetItemsFromQuotedString(line, items);

  if (items.size() >= 3 && items[0].compare(QLatin1String("Settings"), Qt::CaseInsensitive) == 0)
  {
    settings.sACNIP = items[1];
    settings.artNetIP = items[2];
    if (items.size() > 3)
      settings.levelChangesOnly = items[3].toInt() != 0;
    if (items.size() > 4)
      settings.script = items[4];
    if (items.size() > 5)
      settings.otpIP = items[5];
    if (items.size() > 6)
    {
      settings.otpModuleTypes.clear();

      for (qsizetype moduleIndex = 0; moduleIndex < static_cast<qsizetype>(otp::ModuleType::kCount); ++moduleIndex)
      {
        qsizetype offset = moduleIndex + 6;
        if (offset >= items.size())
          break;

        if (items[offset].toInt() != 0)
          settings.otpModuleTypes.insert(static_cast<otp::ModuleType>(moduleIndex));
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::LoadRouteLine(const QString& line, Router::ROUTES& routes, ItemStateTable& itemStateTable)
{
  QStringList items;
  FileUtils::GetItemsFromQuotedString(line, items);
  if (items.isEmpty())
    return;

  if (items.size() > 10)
  {
    Router::sRoute route;

    route.label = items[0];
    route.src.addr.ip = items[1];
    route.src.addr.port = items[2].toUShort();
    route.src.path = items[3];
    StringToTransform(items[4], route.dst.inMin);
    StringToTransform(items[5], route.dst.inMax);

    route.dst.addr.ip = items[6];
    route.dst.addr.port = items[7].toUShort();
    route.dst.path = items[8];
    StringToTransform(items[9], route.dst.outMin);
    StringToTransform(items[10], route.dst.outMax);

    if (items.size() > 11)
    {
      route.dst.scriptText = items[11];
      route.dst.script = !route.dst.scriptText.isEmpty();
    }

    if (items.size() > 12)
      route.src.multicastInterfaceIP = items[12];

    if (items.size() > 13)
      route.src.protocol = SanitizeProtocol(items[13].toInt());

    if (items.size() > 14)
      route.dst.protocol = SanitizeProtocol(items[14].toInt());

    if (items.size() > 15)
      route.enable = (items[15].toInt() != 0);

    if (items.size() > 16)
      route.mute = (items[16].toInt() == 0);

    if (items.size() > 17)
      route.dst.multicastInterfaceIP = items[17];

    routes.push_back(route);
  }
  else if (items.size() == 3 && items[0].compare(QLatin1String("Mute"), Qt::CaseInsensitive) == 0)
  {
    // Deviation from upstream, which reads items[0] and items[1] here. items[0]
    // is the "Mute" keyword, so it never parses as a number and "mute all
    // incoming" could never be restored from a file, while "mute all outgoing"
    // was restored from the incoming field. The record is written as
    // "Mute,<incoming>,<outgoing>" by Save below, so these are the right fields.
    itemStateTable.SetMuteAllIncoming(items[1].toInt() != 0);
    itemStateTable.SetMuteAllOutgoing(items[2].toInt() != 0);
  }
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::LoadConnectionLine(const QString& line, Router::CONNECTIONS& connections)
{
  QStringList items;
  FileUtils::GetItemsFromQuotedString(line, items);

  if (items.size() == 5)
  {
    Router::sConnection connection;

    connection.label = items[0];

    bool ok = false;
    int n = items[1].toInt(&ok);
    connection.server = (ok && n != 0);

    n = items[2].toInt(&ok);
    connection.frameMode = ((ok && n >= 0 && n < OSCStream::FRAME_MODE_COUNT) ? static_cast<OSCStream::EnumFrameMode>(n) : OSCStream::FRAME_MODE_INVALID);

    connection.addr.ip = items[3];
    connection.addr.port = items[4].toUShort();

    connections.push_back(connection);
  }
}

////////////////////////////////////////////////////////////////////////////////

QStringList ConfigFile::SplitLines(const QString& contents)
{
  QString text(contents);
  text.remove(QLatin1Char('\r'));
  return text.split(QLatin1Char('\n'));
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::Validate(Contents& contents)
{
  Router::ROUTES routes;
  for (Router::ROUTES::const_iterator i = contents.routes.begin(); i != contents.routes.end(); i++)
  {
    if (!ValidPort(i->src.protocol, i->src.addr.port))
      continue;

    bool duplicate = false;
    for (Router::ROUTES::const_iterator j = routes.begin(); j != routes.end(); j++)
    {
      if (j->src == i->src && j->dst == i->dst)
      {
        duplicate = true;
        break;
      }
    }

    if (!duplicate)
      routes.push_back(*i);
  }
  contents.routes.swap(routes);

  Router::CONNECTIONS connections;
  for (Router::CONNECTIONS::const_iterator i = contents.connections.begin(); i != contents.connections.end(); i++)
  {
    if (i->addr.port == 0)
      continue;

    bool duplicate = false;
    for (Router::CONNECTIONS::const_iterator j = connections.begin(); j != connections.end(); j++)
    {
      if (j->addr == i->addr)
      {
        duplicate = true;
        break;
      }
    }

    if (!duplicate)
      connections.push_back(*i);
  }
  contents.connections.swap(connections);
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::LoadLines(const QStringList& lines, Contents& contents)
{
  for (QStringList::const_iterator i = lines.begin(); i != lines.end(); i++)
  {
    LoadSettingsLine(*i, contents.settings);
    LoadRouteLine(*i, contents.routes, contents.itemStateTable);
    LoadConnectionLine(*i, contents.connections);
  }

  Validate(contents);
}

////////////////////////////////////////////////////////////////////////////////

bool ConfigFile::LoadFile(const QString& path, Contents& contents)
{
  QFile file(path);
  if (!file.open(QFile::ReadOnly | QFile::Text))
    return false;

  QTextStream stream(&file);
  stream.setEncoding(QStringConverter::Utf8);
  LoadLines(SplitLines(stream.readAll()), contents);
  return true;
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::SaveSettings(QTextStream& stream, const Router::Settings& settings)
{
  stream << QStringLiteral("Settings,%1,%2,%3,%4,%5")
                .arg(FileUtils::QuotedString(settings.sACNIP))
                .arg(FileUtils::QuotedString(settings.artNetIP))
                .arg(settings.levelChangesOnly ? 1 : 0)
                .arg(FileUtils::QuotedString(settings.script))
                .arg(FileUtils::QuotedString(settings.otpIP));

  for (size_t moduleIndex = 0; moduleIndex < static_cast<size_t>(otp::ModuleType::kCount); ++moduleIndex)
  {
    bool moduleEnabled = settings.otpModuleTypes.find(static_cast<otp::ModuleType>(moduleIndex)) != settings.otpModuleTypes.end();
    stream << "," + QString::number(moduleEnabled ? 1 : 0);
  }

  stream << QLatin1Char('\n');
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::SaveRoutes(QTextStream& stream, const Router::ROUTES& routes, const ItemStateTable& itemStateTable)
{
  stream << QStringLiteral("Mute,%1,%2\n").arg(itemStateTable.GetMuteAllIncoming() ? 1 : 0).arg(itemStateTable.GetMuteAllOutgoing() ? 1 : 0);

  for (Router::ROUTES::const_iterator i = routes.begin(); i != routes.end(); i++)
  {
    const Router::sRoute& route = *i;

    QString inMinStr;
    TransformToString(route.dst.inMin, inMinStr);
    QString inMaxStr;
    TransformToString(route.dst.inMax, inMaxStr);
    QString outMinStr;
    TransformToString(route.dst.outMin, outMinStr);
    QString outMaxStr;
    TransformToString(route.dst.outMax, outMaxStr);

    stream << FileUtils::QuotedString(route.label);
    stream << QStringLiteral(",%1").arg(FileUtils::QuotedString(route.src.addr.ip));
    stream << QStringLiteral(",%1").arg(route.src.addr.port);
    stream << QStringLiteral(",%1").arg(FileUtils::QuotedString(route.src.path));
    stream << QStringLiteral(",%1").arg(inMinStr);
    stream << QStringLiteral(",%1").arg(inMaxStr);
    stream << QStringLiteral(",%1").arg(FileUtils::QuotedString(route.dst.addr.ip));
    stream << QStringLiteral(",%1").arg(route.dst.addr.port);
    stream << QStringLiteral(",%1").arg(FileUtils::QuotedString(route.dst.path));
    stream << QStringLiteral(",%1").arg(outMinStr);
    stream << QStringLiteral(",%1").arg(outMaxStr);
    stream << QStringLiteral(",%1").arg(route.dst.script ? FileUtils::QuotedString(route.dst.scriptText) : QString());
    stream << QStringLiteral(",%1").arg(FileUtils::QuotedString(route.src.multicastInterfaceIP));
    stream << QStringLiteral(",%1").arg(static_cast<int>(route.src.protocol));
    stream << QStringLiteral(",%1").arg(static_cast<int>(route.dst.protocol));
    stream << QStringLiteral(",%1").arg(route.enable ? 1 : 0);
    stream << QStringLiteral(",%1").arg(route.mute ? 0 : 1);
    stream << QStringLiteral(",%1").arg(FileUtils::QuotedString(route.dst.multicastInterfaceIP));
    stream << QLatin1Char('\n');
  }
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::SaveConnections(QTextStream& stream, const Router::CONNECTIONS& connections)
{
  for (Router::CONNECTIONS::const_iterator i = connections.begin(); i != connections.end(); i++)
  {
    const Router::sConnection& connection = *i;

    stream << FileUtils::QuotedString(connection.label);
    stream << QStringLiteral(",%1").arg(static_cast<int>(connection.server ? 1 : 0));
    stream << QStringLiteral(",%1").arg(static_cast<int>(connection.frameMode));
    stream << QStringLiteral(",%1").arg(FileUtils::QuotedString(connection.addr.ip));
    stream << QStringLiteral(",%1").arg(connection.addr.port);
    stream << QLatin1Char('\n');
  }
}

////////////////////////////////////////////////////////////////////////////////

void ConfigFile::Save(QTextStream& stream, const Contents& contents)
{
  // Section order matches MainWindow::SaveToDevice.
  SaveSettings(stream, contents.settings);
  SaveRoutes(stream, contents.routes, contents.itemStateTable);
  SaveConnections(stream, contents.connections);
}

////////////////////////////////////////////////////////////////////////////////

bool ConfigFile::SaveFile(const QString& path, const Contents& contents)
{
  QDir().mkpath(QFileInfo(path).absolutePath());

  QFile file(path);
  if (!file.open(QFile::WriteOnly | QFile::Truncate))
    return false;

  QTextStream stream(&file);
  stream.setEncoding(QStringConverter::Utf8);
  Save(stream, contents);
  return true;
}

////////////////////////////////////////////////////////////////////////////////
