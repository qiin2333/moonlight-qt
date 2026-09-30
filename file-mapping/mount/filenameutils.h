#pragma once

#include <QString>

namespace FileMapping {

inline QString safeFileName(const QString& name, const QString& fallback, int maxLength)
{
    QString safe = name.trimmed();
    if (safe.isEmpty()) {
        safe = fallback;
    }

    static const QString invalidChars = QStringLiteral("\\/:*?\"<>|");
    for (int i = 0; i < safe.size(); ++i) {
        if (safe.at(i).unicode() < 32 || invalidChars.contains(safe.at(i))) {
            safe[i] = QLatin1Char('_');
        }
    }
    while (safe.endsWith(QLatin1Char('.')) || safe.endsWith(QLatin1Char(' '))) {
        safe.chop(1);
    }
    if (safe.isEmpty() || safe == QStringLiteral(".") || safe == QStringLiteral("..")) {
        safe = fallback;
    }
    return safe.left(maxLength);
}

} // namespace FileMapping
