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
    $$PWD/../startuprecovery.cpp

HEADERS += \
    test_appshell.h \
    $$PWD/../qtest/testcommon.h \
    $$PWD/../qtest/fakeclock.h \
    $$PWD/../qtest/fakesessionstore.h \
    $$PWD/../apppaths.h \
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
    $$PWD/../types.h \
    $$PWD/../historydialog.h \
    $$PWD/../timeline.h \
    $$PWD/../startuprecovery.h
