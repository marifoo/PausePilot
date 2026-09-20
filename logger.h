#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>

class Logger
{
	static bool s_log_to_file_;
	/// Set on the first open/rotate/write/flush failure and never cleared:
	/// file logging is off for the rest of the process once this is true.
	static bool s_disabled_;
	QFile *logfile_;
	const static int max_lines_ = 1024;
#ifndef QT_NO_DEBUG
	static bool s_force_write_failure_dbg_;
#endif

	Logger();
	void log(const QString & text);
	/// Emits the ONE diagnostic for the process's first logging failure
	/// (to stderr, never through Logger::Log or qWarning) and sets
	/// s_disabled_. No-op if a failure was already reported.
	void reportFailure(const char* what);

public:
	/// Capture the logging configuration Logger needs, as an immutable value,
	/// once at startup. Logger no longer holds a pointer to Settings, so its
	/// correctness does not depend on that object outliving log calls or its
	/// value not changing underneath.
	/// Pass false to disable all logging (safe to call from test cleanup).
	static void configure(bool logToFile);
	static void Log(const QString & text);
	/// Returns the absolute path to the log file (applicationDir/uTimer.log).
	/// This is a pure path computation — does not check if the file exists.
	static QString logFilePath();
#ifndef QT_NO_DEBUG
	/// Test seam: makes the next log()'s write+flush be treated as failed,
	/// without touching the real QFile. There is no behavioural route to a
	/// mid-run write/flush failure (a QTemporaryDir-backed file won't
	/// portably fail QFile::write/flush), so tests need this to exercise the
	/// fail-once-quietly-permanently path. Self-resetting once consumed.
	static void forceWriteFailure_dbg(bool fail);
#endif
	~Logger();
};

#endif // LOGGER_H
