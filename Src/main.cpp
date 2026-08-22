#include "MainWindow.h"
#include "Theme.h"
#include "TsFileDocument.h"

#include <QApplication>

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("TsFileViewer"));

#ifdef _WIN32
    // The CRT delivers argv in the active code page (GBK on zh-CN) but the
    // tsfile library opens paths as UTF-8. Rebuild argv as UTF-8 from the
    // wide command line; containers must outlive main() (static).
    static std::vector<std::string> g_utf8Argv;
    static std::vector<char*> g_utf8ArgvPtrs;
    {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv != nullptr)
        {
            g_utf8Argv.reserve(static_cast<size_t>(wargc));
            for (int i = 0; i < wargc; ++i)
            {
                const int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                                                    nullptr, 0, nullptr, nullptr);
                std::string s(static_cast<size_t>(len > 0 ? len : 1), '\0');
                if (len > 1)
                {
                    WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len,
                                        nullptr, nullptr);
                    s.resize(static_cast<size_t>(len - 1));
                }
                else
                {
                    s.clear();
                }
                g_utf8Argv.push_back(std::move(s));
            }
            LocalFree(wargv);
            g_utf8ArgvPtrs.reserve(g_utf8Argv.size());
            for (auto& a : g_utf8Argv)
            {
                g_utf8ArgvPtrs.push_back(a.data());
            }
            argc = wargc;
            argv = g_utf8ArgvPtrs.data();
        }
    }
#endif

    theme::apply(app);

    MainWindow window;
    window.show();
    if (argc >= 2)
    {
        window.openFile(QString::fromStdString(argv[1]));
    }
    return QApplication::exec();
}
