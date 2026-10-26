TARGET = PausePilot

HEADERS = \
   $$PWD/apppaths.h \
   $$PWD/contentwidget.h \
   $$PWD/dayvalidity.h \
   $$PWD/historyeligibility.h \
   $$PWD/sqlitesessionstore.h \
   $$PWD/healthmonitor.h \
   $$PWD/sessionstore.h \
   $$PWD/mainwin.h \
   $$PWD/shutdowncoordinator.h \
   $$PWD/timetracker.h \
   $$PWD/lockstatewatcher.h \
   $$PWD/settings.h \
   $$PWD/localtime.h \
   $$PWD/types.h \
   $$PWD/timeformat.h \
   $$PWD/splitdialog.h \
   $$PWD/logger.h \
   $$PWD/historydialog.h \
   $$PWD/historyeditsession.h \
   $$PWD/timeline.h \
   $$PWD/startuprecovery.h \
   $$PWD/lockdecision.h \
   $$PWD/unixsignalbridge.h

SOURCES = \
   $$PWD/contentwidget.cpp \
   $$PWD/sqlitesessionstore.cpp \
   $$PWD/healthmonitor.cpp \
   $$PWD/main.cpp \
   $$PWD/mainwin.cpp \
   $$PWD/shutdowncoordinator.cpp \
   $$PWD/timetracker.cpp \
   $$PWD/lockstatewatcher.cpp \
   $$PWD/splitdialog.cpp \
   $$PWD/historydialog.cpp \
   $$PWD/historyeditsession.cpp \
   $$PWD/settings.cpp \
   $$PWD/timeformat.cpp \
   $$PWD/logger.cpp \
   $$PWD/timeline.cpp \
   $$PWD/startuprecovery.cpp \
   $$PWD/unixsignalbridge.cpp

INCLUDEPATH = \
    $$PWD/.

TEMPLATE = app

CONFIG += qt c++17

QT += widgets sql

# Platform-specific configuration
win32 {
    LIBS += -lUser32
}

linux {
    QT += dbus
    OBJECTS_DIR = build
    MOC_DIR = build
    RCC_DIR = build
}

RESOURCES += \
    icon.qrc

DISTFILES += \
    README.md \
	QT-LICENSE.md

