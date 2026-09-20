#ifndef TEST_SELFTEST_AGGREGATE_H
#define TEST_SELFTEST_AGGREGATE_H

#include <QObject>

/// D13d: verifies that main.cpp's suite-aggregation logic (the runTest loop's
/// nonzero-status latch, and the tee thread's QWARN counter) actually does
/// what it claims. Launches this same qtest binary as a child process in a
/// dedicated self-test mode (see main.cpp's --self-test-child= handling)
/// that runs a deliberately failing suite and a deliberately QWARN-emitting
/// suite, and checks the child's exit code and output.
class SelfTestAggregateTest : public QObject
{
    Q_OBJECT

private slots:
    void test_failingChildSuite_makesAggregateNonzero_andLaterSuiteStillRuns();
    void test_warnChildSuite_makesAggregateNonzero();
    void test_abortChildSuite_reportsTruncation();
};

// --- Child-mode-only fixtures ------------------------------------------------
// Instantiated only by main.cpp's --self-test-child=<fail|warn> branch, never
// by the normal suite run, so they can never contribute a real failure or
// QWARN to an ordinary qtest run.

/// Deliberately fails one test, to exercise main()'s nonzero-status latch.
class SelfTestChildFailSuite : public QObject
{
    Q_OBJECT
private slots:
    void test_deliberateFailure();
};

/// Trivially passes. Its presence in the child's output proves a suite
/// launched after SelfTestChildFailSuite still ran and was accounted for.
class SelfTestChildPassSuite : public QObject
{
    Q_OBJECT
private slots:
    void test_deliberatePass();
};

/// Deliberately emits one QWARN (and otherwise passes), to exercise the tee
/// thread's QWARN counter independent of any QCOMPARE/QVERIFY failure.
class SelfTestChildWarnSuite : public QObject
{
    Q_OBJECT
private slots:
    void test_deliberateWarn();
};

/// Deliberately aborts (qFatal) mid-run, to exercise D15's truncation
/// detection: main.cpp's SIGABRT handler must report how many suites
/// completed before the abort, and the fatal diagnostic must survive the
/// now-line-buffered stdout pipe instead of being lost with the tee thread.
class SelfTestChildAbortSuite : public QObject
{
    Q_OBJECT
private slots:
    void test_deliberateAbort();
};

#endif // TEST_SELFTEST_AGGREGATE_H
