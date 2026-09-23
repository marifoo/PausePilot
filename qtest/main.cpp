#include <QApplication>
#include <QtTest>
#include <atomic>
#include <csignal>
#include <cstring>
#include <string>
#ifndef _WIN32
#  include <pthread.h>
#  include <unistd.h>
#endif

// Forward declarations of all test classes
class LoggerTest;
class SettingsTest;
class HelpersTest;
class LockStateWatcherTest;
class CleanDurationsTest;
class TimerTest;
class DatabaseTest;
class IntegrationTest;
class HistoryDialogTest;
class HistoryEditSessionTest;
class HistoryCommitTest;
class ShutdownCoordinatorTest;
class HealthMonitorTest;
class TimelineTest;
class RenamesTest;
class PersistenceContractTest;
class ContractMatrixTest;
class DayValidityTest;
class SelfTestAggregateTest;

// Include test class headers
#include "test_logger.h"
#include "test_settings.h"
#include "test_helpers.h"
#include "test_lockstatewatcher.h"
#include "test_cleanduration.h"
#include "test_timer.h"
#include "test_database.h"
#include "test_integration.h"
#include "test_historydialog.h"
#include "test_historyeditsession.h"
#include "test_history_commit.h"
#include "test_shutdowncoordinator.h"
#include "test_healthmonitor.h"
#include "test_timeline.h"
#include "test_renames.h"
#include "test_persistence_contract.h"
#include "test_contract_matrix.h"
#include "test_dayvalidity.h"
#include "test_selftest_aggregate.h"

// ---------------------------------------------------------------------------
// QWARN counter: pipe-based stdout tee (POSIX/Linux only)
//
// QTest::qExec installs its own message handler and does not chain to the
// previously installed handler, so qInstallMessageHandler alone cannot observe
// warnings emitted during test execution.  Instead we redirect stdout through a
// pipe, tee every byte to the real stdout, and count every "QWARN" line.
//
// Any QWARN output now counts as unexpected — the tests are written to produce
// zero warnings (no widget realization under the offscreen QPA plugin).
//
// On Windows this entire mechanism is compiled out — no redirection, no thread,
// no QWARN enforcement (the offscreen-QPA noise and CI test runs are Linux-only);
// a NOTE banner prints to state that "QWARN enforcement is disabled" instead.
// ---------------------------------------------------------------------------

static std::atomic<int> g_unexpectedWarnings{0};

// ---------------------------------------------------------------------------
// D15 part 1: truncation detection.
//
// A Q_ASSERT (or any qFatal) inside a suite calls abort() and never returns
// to runTest()/runChildTest() -- the normal nonzero-status latch never runs,
// and (before this fix) the fully-buffered stdout pipe from the QWARN tee
// above discarded whatever QFATAL/FAIL! diagnostics had already been queued.
// A SIGABRT handler is the only place that reliably runs at the moment of
// abort: it reports how many suites had completed and which one was running,
// written directly to the real (pre-dup2) stdout fd so the message survives
// even if the tee thread dies mid-drain.
// ---------------------------------------------------------------------------

static int g_realStdoutFd = -1;
static std::atomic<int> g_suitesCompleted{0};
static std::atomic<int> g_expectedSuiteCount{19};
static const char* g_currentSuiteName = "<none>";

#ifndef _WIN32

static void abortDiagnosticHandler(int sig)
{
    char buf[256];
    const int n = snprintf(buf, sizeof(buf),
        "\nTRUNCATED: qtest aborted after %d of %d suite(s) completed "
        "(last started: %s) -- see QFATAL/FAIL! diagnostics above for cause\n",
        g_suitesCompleted.load(), g_expectedSuiteCount.load(), g_currentSuiteName);
    if (n > 0 && g_realStdoutFd >= 0)
        write(g_realStdoutFd, buf, static_cast<size_t>(n));

    // Restore the default handler and re-raise so the process still
    // terminates via SIGABRT (rc=134) exactly as it would have without this
    // handler -- we only add the diagnostic, not new exit semantics.
    signal(sig, SIG_DFL);
    raise(sig);
}

#endif // !_WIN32

#ifndef _WIN32

struct TeeCtx {
    int readFd;
    int realStdout;
};

