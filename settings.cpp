/**
 * Settings - Persistent configuration manager.
 *
 * Acts as the Source of Truth for user preferences.
 * Uses Qt's QSettings (IniFormat) for cross-platform file storage.
 *
 * Load/save separation:
 * - readSettingsFile() reads known keys and applies defaults/bounds.
 * - writeAndSync() writes only the known keys and calls sync(), recording the
 *   outcome in last_sync_status_. Unknown keys already in the file are preserved.
 * - The constructor normalizes missing or out-of-range values by writing them back,
 *   but does NOT call clear() before writing: unknown keys survive a round-trip.
 */

#include "settings.h"
#include "timeformat.h"
#include "logger.h"

namespace {
static const QString kKeyPressStart        = "uTimer/press_start_button_on_app_start";
static const QString kKeyAutopause         = "uTimer/autopause_enabled";
static const QString kKeyAutopauseThresh   = "uTimer/autopause_threshold_minutes";
static const QString kKeyStartMinimized    = "uTimer/start_minimized_to_tray";
static const QString kKeyStartPinned       = "uTimer/start_pinned_to_top";
static const QString kKeyWarnNoPause       = "uTimer/show_warning_when_not_30min_pause_after_6h_activity";
static const QString kKeyWarnActivity      = "uTimer/show_warning_after_9h45min_activity";
static const QString kKeyLogToFile         = "uTimer/debug_log_to_file";
static const QString kKeyHistoryDays       = "uTimer/history_days_to_keep";
static const QString kKeyBootTimeSec       = "uTimer/boot_time_seconds";
static const QString kKeyCheckpointMin     = "uTimer/checkpoint_interval_minutes";

/// Reads an int setting, falling back to default_val (with a Logger note,
/// no UI) if the key is present but not a valid integer. Range clamping is
/// left to the caller (qBound), unchanged from before.
int readIntChecked(QSettings& sfile, const QString& key, int default_val)
{
	if (!sfile.contains(key)) return default_val;
	const QVariant v = sfile.value(key);
	bool ok = false;
	const int val = v.toInt(&ok);
	if (!ok) {
		Logger::Log("Settings: malformed integer for " + key + " (\"" + v.toString() +
					"\"), using default " + QString::number(default_val));
		return default_val;
	}
	return val;
}

/// Same as readIntChecked() for unsigned settings.
unsigned int readUIntChecked(QSettings& sfile, const QString& key, unsigned int default_val)
{
	if (!sfile.contains(key)) return default_val;
	const QVariant v = sfile.value(key);
	bool ok = false;
	const unsigned int val = v.toUInt(&ok);
	if (!ok) {
		Logger::Log("Settings: malformed unsigned integer for " + key + " (\"" + v.toString() +
					"\"), using default " + QString::number(default_val));
		return default_val;
	}
	return val;
}

/// QVariant::toBool() has no ok-overload and silently treats any non-empty
/// string that isn't "0"/"false" as true, so a malformed value would flip a
/// default-false setting on instead of being rejected. Recognize only the
/// forms QSettings itself writes/reads and fall back otherwise.
bool readBoolChecked(QSettings& sfile, const QString& key, bool default_val)
{
	if (!sfile.contains(key)) return default_val;
	const QVariant v = sfile.value(key);
	const QString s = v.toString().trimmed().toLower();
	if (s == "true" || s == "1") return true;
	if (s == "false" || s == "0") return false;
	Logger::Log("Settings: malformed boolean for " + key + " (\"" + v.toString() +
				"\"), using default " + QString(default_val ? "true" : "false"));
	return default_val;
}
} // namespace

Settings::Settings(const QString filename) : sfile_(filename, QSettings::IniFormat)
{
	sfile_.setIniCodec("UTF-8");
	readSettingsFile();
	// Write back normalized values (defaults filled in, bounds applied).
	// No clear() so unknown keys in the file are preserved.
	writeAndSync();
}

