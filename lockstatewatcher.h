#ifndef LOCKSTATEWATCHER_H
#define LOCKSTATEWATCHER_H

#include <QObject>
#include <QDateTime>
#include <deque>
#include <optional>
#include <memory>
#include <functional>
#include <vector>
#include <QString>
#include "settings.h"
#include "types.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

/// Tri-state result of a lock-state query.
/// Unknown is returned when the query fails; the debounce buffer is not updated.
enum class LockQueryResult { Locked, Unlocked, Unknown };

class LockStateWatcher : public QObject
{
	Q_OBJECT

#ifdef Q_OS_LINUX
public:
	/// One Linux lock-query backend: a diagnostic name and the operation that
	/// performs the real per-session lock-state query (not merely a manager/
	/// service presence probe) and returns Unknown on failure. Public so
	/// debug-build tests can inject a fake chain (D3, R12-001).
	struct LinuxLockBackend {
		QString name;
		std::function<LockQueryResult()> query;
	};
#endif

private:
	const Settings & settings_;
	/// Wall-clock instant of the most recent Lock, captured via now_provider_.
	/// Recorded here as the injectable wall-clock lock-time source (the A2 seam).
	QDateTime lock_time_;
	/// Injectable lock-state probe. Defaults to isSessionLocked(); tests stub it
	/// to script lock/unlock samples without touching DBus/WTS. Takes the same
	/// civil `now` update() captured, so a query implementation that needs a
	/// timestamp (the Linux backoff window) never re-reads the clock itself.
	std::function<LockQueryResult(const QDateTime &)> query_provider_;
	/// Injectable wall-clock source. Defaults to QDateTime::currentDateTime.
	std::function<QDateTime()> now_provider_;
	std::deque<LockQueryResult> lock_state_buffer_;
	/// Debounced lock level. Flips to true (Locked) only when the last 3 samples
	/// are all Locked, and back to false when the last 3 are all Unlocked; the
	/// flip is what emits Lock/Unlock. Starts Unlocked to match the initial buffer.
	bool debounced_locked_ = false;

#ifdef Q_OS_LINUX
	// Ordered candidate backends; built once in the constructor. Selection
	// is driven entirely by a successful real query (queryLinuxBackends()),
	// never by a cheap manager/service presence probe (R12-001).
	std::vector<LinuxLockBackend> linux_backends_;
	/// Index into linux_backends_ currently trusted, if any. Queried directly
	/// (no chain scan) every tick while it keeps answering.
	std::optional<size_t> linux_active_backend_;
	/// Bounded exponential backoff between full-chain scans. 0 means "not
	/// currently rate-limited" -- the steady state once a trusted backend is
	/// answering directly. A failed scan (one that finds no working backend)
	/// always grows this and reschedules linux_next_reprobe_at_ (round-3
	/// review M2: a flapping fallback must not turn back-to-back scans into
	/// a per-tick synchronous storm). It is reset to the floor -- not left
	/// at whatever it had climbed to -- the moment a scan DOES find a
	/// working backend, and reset to 0 once that backend goes on to answer
	/// directly without needing another scan (round-4 review F1: otherwise
	/// a backend that recovers after a long outage, then blips once right
	/// after, would be re-armed at the accumulated maximum and go unqueried
	/// for up to the full ~30s cap instead of the ~1s floor).
	int linux_backoff_ms_ = 0;
	static constexpr int kLinuxInitialBackoffMs = 1000;
	static constexpr int kLinuxMaxBackoffMs = 30000;
	/// Wall-clock instant at which the next full-chain scan is allowed while
	/// backing off. Only meaningful when linux_backoff_ms_ > 0.
	QDateTime linux_next_reprobe_at_;
	/// Rate-limits the "all backends failed" diagnostic to once per episode.
	bool linux_all_failed_logged_ = false;
	/// Index last announced via the "Using 'X'" log, so re-selecting the same
	/// backend on a later scan does not repeat the message every tick
	/// (round-3 review M2).
	std::optional<size_t> linux_last_logged_backend_;
	/// Index last announced via the "Backend 'X' query failed" diagnostic
	/// (round-4 review F3), so a backend that keeps failing without an
	/// intervening recovery cannot repeat that message either. Cleared on
	/// direct-success recovery, mirroring linux_all_failed_logged_.
	std::optional<size_t> linux_last_failure_logged_backend_;

