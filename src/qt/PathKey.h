#pragma once
#include "platform/Platform.h"
#include <QString>

// The key under which two spellings of a path name the same file: case-folded where the file system ignores
// case, so Linux keeps "Clip.mp4" and "clip.mp4" apart while Windows and macOS treat them as one.
inline QString pathKey(const QString& path) {
    static const auto matching=Platform::FileNameMatching();
    if(matching==Platform::NameMatching::Exact) return path;
    if(matching==Platform::NameMatching::IgnoreCaseAndAccentForm) return path.normalized(QString::NormalizationForm_C).toCaseFolded();
    return path.toCaseFolded();
}
inline bool samePath(const QString& a,const QString& b) { return pathKey(a)==pathKey(b); }
