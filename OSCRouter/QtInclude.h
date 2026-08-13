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
#ifndef QT_INCLUDE_H
#define QT_INCLUDE_H

////////////////////////////////////////////////////////////////////////////////

#define TEXT_COLOR QColor(220, 220, 220)
#define MUTED_COLOR QColor(100, 100, 100)
#define OFF_COLOR QColor(170, 124, 223)
#define SUCCESS_COLOR QColor(16, 183, 87)
#define ERROR_COLOR QColor(164, 66, 66)
#define WARNING_COLOR QColor(172, 122, 57)
#define RECV_COLOR QColor(255, 187, 255)
#define SEND_COLOR QColor(0, 181, 149)
#define CONNECT_COLOR QColor(105, 92, 152)
#define ACTIVITY_COLOR QColor(200, 200, 200)
#define BG_COLOR QColor(40, 40, 40)
#define DARK_BG_COLOR QColor(20, 20, 20)

#ifdef WIN32
#include <Winsock2.h>
#endif

#include <QtCore/QtCore>

// The headless daemon (oscrouterd) builds the routing engine without any user
// interface, so it links neither Qt Gui nor Qt Widgets. Nothing reachable from
// Router.h needs them; only the widgets in MainWindow.cpp and LogWidget.cpp do.
#ifndef OSCROUTER_HEADLESS
#include <QtGui/QtGui>
#include <QtWidgets/QtWidgets>
#endif

#include <QtNetwork/QtNetwork>
#include <QtQml/QJSEngine>

////////////////////////////////////////////////////////////////////////////////

#endif
