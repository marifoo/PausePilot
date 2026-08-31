#ifndef UNIXSIGNALBRIDGE_H
#define UNIXSIGNALBRIDGE_H

#include <QtGlobal>

#ifdef Q_OS_LINUX

#include <signal.h>

/// Moves SIGTERM/SIGINT/SIGHUP delivery out of signal-handler context and
/// into the Qt event loop (D4, R15-003).
///
/// setup() creates a nonblocking, close-on-exec socketpair and installs
/// handlers for the three signals; the handler performs only an
/// async-signal-safe write of one byte to the write end. The Qt side polls
/// the read end (notifierFd()) with a QSocketNotifier and calls
/// drainPending() to coalesce however many bytes arrived into a single
/// "shutdown requested" decision.
///
/// Not copyable: only one bridge may be the active signal-handler target at
/// a time (the OS signal-handler table is process-global, and the handler
/// itself is a free function that reaches its bridge through a single
/// static pointer).
class UnixSignalBridge
{
public:
    /// Outcome of one async-signal-safe write attempt (D4 step 2).
    enum class WriteOutcome {
        Written,        // the byte was written.
        AlreadyPending, // EAGAIN: the peer has not drained an earlier byte yet.
        Failed,         // any other error; nothing safe to do about it here.
    };

    UnixSignalBridge();
    ~UnixSignalBridge();

    UnixSignalBridge(const UnixSignalBridge&) = delete;
    UnixSignalBridge& operator=(const UnixSignalBridge&) = delete;

    /// Creates the nonblocking, close-on-exec socketpair and installs the
    /// SIGTERM/SIGINT/SIGHUP handlers. On any failure, cleans up whatever
    /// was already created/installed (closed descriptors, restored
    /// handlers) and returns false; the caller is responsible for the D4
    /// step-5 diagnostic/continuation policy. Safe to call again after a
    /// failed attempt.
    bool setup();

    /// Restores every handler this setup() installed and closes both
    /// descriptors. Idempotent, and safe to call whether or not setup()
    /// was ever called or succeeded (including from the destructor).
    void teardown();

    /// The read end for a QSocketNotifier, or -1 if setup() has not
    /// succeeded (or teardown() has since run).
    int notifierFd() const { return fds_[1]; }

    /// Drains every byte currently queued on the read end. Returns true if
    /// at least one byte was read, so any number of signals delivered
    /// between two Qt event-loop turns collapse into one shutdown
    /// invocation (D4 step 3).
    bool drainPending();

#ifndef QT_NO_DEBUG
    // ---- Debug-build test probes ----
    /// Directly exercises the handler's write/errno/retry policy without a
    /// real signal, so the EINTR/EAGAIN decision and a full channel can be
    /// driven deterministically with real socketpair descriptors.
    static WriteOutcome writeSignalByte_dbg(int fd, char value) { return writeSignalByte(fd, value); }
    /// Substitutes the write(2) call used by writeSignalByte() so tests can
    /// script EINTR/EAGAIN sequences. Pass nullptr to restore ::write.
    static void setWriteSyscall_dbg(ssize_t (*fn)(int, const void*, size_t));
    /// The signal-handler (write) end, so tests can assert its flags and
    /// drive writeSignalByte_dbg() against the exact descriptor the real
    /// handler would use.
    int writeFd_dbg() const { return fds_[0]; }
#endif

private:
    static void handleSignal(int signum);
    static WriteOutcome writeSignalByte(int fd, char value);

    int fds_[2];
    struct sigaction old_term_;
    struct sigaction old_int_;
    struct sigaction old_hup_;
    bool term_installed_;
    bool int_installed_;
    bool hup_installed_;

    static UnixSignalBridge* active_instance_;
    static ssize_t (*write_syscall_)(int, const void*, size_t);
};

#endif // Q_OS_LINUX

#endif // UNIXSIGNALBRIDGE_H
