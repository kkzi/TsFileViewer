#include "TsFileDocument.h"

#include "common/record.h"
#include "common/tsfile_common.h"
#include "file/restorable_tsfile_io_writer.h"
#include "reader/tsfile_reader.h"
#include "reader/tsfile_tree_reader.h"

// After the tsfile headers: PathBridge pulls qt_windows.h, whose macros
// clash with tsfile names (IN vs LocateStatus { BEFORE, IN, AFTER };
// BOOLEAN vs common::BOOLEAN). Clear them before continuing.
#include "PathBridge.h"

#ifdef IN
#undef IN
#endif
#ifdef BOOLEAN
#undef BOOLEAN
#endif

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>

namespace
{
// No "using namespace common" here: windows.h (via PathBridge.h) defines
// global typedefs BOOLEAN/INT32/INT64 that would be ambiguous with the
// common:: enum members. Qualify explicitly instead.
QString dataTypeName(int t)
{
    switch (t)
    {
        case common::BOOLEAN: return QStringLiteral("BOOLEAN");
        case common::INT32: return QStringLiteral("INT32");
        case common::INT64: return QStringLiteral("INT64");
        case common::FLOAT: return QStringLiteral("FLOAT");
        case common::DOUBLE: return QStringLiteral("DOUBLE");
        case common::TEXT: return QStringLiteral("TEXT");
        default: return QStringLiteral("type(%1)").arg(t);
    }
}

QString encodingName(int e)
{
    switch (e)
    {
        case common::PLAIN: return QStringLiteral("PLAIN");
        case common::TS_2DIFF: return QStringLiteral("TS_2DIFF");
        case common::RLE: return QStringLiteral("RLE");
        case common::GORILLA: return QStringLiteral("GORILLA");
        default: return QStringLiteral("enc(%1)").arg(e);
    }
}

QString compressionName(int c)
{
    switch (c)
    {
        case common::UNCOMPRESSED: return QStringLiteral("UNCOMPRESSED");
        case common::SNAPPY: return QStringLiteral("SNAPPY");
        case common::GZIP: return QStringLiteral("GZIP");
        case common::LZ4: return QStringLiteral("LZ4");
        default: return QStringLiteral("cmp(%1)").arg(c);
    }
}

// Reads one Field into a numeric double + optional text string.
// Returns false when the field is null.
bool fieldToValues(const storage::Field* f, double& v, QString& text)
{
    if (f == nullptr || f->type_ == common::NULL_TYPE)
    {
        return false;
    }
    switch (f->type_)
    {
        case common::BOOLEAN:
            v = f->value_.bval_ ? 1.0 : 0.0;
            break;
        case common::INT32: v = static_cast<double>(f->value_.ival_); break;
        case common::INT64:
        case common::TIMESTAMP: v = static_cast<double>(f->value_.lval_); break;
        case common::FLOAT: v = static_cast<double>(f->value_.fval_); break;
        case common::DOUBLE: v = f->value_.dval_; break;
        case common::TEXT:
            if (f->value_.strval_ != nullptr)
            {
                text = QString::fromUtf8(f->value_.strval_->buf_,
                                         static_cast<int>(f->value_.strval_->len_));
            }
            break;
        default: break;
    }
    return true;
}
}  // namespace

// Helpers shared with the model layer (Models.cpp uses the tree tooltips).
namespace TsFileNames
{
QString dataType(int t) { return dataTypeName(t); }
QString encoding(int e) { return encodingName(e); }
QString compression(int c) { return compressionName(c); }
}  // namespace TsFileNames

