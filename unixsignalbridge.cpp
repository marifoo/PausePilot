#include "unixsignalbridge.h"

#ifdef Q_OS_LINUX

#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

UnixSignalBridge* UnixSignalBridge::active_instance_ = nullptr;
ssize_t (*UnixSignalBridge::write_syscall_)(int, const void*, size_t) = &::write;

UnixSignalBridge::UnixSignalBridge()
    : fds_{-1, -1}, term_installed_(false), int_installed_(false), hup_installed_(false)
{
}

UnixSignalBridge::~UnixSignalBridge()
{
    teardown();
}

#ifndef QT_NO_DEBUG
void UnixSignalBridge::setWriteSyscall_dbg(ssize_t (*fn)(int, const void*, size_t))
{
    write_syscall_ = fn ? fn : &::write;
}
#endif

// Async-signal-safe: no allocation, no Qt calls, only write(2)/errno. Retries
// only EINTR; EAGAIN means a byte is already queued for the Qt side to
// drain, which is a benign outcome, not an error to retry. Any other error
// leaves nothing safe to do from signal-handler context, so it is dropped.
UnixSignalBridge::WriteOutcome UnixSignalBridge::writeSignalByte(int fd, char value)
{
    const int saved_errno = errno;
    WriteOutcome outcome = WriteOutcome::Failed;
    for (;;) {
        const ssize_t n = write_syscall_(fd, &value, sizeof(value));
        if (n >= 0) { outcome = WriteOutcome::Written; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) { outcome = WriteOutcome::AlreadyPending; break; }
        break;
    }
    errno = saved_errno;
    return outcome;
}

void UnixSignalBridge::handleSignal(int signum)
{
    (void)signum;
    if (active_instance_)
        writeSignalByte(active_instance_->fds_[0], 1);
}

bool UnixSignalBridge::setup()
{
    active_instance_ = this;

    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_) != 0) {
        active_instance_ = nullptr;
        return false;
    }

    for (int fd : fds_) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            teardown();
            return false;
        }
        const int fdflags = ::fcntl(fd, F_GETFD, 0);
        if (fdflags == -1 || ::fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) == -1) {
            teardown();
            return false;
        }
    }

    struct sigaction sa;
    sa.sa_handler = &UnixSignalBridge::handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (::sigaction(SIGTERM, &sa, &old_term_) != 0) { teardown(); return false; }
    term_installed_ = true;
    if (::sigaction(SIGINT, &sa, &old_int_) != 0) { teardown(); return false; }
    int_installed_ = true;
    if (::sigaction(SIGHUP, &sa, &old_hup_) != 0) { teardown(); return false; }
    hup_installed_ = true;

    return true;
}

void UnixSignalBridge::teardown()
{
    // Restore handlers first: once sigaction() returns, no further delivery
    // of that signal reaches handleSignal(), so it is then safe to drop
    // this instance as the active target and close its descriptors.
    if (hup_installed_) { ::sigaction(SIGHUP, &old_hup_, nullptr); hup_installed_ = false; }
    if (int_installed_) { ::sigaction(SIGINT, &old_int_, nullptr); int_installed_ = false; }
    if (term_installed_) { ::sigaction(SIGTERM, &old_term_, nullptr); term_installed_ = false; }

    if (active_instance_ == this)
        active_instance_ = nullptr;

    if (fds_[0] != -1) { ::close(fds_[0]); fds_[0] = -1; }
    if (fds_[1] != -1) { ::close(fds_[1]); fds_[1] = -1; }
}

bool UnixSignalBridge::drainPending()
{
    if (fds_[1] == -1)
        return false;

    char buf[64];
    bool any = false;
    for (;;) {
        const ssize_t n = ::read(fds_[1], buf, sizeof(buf));
        if (n > 0) { any = true; continue; }
        if (n == 0) break;          // peer closed
        if (errno == EINTR) continue;
        break;                      // EAGAIN/EWOULDBLOCK: nothing more pending
    }
    return any;
}

#endif // Q_OS_LINUX