void Settings::readSettingsFile()
{
	autostart_timing_ = readBoolChecked(sfile_, kKeyPressStart, true);
	autopause_enabled_ = readBoolChecked(sfile_, kKeyAutopause, true);
	backpause_min_ = qBound(0, readIntChecked(sfile_, kKeyAutopauseThresh, 15), 99);
	start_minimized_ = readBoolChecked(sfile_, kKeyStartMinimized, false);
	start_pinned_to_top_ = readBoolChecked(sfile_, kKeyStartPinned, false);
	warning_nopause_ = readBoolChecked(sfile_, kKeyWarnNoPause, false);
	warning_nopause_min_ = 6*60;
	pause_for_warning_nopause_min_ = 30;
	warning_activity_ = readBoolChecked(sfile_, kKeyWarnActivity, false);
	warning_activity_min_ = 9*60+45;
	history_days_to_keep_ = qMax(0, readIntChecked(sfile_, kKeyHistoryDays, 99));
	log_to_file_ = readBoolChecked(sfile_, kKeyLogToFile, false);
	boot_time_sec_ = qBound(0u, readUIntChecked(sfile_, kKeyBootTimeSec, 0u), 3600u);
	checkpoint_interval_min_ = qBound(0, readIntChecked(sfile_, kKeyCheckpointMin, 5), 60);
}

void Settings::writeAndSync()
{
	sfile_.setValue(kKeyPressStart, autostart_timing_);
	sfile_.setValue(kKeyAutopause, autopause_enabled_);
	sfile_.setValue(kKeyAutopauseThresh, backpause_min_);
	sfile_.setValue(kKeyStartMinimized, start_minimized_);
	sfile_.setValue(kKeyStartPinned, start_pinned_to_top_);
	sfile_.setValue(kKeyWarnNoPause, warning_nopause_);
	sfile_.setValue(kKeyWarnActivity, warning_activity_);
	sfile_.setValue(kKeyLogToFile, log_to_file_);
	sfile_.setValue(kKeyHistoryDays, history_days_to_keep_);
	sfile_.setValue(kKeyBootTimeSec, boot_time_sec_);
	sfile_.setValue(kKeyCheckpointMin, checkpoint_interval_min_);

	sfile_.sync();
	last_sync_status_ = sfile_.status();

	if (log_to_file_) {
		if (autopause_enabled_)
			Logger::Log("Autopause is enabled with Threshold = " + QString::number(backpause_min_) + "min");
		else
			Logger::Log("Autopause is disabled");
	}
}

int Settings::getHistoryDays() const
{
	return history_days_to_keep_;
}

bool Settings::isAutopauseEnabled() const
{
	return autopause_enabled_;
}

bool Settings::isAutostartTimingEnabled() const
{
	return autostart_timing_;
}

bool Settings::isMinimizedStartEnabled() const
{
	return start_minimized_;
}

bool Settings::isPinnedStartEnabled() const
{
	return start_pinned_to_top_;
}

bool Settings::showNoPauseWarning() const
{
	return warning_nopause_;
}

bool Settings::showTooMuchActivityWarning() const
{
	return warning_activity_;
}

bool Settings::logToFile() const
{
	return log_to_file_;
}

QString Settings::getAutopauseThresholdMinString() const
{
	return QString::number(backpause_min_);
}

qint64 Settings::getBackpauseMsec() const
{
	return convMinToMsec(backpause_min_);
}

qint64 Settings::getNoPauseWarningPauseThresholdMsec() const
{
	return convMinToMsec(pause_for_warning_nopause_min_);
}

qint64 Settings::getNoPauseWarningActivityThresholdMsec() const
{
	return convMinToMsec(warning_nopause_min_);
}

qint64 Settings::getExcessiveActivityThresholdMsec() const
{
	return convMinToMsec(warning_activity_min_);
}

QSettings::Status Settings::setAutopauseState(const bool autopause_enabled)
{
	const bool prev = autopause_enabled_;
	autopause_enabled_ = autopause_enabled;
	writeAndSync();
	if (last_sync_status_ != QSettings::NoError)
		autopause_enabled_ = prev;
	return last_sync_status_;
}

QSettings::Status Settings::setPinToTopState(const bool pinned_to_top)
{
	const bool prev = start_pinned_to_top_;
	start_pinned_to_top_ = pinned_to_top;
	writeAndSync();
	if (last_sync_status_ != QSettings::NoError)
		start_pinned_to_top_ = prev;
	return last_sync_status_;
}

unsigned int Settings::getBootTimeSec() const
{
	return boot_time_sec_;
}

qint64 Settings::getCheckpointIntervalMsec() const
{
	return convMinToMsec(checkpoint_interval_min_);
}