// Runs on the worker thread. Holds the readers; the UI thread only ever sees
// copies of plain data via queued signals.
class TsFileDocument::Worker : public QObject
{
    Q_OBJECT
public:
    explicit Worker(QObject* parent = nullptr) : QObject(parent) {}

public slots:
    void open(const QString& path)    {
        // Bridge to an ACP-safe path for the library (see PathBridge.h);
        // meta.path below keeps the original for display.
        const std::string libPath = pathbridge::toLibPath(path).toStdString();

        // Plain reader validates the file (footer present). On failure, fall
        // back to RestorableTsFileIOWriter: it truncates the corrupted tail
        // and seals the file in place (footer rewrite), mirroring TsFileStat
        // --repair. The user is asked first (see MainWindow::onRepairQuestion).
        storage::TsFileReader reader;
        int r = reader.open(libPath);
        bool recovered = false;
        qint64 truncatedBytes = 0;
        if (r != common::E_OK)
        {
            if (!repairAsked_ && !repairAllowed_)
            {
                // First failure: ask the user once whether to repair in place.
                repairAsked_ = true;
                emit repairConfirmRequested(path, r);
                return;
            }
            if (!repairAllowed_)
            {
                emit openFailed(QStringLiteral(
                                    "open failed (code %1); file was not modified")
                                    .arg(r));
                return;
            }
            storage::RestorableTsFileIOWriter rw;
            const int rr = rw.open(libPath, /*truncate_corrupted=*/true);
            if (rr != common::E_OK)
            {
                emit openFailed(QStringLiteral(
                                    "open failed (code %1); not recoverable")
                                    .arg(r));
                return;
            }
            truncatedBytes = rw.get_truncated_size();
            if (rw.can_write())
            {
                if (rw.end_file() != common::E_OK)
                {
                    rw.close();
                    emit openFailed(
                        QStringLiteral("repair sealing failed (file untouched beyond truncation)"));
                    return;
                }
                rw.close();
                recovered = true;
            }
            else
            {
                rw.close();
            }
            r = reader.open(libPath);
            if (r != common::E_OK)
            {
                emit openFailed(QStringLiteral(
                                    "open failed (code %1) even after repair")
                                    .arg(r));
                return;
            }
        }

        MetaInfo meta;
        meta.path = path;
        QVector<ParamInfo> params;

        // ---- tree part: devices > measurements ----------------------------
        auto devices = reader.get_all_device_ids();
        for (const auto& device : devices)
        {
            const QString deviceName =
                QString::fromStdString(device->get_device_name());
            meta.devices << deviceName;
            std::vector<storage::MeasurementSchema> schemas;
            reader.get_timeseries_schema(device, schemas);
            for (const auto& s : schemas)
            {
                ParamInfo p;
                p.source = ParamSource::Tree;
                p.device = deviceName;
                p.measurement = QString::fromStdString(s.measurement_name_);
                p.dataType = static_cast<int>(s.data_type_);
                p.encoding = static_cast<int>(s.encoding_);
                p.compression = static_cast<int>(s.compression_type_);
                params.push_back(p);
            }
        }
        meta.deviceCount = static_cast<int>(devices.size());

        // ---- table part: real (non-virtual) tables > field columns --------
        auto tableSchemas = reader.get_all_table_schemas();
        for (const auto& ts : tableSchemas)
        {
            if (ts == nullptr || ts->is_virtual_table())
            {
                continue;  // virtual tree-derived tables are just the tree part
            }
            const QString tableName = QString::fromStdString(ts->get_table_name());
            meta.tables << tableName;
            const auto names = ts->get_measurement_names();
            const auto types = ts->get_data_types();
            const auto categories = ts->get_column_categories();
            for (size_t i = 0; i < names.size(); ++i)
            {
                if (categories[i] != common::ColumnCategory::FIELD)
                {
                    continue;  // TAG/ATTRIBUTE columns are not time series
                }
                ParamInfo p;
                p.source = ParamSource::Table;
                p.device = tableName;
                p.measurement = QString::fromStdString(names[i]);
                p.dataType = static_cast<int>(types[i]);
                params.push_back(p);
            }
        }
        meta.tableCount = static_cast<int>(meta.tables.size());
        reader.close();

        if (meta.deviceCount == 0 && meta.tableCount == 0)
        {
            emit openFailed(QStringLiteral("file has no tree devices and no tables"));
            return;
        }

        meta.paramCount = static_cast<int>(params.size());
        const QFileInfo fi(path);
        meta.fileSize = fi.size();
        meta.repaired = recovered;
        meta.truncatedBytes = truncatedBytes;

        emit opened(meta, params);
    }

