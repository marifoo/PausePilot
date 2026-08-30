#include "test_dayvalidity.h"
#include "dayvalidity.h"
#include <QDateTime>

namespace {

// RAII guard: restores TZ even if a QVERIFY/QCOMPARE assertion fires return early.
struct TzGuard {
    QByteArray orig;
    TzGuard() : orig(qgetenv("TZ")) {}
    ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
};

void setTz(const char* tz)
{
    qputenv("TZ", tz);
    tzset();
}

void addDayOverlapColumns()
{
    QTest::addColumn<QString>("tz");
    QTest::addColumn<int>("startY"); QTest::addColumn<int>("startMo"); QTest::addColumn<int>("startD");
    QTest::addColumn<int>("startH"); QTest::addColumn<int>("startMi"); QTest::addColumn<int>("startS");
    QTest::addColumn<int>("endY");   QTest::addColumn<int>("endMo");   QTest::addColumn<int>("endD");
    QTest::addColumn<int>("endH");   QTest::addColumn<int>("endMi");   QTest::addColumn<int>("endS");
    QTest::addColumn<int>("queryY"); QTest::addColumn<int>("queryMo"); QTest::addColumn<int>("queryD");
    QTest::addColumn<bool>("expectedOverlap");
    QTest::addColumn<bool>("expectedCrossDay");

    for (const auto& r : TestCommon::dayOverlapRows()) {
        QTest::newRow(r.name)
            << QString(r.tz)
            << r.startY << r.startMo << r.startD << r.startH << r.startMi << r.startS
            << r.endY   << r.endMo   << r.endD   << r.endH   << r.endMi   << r.endS
            << r.queryY << r.queryMo << r.queryD
            << r.expectedOverlap << r.expectedCrossDay;
    }
}

} // namespace

void DayValidityTest::test_overlapsLocalDate_data() { addDayOverlapColumns(); }

void DayValidityTest::test_overlapsLocalDate()
{
    QFETCH(QString, tz);
    QFETCH(int, startY); QFETCH(int, startMo); QFETCH(int, startD);
    QFETCH(int, startH); QFETCH(int, startMi); QFETCH(int, startS);
    QFETCH(int, endY);   QFETCH(int, endMo);   QFETCH(int, endD);
    QFETCH(int, endH);   QFETCH(int, endMi);   QFETCH(int, endS);
    QFETCH(int, queryY); QFETCH(int, queryMo); QFETCH(int, queryD);
    QFETCH(bool, expectedOverlap);

    TzGuard guard;
    setTz(tz.toUtf8().constData());

    const QDateTime start(QDate(startY, startMo, startD), QTime(startH, startMi, startS), Qt::LocalTime);
    const QDateTime end(QDate(endY, endMo, endD), QTime(endH, endMi, endS), Qt::LocalTime);
    const QDate queryDate(queryY, queryMo, queryD);

    QCOMPARE(DayValidity::overlapsLocalDate(start, end, queryDate), expectedOverlap);
}

void DayValidityTest::test_isCrossMidnight_data() { addDayOverlapColumns(); }

void DayValidityTest::test_isCrossMidnight()
{
    QFETCH(QString, tz);
    QFETCH(int, startY); QFETCH(int, startMo); QFETCH(int, startD);
    QFETCH(int, startH); QFETCH(int, startMi); QFETCH(int, startS);
    QFETCH(int, endY);   QFETCH(int, endMo);   QFETCH(int, endD);
    QFETCH(int, endH);   QFETCH(int, endMi);   QFETCH(int, endS);
    QFETCH(bool, expectedCrossDay);

    TzGuard guard;
    setTz(tz.toUtf8().constData());

    const QDateTime start(QDate(startY, startMo, startD), QTime(startH, startMi, startS), Qt::LocalTime);
    const QDateTime end(QDate(endY, endMo, endD), QTime(endH, endMi, endS), Qt::LocalTime);

    QCOMPARE(isCrossMidnight(start, end), expectedCrossDay);
}

