#ifndef TEST_HEALTHMONITOR_H
#define TEST_HEALTHMONITOR_H

#include <QObject>
#include "testcommon.h"

class HealthMonitorTest : public QObject
{
    Q_OBJECT

private slots:
    // Test J: fires exactly once per threshold per session
    void test_J_activity_warning_fires_once();

    // Test K: no-pause warning fires only when activity high AND pause low
    void test_K_nopause_warning_condition();

    // Test L: reset re-arms both warnings
    void test_L_reset_rearms_warnings();

    // Test M: warning re-fires per session when wired to Timer::stopped (A13)
    void test_M_warning_refires_per_session();
};

#endif // TEST_HEALTHMONITOR_H