    void query(const ParamInfo& param, qint64 page)
    {
        if (path_.isEmpty())
        {
            emit queryFailed(QStringLiteral("no file open"));
            return;
        }

        // Supersede check: a newer selection may already be waiting.
        if (pendingParam_.has_value() && *pendingParam_ != param)
        {
            return;  // drop this stale request
        }
        pendingParam_.reset();

        storage::TsFileReader reader;
        if (reader.open(path_.toStdString()) != common::E_OK)
        {
            emit queryFailed(QStringLiteral("TsFileReader could not open file"));
            return;
        }

        // Paged query: queryByRow pushes offset/limit down (chunk/page level
        // for dense devices), so paging does not decode skipped rows.
        const qint64 offset = page * kPageSize;
        storage::ResultSet* result = nullptr;
        int q = common::E_OK;
        if (param.source == ParamSource::Table)
        {
            q = reader.queryByRow(param.device.toStdString(),
                                  {param.measurement.toStdString()},
                                  static_cast<int>(offset),
                                  static_cast<int>(kPageSize), result);
        }
        else
        {
            // Tree path query: device + "." + measurement.
            std::vector<std::string> pathList{
                param.device.toStdString() + "." + param.measurement.toStdString()};
            q = reader.queryByRow(pathList, static_cast<int>(offset),
                                  static_cast<int>(kPageSize), result);
        }
        if (q != common::E_OK || result == nullptr)
        {
            reader.close();
            emit queryFailed(QStringLiteral("query failed (code %1)").arg(q));
            return;
        }

        SeriesData out;
        out.device = param.device;
        out.measurement = param.measurement;
        out.numeric = param.dataType != common::TEXT;
        out.offset = offset;

        bool haveData = false;
        int64_t minTs = std::numeric_limits<int64_t>::max();
        int64_t maxTs = std::numeric_limits<int64_t>::min();

        // Progressive delivery: flush the accumulated rows to the UI every
        // flushRows rows so the table and plot grow visibly during long
        // queries instead of freezing until completion. Memory is bounded by
        // the page size (one page = kPageSize rows in the model).
        constexpr int kFlushRows = 500000;
        QElapsedTimer sinceFlush;
        sinceFlush.start();
        qint64 pageRows = 0;  // rows delivered for this page (across flushes)

        bool hasNext = false;
        while (true)
        {
            const int nextRet = result->next(hasNext);
            if (nextRet != common::E_OK)
            {
                reader.destroy_query_data_set(result);
                reader.close();
                emit queryFailed(
                    QStringLiteral("query iteration failed (code %1)").arg(nextRet));
                return;
            }
            if (!hasNext)
            {
                break;
            }
            // RowRecord fields are 0-based: field 0 = time, field 1 = value.
            storage::RowRecord* row = result->get_row_record();
            if (row == nullptr)
            {
                continue;
            }
            const int64_t ts = row->get_field(0)->get_value<int64_t>();
            double v = std::numeric_limits<double>::quiet_NaN();
            QString text;
            if (fieldToValues(row->get_field(1), v, text))
            {
                if (!text.isEmpty())
                {
                    out.text.push_back(text);
                }
            }
            out.ts.push_back(ts);
            out.value.push_back(v);
            if (!haveData || ts < minTs) minTs = ts;
            if (!haveData || ts > maxTs) maxTs = ts;
            haveData = true;

            // Supersede check inside the loop: a new selection aborts this
            // query early; the already-flushed chunks stay on screen until
            // the new query's first chunk replaces them.
            if (pendingParam_.has_value())
            {
                reader.destroy_query_data_set(result);
                reader.close();
                return;
            }

            if (out.ts.size() >= kFlushRows && sinceFlush.elapsed() >= 200)
            {
                pageRows += out.ts.size();
                emit valuesChunk(out, /*done=*/false);
                out.ts.clear();
                out.value.clear();
                out.text.clear();
                sinceFlush.restart();
            }
        }
        reader.destroy_query_data_set(result);
        reader.close();
        pageRows += out.ts.size();

        // Fill the file-level time range lazily: the first query defines it,
        // later ones widen it (matches TsFileStat's global min/max semantics).
        if (haveData)
        {
            if (!haveRange_)
            {
                firstTs_ = minTs;
                lastTs_ = maxTs;
                haveRange_ = true;
            }
            else
            {
                firstTs_ = std::min(firstTs_, minTs);
                lastTs_ = std::max(lastTs_, maxTs);
            }
        }

        // A full page implies more rows may follow. The exact series total
        // comes free from the metadata statistic (no drain needed).
        out.hasMore = pageRows >= kPageSize;
        out.totalRows = seriesTotalRows(param);
        emit valuesChunk(out, /*done=*/true);
        emit timeRangeKnown(firstTs_, lastTs_, haveRange_);
    }