// Reader thread: tees stdout pipe to real stdout and counts every QWARN line.
// Uses a residual line buffer so tokens split across read() boundaries are
// handled correctly — only complete '\n'-terminated lines are scanned.
static void* teeThread(void* arg) {
    TeeCtx* ctx = static_cast<TeeCtx*>(arg);
    char buf[4096];
    ssize_t n;
    std::string residual;
    while ((n = read(ctx->readFd, buf, sizeof(buf))) > 0) {
        // Always write to real stdout so output is preserved.
        write(ctx->realStdout, buf, static_cast<size_t>(n));
        // Append to residual and scan complete lines.
        residual.append(buf, static_cast<size_t>(n));
        size_t pos = 0;
        size_t nl;
        while ((nl = residual.find('\n', pos)) != std::string::npos) {
            std::string line = residual.substr(pos, nl - pos);
            if (line.find("QWARN") != std::string::npos)
                g_unexpectedWarnings.fetch_add(1);
            pos = nl + 1;
        }
        residual.erase(0, pos);
    }
    // Scan any trailing partial line after EOF.
    if (!residual.empty() && residual.find("QWARN") != std::string::npos)
        g_unexpectedWarnings.fetch_add(1);
    return nullptr;
}

#endif // !_WIN32

// Test runner main function
// Executes all test suites sequentially using QTest::qExec()
// Returns non-zero exit code if any test suite fails
int main(int argc, char *argv[])
{
    // Silence KDE frameworkintegration's "kf.i18n: Using an empty domain"
    // QWARN, emitted when its platform theme localizes QMessageBox standard
    // buttons via KLocalizedString on a KDE desktop.
    //
    // NOTE: this is a deliberate source-level silence of third-party noise —
    // nothing prints, the category is suppressed before Qt even formats a QWARN
    // line.  It is NOT a printed-QWARN allowlist.  The tee-based QWARN counter
    // below enforces zero printed QWARN lines; this rule keeps that count honest
    // by preventing environmental KDE noise from inflating it.
    QByteArray loggingRules = qgetenv("QT_LOGGING_RULES");
    if (!loggingRules.isEmpty())
        loggingRules.append(';');
    loggingRules.append("kf.i18n.warning=false");
    qputenv("QT_LOGGING_RULES", loggingRules);

#ifndef _WIN32
    // Redirect stdout through a pipe so the tee thread can count QWARN lines.
    int pipefd[2];
    pipe(pipefd);
    int realStdout = dup(STDOUT_FILENO);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    // D15 part 1: the redirected stdout above is a pipe, so the C library
    // defaults it to fully buffered. QTest's own PASS/FAIL/QFATAL diagnostics
    // are written through this same stdout FILE*; a Q_ASSERT's abort() never
    // flushes that buffer, so a fatal message queued just before the abort
    // was silently discarded. Line-buffering makes every diagnostic line hit
    // the pipe (and the tee thread) as soon as it is printed.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    g_realStdoutFd = realStdout;
    signal(SIGABRT, abortDiagnosticHandler);

    TeeCtx teeCtx = { pipefd[0], realStdout };
    pthread_t tid;
    pthread_create(&tid, nullptr, teeThread, &teeCtx);
#endif // !_WIN32

    QApplication app(argc, argv);
    int status = 0;

    // D13d: dedicated self-test child mode, used only by SelfTestAggregateTest
    // (test_selftest_aggregate.cpp) to verify this file's own pass/fail/QWARN
    // aggregation actually works. Never set during a normal run -- it is only
    // ever passed by SelfTestAggregateTest launching this binary as a child
    // process -- so it can never make the normal 19-suite run below execute a
    // deliberately failing or QWARN-emitting suite.
    const char* selfTestChildMode = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test-child=fail") == 0) selfTestChildMode = "fail";
        else if (std::strcmp(argv[i], "--self-test-child=warn") == 0) selfTestChildMode = "warn";
        else if (std::strcmp(argv[i], "--self-test-child=abort") == 0) selfTestChildMode = "abort";
    }

    // Lambda to run a test suite and track failures
    auto runTest = [&](auto* test, const char* name) {
        g_currentSuiteName = name;
        qDebug() << "\n================" << name << "================";
        int result = QTest::qExec(test, argc, argv);
        delete test;
        if (result != 0) {
            status = result;
        }
        g_suitesCompleted.fetch_add(1);
        return result;
    };

    // Same as runTest, but without forwarding argc/argv: QTest::qExec()
    // treats unrecognized arguments as test-function-name filters, and this
    // process's own argv carries the --self-test-child= flag, which is not a
    // test function.
    auto runChildTest = [&](auto* test, const char* name) {
        g_currentSuiteName = name;
        qDebug() << "\n================" << name << "================";
        int result = QTest::qExec(test);
        delete test;
        if (result != 0) {
            status = result;
        }
        g_suitesCompleted.fetch_add(1);
        return result;
    };

    if (selfTestChildMode) {
        if (std::strcmp(selfTestChildMode, "fail") == 0) {
            g_expectedSuiteCount = 2;
            runChildTest(new SelfTestChildFailSuite, "SelfTestChildFailSuite");
            runChildTest(new SelfTestChildPassSuite, "SelfTestChildPassSuite");
        } else if (std::strcmp(selfTestChildMode, "abort") == 0) {
            // D15 part 1: pins the truncation detector itself. The abort
            // suite never returns from qExec(), so g_suitesCompleted stays
            // at 1 and the SIGABRT handler above reports exactly that.
            g_expectedSuiteCount = 2;
            runChildTest(new SelfTestChildPassSuite, "SelfTestChildPassSuite");
            runChildTest(new SelfTestChildAbortSuite, "SelfTestChildAbortSuite");
        } else {
            g_expectedSuiteCount = 1;
            runChildTest(new SelfTestChildWarnSuite, "SelfTestChildWarnSuite");
        }
    } else {
        // Run test suites
        runTest(new LoggerTest, "LoggerTest");
        runTest(new SettingsTest, "SettingsTest");
        runTest(new HelpersTest, "HelpersTest");
        runTest(new LockStateWatcherTest, "LockStateWatcherTest");
        runTest(new CleanDurationsTest, "CleanDurationsTest");
        runTest(new TimerTest, "TimerTest");
        runTest(new DatabaseTest, "DatabaseTest");
        runTest(new IntegrationTest, "IntegrationTest");
        runTest(new HistoryDialogTest, "HistoryDialogTest");
        runTest(new HistoryEditSessionTest, "HistoryEditSessionTest");
        runTest(new HistoryCommitTest, "HistoryCommitTest");
        runTest(new ShutdownCoordinatorTest, "ShutdownCoordinatorTest");
        runTest(new HealthMonitorTest, "HealthMonitorTest");
        runTest(new TimelineTest, "TimelineTest");
        runTest(new RenamesTest, "RenamesTest");
        runTest(new PersistenceContractTest, "PersistenceContractTest");
        runTest(new ContractMatrixTest, "ContractMatrixTest");
        runTest(new DayValidityTest, "DayValidityTest");
        runTest(new SelfTestAggregateTest, "SelfTestAggregateTest");
    }

    // D15 part 1: defensive check that the suite loop above actually reached
    // its expected suite count. This is unreachable on a real abort -- the
    // process dies via SIGABRT before returning here; abortDiagnosticHandler
    // above is what reports that case -- it only catches a suite silently
    // dropped from the loop without aborting.
    if (g_suitesCompleted.load() != g_expectedSuiteCount.load()) {
        fprintf(stdout, "TRUNCATED: only %d of %d suite(s) completed\n",
                g_suitesCompleted.load(), g_expectedSuiteCount.load());
        if (status == 0) status = 1;
    }

    qDebug() << "\n================ All Tests Complete ================";

#ifndef _WIN32
    // Closing the pipe write end signals EOF to the tee thread.  We must NOT
    // close the read end until after pthread_join (the thread owns pipefd[0]).
    fflush(stdout);
    dup2(realStdout, STDOUT_FILENO);  // restores stdout; old fd (pipe-write) is closed
    pthread_join(tid, nullptr);       // wait for thread to drain remaining pipe data
    close(pipefd[0]);
    close(realStdout);

    if (g_unexpectedWarnings.load() > 0) {
        fprintf(stdout, "FAIL: %d unexpected QWARN message(s)\n", g_unexpectedWarnings.load());
        if (status == 0) status = 1;
    }
#else
    // Platform-specific banner for Windows: the stdout tee and abort diagnostics
    // are POSIX-only. See qtest/main.cpp:65-66 for the design rationale.
    fprintf(stdout, "NOTE: QWARN enforcement is disabled on this platform -- the stdout tee and abort diagnostics are POSIX-only; this exit status does not reflect warnings\n");
#endif // !_WIN32
    return status;
}
