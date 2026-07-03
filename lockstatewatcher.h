#ifndef LOCKSTATEWATCHER_H
#define LOCKSTATEWATCHER_H

#include <QObject>
#include <QElapsedTimer>
#include <QDateTime>
#include <deque>
#include <optional>
#include <memory>
#include <functional>
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

private:
	const Settings & settings_;
	QElapsedTimer lock_timer_;
	/// Wall-clock instant of the most recent Lock, captured via now_provider_.
	/// Recorded here as the injectable wall-clock lock-time source (the A2 seam).
	QDateTime lock_time_;
	/// Injectable lock-state probe. Defaults to isSessionLocked(); tests stub it
	/// to script lock/unlock samples without touching DBus/WTS.
	std::function<LockQueryResult()> query_provider_;
	/// Injectable wall-clock source. Defaults to QDateTime::currentDateTime.
	std::function<QDateTime()> now_provider_;
	std::deque<LockQueryResult> lock_state_buffer_;
	const std::deque<LockQueryResult> buffer_for_lock;
	const std::deque<LockQueryResult> buffer_for_unlock;

#ifdef Q_OS_LINUX
	enum class LinuxLockMethod {
		SystemdLogind,
		FreedesktopScreenSaver,
		GnomeScreenSaver,
		KdeScreenSaver
	};
	std::optional<LinuxLockMethod> linux_lock_method_; // set once in initializeLinuxLockDetection()
	bool initializeLinuxLockDetection();
	LockQueryResult querySystemdLogind();
	LockQueryResult queryScreenSaverDBus(const QString &service, const QString &path, const QString &interface);
#endif

	LockQueryResult isSessionLocked();
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
    void setQueryProvider_dbg(std::function<LockQueryResult()> provider) { query_provider_ = std::move(provider); }
    /// Injects a controllable wall-clock source for the lock-time recording.
    void setNowProvider_dbg(std::function<QDateTime()> provider) { now_provider_ = std::move(provider); }
    /// Returns the wall-clock instant recorded at the last Lock (invalid if none).
    QDateTime lockTime_dbg() const { return lock_time_; }
#endif

signals:
	void desktopLockEvent(LockEvent event);

public slots:
	void update();
};

#endif // LOCKSTATEWATCHER_H
