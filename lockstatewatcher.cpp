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
	// these via setQueryProvider_dbg()/setNowProvider_dbg().
	query_provider_ = [this]() { return isSessionLocked(); };
	now_provider_ = QDateTime::currentDateTime;

#ifdef Q_OS_LINUX
	initializeLinuxLockDetection();
#endif

	Logger::Log("[LOCK] LockStateWatcher initialized, BufferSize = " + QString::number(lock_state_buffer_.size()));
}

/**
 * Platform-agnostic check for the current session lock state.
 *
 * Returns LockQueryResult::Unknown on any query failure; the debounce buffer
 * is left unchanged so a transient DBus or API error is not treated as an unlock.
 *
 * Windows: WTSQuerySessionInformation from WtsApi32.dll (loaded dynamically).
 * Linux: delegates to the DBus method selected during initialization.
 * Other: always returns Unknown (unsupported platform).
 */
LockQueryResult LockStateWatcher::isSessionLocked()
{
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
		if (dwBytesReturned > 0 && ppBuffer) {
			auto pInfo = reinterpret_cast<WTSINFOEXW*>(ppBuffer);
			if (pInfo->Level == 1) {
				result = (pInfo->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK)
				         ? LockQueryResult::Locked : LockQueryResult::Unlocked;
			}
		}
		pWTSFreeMemory(ppBuffer);
	}

	return result;
#elif defined(Q_OS_LINUX)
	if (!linux_lock_method_.has_value())
		return LockQueryResult::Unknown;
	switch (linux_lock_method_.value()) {
		case LinuxLockMethod::SystemdLogind:
			return querySystemdLogind();
		case LinuxLockMethod::FreedesktopScreenSaver:
			return queryScreenSaverDBus("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver");
		case LinuxLockMethod::GnomeScreenSaver:
			return queryScreenSaverDBus("org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver");
		case LinuxLockMethod::KdeScreenSaver:
			return queryScreenSaverDBus("org.kde.screensaver", "/ScreenSaver", "org.kde.screensaver");
	}
	Q_UNREACHABLE();
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
 * 3. On Lock: Record wall-clock lock_time_, emit Lock event
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
	const LockQueryResult q = query_provider_();
	const QDateTime now = now_provider_();
	const LockEvent lock_event = determineLockEvent(q);

	if (lock_event == LockEvent::Lock) {
		Logger::Log("[LOCK] >> Lock determined");
		lock_time_ = now;
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
 * Probes available DBus services to determine the best lock detection method.
 *
 * Priority Order:
 * 1. systemd-logind: The modern standard. Most reliable on recent distros.
 * 2. freedesktop.ScreenSaver: The standard interface implemented by many DEs (XFCE, MATE, etc.).
 * 3. GNOME ScreenSaver: Legacy specific interface.
 * 4. KDE ScreenSaver: Legacy specific interface.
 *
 * The first service found to be valid is selected and cached in linux_lock_method_.
 */
bool LockStateWatcher::initializeLinuxLockDetection()
{
	// Check services in order of preference

	// 1. systemd-logind (most reliable on systemd systems)
	QDBusInterface logindCheck(
		"org.freedesktop.login1",
		"/org/freedesktop/login1",
		"org.freedesktop.login1.Manager",
		QDBusConnection::systemBus()
	);
	if (logindCheck.isValid()) {
		linux_lock_method_ = LinuxLockMethod::SystemdLogind;
		Logger::Log("[LOCK] Using systemd-logind for lock detection");
		return true;
	}

	// 2. Freedesktop ScreenSaver
	QDBusInterface fdoCheck(
		"org.freedesktop.ScreenSaver",
		"/org/freedesktop/ScreenSaver",
		"org.freedesktop.ScreenSaver",
		QDBusConnection::sessionBus()
	);
	if (fdoCheck.isValid()) {
		linux_lock_method_ = LinuxLockMethod::FreedesktopScreenSaver;
		Logger::Log("[LOCK] Using freedesktop.ScreenSaver for lock detection");
		return true;
	}

	// 3. GNOME ScreenSaver
	QDBusInterface gnomeCheck(
		"org.gnome.ScreenSaver",
		"/org/gnome/ScreenSaver",
		"org.gnome.ScreenSaver",
		QDBusConnection::sessionBus()
	);
	if (gnomeCheck.isValid()) {
		linux_lock_method_ = LinuxLockMethod::GnomeScreenSaver;
		Logger::Log("[LOCK] Using GNOME ScreenSaver for lock detection");
		return true;
	}

	// 4. KDE ScreenSaver
	QDBusInterface kdeCheck(
		"org.kde.screensaver",
		"/ScreenSaver",
		"org.kde.screensaver",
		QDBusConnection::sessionBus()
	);
	if (kdeCheck.isValid()) {
		linux_lock_method_ = LinuxLockMethod::KdeScreenSaver;
		Logger::Log("[LOCK] Using KDE ScreenSaver for lock detection");
		return true;
	}

	Logger::Log("[LOCK] WARNING: No lock detection method available on this Linux system");
	return false;
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
