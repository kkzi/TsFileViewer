// HTTPS support test: QtNetwork loads OpenSSL at runtime; print what it
// found. Run with the two 1_1-x64 DLLs next to the exe.
#include <QCoreApplication>
#include <QtNetwork>

#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    printf("buildSSL:          %d (%s)\n",
           QSslSocket::supportsSsl(),
           QSslSocket::sslLibraryBuildVersionString().toStdString().c_str());
    printf("runtime SSL:       %s\n",
           QSslSocket::sslLibraryVersionString().toStdString().c_str());
    QNetworkAccessManager nam;
    QNetworkReply* r = nam.get(QNetworkRequest(
        QUrl("https://api.github.com/repos/kkzi/TsFileViewer/releases/latest")));
    QObject::connect(r, &QNetworkReply::finished, [&]
    {
        printf("HTTPS GET:         %s\n",
               r->error() == QNetworkReply::NoError ? "OK" :
               r->errorString().toStdString().c_str());
        const QByteArray body = r->readAll();
        const int i = body.indexOf("tag_name");
        if (i >= 0)
        {
            printf("body tag_name:     %.30s\n", body.mid(i + 10).constData());
        }
        app.quit();
    });
    QTimer::singleShot(15000, &app, &QCoreApplication::quit);
    return app.exec();
}
