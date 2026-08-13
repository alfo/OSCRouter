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

// oscrouterd -- OSCRouter without a user interface.
//
// Runs the same routing engine as the desktop application and exposes it over
// HTTP so it can be driven from a browser. This is what the container image
// runs, and what the Home Assistant add-on wraps.

#include <csignal>
#include <cstdio>
#include <unistd.h>

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QSocketNotifier>
#include <QtNetwork/QHostAddress>

#include "EosTimer.h"
#include "HttpServer.h"
#include "RouterController.h"
#include "Version.h"

////////////////////////////////////////////////////////////////////////////////

namespace
{
// Self-pipe, so a signal can be turned into an ordinary Qt event. Docker and
// the Home Assistant supervisor both stop the add-on with SIGTERM, and the
// routing engine has to shut its sockets down cleanly when that happens.
int g_SignalFds[2] = {-1, -1};

void OnPosixSignal(int)
{
  const char byte = 1;
  const ssize_t written = ::write(g_SignalFds[0], &byte, sizeof(byte));
  Q_UNUSED(written);
}

bool InstallSignalHandler(QObject* parent, RouterController& controller)
{
  if (0 != ::pipe(g_SignalFds))
    return false;

  QSocketNotifier* notifier = new QSocketNotifier(g_SignalFds[1], QSocketNotifier::Read, parent);
  QObject::connect(notifier, &QSocketNotifier::activated, parent, [&controller, notifier]() {
    notifier->setEnabled(false);

    char byte = 0;
    const ssize_t bytesRead = ::read(g_SignalFds[1], &byte, sizeof(byte));
    Q_UNUSED(bytesRead);

    printf("shutting down\n");
    fflush(stdout);

    controller.Stop();
    QCoreApplication::quit();
  });

  ::signal(SIGINT, OnPosixSignal);
  ::signal(SIGTERM, OnPosixSignal);
  // Sending on a closed TCP connection must not take the process down.
  ::signal(SIGPIPE, SIG_IGN);

  return true;
}
}  // namespace

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char* argv[])
{
  EosTimer::Init();

  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("oscrouterd"));
  QCoreApplication::setApplicationVersion(QStringLiteral("%1.%2.%3").arg(OSCROUTER_VERSION_MAJOR).arg(OSCROUTER_VERSION_MINOR).arg(OSCROUTER_VERSION_PATCH));

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("OSCRouter routing engine with a web interface."));
  parser.addHelpOption();
  parser.addVersionOption();

  const QCommandLineOption configOption(QStringList() << "c" << "config", QStringLiteral("Routing configuration file to load and save."), QStringLiteral("path"),
                                        QStringLiteral("/config/oscrouter.osc.txt"));
  const QCommandLineOption portOption(QStringList() << "p" << "port", QStringLiteral("Port for the web interface."), QStringLiteral("port"), QStringLiteral("8099"));
  const QCommandLineOption bindOption(QStringLiteral("bind"), QStringLiteral("Address to listen on."), QStringLiteral("address"), QStringLiteral("0.0.0.0"));
  const QCommandLineOption allowOption(QStringLiteral("allow"), QStringLiteral("Only accept connections from this address, e.g. the Home Assistant ingress proxy."),
                                       QStringLiteral("address"));
  const QCommandLineOption reconnectOption(QStringLiteral("reconnect-delay"), QStringLiteral("Milliseconds to wait before retrying a failed connection."), QStringLiteral("ms"),
                                           QStringLiteral("5000"));
  const QCommandLineOption noStartOption(QStringLiteral("no-start"), QStringLiteral("Load the configuration but do not start routing until asked to."));
  const QCommandLineOption logPacketsOption(QStringLiteral("log-packets"), QStringLiteral("Also echo per-packet traffic to stdout. Noisy; the web interface shows it either way."));
  const QCommandLineOption quietOption(QStringLiteral("quiet"), QStringLiteral("Do not echo the routing log to stdout."));
  const QCommandLineOption directPortOption(QStringLiteral("direct-port"),
                                            QStringLiteral("Additionally serve the interface on this port with no peer restriction. 0 disables it."),
                                            QStringLiteral("port"), QStringLiteral("0"));

  parser.addOption(configOption);
  parser.addOption(portOption);
  parser.addOption(bindOption);
  parser.addOption(allowOption);
  parser.addOption(reconnectOption);
  parser.addOption(noStartOption);
  parser.addOption(directPortOption);
  parser.addOption(logPacketsOption);
  parser.addOption(quietOption);
  parser.process(app);

  const QString configPath = parser.value(configOption);
  const quint16 port = static_cast<quint16>(parser.value(portOption).toUShort());
  const QString bindAddress = parser.value(bindOption);
  const unsigned int reconnectDelay = parser.value(reconnectOption).toUInt();

  RouterController controller(configPath, reconnectDelay);
  controller.SetEchoLogToStdout(!parser.isSet(quietOption));
  controller.SetEchoPacketsToStdout(parser.isSet(logPacketsOption));

  if (!InstallSignalHandler(&app, controller))
    fprintf(stderr, "warning: unable to install signal handler, shutdown may not be clean\n");

  QString error;
  if (controller.LoadConfigFile(error))
  {
    printf("loaded configuration from %s\n", qPrintable(configPath));
  }
  else
  {
    // Not fatal: starting with nothing configured is normal on a first run, and
    // the web interface is how the user is expected to fix that.
    printf("starting with an empty configuration (%s)\n", qPrintable(error));
  }

  HttpServer server(controller);
  if (!parser.value(allowOption).isEmpty())
    server.SetAllowedPeer(parser.value(allowOption));

  if (!server.listen(QHostAddress(bindAddress), port))
  {
    fprintf(stderr, "error: unable to listen on %s:%u (%s)\n", qPrintable(bindAddress), port, qPrintable(server.errorString()));
    return 1;
  }

  printf("web interface listening on %s:%u\n", qPrintable(bindAddress), port);

  // A second, unrestricted listener. Under Home Assistant the main one is
  // reachable only by the ingress proxy, which is the right default; this is
  // for reaching the interface directly from elsewhere on the network.
  const quint16 directPort = static_cast<quint16>(parser.value(directPortOption).toUShort());
  HttpServer directServer(controller);
  if (directPort != 0)
  {
    if (directServer.listen(QHostAddress(bindAddress), directPort))
      printf("web interface also listening on %s:%u (unrestricted)\n", qPrintable(bindAddress), directPort);
    else
      fprintf(stderr, "warning: unable to listen on %s:%u (%s)\n", qPrintable(bindAddress), directPort, qPrintable(directServer.errorString()));
  }

  fflush(stdout);

  if (!parser.isSet(noStartOption))
    controller.Start();

  return app.exec();
}

////////////////////////////////////////////////////////////////////////////////
