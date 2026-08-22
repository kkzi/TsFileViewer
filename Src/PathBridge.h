#pragma once

// The tsfile library (since the fork's UTF-8 open patch) converts UTF-8
// path bytes to wide chars and calls _wopen — paths pass through verbatim.
// The historical bridge here (8.3 short path / ACP transcoding) predates
// that patch and is now actively harmful: its ACP fallback handed the
// library GBK bytes, which the UTF-8->wide conversion rejects (code 28).

#include <QString>

namespace pathbridge
{
// POSIX and Windows alike: hand the library the UTF-8 path unchanged.
inline QString toLibPath(const QString& path) { return path; }
}  // namespace pathbridge
