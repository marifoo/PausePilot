/**
 * LockStateWatcher - Cross-platform desktop lock detection.
 *
 * Polls the desktop lock state every 100ms (driven by main.cpp timer) and emits
 * lock events when state transitions are detected.
 *
 * Platform implementations:
 * - Windows: WtsApi32.dll WTSQuerySessionInformation for session lock state
 * - Linux: DBus queries with fallback chain (systemd-logind → Freedesktop → GNOME → KDE)
 *
 * Debouncing:
 * - Hysteresis on a debounced level tracked in a 5-sample buffer
 * - Flips to Locked when the last 3 samples are all Locked (emits Lock)
 * - Flips to Unlocked when the last 3 samples are all Unlocked (emits Unlock)
 * - Tolerates a single flaky sample and keeps Lock/Unlock strictly alternating
 *
 * Events emitted:
 * - LockEvent::Lock - Desktop just locked
 * - LockEvent::Unlock - Desktop just unlocked
 * - LockEvent::LongOngoingLock - Lock duration exceeded backpause threshold
 */

#include "lockstatewatcher.h"
#include <QDebug>
#include <QDateTime>
#include <algorithm>
#include "logger.h"

#ifdef Q_OS_WIN
#include <WtsApi32.h>

// RAII wrapper for Windows library handle to prevent leaks
struct ScopedLibrary {
    HMODULE handle;
    explicit ScopedLibrary(LPCWSTR name) : handle(LoadLibraryW(name)) {}
    ~ScopedLibrary() { if (handle) FreeLibrary(handle); }
    explicit operator bool() const { return handle != nullptr; }
    ScopedLibrary(const ScopedLibrary&) = delete;
    ScopedLibrary& operator=(const ScopedLibrary&) = delete;
};
#endif

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#endif

LockStateWatcher::LockStateWatcher(const Settings &settings, QObject *parent)
	: QObject(parent),
	settings_(settings)
{
	lock_state_buffer_ = {
		LockQueryResult::Unlocked, LockQueryResult::Unlocked, LockQueryResult::Unlocked,
		LockQueryResult::Unlocked, LockQueryResult::Unlocked };

	// Default seams: real platform probe and the real wall clock. Tests override
	// these via setQueryProvider_dbg()/setNowProvider_dbg(). The provider takes
	// the same civil `now` update() captured, so isSessionLocked() never has to
	// re-read the clock itself (round-3 review L7).
	query_provider_ = [this](const QDateTime &now) { return isSessionLocked(now); };
	now_provider_ = QDateTime::currentDateTime;

#ifdef Q_OS_LINUX
	buildLinuxBackends();
#endif

	Logger::Log("[LOCK] LockStateWatcher initialized, BufferSize = " + QString::number(lock_state_buffer_.size()));
}

/**
 * Pure translation of a WTSINFOEX Level-1 SessionFlags value into the domain
 * tri-state (R12-002). The documented values (wtsapi32.h) are duplicated as
 * literals -- rather than including WtsApi32.h -- so this mapping compiles
 * and is unit-testable on Linux too:
 *   WTS_SESSIONSTATE_LOCK    = 0
 *   WTS_SESSIONSTATE_UNLOCK  = 1
 *   WTS_SESSIONSTATE_UNKNOWN = 0xFFFFFFFF
 * Only exact LOCK/UNLOCK map to the corresponding tri-state value; UNKNOWN,
 * any other flags value, and any level other than 1 (a malformed response)
 * all remain Unknown.
 */
LockQueryResult LockStateWatcher::translateWtsSessionState(quint32 level, quint32 sessionFlags)
{
	constexpr quint32 kWtsSessionStateLock = 0;
	constexpr quint32 kWtsSessionStateUnlock = 1;

	if (level != 1)
		return LockQueryResult::Unknown;
	if (sessionFlags == kWtsSessionStateLock)
		return LockQueryResult::Locked;
	if (sessionFlags == kWtsSessionStateUnlock)
		return LockQueryResult::Unlocked;
	return LockQueryResult::Unknown;
}

/**
 * Platform-agnostic check for the current session lock state.
 *
 * Returns LockQueryResult::Unknown on any query failure; the debounce buffer
 * is left unchanged so a transient DBus or API error is not treated as an unlock.
 *
 * Windows: WTSQuerySessionInformation from WtsApi32.dll (loaded dynamically).
 * Linux: delegates to queryLinuxBackends()'s failover/backoff operation.
 * Other: always returns Unknown (unsupported platform).
 */
