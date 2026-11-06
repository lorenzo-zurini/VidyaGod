#ifndef GUIFORMAT_H
#define GUIFORMAT_H

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

#endif // GUIFORMAT_H
