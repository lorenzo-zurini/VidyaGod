#ifndef GUIFORMAT_H
#define GUIFORMAT_H

#include <QDate>
#include <QDateTime>
#include <QString>

//Compact human-readable byte size for GUI surfaces — "—" for an unknown/negative count (the engine-side
//std::string HumanBytes in commonutils prints "?" instead). Was duplicated verbatim in DownloadManager
//and the IPFS tab.
inline QString HumanBytesQ(long long N)
{
    if (N < 0) return QStringLiteral("—");
    double B = double(N); const char * U[] = { "B", "KB", "MB", "GB", "TB" }; int I = 0;
    while (B >= 1024.0 && I < 4) { B /= 1024.0; ++I; }
    return QString::number(B, 'f', I == 0 ? 0 : 1) + " " + U[I];
}

//A button's or checkbox's text as given, with every & shown as one: Qt reads a lone & as a shortcut marker, so a
//game titled "Command & Conquer" (any text from a package or a friend) would lose it and gain an underline.
inline QString PlainLabel(QString S) { return S.replace('&', QStringLiteral("&&")); }

//When a game was last played, from a LASTRUN (ISO-8601): "today", "yesterday", "12 days ago", else the date ("3 Mar
//2026"). "" for none, or one that does not parse. The one phrasing of every surface that says it.
inline QString PlayedWhen(const QString &Iso)
{
    const QDateTime At = QDateTime::fromString(Iso, Qt::ISODate);
    if (!At.isValid()) return QString();
    const qint64 Days = At.toLocalTime().date().daysTo(QDate::currentDate());
    return Days <= 0 ? QStringLiteral("today") : Days == 1 ? QStringLiteral("yesterday")
         : Days < 30 ? QString("%1 days ago").arg(Days) : At.toLocalTime().date().toString("d MMM yyyy");
}

#endif // GUIFORMAT_H