LockQueryResult LockStateWatcher::isSessionLocked(const QDateTime &now)
{
	Q_UNUSED(now); // only the Linux branch below needs it (backoff window)
#ifdef Q_OS_WIN
	typedef BOOL(PASCAL *WTSQuerySessionInformation)(HANDLE hServer, DWORD SessionId, WTS_INFO_CLASS WTSInfoClass, LPTSTR* ppBuffer, DWORD* pBytesReturned);
	typedef void(PASCAL *WTSFreeMemory)(PVOID pMemory);

	ScopedLibrary hLib(L"wtsapi32.dll");
	if (!hLib)
		return LockQueryResult::Unknown;

	auto pWTSQuerySessionInformation = reinterpret_cast<WTSQuerySessionInformation>(
		GetProcAddress(hLib.handle, "WTSQuerySessionInformationW"));
	if (!pWTSQuerySessionInformation)
		return LockQueryResult::Unknown;

	auto pWTSFreeMemory = reinterpret_cast<WTSFreeMemory>(
		GetProcAddress(hLib.handle, "WTSFreeMemory"));
	if (!pWTSFreeMemory)
		return LockQueryResult::Unknown;

	LPTSTR ppBuffer = nullptr;
	DWORD dwBytesReturned = 0;
	LockQueryResult result = LockQueryResult::Unknown;

	// Query OUR session, not the physical console's: under RDP they differ and
	// the console's lock state is meaningless for this user.
	if (pWTSQuerySessionInformation(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
	                                WTSSessionInfoEx, &ppBuffer, &dwBytesReturned)) {
		// Read SessionFlags only once the buffer is proven large enough to
		// hold a Level-1 WTSINFOEX, and only after confirming Level == 1 --
		// otherwise the union member being read may not be the one the
		// server actually populated (round-3 review L3; pre-D3 also checked
		// Level == 1 before touching the union).
		if (dwBytesReturned >= sizeof(WTSINFOEXW) && ppBuffer) {
			auto pInfo = reinterpret_cast<WTSINFOEXW*>(ppBuffer);
			if (pInfo->Level == 1) {
				result = translateWtsSessionState(static_cast<quint32>(pInfo->Level),
				                                   static_cast<quint32>(pInfo->Data.WTSInfoExLevel1.SessionFlags));
			}
		}
		pWTSFreeMemory(ppBuffer);
	}

	return result;
#elif defined(Q_OS_LINUX)
	return queryLinuxBackends(now);
#else
	return LockQueryResult::Unknown;
#endif
}

LockEvent LockStateWatcher::determineLockEvent(LockQueryResult query_result)
{
	// Unknown means the query failed; leave the buffer unchanged so a transient
	// failure is not mistaken for an unlock transition.
	if (query_result == LockQueryResult::Unknown)
		return LockEvent::None;

	lock_state_buffer_.push_back(query_result);
	lock_state_buffer_.pop_front();

	// Hysteresis on a debounced level: flip only when the three most recent
	// samples agree, and emit the event on the flip. A single flaky sample
	// during a transition no longer permanently misses the event, and because
	// each flip is gated on the current level, Lock/Unlock strictly alternate.
	auto lastThreeAll = [this](LockQueryResult v) {
		const size_t n = lock_state_buffer_.size();
		return lock_state_buffer_[n - 1] == v
		    && lock_state_buffer_[n - 2] == v
		    && lock_state_buffer_[n - 3] == v;
	};

	if (!debounced_locked_ && lastThreeAll(LockQueryResult::Locked)) {
		debounced_locked_ = true;
		return LockEvent::Lock;
	}
	if (debounced_locked_ && lastThreeAll(LockQueryResult::Unlocked)) {
		debounced_locked_ = false;
		return LockEvent::Unlock;
	}
	return LockEvent::None;
}

/**
 * Main polling method, called every 100ms by the application timer.
 *
 * Workflow:
 * 1. Query current lock state via platform-specific isSessionLocked()
 * 2. Feed state into debounce buffer via determineLockEvent()
 * 3. On Lock: Record wall-clock lock_time_ from the first observed Locked sample of the run (not the accepting tick),
 *    so a suspend between the first sample and the debounced flip does not move the lock instant to the resume time.
 *    Emit Lock event.
 * 4. On Unlock: Clear lock_time_, emit Unlock event
 * 5. While locked: Check if wall-clock duration (now - lock_time_) exceeded threshold → emit LongOngoingLock
 *
 * The wall-clock lock_time_ is cleared after emitting LongOngoingLock to prevent
 * repeated emissions during the same lock session.
 */
bool LockStateWatcher::shouldEmitLongLock(LockQueryResult latest, qint64 elapsedMs) const
{
	return latest != LockQueryResult::Unknown
	       && elapsedMs >= settings_.getBackpauseMsec();
}

