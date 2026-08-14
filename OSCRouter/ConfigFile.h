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
#ifndef CONFIG_FILE_H
#define CONFIG_FILE_H

#ifndef ROUTER_H
#include "Router.h"
#endif

class QTextStream;

////////////////////////////////////////////////////////////////////////////////

// Reading and writing of the ".osc.txt" file format, with no dependency on the
// Qt Widgets user interface.
//
// This logic previously lived inside TcpWidget, SettingsWidget and
// RoutingWidget in MainWindow.cpp. Those widgets now delegate here, so the
// desktop application and the headless daemon share one implementation of the
// file format.
//
// The format is line based, and each line is a comma separated list of
// optionally quoted fields. A record is identified by its field count and, for
// the "Settings" and "Mute" records, by a keyword in the first field:
//
//   Settings,<sACNIP>,<artNetIP>,<levelChangesOnly>,<script>,<otpIP>,<otpModule>...
//   Mute,<muteAllIncoming>,<muteAllOutgoing>
//   <label>,<srcIP>,<srcPort>,<srcPath>,<inMin>,<inMax>,
//     <dstIP>,<dstPort>,<dstPath>,<outMin>,<outMax>[,<script>,<srcMulticastIP>,
//     <srcProtocol>,<dstProtocol>,<enabled>,<unmuted>,<dstMulticastIP>]  (route)
//   <label>,<isServer>,<frameMode>,<ip>,<port>                          (TCP connection)
//
// Every parser ignores lines it does not recognise, because the original format
// is read in one pass per section over the same set of lines.

class FileUtils
{
public:
  static QString QuotedString(const QString& str);
  static void GetItemsFromQuotedString(const QString& str, QStringList& items);
};

////////////////////////////////////////////////////////////////////////////////

class ConfigFile
{
public:
  struct Contents
  {
    Router::ROUTES routes;
    Router::CONNECTIONS connections;
    Router::Settings settings;
    ItemStateTable itemStateTable;
  };

  // Per-record parsers, each appending to or updating the supplied output.
  static void LoadSettingsLine(const QString& line, Router::Settings& settings);
  static void LoadRouteLine(const QString& line, Router::ROUTES& routes, ItemStateTable& itemStateTable);
  static void LoadConnectionLine(const QString& line, Router::CONNECTIONS& connections);

  // Something about the configuration worth telling the user, found by
  // Diagnose. A route can be perfectly well formed and still never carry
  // anything, and the only sign of that is one line in the log, so these are
  // surfaced in the interface instead.
  struct Issue
  {
    enum class Level
    {
      kWarning,  // will run, but probably not as intended
      kError,    // will not run at all
    };

    Level level = Level::kWarning;
    int routeIndex = -1;  // -1 when it concerns the configuration as a whole
    QString message;
  };

  typedef std::vector<Issue> ISSUES;

  // Explains what the routing engine will refuse to run and why, plus the
  // configurations that are valid but rarely what someone meant.
  static ISSUES Diagnose(const Contents& contents);

  // Discards records that are artefacts of parsing rather than anything a
  // person wrote: a "Settings" line has five fields and so also parses as a TCP
  // connection record, which would then be written back out as
  // "Settings,0,2,0,0" and corrupt the file. Routes are left untouched even
  // when the engine cannot use them, because deleting half-finished work is
  // worse than carrying it; see the comment on the implementation.
  static void Validate(Contents& contents);

  // Whole-file helpers.
  static void LoadLines(const QStringList& lines, Contents& contents);
  static bool LoadFile(const QString& path, Contents& contents);
  static QStringList SplitLines(const QString& contents);

  // Serialisation. These take plain structs, so the caller is responsible for
  // having already gathered them (from widgets, or from the daemon's state).
  static void SaveSettings(QTextStream& stream, const Router::Settings& settings);
  static void SaveRoutes(QTextStream& stream, const Router::ROUTES& routes, const ItemStateTable& itemStateTable);
  static void SaveConnections(QTextStream& stream, const Router::CONNECTIONS& connections);
  static void Save(QTextStream& stream, const Contents& contents);
  static bool SaveFile(const QString& path, const Contents& contents);

  // Field helpers shared by the parsers and the user interface.
  static void StringToTransform(const QString& str, EosRouteDst::sTransform& transform);
  static void TransformToString(const EosRouteDst::sTransform& transform, QString& str);
  static Protocol SanitizeProtocol(int protocol);
};

////////////////////////////////////////////////////////////////////////////////

#endif