    // Whole-series row count from the timeseries metadata statistic
    // (already deserialized; no data drain). -1 when unavailable.
    qint64 seriesTotalRows(const ParamInfo& param)
    {
        qint64 total = -1;
        storage::TsFileReader reader;
        if (reader.open(path_.toStdString()) != common::E_OK)
        {
            return total;
        }
        const auto meta = reader.get_timeseries_metadata();
        for (const auto& kv : meta)
        {
            for (const auto& tsip : kv.second)
            {
                if (tsip->get_measurement_name().to_std_string() !=
                    param.measurement.toStdString())
                {
                    continue;
                }
                // Aligned series: the value sub-index carries the value stats.
                const storage::Statistic* st = nullptr;
                auto* aligned =
                    dynamic_cast<storage::AlignedTimeseriesIndex*>(tsip.get());
                if (aligned != nullptr && aligned->value_ts_idx_ != nullptr)
                {
                    st = aligned->value_ts_idx_->get_statistic();
                }
                else
                {
                    st = tsip->get_statistic();
                }
                if (st != nullptr)
                {
                    total = st->get_count();
                }
                reader.close();
                return total;
            }
        }
        reader.close();
        return total;
    }

    void setFile(const QString& path)
    {
        // Upstream lib opens paths via CRT ::open() in the active code page;
        // bridge UTF-8 (Qt) paths to the ASCII 8.3 short path on Windows.
        path_ = pathbridge::toLibPath(path);
        haveRange_ = false;
        firstTs_ = 0;
        lastTs_ = 0;
    }

signals:
    void opened(const MetaInfo& meta, const QVector<ParamInfo>& params);
    void openFailed(const QString& error);
    void repairConfirmRequested(const QString& path, int code);
    void valuesChunk(const SeriesData& chunk, bool done);
    void queryFailed(const QString& error);
    void timeRangeKnown(qint64 firstTs, qint64 lastTs, bool haveRange);

public:
    // Full-series CSV export (blocking; called via BlockingQueuedConnection).
    bool exportCsv(const ParamInfo& param, const QString& csvPath,
                   QString* errorText)
    {
        if (path_.isEmpty())
        {
            if (errorText) *errorText = QStringLiteral("no file open");
            return false;
        }
        QFile f(pathbridge::toLibPath(csvPath));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            if (errorText)
                *errorText = QStringLiteral("cannot create %1").arg(csvPath);
            return false;
        }
        storage::TsFileReader reader;
        if (reader.open(path_.toStdString()) != common::E_OK)
        {
            if (errorText) *errorText = QStringLiteral("open failed");
            return false;
        }
        storage::ResultSet* result = nullptr;
        int q = common::E_OK;
        if (param.source == ParamSource::Table)
        {
            q = reader.query(param.device.toStdString(),
                             {param.measurement.toStdString()},
                             std::numeric_limits<int64_t>::min(),
                             std::numeric_limits<int64_t>::max(), result);
        }
        else
        {
            std::vector<std::string> pathList{
                param.device.toStdString() + "." + param.measurement.toStdString()};
            q = reader.query(pathList, std::numeric_limits<int64_t>::min(),
                             std::numeric_limits<int64_t>::max(), result);
        }
        if (q != common::E_OK || result == nullptr)
        {
            reader.close();
            if (errorText)
                *errorText = QStringLiteral("query failed (code %1)").arg(q);
            return false;
        }

        f.write("Time,Value\n");
        bool hn = false;
        qint64 rows = 0;
        // Fixed-precision formatting, no scientific notation: values are
        // formatted with up to 17 significant digits in plain decimal and
        // trailing zeros trimmed.
        while (result->next(hn) == common::E_OK && hn)
        {
            storage::RowRecord* row = result->get_row_record();
            if (row == nullptr) continue;
            const int64_t ts = row->get_field(0)->get_value<int64_t>();
            double v = std::numeric_limits<double>::quiet_NaN();
            QString text;
            fieldToValues(row->get_field(1), v, text);
            // Seconds with microsecond precision, trailing zeros trimmed
            // (matches the table's Time column), then the CSV separator.
            char tbuf[48];
            std::snprintf(tbuf, sizeof(tbuf), "%.6f",
                          static_cast<double>(ts) / 1e6);
            char* end = tbuf + std::strlen(tbuf) - 1;
            while (end > tbuf && *end == '0') *end-- = '\0';
            if (*end == '.') *end = '\0';
            const size_t tlen = std::strlen(tbuf);
            tbuf[tlen] = ',';
            tbuf[tlen + 1] = '\0';
            f.write(tbuf);
            if (!text.isEmpty())
            {
                f.write(text.toUtf8());
            }
            else
            {
                // %.10f keeps plain decimal (no scientific), then trailing
                // zeros trimmed.
                char buf[48];
                std::snprintf(buf, sizeof(buf), "%.10f", v);
                char* end = buf + std::strlen(buf) - 1;
                while (end > buf && *end == '0') *end-- = '\0';
                if (*end == '.') *end = '\0';
                f.write(buf);
            }
            f.write("\n");
            ++rows;
        }
        reader.destroy_query_data_set(result);
        reader.close();
        f.close();
        return f.error() == QFile::NoError;
    }

