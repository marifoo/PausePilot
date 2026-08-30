#include <QApplication>
#include <QtTest>
#include <atomic>
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
// no QWARN enforcement (the offscreen-QPA noise and CI test runs are Linux-only).
// ---------------------------------------------------------------------------

static std::atomic<int> g_unexpectedWarnings{0};

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

    TeeCtx teeCtx = { pipefd[0], realStdout };
    pthread_t tid;
    pthread_create(&tid, nullptr, teeThread, &teeCtx);
#endif // !_WIN32

    QApplication app(argc, argv);
    int status = 0;

    // Lambda to run a test suite and track failures
    auto runTest = [&](auto* test, const char* name) {
        qDebug() << "\n================" << name << "================";
        int result = QTest::qExec(test, argc, argv);
        delete test;
        if (result != 0) {
            status = result;
        }
        return result;
    };

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
#endif // !_WIN32
    return status;
}