	void buildLinuxBackends();
	/// Owns backend selection/failover/backoff as one operation (R12-001):
	/// query the active backend; on failure, run a full-chain scan (rate-
	/// limited by backoff, same as the "no active backend" case) that tries
	/// the remaining backends once. Always retains Unknown on failure --
	/// never synthesizes Unlock.
	LockQueryResult queryLinuxBackends(const QDateTime &now);
	/// Runs one rate-limiting-eligible full-chain scan, skipping excludeIndex
	/// (the backend that was just active and already failed this call, so it
	/// is not queried twice in the same tick). Always grows/reschedules the
	/// backoff window before querying, so repeated scans -- success or not --
	/// stay bounded.
	LockQueryResult scanLinuxBackends(const QDateTime &now, std::optional<size_t> excludeIndex);
	/// True once the backoff window has elapsed and a new scan may run.
	/// Guards against a backward wall-clock step stalling detection for the
	/// full stale window: if `now` is further in the past than the window
	/// itself, the window is treated as elapsed rather than waited out.
	bool linuxReprobeWindowElapsed(const QDateTime &now) const;
	LockQueryResult querySystemdLogind();
	LockQueryResult queryScreenSaverDBus(const QString &service, const QString &path, const QString &interface);
#endif

	LockQueryResult isSessionLocked(const QDateTime &now);
	/// Pure translation of a WTSINFOEX Level-1 SessionFlags value into the
	/// domain tri-state (R12-002). Only exact WTS_SESSIONSTATE_UNLOCK is
	/// Unlocked, only exact WTS_SESSIONSTATE_LOCK is Locked; any other level
	/// or flags value (including the documented WTS_SESSIONSTATE_UNKNOWN,
	/// other values, or a malformed response) is Unknown. Compiled
	/// unconditionally -- not behind Q_OS_WIN -- so it is unit-testable on
	/// Linux; production Windows code is the only caller.
	static LockQueryResult translateWtsSessionState(quint32 level, quint32 sessionFlags);
	LockEvent determineLockEvent(LockQueryResult query_result);
	bool shouldEmitLongLock(LockQueryResult latest, qint64 elapsedMs) const;

public:
	explicit LockStateWatcher(const Settings & settings, QObject *parent = nullptr);

#ifndef QT_NO_DEBUG
    // ---- Debug-build test probes ----
    /// Exposes the debounce logic for direct unit testing.
    LockEvent determineLockEvent_dbg(LockQueryResult result) { return determineLockEvent(result); }
    /// Exposes the long-lock maturation check for direct unit testing.
    bool shouldEmitLongLock_dbg(LockQueryResult latest, qint64 elapsedMs) const { return shouldEmitLongLock(latest, elapsedMs); }
    /// Stubs the lock-state probe so update() can be driven with scripted samples.
    void setQueryProvider_dbg(std::function<LockQueryResult(const QDateTime &)> provider) { query_provider_ = std::move(provider); }
    /// Injects a controllable wall-clock source for the lock-time recording.
    void setNowProvider_dbg(std::function<QDateTime()> provider) { now_provider_ = std::move(provider); }
    /// Returns the wall-clock instant recorded at the last Lock (invalid if none).
    QDateTime lockTime_dbg() const { return lock_time_; }
    /// Exposes the pure WTSINFOEX Level-1 SessionFlags -> tri-state mapping
    /// for direct unit testing on any platform (D3, R12-002).
    static LockQueryResult translateWtsSessionState_dbg(quint32 level, quint32 sessionFlags) {
        return translateWtsSessionState(level, sessionFlags);
    }
#ifdef Q_OS_LINUX
    /// Replaces the real DBus backend chain with a fake one and resets
    /// selection/backoff state, so failover/backoff can be driven without
    /// touching DBus (D3, R12-001).
    void setLinuxBackends_dbg(std::vector<LinuxLockBackend> backends) {
        linux_backends_ = std::move(backends);
        linux_active_backend_.reset();
        linux_backoff_ms_ = 0;
        linux_next_reprobe_at_ = QDateTime();
        linux_all_failed_logged_ = false;
        linux_last_logged_backend_.reset();
        linux_last_failure_logged_backend_.reset();
    }
    /// Drives the backend selection/failover/backoff operation directly.
    LockQueryResult queryLinuxBackends_dbg(const QDateTime &now) { return queryLinuxBackends(now); }
    /// True once a backend is currently trusted (selected).
    bool hasActiveLinuxBackend_dbg() const { return linux_active_backend_.has_value(); }
    /// Current accumulated backoff, in milliseconds (0 before any all-fail episode).
    int linuxBackoffMs_dbg() const { return linux_backoff_ms_; }
#endif
#endif

signals:
	void desktopLockEvent(LockEvent event, const QDateTime& lockTime);

public slots:
	void update();
};

#endif // LOCKSTATEWATCHER_H
