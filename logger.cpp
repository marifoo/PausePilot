#include "logger.h"
#include "apppaths.h"
#include "settings.h"
#include <QDateTime>
#include <QFile>
#include <QTextStream>

const Settings* Logger::s_settings_ = nullptr;

void Logger::registerSettings(const Settings* s)
{
    s_settings_ = s;
}

Logger::Logger()
{
    logfile_ = new QFile();
    logfile_->setFileName(AppPaths::logFile());
    logfile_->open(QIODevice::ReadWrite | QIODevice::Append | QIODevice::Text);

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
        logfile_->resize(0);
        logfile_->seek(0);
        QTextStream out(logfile_);
        out.setCodec("UTF-8");
        for (const QString& l : lines) {
            out << l << "\n";
        }
        out.flush();
        logfile_->flush();
    }

    log("uTimer Startup >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
}

void Logger::Log(const QString &text)
{
    if (!s_settings_ || !s_settings_->logToFile()) return;
    static Logger instance;
    instance.log(text);
}

QString Logger::logFilePath()
{
    return AppPaths::logFile();
}

void Logger::log(const QString &text)
{
    const QString msg = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz: ") + text + "\n";
    QTextStream out(logfile_);
    out.setCodec("UTF-8");
    out << msg;
    out.flush();
    logfile_->flush();
}

Logger::~Logger()
{
    log("uTimer Shutdown <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");

    if (logfile_ != nullptr) {
        logfile_->flush();
        logfile_->close();
    }
}

