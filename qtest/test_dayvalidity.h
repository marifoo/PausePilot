#ifndef TEST_DAYVALIDITY_H
#define TEST_DAYVALIDITY_H

#include <QObject>
#include "testcommon.h"

// D1 (R16-002/R08-004/R13-002/R04-004): primitive-level tests for
// DayValidity's overlap-against-a-requested-local-date extension.
class DayValidityTest : public QObject
{
    Q_OBJECT

private slots:
    // Driven by TestCommon::dayOverlapRows() -- see testcommon.h for which
    // other layers currently consume rows from this table.
    void test_overlapsLocalDate_data();
    void test_overlapsLocalDate();
    void test_isCrossMidnight_data();
    void test_isCrossMidnight();

    // Direct DST arithmetic checks: a local day must be computed via
    // timezone-aware QDateTime (never a fixed 24h/86400000ms constant), so a
    // spring-forward day is 23h and a fall-back day is 25h.
    void test_localDayEnd_berlin_spring_forward_is_23_hours();
    void test_localDayEnd_berlin_fall_back_is_25_hours();
    void test_localDayEnd_non_dst_zone_is_24_hours();
    void test_localDayStart_santiago_spring_forward_gap_normalizes_to_valid_instant();

    void test_clippedMsecInLocalDate_clips_to_day_bounds();
};

#endif // TEST_DAYVALIDITY_H
