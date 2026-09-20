#include "logger.h"
#include "apppaths.h"
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <cstdio>

bool Logger::s_log_to_file_ = false;
bool Logger::s_disabled_ = false;
#ifndef QT_NO_DEBUG
bool Logger::s_force_write_failure_dbg_ = false;
#endif

void Logger::configure(bool logToFile)
{
    s_log_to_file_ = logToFile;
}

Logger::Logger()
{
    logfile_ = new QFile();
    logfile_->setFileName(AppPaths::logFile());
    if (!logfile_->open(QIODevice::ReadWrite | QIODevice::Append | QIODevice::Text)) {
        reportFailure("open");
        return;
    }

    // Rotate on startup rather than in the destructor: the destructor never runs
    // on a crash or a Windows WM_ENDSESSION termination, which would otherwise
    // let the file grow unbounded across sessions that never exit cleanly.
    logfile_->seek(0);
    QTextStream in(logfile_);
    in.setCodec("UTF-8");
    QStringList lines;
    while (!in.atEnd()) {
        lines << in.readLine();
    }
    if (lines.size() > max_lines_) {
        lines = lines.mid(lines.size() - max_lines_);
        const QByteArray rotated = (lines.join("\n") + "\n").toUtf8();
        if (!logfile_->resize(0)) {
            reportFailure("rotate");
            return;
        }
        logfile_->seek(0);
        const qint64 written = logfile_->write(rotated);
        if (written != rotated.size() || !logfile_->flush()) {
            reportFailure("rotate");
            return;
        }
    }

    log("uTimer Startup >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
}

void Logger::Log(const QString &text)
{
    if (!s_log_to_file_ || s_disabled_) return;
    static Logger instance;
    instance.log(text);
}

QString Logger::logFilePath()
{
    return AppPaths::logFile();
}

void Logger::log(const QString &text)
{
    if (s_disabled_) return;

    const QString msg = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz: ") + text + "\n";
    const QByteArray bytes = msg.toUtf8();

#ifndef QT_NO_DEBUG
    if (s_force_write_failure_dbg_) {
        s_force_write_failure_dbg_ = false;
        reportFailure("write");
        return;
    }
#endif

    const qint64 written = logfile_->write(bytes);
    if (written != bytes.size() || !logfile_->flush()) {
        reportFailure(written != bytes.size() ? "write" : "flush");
        return;
    }
}

void Logger::reportFailure(const char* what)
{
    // Only the process's first logging failure is reported; every later one
    // is silent -- logging is already off, and re-reporting would just be
    // repeated stderr noise for the same underlying condition.
    if (s_disabled_) return;
    s_disabled_ = true;
    std::fprintf(stderr, "[Logger] disabling file logging after %s failure (%s)\n",
                 what, qPrintable(logfile_ ? logfile_->fileName() : QString()));
    std::fflush(stderr);
}

#ifndef QT_NO_DEBUG
void Logger::forceWriteFailure_dbg(bool fail)
{
    s_force_write_failure_dbg_ = fail;
}
#endif

Logger::~Logger()
{
    log("uTimer Shutdown <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");

    if (logfile_ != nullptr) {
        logfile_->flush();
        logfile_->close();
    }
}
