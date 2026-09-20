#include "test_selftest_aggregate.h"
#include <QtTest>
#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>

void SelfTestChildFailSuite::test_deliberateFailure()
{
    QVERIFY2(false, "deliberate self-test failure (D13d aggregate self-test)");
}

void SelfTestChildPassSuite::test_deliberatePass()
{
    QVERIFY(true);
}

void SelfTestChildWarnSuite::test_deliberateWarn()
{
    qWarning("deliberate self-test QWARN (D13d aggregate self-test)");
}

namespace {

struct ChildRun {
    int exitCode;
    QString output;
};

// Launches this same qtest binary with --self-test-child=<mode>, inheriting
// the environment (so QT_QPA_PLATFORM=offscreen carries through) but no
// other arguments -- main.cpp's own qExec(test) calls in child mode take no
// argv, so the flag never reaches QTest's own CLI parser.
ChildRun runSelfTestChild(const QString& mode)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process.start(QCoreApplication::applicationFilePath(),
                   { QStringLiteral("--self-test-child=%1").arg(mode) });
    const bool started = process.waitForStarted(5000);
    if (!started)
        return { -1, QStringLiteral("<failed to start child process>") };
    process.waitForFinished(20000);
    return { process.exitCode(), QString::fromUtf8(process.readAll()) };
}

} // namespace

void SelfTestAggregateTest::test_failingChildSuite_makesAggregateNonzero_andLaterSuiteStillRuns()
{
    const ChildRun run = runSelfTestChild(QStringLiteral("fail"));
    QVERIFY2(run.exitCode != 0, "a failing child suite must make the aggregate exit nonzero");
    QVERIFY2(run.output.contains("FAIL!"),
        qPrintable("the failure must actually be reported, not swallowed:\n" + run.output));
    // SelfTestChildPassSuite is launched after the failing suite -- its
    // header and a passing result must still be present, proving the
    // aggregate did not stop accounting for suites after the earlier failure.
    QVERIFY2(run.output.contains("SelfTestChildPassSuite"),
        qPrintable("a suite launched after a failing suite must still run and be accounted for:\n" + run.output));
}

void SelfTestAggregateTest::test_warnChildSuite_makesAggregateNonzero()
{
    const ChildRun run = runSelfTestChild(QStringLiteral("warn"));
    QVERIFY2(run.exitCode != 0, "a QWARN-emitting child suite must make the aggregate exit nonzero");
    QVERIFY2(run.output.contains("unexpected QWARN"),
        qPrintable("the QWARN must actually be counted, not swallowed:\n" + run.output));
}