void LockStateWatcher::update()
{
	// One civil `now` captured for this tick and threaded into the query
	// provider, rather than letting isSessionLocked() re-read the clock
	// itself (round-3 review L7).
	const QDateTime now = now_provider_();
	const LockQueryResult q = query_provider_(now);

	// Anchor the lock instant to the first Locked sample of the run. A suspend
	// between the first sample and the debounced flip must not move the lock
	// instant to the resume time. Unknown neither arms nor clears the anchor,
	// matching the debounce buffer treatment.
	if (q == LockQueryResult::Locked) {
		if (!first_locked_sample_.isValid())
			first_locked_sample_ = now;
	} else if (q == LockQueryResult::Unlocked) {
		first_locked_sample_ = QDateTime();
	}

	const LockEvent lock_event = determineLockEvent(q);

	if (lock_event == LockEvent::Lock) {
		Logger::Log("[LOCK] >> Lock determined");
		lock_time_ = first_locked_sample_;
		emit desktopLockEvent(LockEvent::Lock, lock_time_);
	}
	else if (lock_event == LockEvent::Unlock) {
		if (lock_time_.isValid())
			Logger::Log("[LOCK] Current Lock Duration = " + QString::number(lock_time_.msecsTo(now)) + "ms");
		lock_time_ = QDateTime();
		Logger::Log("[LOCK] Unlock determined <<");
		emit desktopLockEvent(LockEvent::Unlock, QDateTime());
	}

	// The maturation decision uses the wall-clock lock instant (lock_time_)
	// so it is not affected by system suspend events.
	if (lock_time_.isValid() && shouldEmitLongLock(q, lock_time_.msecsTo(now))) {
		Logger::Log("[LOCK] Current Lock Duration = " + QString::number(lock_time_.msecsTo(now)) + "ms");
		const QDateTime lockInstant = lock_time_;
		lock_time_ = QDateTime();
		Logger::Log("[LOCK] Ongoing Lock is long enough to be counted as a Pause");
		emit desktopLockEvent(LockEvent::LongOngoingLock, lockInstant);
	}
}

#ifdef Q_OS_LINUX

/**
 * Builds the ordered candidate backend list.
 *
 * Priority Order:
 * 1. systemd-logind: The modern standard. Most reliable on recent distros.
 * 2. freedesktop.ScreenSaver: The standard interface implemented by many DEs (XFCE, MATE, etc.).
 * 3. GNOME ScreenSaver: Legacy specific interface.
 * 4. KDE ScreenSaver: Legacy specific interface.
 *
 * This only builds the list; it performs no DBus I/O and selects nothing.
 * queryLinuxBackends() validates and selects a backend using its real query
 * (R12-001) -- a manager/service presence probe is not sufficient, since a
 * service can be registered while its actual per-session query fails.
 */
void LockStateWatcher::buildLinuxBackends()
{
	linux_backends_ = {
		{ "systemd-logind", [this]() { return querySystemdLogind(); } },
		{ "freedesktop.ScreenSaver", [this]() {
			return queryScreenSaverDBus("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver"); } },
		{ "GNOME ScreenSaver", [this]() {
			return queryScreenSaverDBus("org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver"); } },
		{ "KDE ScreenSaver", [this]() {
			return queryScreenSaverDBus("org.kde.screensaver", "/ScreenSaver", "org.kde.screensaver"); } },
	};
}

/**
 * True once a new full-chain scan is allowed.
 *
 * Normally that is simply `now >= linux_next_reprobe_at_`. But
 * linux_next_reprobe_at_ is a wall-clock instant, so a large backward NTP/
 * manual step could otherwise make `now` land before it and stall lock
 * detection for the whole (up to a hour+) gap -- far beyond the bounded
 * backoff cap. Detect that case by checking whether `now` is further in the
 * past than the window is wide: if so the window is stale relative to `now`
 * and is treated as elapsed rather than waited out (round-3 review L2).
 */
bool LockStateWatcher::linuxReprobeWindowElapsed(const QDateTime &now) const
{
	if (linux_backoff_ms_ <= 0)
		return true;
	if (now >= linux_next_reprobe_at_)
		return true;
	return now < linux_next_reprobe_at_.addMSecs(-linux_backoff_ms_);
}

/**
 * Owns backend selection and runtime failover as one operation (R12-001).
 *
 * - If a backend is currently trusted, query only it. Success keeps it
 *   trusted and clears any backoff -- this is the only path that resets
 *   linux_backoff_ms_ to 0, since it is the sole evidence of a stable,
 *   healthy backend that does not need rate-limited rescanning.
 * - Otherwise (no trusted backend, or the trusted one just failed above), a
 *   full-chain scan is attempted, but only once the backoff window has
 *   elapsed. This gate applies identically to both origins: a fallback that
 *   flaps (repeatedly selected via a scan, then fails) is rate-limited the
 *   same as a cold start with every backend down (round-3 review M2) -- the
 *   scan itself, not merely the "every backend still down" outcome, is what
 *   is rate-limited.
 */
