#ifndef SETTINGS_H
#define SETTINGS_H

#include <QtGlobal>
#include <QSettings>
#include <QString>

class Settings
{
private:
	QSettings sfile_;
	int backpause_min_;
	bool autopause_enabled_;
	bool autostart_timing_;
	bool start_minimized_;
	bool start_pinned_to_top_;
	bool warning_nopause_;
	bool warning_activity_;
	int warning_nopause_min_;
	int pause_for_warning_nopause_min_;
	int warning_activity_min_;
	bool log_to_file_;
	int history_days_to_keep_;
	unsigned int boot_time_sec_;
	int checkpoint_interval_min_;
	QSettings::Status last_sync_status_ = QSettings::NoError;
	void readSettingsFile();
	void writeAndSync();

public:
	Settings(const QString filename);
	/// Returns the status from the most recent sync() call (NoError on success).
	QSettings::Status syncStatus() const { return last_sync_status_; }
	int getHistoryDays() const;
	bool isAutopauseEnabled() const;
	bool isAutostartTimingEnabled() const;
	bool isMinimizedStartEnabled() const;
	bool isPinnedStartEnabled() const;
	bool showNoPauseWarning() const;
	bool showTooMuchActivityWarning() const;
	bool logToFile() const;
	QString getAutopauseThresholdMinString() const;
	qint64 getBackpauseMsec() const;
	qint64 getNoPauseWarningPauseThresholdMsec() const;
	qint64 getNoPauseWarningActivityThresholdMsec() const;
	qint64 getExcessiveActivityThresholdMsec() const;
	unsigned int getBootTimeSec() const;
	qint64 getCheckpointIntervalMsec() const;
	/// Persists the new state, then applies it in-memory only if the write
	/// succeeded. Returns the resulting syncStatus() (NoError on success);
	/// on failure the setting keeps its prior value.
	QSettings::Status setAutopauseState(const bool autopause_enabled);
	QSettings::Status setPinToTopState(const bool pinned_to_top);
};

#endif // SETTINGS_H
