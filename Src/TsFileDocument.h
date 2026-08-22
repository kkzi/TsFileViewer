#pragma once

#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <memory>

// Data layer for the viewer. Owns a worker QThread; all tsfile reads
// (open/schema walk/queries) run there so the UI stays responsive.
// The reader objects are created and destroyed on the worker thread.

enum class ParamSource
{
    Tree,   // tree model: device > measurement via TsFileTreeReader
    Table,  // table model: table > field column via TsFileReader table query
};

// Registered with qRegisterMetaType so results can cross threads via signals.
struct ParamInfo
{
    ParamSource source = ParamSource::Tree;
    QString device;       // tree: device id; table: table name
    QString measurement;  // tree: measurement; table: field column name
    int dataType = 0;     // common::TSDataType
    int encoding = 0;     // common::TSEncoding
    int compression = 0;  // common::CompressionType
    QString key() const { return device + QLatin1Char('/') + measurement; }
    bool operator==(const ParamInfo& o) const
    {
        return source == o.source && device == o.device &&
               measurement == o.measurement;
    }
    bool operator!=(const ParamInfo& o) const { return !(*this == o); }
};
Q_DECLARE_METATYPE(ParamInfo)

struct MetaInfo
{
    QString path;
    qint64 fileSize = 0;
    int deviceCount = 0;   // tree devices
    int tableCount = 0;    // real (non-virtual) tables
    int paramCount = 0;    // total measurements/columns
    qint64 firstTs = 0;
    qint64 lastTs = 0;
    bool haveTimeRange = false;  // false until the first query filled it
    QStringList devices;
    QStringList tables;
};
Q_DECLARE_METATYPE(MetaInfo)

struct SeriesData
{
    QString device;        // tree device or table name
    QString measurement;   // measurement / column name
    QVector<qint64> ts;    // us
    QVector<double> value;  // NaN where the value is not numeric
    QVector<QString> text;  // non-empty only for STRING columns (parallel to value)
    bool numeric = true;    // false when the column holds text/boolean rows
    qint64 offset = 0;      // absolute row index of ts[0] within the series
    bool hasMore = false;   // final chunk only: a further page exists
    qint64 totalRows = -1;  // whole series row count from metadata (-1 unknown)
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
    // Async: emits valuesChunk(SeriesData, bool done) progressively — the
    // series carries the rows appended since the last chunk; done=true on
    // the final chunk. queryFailed(QString) on error. Selecting a new
    // parameter while a query is running supersedes the old one (its
    // remaining chunks are dropped).
    // Pages: rows [page * kPageSize, (page+1) * kPageSize). Page >= 0.
    static constexpr qint64 kPageSize = 1000000;
    void queryValuesAsync(const ParamInfo& param, qint64 page = 0);

    // Export the parameter's FULL series (all rows, ignoring paging) to a
    // UTF-8 CSV: header "Time,Value", plain decimal (no scientific).
    // Returns false on error (errorText set). Runs synchronously on the
    // worker thread pool — call from a QtConcurrent job, not the UI thread.
    bool exportCsvBlocking(const ParamInfo& param, const QString& csvPath,
                           QString* errorText = nullptr);

    // Stop the worker and wait. Call before destruction from the UI thread.
    void shutdown();

signals:
    void opened(const MetaInfo& meta, const QVector<ParamInfo>& params);
    void openFailed(const QString& error);
    void valuesChunk(const SeriesData& chunk, bool done);
    void queryFailed(const QString& error);
    // Internal: tell the worker a newer query should supersede a running one.
    void supersedeRequested(const ParamInfo& param);

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
