TARGET = apptest
TEMPLATE = app
CONFIG += qt console c++17 debug
QT += testlib widgets sql

# Platform-specific configuration
win32 {
    LIBS += -lUser32
}
linux {
    QT += dbus
}

INCLUDEPATH += $$PWD/.. $$PWD/../qtest

# Absolute path to the production binary. PausePilot.pro is apptest.pro's sibling
# directory, and qmake places a target's output directly under that target's
# own build directory, so the production binary lands one directory above
# apptest's own build directory ($$OUT_PWD) for both an in-tree build
# ($$OUT_PWD == $$PWD, so this is $$PWD/..) and a whole-project shadow build
# whose shadow tree mirrors the source tree's apptest/ subdirectory. An
# apptest-only shadow build (no sibling PausePilot build) correctly resolves to a
# path with no binary present; PAUSEPILOT_BIN lets the harness override it at
# test run time for a binary built elsewhere.
PAUSEPILOT_PRODUCTION_DIR = $$absolute_path($$OUT_PWD/..)
PAUSEPILOT_PRODUCTION_BIN = $$PAUSEPILOT_PRODUCTION_DIR/PausePilot
win32: PAUSEPILOT_PRODUCTION_BIN = $$PAUSEPILOT_PRODUCTION_DIR/PausePilot.exe
# shell_quote() is required so a checkout path containing a space still
# produces a single valid -D switch (a naive \" ... \" is stripped by qmake).
DEFINES += PAUSEPILOT_PRODUCTION_BINARY=$$shell_quote(\"$$PAUSEPILOT_PRODUCTION_BIN\")

SOURCES += \
    main.cpp \
    test_appshell.cpp \
    $$PWD/../qtest/testcommon.cpp \
    $$PWD/../qtest/fakesessionstore.cpp \
    $$PWD/../mainwin.cpp \
    $$PWD/../contentwidget.cpp \
    $$PWD/../healthmonitor.cpp \
    $$PWD/../splitdialog.cpp \
    $$PWD/../historydialog.cpp \
    $$PWD/../historyeditsession.cpp \
    $$PWD/../shutdowncoordinator.cpp \
    $$PWD/../timetracker.cpp \
    $$PWD/../lockstatewatcher.cpp \
    $$PWD/../sqlitesessionstore.cpp \
    $$PWD/../timeformat.cpp \
    $$PWD/../settings.cpp \
    $$PWD/../logger.cpp \
    $$PWD/../timeline.cpp \
    $$PWD/../startuprecovery.cpp \
    $$PWD/../unixsignalbridge.cpp

HEADERS += \
    test_appshell.h \
    $$PWD/../qtest/testcommon.h \
    $$PWD/../qtest/fakeclock.h \
    $$PWD/../qtest/fakesessionstore.h \
    $$PWD/../apppaths.h \
    $$PWD/../dayvalidity.h \
    $$PWD/../historyeligibility.h \
    $$PWD/../mainwin.h \
    $$PWD/../contentwidget.h \
    $$PWD/../splitdialog.h \
    $$PWD/../historyeditsession.h \
    $$PWD/../healthmonitor.h \
    $$PWD/../shutdowncoordinator.h \
    $$PWD/../timetracker.h \
    $$PWD/../lockstatewatcher.h \
    $$PWD/../sqlitesessionstore.h \
    $$PWD/../sessionstore.h \
    $$PWD/../timeformat.h \
    $$PWD/../settings.h \
    $$PWD/../logger.h \
    $$PWD/../localtime.h \
    $$PWD/../types.h \
    $$PWD/../historydialog.h \
    $$PWD/../timeline.h \
    $$PWD/../startuprecovery.h \
    $$PWD/../lockdecision.h \
    $$PWD/../unixsignalbridge.h
