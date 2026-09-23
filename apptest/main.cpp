#include <QApplication>
#include <QtTest>
#include <atomic>
#include <string>
#ifndef _WIN32
#  include <unistd.h>
#  include <pthread.h>
#endif

// Application-shell composition test runner (A3). Narrow on purpose: it
// exercises MainWin composed with a real ShutdownCoordinator/TimeTracker,
// which qtest's own aggregate cannot do (qtest never links mainwin.cpp /
// contentwidget.cpp). B2 and C1 add their cases here; D13 later extends it
// to full composition coverage.
#include "test_appshell.h"

// ---------------------------------------------------------------------------
// QWARN counter, mirroring qtest/main.cpp's tee-based approach. Unlike
// qtest, this target constructs real top-level widgets (MainWin, its tray
// icon, modal QMessageBoxes) under the offscreen QPA plugin, which itself
// emits a couple of known, content-stable, benign warnings that have nothing
// to do with application correctness (no equivalent Qt logging category
// exists to silence them at the source, unlike the kf.i18n case in
// qtest/main.cpp). Those two exact substrings are allow-listed by content;
// any other QWARN still fails the run.
// ---------------------------------------------------------------------------

static std::atomic<int> g_unexpectedWarnings{0};

#ifndef _WIN32

static bool isAllowedOffscreenNoise(const std::string& line)
{
    return line.find("No such signal QPlatformNativeInterface::systemTrayWindowChanged") != std::string::npos
        || line.find("This plugin does not support propagateSizeHints()") != std::string::npos;
}

struct TeeCtx {
    int readFd;
    int realStdout;
};

static void* teeThread(void* arg)
{
    TeeCtx* ctx = static_cast<TeeCtx*>(arg);
    char buf[4096];
    ssize_t n;
    std::string residual;
    while ((n = read(ctx->readFd, buf, sizeof(buf))) > 0) {
        write(ctx->realStdout, buf, static_cast<size_t>(n));
        residual.append(buf, static_cast<size_t>(n));
        size_t pos = 0;
        size_t nl;
        while ((nl = residual.find('\n', pos)) != std::string::npos) {
            std::string line = residual.substr(pos, nl - pos);
            if (line.find("QWARN") != std::string::npos && !isAllowedOffscreenNoise(line))
                g_unexpectedWarnings.fetch_add(1);
            pos = nl + 1;
        }
        residual.erase(0, pos);
    }
    if (!residual.empty() && residual.find("QWARN") != std::string::npos && !isAllowedOffscreenNoise(residual))
        g_unexpectedWarnings.fetch_add(1);
    return nullptr;
}

#endif // !_WIN32

int main(int argc, char *argv[])
{
#ifndef _WIN32
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

    AppShellTest test;
    int status = QTest::qExec(&test, argc, argv);

#ifndef _WIN32
    fflush(stdout);
    dup2(realStdout, STDOUT_FILENO);
    pthread_join(tid, nullptr);
    close(pipefd[0]);
    close(realStdout);

    if (g_unexpectedWarnings.load() > 0) {
        fprintf(stdout, "FAIL: %d unexpected QWARN message(s)\n", g_unexpectedWarnings.load());
        if (status == 0) status = 1;
    }
#else
    // Platform-specific banner for Windows: the stdout tee is POSIX-only.
    fprintf(stdout, "NOTE: QWARN enforcement is disabled on this platform -- the stdout tee is POSIX-only; this exit status does not reflect warnings\n");
#endif // !_WIN32
    return status;
}
