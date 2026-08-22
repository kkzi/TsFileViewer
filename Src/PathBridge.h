#pragma once

// The upstream tsfile ReadFile::open() passes the path bytes straight to
// CRT ::open(). On POSIX that is UTF-8-clean already. On Windows the CRT
// interprets the bytes in the active code page (GBK on zh-CN), while our
// paths are UTF-8 (Qt + rebuilt argv), so non-ASCII paths fail with
// E_FILE_OPEN_ERR (28). The vendored TsFileCpp tree had a local
// utf8_file_open.h patch (_wopen) for this; the submodule is upstream-clean,
// so we bridge here on Windows only:
//
//   1. ASCII paths pass through untouched (zero-cost fast path).
//   2. Otherwise try the 8.3 short path (pure ASCII, safe in any ACP).
//   3. Otherwise transcode UTF-8 -> ACP bytes when losslessly possible.
//   4. Otherwise return the original path and let the library report its
//      own open error.

#include <QString>
#include <string>

#ifdef _WIN32
#include <qt_windows.h>
#else
namespace pathbridge
{
// POSIX: the library treats paths as raw bytes == UTF-8, nothing to do.
inline QString toLibPath(const QString& path) { return path; }
}  // namespace pathbridge
#endif

#ifdef _WIN32
namespace pathbridge
{
inline bool isAscii(const QString& s)
{
    for (const QChar c : s)
    {
        if (c > QChar(0x7F))
        {
            return false;
        }
    }
    return true;
}

inline QString toLibPath(const QString& utf8Path)
{
    if (utf8Path.isEmpty() || isAscii(utf8Path))
    {
        return utf8Path;  // ASCII path: CRT open handles it as-is
    }

    const std::wstring wide = utf8Path.toStdWString();

    // Try the 8.3 short path first. Volumes with 8.3 names disabled return
    // the long path unchanged (still non-ASCII), which the check below
    // rejects.
    const DWORD shortLen = GetShortPathNameW(wide.c_str(), nullptr, 0);
    if (shortLen > 0)
    {
        std::wstring shortPath(shortLen, L'\0');
        const DWORD written =
            GetShortPathNameW(wide.c_str(), &shortPath[0], shortLen);
        if (written > 0 && written < shortLen)
        {
            shortPath.resize(written);
            bool ascii = true;
            for (wchar_t c : shortPath)
            {
                if (c > 0x7F)
                {
                    ascii = false;
                    break;
                }
            }
            if (ascii)
            {
                return QString::fromStdWString(shortPath);
            }
        }
    }

    // Fallback: transcode to ACP bytes when losslessly possible.
    // WC_NO_BEST_FIT_CHARS makes the conversion fail instead of mapping
    // characters like '?' onto lookalikes; the lossy flag covers the rest.
    BOOL lossy = FALSE;
    const int acpLen =
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr,
                            &lossy);
    if (acpLen > 0 && !lossy)
    {
        std::string acpPath(static_cast<size_t>(acpLen), '\0');
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(),
                            static_cast<int>(wide.size()), &acpPath[0], acpLen,
                            nullptr, nullptr);
        return QString::fromStdString(acpPath);
    }

    return utf8Path;  // let the library surface its own error
}
}  // namespace pathbridge
#endif
