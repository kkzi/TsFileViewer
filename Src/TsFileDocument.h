#pragma once

#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <memory>

// Data layer for the viewer. Owns a worker QThread; all tsfile reads
// (open/schema walk/queries) run there so the UI stays responsive.
// The reader objects are created and destroyed on the worker thread.

// Registered with qRegisterMetaType so results can cross threads via signals.
struct ParamInfo
{
    QString device;
    QString measurement;
    int dataType = 0;     // common::TSDataType
    int encoding = 0;     // common::TSEncoding
    int compression = 0;  // common::CompressionType
    QString key() const { return device + QLatin1Char('/') + measurement; }
};
Q_DECLARE_METATYPE(ParamInfo)

struct MetaInfo
{
    QString path;
    qint64 fileSize = 0;
    int deviceCount = 0;
    int paramCount = 0;  // total measurements across devices
    qint64 firstTs = 0;
    qint64 lastTs = 0;
    bool haveTimeRange = false;  // false until the first query filled it
    QStringList devices;
};
Q_DECLARE_METATYPE(MetaInfo)

struct SeriesData
{
    QString device;
    QString measurement;
    QVector<qint64> ts;    // us
    QVector<double> value;  // NaN where the value is not numeric
    bool numeric = true;    // false when the measurement holds text/boolean rows
    QString key() const { return device + QLatin1Char('/') + measurement; }
};
Q_DECLARE_METATYPE(SeriesData)

class TsFileDocument : public QObject
{
    Q_OBJECT
public:
    explicit TsFileDocument(QObject* parent = nullptr);
    ~TsFileDocument() override;

    // Async: emits opened(MetaInfo, QVector<ParamInfo>) or openFailed(QString).
    void openAsync(const QString& path);
    // Async: emits valuesReady(SeriesData) or queryFailed(QString). Queries
    // queue on the worker thread; the last one wins on the UI side.
    void queryValuesAsync(const ParamInfo& param);

    // Stop the worker and wait. Call before destruction from the UI thread.
    void shutdown();

signals:
    void opened(const MetaInfo& meta, const QVector<ParamInfo>& params);
    void openFailed(const QString& error);
    void valuesReady(const SeriesData& series);
    void queryFailed(const QString& error);

private:
    // Worker lives on thread_; owns every reader object.
    class Worker;
    Worker* worker_ = nullptr;
    QThread thread_;
};

// Human-readable names for the raw tsfile enum ints stored in ParamInfo.
namespace TsFileNames
{
QString dataType(int t);
QString encoding(int e);
QString compression(int c);
}  // namespace TsFileNames
