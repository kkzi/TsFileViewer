#pragma once

#include <QHash>
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
    // Data-integrity flag for the tree view (red label): set when the
    // sources' footer statistics contradict each other (overlapping time
    // ranges across files — one of them is likely corrupted), or when the
    // file needed a tail repair. The viewer still shows the data as-is.
    bool suspicious = false;
    // Tree/key lookup helper used by the UI (search box, param tracking).
    QString key() const { return device + QLatin1Char('/') + measurement; }
    bool operator==(const ParamInfo& o) const
    {
        return source == o.source && device == o.device &&
               measurement == o.measurement;
    }
    bool operator!=(const ParamInfo& o) const { return !(*this == o); }
};
Q_DECLARE_METATYPE(ParamInfo)

// Per-device file summary for the tree tooltips: the files contributing
// the device's parameters and their footer-statistics aggregates (rows,
// time coverage) — no data decoding involved.
struct DeviceFileInfo
{
    QStringList files;        // contributing file paths
    qint64 totalRows = 0;     // sum over the device's params across files
    qint64 firstTs = 0;       // coverage: min slice start .. max slice end
    qint64 lastTs = 0;
    bool haveRange = false;   // false when no file carried statistics
};
Q_DECLARE_METATYPE(DeviceFileInfo)

// Per-file footer-level summary for the toolbar file dialog: gathered
// while opening (no data decoding).
struct FileInfoEntry
{
    QString path;
    qint64 size = 0;
    int deviceCount = 0;    // tree devices in this file
    int tableCount = 0;     // real (non-virtual) tables in this file
    int paramCount = 0;     // measurements + table field columns
    qint64 chunkCount = 0;  // sum of per-series chunk metadata list sizes
    qint64 rowCount = 0;    // sum of footer statistics counts
    qint64 firstTs = 0;
    qint64 lastTs = 0;
    bool haveRange = false;
};
Q_DECLARE_METATYPE(FileInfoEntry)

struct MetaInfo
{
    QString path;           // file, or the directory in multi-file mode
    qint64 fileSize = 0;    // single file: its size; directory: sum
    int deviceCount = 0;   // tree devices
    int tableCount = 0;    // real (non-virtual) tables
    int paramCount = 0;    // total measurements/columns
    qint64 firstTs = 0;
    qint64 lastTs = 0;
    bool haveTimeRange = false;  // false until the first query filled it
    QStringList devices;
    QStringList tables;
    bool repaired = false;       // open() truncated+sealed a corrupted tail
    qint64 truncatedBytes = 0;   // bytes dropped by the repair
    // Multi-file mode: files actually loaded (corrupted ones are skipped).
    int fileCount = 1;
    int skippedFileCount = 0;    // unreadable files excluded from the set
    QStringList skippedFiles;    // their paths (tooltip / tree flagging)
    QStringList skippedErrors;   // parallel to skippedFiles: why each failed
    qint64 overlappingParamCount = 0;  // params whose files overlap in time
    // Corrupt-file identification (footer statistics only, no data decode):
    // proven-bad files (self-contradictory statistics) vs heuristic suspects
    // (statistics contradict the majority of files).
    QStringList corruptFiles;
    QStringList suspectFiles;
    // Per-device (or per-table) file summary for the left-tree tooltips.
    QHash<QString, DeviceFileInfo> deviceInfo;
    // One entry per loaded file, in load (time) order.
    QVector<FileInfoEntry> fileEntries;
};
Q_DECLARE_METATYPE(MetaInfo)

struct SeriesData
{
    QString device;        // tree device or table name
    QString measurement;   // measurement / column name
    QVector<qint64> ts;    // ms (normalized from ms/us/ns input)
    QVector<double> value;  // NaN where the value is not numeric
    QVector<QString> text;  // non-empty only for STRING columns (parallel to value)
    bool numeric = true;    // false when the column holds text/boolean rows
    int dataType = 0;       // common::TSDataType of the measurement
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
    // A corrupted file triggers repairConfirmRequested(path, code); call
    // retryWithRepair(path, true/false) afterwards to proceed.
    void openAsync(const QString& path);
    // Open several files at once: params aggregate across files into one
    // device->param tree; a param present in several files has its values
    // concatenated in file (time) order at query time.
    void openFilesAsync(const QStringList& paths);
    // User answered the repair question: true = truncate+seal then open,
    // false = report the failure unchanged.
    void retryWithRepair(const QString& path, bool allow);
    // Repair an unopenable file in place (RestorableTsFileIOWriter truncate
    // + seal, mirroring the single-file repair). Emits repairFinished.
    void repairFileAsync(const QString& path);
    // Async: emits valuesChunk(SeriesData, bool done) progressively — the
    // series carries the rows appended since the last chunk; done=true on
    // the final chunk. queryFailed(QString) on error. Selecting a new
    // parameter while a query is running supersedes the old one (its
    // remaining chunks are dropped).
    // The whole series is streamed (no paging); limit < 0 = unlimited.
    void queryValuesAsync(const ParamInfo& param);

    // Export the parameter's FULL series to a
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
    void repairConfirmRequested(const QString& path, int code);
    void repairFinished(const QString& path, bool ok, const QString& message);
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
// Types whose values are integral: rendered without decimals (table,
// stats, tracer, plot y-axis).
bool isIntegerType(int t);
}  // namespace TsFileNames
