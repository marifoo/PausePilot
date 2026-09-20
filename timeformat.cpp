#include "timeformat.h"
#include <QStringList>

qint64 convMinToMsec(const int& minutes)
{
    return (static_cast<qint64>(minutes) * 60000);
}

QString convMSecToTimeStr(const qint64& time)
{
    const bool negative = time < 0;
    const qint64 total_sec = (negative ? -time : time) / 1000;
    const qint64 hours = total_sec / 3600;
    const qint64 minutes = (total_sec % 3600) / 60;
    const qint64 seconds = total_sec % 60;
    return QString("%1%2:%3:%4")
            .arg(negative ? "-" : "")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
}

QString convMinAndSecToHourPctString(const int min, const int sec)
{
    return QString::number((min * 60 + sec) / 36).rightJustified(2, '0');
}

QString convTimeStrToDurationStr(const QString& time_str)
{
    const QStringList split = time_str.split(":");
    QString hours = QString::number(split[0].toInt());
    QString hour_frac = convMinAndSecToHourPctString(split[1].toInt(), split[2].toInt());
    return hours + "." + hour_frac;
}
