#pragma once
#include <QString>

namespace Csv {
inline QString field(QString value) {
    // Spreadsheet imports must treat user-controlled names as text, including formula-like names.
    const auto trimmed=value.trimmed();
    if(!trimmed.isEmpty() && QString("=+-@").contains(trimmed.front())) value.prepend(QChar(39));
    value.replace(QChar(34),QString(2,QChar(34)));
    return QChar(34)+value+QChar(34);
}
}