private:
    QString path_;
    bool repairAllowed_ = false;  // set after the user confirms
    bool repairAsked_ = false;
    bool haveRange_ = false;
    qint64 firstTs_ = 0;
    qint64 lastTs_ = 0;
    // Set by the UI thread when a new query request arrives while this
    // worker is still draining a previous one; checked at flush points.
    std::optional<ParamInfo> pendingParam_;

    friend class TsFileDocument;
};

#include "TsFileDocument.moc"

TsFileDocument::TsFileDocument(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<ParamInfo>("ParamInfo");
    qRegisterMetaType<MetaInfo>("MetaInfo");
    qRegisterMetaType<SeriesData>("SeriesData");
    qRegisterMetaType<QVector<ParamInfo>>("QVector<ParamInfo>");

    // Required before any reader use (allocator + global config); pairs with
    // libtsfile_destroy() in shutdown(). Without it, readers run on
    // uninitialized arena state and can return garbage rows.
    storage::libtsfile_init();

    worker_ = new Worker();
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    // Forward worker signals to the document's own signals (queued because
    // the worker lives on thread_).
    connect(worker_, &Worker::opened, this, &TsFileDocument::opened);
    connect(worker_, &Worker::openFailed, this, &TsFileDocument::openFailed);
    connect(worker_, &Worker::repairConfirmRequested, this,
            &TsFileDocument::repairConfirmRequested);
    connect(worker_, &Worker::valuesChunk, this, &TsFileDocument::valuesChunk);
    connect(worker_, &Worker::queryFailed, this, &TsFileDocument::queryFailed);
    connect(this, &TsFileDocument::supersedeRequested, worker_,
            [worker = worker_](const ParamInfo& param)
    {
        worker->pendingParam_ = param;
    });
    thread_.start();
}

TsFileDocument::~TsFileDocument()
{
    shutdown();
}

void TsFileDocument::openAsync(const QString& path)
{
    // New file selection resets the repair dialog state.
    QMetaObject::invokeMethod(worker_, [worker = worker_, path]
    {
        worker->repairAsked_ = false;
        worker->repairAllowed_ = false;
        worker->setFile(path);
        worker->open(path);
    });
}

void TsFileDocument::retryWithRepair(const QString& path, bool allow)
{
    QMetaObject::invokeMethod(worker_, [worker = worker_, path, allow]
    {
        worker->repairAllowed_ = allow;
        worker->open(path);
    });
}

void TsFileDocument::queryValuesAsync(const ParamInfo& param, qint64 page){
    // Mark the request so a running query can notice it at its next flush
    // point and abort; the invoke then delivers the new one. The lambda runs
    // on the worker thread (queued), so pendingParam_ stays single-threaded.
    emit supersedeRequested(param);
    QMetaObject::invokeMethod(worker_, [worker = worker_, param, page]
    {
        worker->query(param, page);
    });
}

bool TsFileDocument::exportCsvBlocking(const ParamInfo& param,
                                       const QString& csvPath,
                                       QString* errorText)
{
    // Runs on the worker thread (queued): one full-range query, rows written
    // straight to disk so memory stays bounded regardless of series size.
    bool ok = false;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, param, csvPath, errorText, &ok]
        {
            ok = worker->exportCsv(param, csvPath, errorText);
        },
        Qt::BlockingQueuedConnection);
    return ok;
}

void TsFileDocument::shutdown()
{
    if (thread_.isRunning())
    {
        thread_.quit();
        thread_.wait();
        storage::libtsfile_destroy();
    }
}