LockQueryResult LockStateWatcher::queryLinuxBackends(const QDateTime &now)
{
	if (linux_backends_.empty())
		return LockQueryResult::Unknown;

	std::optional<size_t> failedActive;
	if (linux_active_backend_.has_value()) {
		const size_t active = *linux_active_backend_;
		const LockQueryResult r = linux_backends_[active].query();
		if (r != LockQueryResult::Unknown) {
			linux_backoff_ms_ = 0;
			linux_all_failed_logged_ = false;
			linux_last_failure_logged_backend_.reset(); // F3: recovered, allow the next failure to log again
			return r;
		}
		if (linux_last_failure_logged_backend_ != active) {
			Logger::Log("[LOCK] Backend '" + linux_backends_[active].name
				+ "' query failed; trying fallback backends");
			linux_last_failure_logged_backend_ = active;
		}
		linux_active_backend_.reset();
		failedActive = active;
	}

	if (!linuxReprobeWindowElapsed(now)) {
		// Backing off: skip the DBus round-trip entirely rather than
		// blocking the GUI heartbeat with a fresh scan on every tick.
		return LockQueryResult::Unknown;
	}

	return scanLinuxBackends(now, failedActive);
}

/**
 * Runs one full-chain scan (excluding excludeIndex, which was just queried
 * and failed this same tick -- round-3 review L1). Grows/reschedules the
 * backoff window first, before querying anything, so a scan that finds
 * nothing is rate-limited before the next attempt (round-3 review M2). A
 * scan that DOES find a working backend restarts the ladder at its floor
 * instead of keeping that grown value (round-4 review F1): recovering via a
 * scan is not evidence of a *stable* backend the way a direct active-backend
 * success is, but it also must not leave a long-idle backend blind for the
 * full accumulated backoff if it blips once right after recovering.
 */
LockQueryResult LockStateWatcher::scanLinuxBackends(const QDateTime &now, std::optional<size_t> excludeIndex)
{
	linux_backoff_ms_ = (linux_backoff_ms_ == 0)
		? kLinuxInitialBackoffMs
		: std::min(linux_backoff_ms_ * 2, kLinuxMaxBackoffMs);
	linux_next_reprobe_at_ = now.addMSecs(linux_backoff_ms_);

	for (size_t i = 0; i < linux_backends_.size(); ++i) {
		if (excludeIndex.has_value() && i == *excludeIndex)
			continue;
		const LockQueryResult r = linux_backends_[i].query();
		if (r != LockQueryResult::Unknown) {
			linux_active_backend_ = i;
			linux_all_failed_logged_ = false;
			// F1: a scan that finds a working backend restarts the backoff
			// ladder at its floor rather than keeping the accumulated
			// maximum -- otherwise a backend recovering after a long
			// outage that blips once right on this tick would leave the
			// window pinned near the ~30s cap and go unqueried that long.
			linux_backoff_ms_ = kLinuxInitialBackoffMs;
			linux_next_reprobe_at_ = now.addMSecs(linux_backoff_ms_);
			if (linux_last_logged_backend_ != i) {
				Logger::Log("[LOCK] Using '" + linux_backends_[i].name + "' for lock detection");
				linux_last_logged_backend_ = i;
			}
			return r;
		}
	}

	if (!linux_all_failed_logged_) {
		Logger::Log("[LOCK] WARNING: all lock-detection backends failed; retaining Unknown");
		linux_all_failed_logged_ = true;
	}
	return LockQueryResult::Unknown;
}

LockQueryResult LockStateWatcher::querySystemdLogind()
{
	QDBusMessage msg = QDBusMessage::createMethodCall(
		"org.freedesktop.login1",
		"/org/freedesktop/login1/session/auto",
		"org.freedesktop.DBus.Properties",
		"Get"
	);
	msg << "org.freedesktop.login1.Session" << "LockedHint";

	// Use short timeout (100ms) to avoid blocking GUI - local DBus calls should be fast
	QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 100);

	if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
		QVariant v = reply.arguments().first();
		if (v.canConvert<QDBusVariant>()) {
			return v.value<QDBusVariant>().variant().toBool()
			       ? LockQueryResult::Locked : LockQueryResult::Unlocked;
		}
	}
	return LockQueryResult::Unknown;
}

LockQueryResult LockStateWatcher::queryScreenSaverDBus(const QString &service, const QString &path, const QString &interface)
{
	QDBusMessage msg = QDBusMessage::createMethodCall(service, path, interface, "GetActive");

	// Use short timeout (100ms) to avoid blocking GUI
	QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 100);

	if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
		return reply.arguments().first().toBool()
		       ? LockQueryResult::Locked : LockQueryResult::Unlocked;
	}
	return LockQueryResult::Unknown;
}

#endif // Q_OS_LINUX
