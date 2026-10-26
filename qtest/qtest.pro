TARGET = qtest
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

INCLUDEPATH += $$PWD/..

# Source-reading tests must also work from shadow builds and arbitrary CWDs.
PAUSEPILOT_SOURCE_DIR = $$absolute_path($$PWD/..)
DEFINES += PAUSEPILOT_SOURCE_DIR=$$shell_quote(\"$$PAUSEPILOT_SOURCE_DIR\")

SOURCES += \
    main.cpp \
    testcommon.cpp \
    fakesessionstore.cpp \
    test_logger.cpp \
    test_settings.cpp \
    test_helpers.cpp \
    test_lockstatewatcher.cpp \
    test_cleanduration.cpp \
    test_timer.cpp \
    test_database.cpp \
    test_integration.cpp \
    test_historydialog.cpp \
    test_historyeditsession.cpp \
    test_history_commit.cpp \
    test_shutdowncoordinator.cpp \
    test_healthmonitor.cpp \
    test_timeline.cpp \
    test_renames.cpp \
    test_persistence_contract.cpp \
    test_contract_matrix.cpp \
    test_dayvalidity.cpp \
    test_selftest_aggregate.cpp \
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
    $$PWD/../timeline.cpp

HEADERS += \
    testcommon.h \
    fakeclock.h \
    fakesessionstore.h \
    test_logger.h \
    test_settings.h \
    test_helpers.h \
    test_lockstatewatcher.h \
    test_cleanduration.h \
    test_timer.h \
    test_database.h \
    test_integration.h \
    test_historydialog.h \
    test_historyeditsession.h \
    test_history_commit.h \
    test_shutdowncoordinator.h \
    test_healthmonitor.h \
    test_timeline.h \
    test_renames.h \
    test_persistence_contract.h \
    test_contract_matrix.h \
    test_dayvalidity.h \
    test_selftest_aggregate.h \
    $$PWD/../apppaths.h \
    $$PWD/../dayvalidity.h \
    $$PWD/../historyeligibility.h \
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
    $$PWD/../timeline.h