void DayValidityTest::test_localDayEnd_berlin_spring_forward_is_23_hours()
{
    TzGuard guard;
    setTz("Europe/Berlin");
    const QDateTime dayStart = DayValidity::localDayStart(QDate(2025, 3, 30));
    const QDateTime dayEnd = DayValidity::localDayEnd(QDate(2025, 3, 30));
    QCOMPARE(dayStart.msecsTo(dayEnd), static_cast<qint64>(23) * 3600 * 1000);
}

void DayValidityTest::test_localDayEnd_berlin_fall_back_is_25_hours()
{
    TzGuard guard;
    setTz("Europe/Berlin");
    const QDateTime dayStart = DayValidity::localDayStart(QDate(2025, 10, 26));
    const QDateTime dayEnd = DayValidity::localDayEnd(QDate(2025, 10, 26));
    QCOMPARE(dayStart.msecsTo(dayEnd), static_cast<qint64>(25) * 3600 * 1000);
}

void DayValidityTest::test_localDayEnd_non_dst_zone_is_24_hours()
{
    TzGuard guard;
    setTz("Asia/Kolkata");
    const QDateTime dayStart = DayValidity::localDayStart(QDate(2025, 6, 15));
    const QDateTime dayEnd = DayValidity::localDayEnd(QDate(2025, 6, 15));
    QCOMPARE(dayStart.msecsTo(dayEnd), static_cast<qint64>(24) * 3600 * 1000);
}

void DayValidityTest::test_localDayStart_santiago_spring_forward_gap_normalizes_to_valid_instant()
{
    // D1 LOW-2: local midnight of 2025-09-07 does not exist in
    // America/Santiago -- clocks jump from 23:59:59 (Sep 6) straight to
    // 01:00:00 (Sep 7). localDayStart(2025-09-07) must still return a valid
    // QDateTime (the first valid instant of that date, 01:00 local) rather
    // than the invalid QDateTime a direct QDateTime(date, QTime(0,0),
    // Qt::LocalTime) construction produces on this Qt version -- an invalid
    // bound would silently corrupt overlapsLocalDate() for every query
    // against this date (invalid sorts as less than any valid QDateTime, so
    // it would act as an unbounded lower edge). 2025-09-07 is the day that
    // opens with the gap, so it is the *short* day: 23 hours from its first
    // valid instant to the following (unaffected) local midnight.
    TzGuard guard;
    setTz("America/Santiago");

    const QDateTime dayStart = DayValidity::localDayStart(QDate(2025, 9, 7));
    QVERIFY(dayStart.isValid());
    QCOMPARE(dayStart, QDateTime(QDate(2025, 9, 7), QTime(1, 0, 0), Qt::LocalTime));

    const QDateTime dayEnd = DayValidity::localDayEnd(QDate(2025, 9, 7));
    QVERIFY(dayEnd.isValid());
    QCOMPARE(dayEnd, QDateTime(QDate(2025, 9, 8), QTime(0, 0, 0), Qt::LocalTime));
    QCOMPARE(dayStart.msecsTo(dayEnd), static_cast<qint64>(23) * 3600 * 1000);
}

void DayValidityTest::test_clippedMsecInLocalDate_clips_to_day_bounds()
{
    TzGuard guard;
    setTz("UTC");
    // Interval spans one local midnight: 2025-06-15 23:00 -> 2025-06-16 01:00.
    const QDateTime start(QDate(2025, 6, 15), QTime(23, 0, 0), Qt::LocalTime);
    const QDateTime end(QDate(2025, 6, 16), QTime(1, 0, 0), Qt::LocalTime);

    // Portion falling on 2025-06-15: 23:00 -> 24:00 = 1h.
    QCOMPARE(DayValidity::clippedMsecInLocalDate(start, end, QDate(2025, 6, 15)),
             static_cast<qint64>(3600) * 1000);
    // Portion falling on 2025-06-16: 00:00 -> 01:00 = 1h.
    QCOMPARE(DayValidity::clippedMsecInLocalDate(start, end, QDate(2025, 6, 16)),
             static_cast<qint64>(3600) * 1000);
    // No portion falls on an unrelated day.
    QCOMPARE(DayValidity::clippedMsecInLocalDate(start, end, QDate(2025, 6, 17)),
             static_cast<qint64>(0));
}
