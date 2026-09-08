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
#include <QDir>
#include <QSet>
#include <QMetaObject>
#include <QtConcurrent>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>

// Timestamp unit normalization. The tsfile format does not record its
// timestamp unit; IoTDB (the reference engine) picks ms/us/ns via its
// timestamp_precision setting, default ms. Recorded data is post-2000:
// in ms that is <= ~1e14 (through year 5140), in us within [~1e14, 1e17),
// in ns >= ~1e17 — three clean decades. The magnitude decides; everything
// is normalized to milliseconds, this viewer's internal unit.
inline qint64 normalizeTsToMs(qint64 raw)
{
    if (raw < qint64(100000000000000))
    {
        return raw;  // already ms (spec default)
    }
    if (raw < qint64(100000000000000000))
    {
        return raw / 1000;  // us
    }
    return raw / 1000000;  // ns
}

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
            text = f->value_.bval_ ? QStringLiteral("true")
                                   : QStringLiteral("false");
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

// Human-readable reason a file was skipped (mapped from the reader's
// open error code; enriched with what the filesystem says).
QString skipReason(const QString& path, int code)
{
    if (code == common::E_FILE_OPEN_ERR)
    {
        return QFileInfo::exists(path)
                   ? QStringLiteral(
                         "open failed (locked by writer or no read permission)")
                   : QStringLiteral("file not found");
    }
    if (code == common::E_FILE_STAT_ERR)
    {
        return QStringLiteral("stat failed");
    }
    if (code == common::E_TSFILE_CORRUPTED)
    {
        return QStringLiteral(
                   "corrupted: bad magic or size too small (%1 bytes) — "
                   "damaged tail or not a tsfile")
            .arg(QFileInfo(path).size());
    }
    return QStringLiteral("open failed (code %1)").arg(code);
}

// Helpers shared with the model layer (Models.cpp uses the tree tooltips).
namespace TsFileNames
{
QString dataType(int t) { return dataTypeName(t); }
QString encoding(int e) { return encodingName(e); }
QString compression(int c) { return compressionName(c); }
bool isIntegerType(int t)
{
    return t == common::INT32 || t == common::INT64 ||
           t == common::TIMESTAMP || t == common::BOOLEAN;
}
}  // namespace TsFileNames

// Runs on the worker thread. Holds the readers; the UI thread only ever sees
// copies of plain data via queued signals.
class TsFileDocument::Worker : public QObject
{
    Q_OBJECT
public:
    explicit Worker(QObject* parent = nullptr) : QObject(parent) {}

    // Per-file slice of one parameter: row count and time range come from
    // the footer statistics (no data decode).
    struct ParamSlice
    {
        qint64 count = 0;
        qint64 startTs = std::numeric_limits<qint64>::max();
        qint64 endTs = std::numeric_limits<qint64>::min();
    };
    // Aggregated routing info for one (device, measurement) pair.
    struct ParamRoute
    {
        QHash<int, ParamSlice> perFile;  // fileIdx -> slice
        // perFile entries sorted by start time (query visit order).
        QVector<QPair<int, ParamSlice>> fileOrder;
        int dataType = 0;
        qint64 totalRows = 0;
        bool treeSource = false;    // seen as tree device measurement
        bool tableSource = false;   // seen as table field column
        bool overlaps = false;      // two files cover overlapping time
    };

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
        // Single-file mode has no per-device footer walk, so the
        // per-device tooltip carries just the file path.
        for (const QString& d : meta.devices + meta.tables)
        {
            meta.deviceInfo[d].files << path;
        }

        // ---- files-dialog numbers: footer walk, same numbers multi-file
        // mode reports (per-series chunk statistics + chunk-list sizes,
        // zero data decode). Table columns have no footer walk in either
        // mode, so they contribute params only.
        FileInfoEntry fe;
        fe.path = path;
        fe.size = QFileInfo(path).size();
        fe.deviceCount = meta.deviceCount;
        fe.tableCount = meta.tableCount;
        fe.paramCount = static_cast<int>(params.size());
        fe.chunkCount = 0;
        fe.rowCount = 0;
        const auto tsMeta = reader.get_timeseries_metadata();
        for (const auto& kv : tsMeta)
        {
            for (const auto& tsip : kv.second)
            {
                if (const storage::Statistic* st = tsip->get_statistic())
                {
                    const qint64 cnt = st->get_count();
                    fe.rowCount += qMax<qint64>(cnt, 0);
                    if (cnt > 0)
                    {
                        const qint64 s0 = normalizeTsToMs(st->start_time_);
                        const qint64 s1 = normalizeTsToMs(st->get_end_time());
                        if (!fe.haveRange)
                        {
                            fe.firstTs = s0;
                            fe.lastTs = s1;
                            fe.haveRange = true;
                        }
                        else
                        {
                            fe.firstTs = qMin(fe.firstTs, s0);
                            fe.lastTs = qMax(fe.lastTs, s1);
                        }
                    }
                }
                // Aligned series keep timestamps in the time chunk list;
                // plain series in the single chunk list.
                if (auto* list = tsip->is_aligned()
                                     ? tsip->get_time_chunk_meta_list()
                                     : tsip->get_chunk_meta_list())
                {
                    fe.chunkCount += list->size();
                }
            }
        }
        reader.close();

        if (meta.deviceCount == 0 && meta.tableCount == 0)
        {
            emit openFailed(QStringLiteral("file has no tree devices and no tables"));
            return;
        }

        meta.paramCount = static_cast<int>(params.size());
        const QFileInfo fi(path);
        meta.fileSize = fi.size();
        meta.fileEntries.append(fe);
        meta.repaired = recovered;
        meta.truncatedBytes = truncatedBytes;
        // A repaired file lost its corrupted tail (crashed write): the tree
        // flags every param red so the truncation is visible, not silent.
        if (recovered)
        {
            for (ParamInfo& p : params)
            {
                p.suspicious = true;
            }
        }

        emit opened(meta, params);
    }

    // Multi-file mode: aggregate the per-file schemas and footer statistics
    // into routes_, and hand the UI one merged device->param list. Corrupted
    // files are skipped (no repair prompts here — fixing one file of a set
    // is a per-file decision).
    void openFiles(const QStringList& paths)
    {
        files_.clear();
        routes_.clear();
        fileEntries_.clear();
        path_.clear();

        if (paths.isEmpty())
        {
            emit openFailed(QStringLiteral("no files given"));
            return;
        }

        MetaInfo meta;
        // Single readable file degrades below; multi-file keeps an empty
        // path (no single path to show) with the file count carrying the
        // info.
        meta.path = paths.size() == 1 ? paths.first() : QString();

        int skipped = 0;
        QStringList skippedFiles;
        QStringList skippedErrors;
        QStringList loadedFiles;
        for (const QString& path : paths)
        {
            int openError = 0;
            if (!appendFileToRoutes(path, loadedFiles.size(), &openError))
            {
                ++skipped;
                skippedFiles << path;
                skippedErrors << skipReason(path, openError);
                continue;
            }
            loadedFiles << path;
        }
        files_ = loadedFiles;

        if (files_.isEmpty())
        {
            emit openFailed(QStringLiteral("none of the %1 file(s) could be opened")
                                .arg(paths.size()));
            return;
        }

        // Per-device file info for the tree tooltips: aggregated footer
        // statistics (rows, coverage) and the contributing files. Built
        // before the single-file degrade clears routes_ below.
        {
            QHash<QString, QSet<int>> deviceFiles;
            for (auto it = routes_.cbegin(); it != routes_.cend(); ++it)
            {
                const int sep = it.key().indexOf(QLatin1Char('\x01'));
                const QString dev = it.key().left(sep);
                const ParamRoute& r = it.value();
                DeviceFileInfo& d = meta.deviceInfo[dev];
                d.totalRows += r.totalRows;
                for (auto fit = r.perFile.cbegin(); fit != r.perFile.cend(); ++fit)
                {
                    deviceFiles[dev].insert(fit.key());
                    const ParamSlice& s = fit.value();
                    if (s.count > 0)
                    {
                        if (!d.haveRange)
                        {
                            d.firstTs = s.startTs;
                            d.lastTs = s.endTs;
                            d.haveRange = true;
                        }
                        else
                        {
                            d.firstTs = std::min(d.firstTs, s.startTs);
                            d.lastTs = std::max(d.lastTs, s.endTs);
                        }
                    }
                }
            }
            for (auto it = deviceFiles.cbegin(); it != deviceFiles.cend(); ++it)
            {
                QList<int> idx = it.value().values();
                std::sort(idx.begin(), idx.end());
                DeviceFileInfo& d = meta.deviceInfo[it.key()];
                for (int i : idx)
                {
                    d.files << files_.at(i);
                }
            }
        }

        // ---- corrupt-file identification (footer statistics only) --------
        // 1) Proven: a param slice in that file contradicts itself
        //    (negative count, or start > end).
        // 2) Heuristic minority vote: for every param, each file taking part
        //    in a time-range overlap (slices sorted by start, next starts
        //    before the previous ends) collects a vote. A single lying file
        //    accumulates votes from every param and both its neighbors;
        //    clean files only from their side of the chain. Reported as
        //    suspects, not facts.
        {
            QSet<int> corruptIdx;
            QHash<int, qint64> votes;
            for (auto it = routes_.cbegin(); it != routes_.cend(); ++it)
            {
                const ParamRoute& r = it.value();
                for (auto fit = r.perFile.cbegin(); fit != r.perFile.cend(); ++fit)
                {
                    const ParamSlice& s = fit.value();
                    if (s.count < 0 ||
                        (s.count > 0 && s.startTs > s.endTs))
                    {
                        corruptIdx.insert(fit.key());
                    }
                }
                QList<QPair<int, const ParamSlice*>> byStart;
                for (auto fit = r.perFile.cbegin(); fit != r.perFile.cend(); ++fit)
                {
                    if (fit.value().count > 0)
                    {
                        byStart.append({fit.key(), &fit.value()});
                    }
                }
                std::sort(byStart.begin(), byStart.end(),
                          [](const QPair<int, const ParamSlice*>& a,
                             const QPair<int, const ParamSlice*>& b)
                          { return a.second->startTs < b.second->startTs; });
                for (int i = 1; i < byStart.size(); ++i)
                {
                    if (byStart.at(i).second->startTs <=
                        byStart.at(i - 1).second->endTs)
                    {
                        ++votes[byStart.at(i).first];
                        ++votes[byStart.at(i - 1).first];
                    }
                }
            }
            for (int idx : qAsConst(corruptIdx))
            {
                meta.corruptFiles << files_.at(idx);
            }
            qint64 maxVotes = 0;
            for (auto it = votes.cbegin(); it != votes.cend(); ++it)
            {
                maxVotes = qMax(maxVotes, it.value());
            }
            for (auto it = votes.cbegin(); it != votes.cend(); ++it)
            {
                if (it.value() == maxVotes && maxVotes > 0)
                {
                    const QString path = files_.at(it.key());
                    if (!meta.corruptFiles.contains(path))
                    {
                        meta.suspectFiles << path;
                    }
                }
            }
        }
        // Exactly one readable file: degrade to single-file mode so
        // query/export use the plain path_ code path.
        if (files_.size() == 1)
        {
            path_ = pathbridge::toLibPath(files_.first());
            files_.clear();
            routes_.clear();
        }

        // Merged, device-grouped param list for the tree model. routes_ keys
        // keep first-seen insertion order; sorting by key groups devices
        // (each device's params stay contiguous) and makes runs stable.
        QStringList keys = routes_.keys();
        std::sort(keys.begin(), keys.end());
        QVector<ParamInfo> params;
        params.reserve(keys.size());
        for (const QString& key : qAsConst(keys))
        {
            const ParamRoute& r = routes_.value(key);
            const int sep = key.indexOf(QLatin1Char('\x01'));
            ParamInfo p;
            p.source = r.tableSource && !r.treeSource
                           ? ParamSource::Table
                           : ParamSource::Tree;
            p.device = key.left(sep);
            p.measurement = key.mid(sep + 1);
            p.dataType = r.dataType;
            // Overlapping footer statistics across files: one of the files is
            // likely corrupted (a truncated write leaves stale statistics) —
            // flag the param so the tree can render it in red. The data
            // itself is still concatenated and shown as-is.
            p.suspicious = r.overlaps;
            params.push_back(p);
        }

        // Device/table names for the metainfo bar (deduplicated, sorted).
        {
            QSet<QString> devices, tables;
            for (const QString& key : qAsConst(keys))
            {
                const int sep = key.indexOf(QLatin1Char('\x01'));
                (routes_.value(key).tableSource ? tables : devices)
                    .insert(key.left(sep));
            }
            meta.devices = QStringList(devices.cbegin(), devices.cend());
            meta.tables = QStringList(tables.cbegin(), tables.cend());
            meta.devices.sort();
            meta.tables.sort();
            meta.deviceCount = meta.devices.size();
            meta.tableCount = meta.tables.size();
        }
        meta.paramCount = keys.size();
        meta.fileCount = files_.size();
        meta.skippedFileCount = skipped;
        for (const QString& f : qAsConst(files_))
        {
            meta.fileSize += QFileInfo(f).size();
        }
        // Global time range across all files (from the per-param slices).
        bool haveAny = false;
        qint64 firstTs = std::numeric_limits<qint64>::max();
        qint64 lastTs = std::numeric_limits<qint64>::min();
        for (const auto& r : routes_)
        {
            for (const auto& s : r.perFile)
            {
                if (s.count <= 0) continue;
                firstTs = std::min(firstTs, s.startTs);
                lastTs = std::max(lastTs, s.endTs);
                haveAny = true;
            }
        }
        if (haveAny)
        {
            meta.firstTs = firstTs;
            meta.lastTs = lastTs;
            meta.haveTimeRange = true;
            firstTs_ = firstTs;
            lastTs_ = lastTs;
            haveRange_ = true;
        }

        // Time order + overlap detection per route: files must be visited
        // oldest-first when concatenating rows. Overlapping time ranges mean
        // the concatenation is not globally time-sorted (flagged to the UI).
        qint64 overlapParamCount = 0;
        for (auto it = routes_.begin(); it != routes_.end(); ++it)
        {
            ParamRoute& r = it.value();
            QList<QPair<int, ParamSlice*>> byStart;
            for (auto fit = r.perFile.begin(); fit != r.perFile.end(); ++fit)
            {
                byStart.append({fit.key(), &fit.value()});
            }
            std::sort(byStart.begin(), byStart.end(),
                      [](const QPair<int, ParamSlice*>& a,
                         const QPair<int, ParamSlice*>& b)
                      { return a.second->startTs < b.second->startTs; });
            r.fileOrder.clear();
            for (const auto& e : byStart)
            {
                r.fileOrder.append({e.first, *e.second});
            }
            r.overlaps = false;
            for (int i = 1; i < byStart.size(); ++i)
            {
                if (byStart[i].second->startTs <=
                    byStart[i - 1].second->endTs)
                {
                    r.overlaps = true;
                    break;
                }
            }
            if (r.overlaps)
            {
                ++overlapParamCount;
            }
        }
        meta.overlappingParamCount = overlapParamCount;
        overlappingParamCount_ = overlapParamCount;
        meta.skippedFiles = skippedFiles;
        meta.skippedErrors = skippedErrors;
        meta.fileEntries = fileEntries_;

        emit opened(meta, params);
    }

    // Read one file's footer and merge its schema/statistics into routes_.
    // Returns false (and stays silent) when the file cannot be opened;
    // *openError then carries the reader's error code.
    bool appendFileToRoutes(const QString& path, int fileIdx,
                            int* openError = nullptr)
    {
        storage::TsFileReader reader;
        const int r =
            reader.open(pathbridge::toLibPath(path).toStdString());
        if (r != common::E_OK)
        {
            if (openError != nullptr)
            {
                *openError = r;
            }
            return false;
        }

        FileInfoEntry fe;
        fe.path = path;
        fe.size = QFileInfo(path).size();
        fe.chunkCount = 0;
        fe.rowCount = 0;
        QSet<QString> devices;

        const auto meta = reader.get_timeseries_metadata();
        // Codec truth lives in the chunk headers and needs a per-series
        // get_timeseries_schema read (fork fix); the viewer shows codecs
        // only in single-file mode, so multi-file skips that walk entirely.
        for (const auto& kv : meta)
        {
            const QString device =
                QString::fromStdString(kv.first->get_device_name());
            devices.insert(device);
            for (const auto& tsip : kv.second)
            {
                const QString measurement = QString::fromStdString(
                    tsip->get_measurement_name().to_std_string());
                const QString key = device + QLatin1Char('\x01') + measurement;
                ParamRoute& r = routes_[key];
                ParamSlice& s = r.perFile[fileIdx];
                r.treeSource = true;
                r.dataType = static_cast<int>(tsip->get_data_type());
                if (const storage::Statistic* st =
                        tsip->get_statistic())  // aligned: value statistic
                {
                    s.count = st->get_count();
                    s.startTs = normalizeTsToMs(st->start_time_);
                    s.endTs = normalizeTsToMs(st->get_end_time());
                }
                r.totalRows += s.count;
                fe.paramCount++;
                fe.rowCount += qMax<qint64>(s.count, 0);
                if (s.count > 0)
                {
                    if (!fe.haveRange)
                    {
                        fe.firstTs = s.startTs;
                        fe.lastTs = s.endTs;
                        fe.haveRange = true;
                    }
                    else
                    {
                        fe.firstTs = qMin(fe.firstTs, s.startTs);
                        fe.lastTs = qMax(fe.lastTs, s.endTs);
                    }
                }
                // Aligned series keep the timestamps in the time chunk
                // list; plain series in the single chunk list.
                if (auto* list = tsip->is_aligned()
                                     ? tsip->get_time_chunk_meta_list()
                                     : tsip->get_chunk_meta_list())
                {
                    fe.chunkCount += list->size();
                }
            }
        }
        fe.deviceCount = devices.size();

        // Table model files: field columns become routable params too.
        const auto tableSchemas = reader.get_all_table_schemas();
        for (const auto& ts : tableSchemas)
        {
            if (ts == nullptr || ts->is_virtual_table())
            {
                continue;
            }
            const QString tableName =
                QString::fromStdString(ts->get_table_name());
            const auto names = ts->get_measurement_names();
            const auto types = ts->get_data_types();
            const auto categories = ts->get_column_categories();
            for (size_t i = 0; i < names.size(); ++i)
            {
                if (categories[i] != common::ColumnCategory::FIELD)
                {
                    continue;
                }
                const QString key = tableName + QLatin1Char('\x01') +
                                    QString::fromStdString(names[i]);
                ParamRoute& r = routes_[key];
                ParamSlice& s = r.perFile[fileIdx];
                r.tableSource = true;
                r.dataType = static_cast<int>(types[i]);
                fe.paramCount++;
            }
            fe.tableCount++;
        }
        reader.close();
        fileEntries_.append(fe);
        return true;
    }

    // Repair an unopenable file in place: truncate the corrupted tail and
    // seal it (footer rewrite), then verify the plain reader accepts it.
    // Same sequence as the repair path in open(). Runs on a thread-pool
    // thread (see TsFileDocument::repairFileAsync), NOT here: a long
    // self-check scan must not block queries on this single thread.
    static void repairFile(const QString& path,
                           std::function<void(bool, QString)> done)
    {
        const std::string libPath = pathbridge::toLibPath(path).toStdString();
        storage::RestorableTsFileIOWriter rw;
        if (rw.open(libPath, /*truncate_corrupted=*/true) != common::E_OK)
        {
            done(false, QStringLiteral("not recoverable (open failed)"));
            return;
        }
        const qint64 truncated = rw.get_truncated_size();
        if (rw.can_write())
        {
            if (rw.end_file() != common::E_OK)
            {
                rw.close();
                done(false,
                     QStringLiteral(
                         "repair sealing failed (file untouched beyond "
                         "truncation)"));
                return;
            }
            rw.close();
        }
        else
        {
            rw.close();
        }
        storage::TsFileReader reader;
        if (reader.open(libPath) != common::E_OK)
        {
            done(false, QStringLiteral("still unopenable after repair"));
            return;
        }
        reader.close();
        done(true,
             QStringLiteral("truncated %1 byte(s) of damaged tail")
                 .arg(truncated));
    }

    void query(const ParamInfo& param)
    {
        const bool multi = !files_.isEmpty();
        if (!multi && path_.isEmpty())
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

        SeriesData out;
        out.device = param.device;
        out.measurement = param.measurement;
        out.numeric = param.dataType != common::TEXT;
        out.dataType = param.dataType;

        bool haveData = false;
        int64_t minTs = std::numeric_limits<int64_t>::max();
        int64_t maxTs = std::numeric_limits<int64_t>::min();

        // Progressive delivery: flush the accumulated rows to the UI every
        // flushRows rows so the table and plot grow visibly during long
        // queries instead of freezing until completion.
        constexpr int kFlushRows = 500000;
        QElapsedTimer sinceFlush;
        sinceFlush.start();

        // Segment list: single-file mode is one segment; multi-file mode
        // concatenates every file's full series in time order.
        struct Seg
        {
            QString path;
            qint64 localOffset;
            int limit;
        };
        QVector<Seg> segs;
        qint64 totalRows = -1;

        if (!multi)
        {
            segs.append({path_, 0, /*limit < 0 = unlimited*/ -1});
        }
        else
        {
            const ParamRoute& r =
                routes_.value(param.device + QLatin1Char('\x01') +
                              param.measurement);
            if (r.perFile.isEmpty())
            {
                emit queryFailed(QStringLiteral(
                    "parameter not found in any loaded file"));
                return;
            }
            totalRows = r.totalRows;
            // Walk files in time order, taking each file's full slice of
            // this parameter. Files missing this param contribute nothing
            // and are skipped.
            for (const auto& e : r.fileOrder)
            {
                if (e.second.count <= 0)
                {
                    continue;
                }
                segs.append({files_.at(e.first), 0,
                             // Statistic counts can drift from the actual
                             // rows (e.g. unflushed tail): take everything,
                             // the reader stops at the real end.
                             -1});
            }
        }

        for (const Seg& seg : qAsConst(segs))
        {
            storage::TsFileReader reader;
            if (reader.open(pathbridge::toLibPath(seg.path).toStdString()) !=
                common::E_OK)
            {
                emit queryFailed(QStringLiteral(
                    "TsFileReader could not open %1").arg(seg.path));
                return;
            }

            // Row query: queryByRow pushes offset/limit down (chunk/page
            // level for dense devices). limit < 0 = unlimited rows, so the
            // whole segment streams without decoding anything twice.
            storage::ResultSet* result = nullptr;
            int q = common::E_OK;
            if (param.source == ParamSource::Table)
            {
                q = reader.queryByRow(param.device.toStdString(),
                                      {param.measurement.toStdString()},
                                      static_cast<int>(seg.localOffset),
                                      seg.limit, result);
            }
            else
            {
                // Tree path query: device + "." + measurement.
                std::vector<std::string> pathList{
                    param.device.toStdString() + "." +
                    param.measurement.toStdString()};
                q = reader.queryByRow(pathList,
                                      static_cast<int>(seg.localOffset),
                                      seg.limit, result);
            }
            if (q != common::E_OK || result == nullptr)
            {
                reader.close();
                emit queryFailed(QStringLiteral("query failed (code %1)").arg(q));
                return;
            }

            bool hasNext = false;
            while (true)
            {
                const int nextRet = result->next(hasNext);
                if (nextRet != common::E_OK)
                {
                    reader.destroy_query_data_set(result);
                    reader.close();
                    emit queryFailed(
                        QStringLiteral("query iteration failed (code %1)")
                            .arg(nextRet));
                    return;
                }
                if (!hasNext)
                {
                    break;
                }
                // RowRecord fields are 0-based: field 0 = time, field 1 =
                // value.
                storage::RowRecord* row = result->get_row_record();
                if (row == nullptr)
                {
                    continue;
                }
                const int64_t ts =
                    normalizeTsToMs(row->get_field(0)->get_value<int64_t>());
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

                // Supersede check inside the loop: a new selection aborts
                // this query early; the already-flushed chunks stay on
                // screen until the new query's first chunk replaces them.
                if (pendingParam_.has_value())
                {
                    reader.destroy_query_data_set(result);
                    reader.close();
                    return;
                }

                if (out.ts.size() >= kFlushRows &&
                    sinceFlush.elapsed() >= 200)
                {
                    emit valuesChunk(out, /*done=*/false);
                    out.ts.clear();
                    out.value.clear();
                    out.text.clear();
                    sinceFlush.restart();
                }
            }
            reader.destroy_query_data_set(result);
            reader.close();
        }

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

        // The exact series total comes free from the metadata statistic
        // (no drain needed).
        out.totalRows = multi ? totalRows : seriesTotalRows(param);
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

    void setFiles(const QStringList& paths)
    {
        // Multi-file mode: path_ stays empty; files_ carries the set.
        // openFiles() fills it after the per-file open succeeds.
        path_.clear();
        files_.clear();
        routes_.clear();
        overlappingParamCount_ = 0;
        haveRange_ = false;
        firstTs_ = 0;
        lastTs_ = 0;
        Q_UNUSED(paths);
    }

    void setFile(const QString& path)
    {
        // Upstream lib opens paths via CRT ::open() in the active code page;
        // bridge UTF-8 (Qt) paths to the ASCII 8.3 short path on Windows.
        path_ = pathbridge::toLibPath(path);
        files_.clear();   // single-file mode: no directory aggregation
        routes_.clear();
        overlappingParamCount_ = 0;
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
        // File list to concatenate: directory mode walks the param's route
        // in time order; single-file mode is just that file.
        QStringList sources;
        if (!files_.isEmpty())
        {
            const ParamRoute& r = routes_.value(
                param.device + QLatin1Char('\x01') + param.measurement);
            for (const auto& e : r.fileOrder)
            {
                sources << files_.at(e.first);
            }
        }
        else
        {
            sources << path_;
        }
        if (sources.isEmpty())
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

        f.write("Time,Value\n");
        for (const QString& source : qAsConst(sources))
        {
            if (!exportOneFile(source, param, f, errorText))
            {
                f.close();
                return false;
            }
        }
        f.close();
        return f.error() == QFile::NoError;
    }

private:
    // Full-range query on one file, rows written straight into f. Returns
    // false (errorText set) on open/query failure.
    bool exportOneFile(const QString& source, const ParamInfo& param,
                       QFile& f, QString* errorText)
    {
        storage::TsFileReader reader;
        if (reader.open(pathbridge::toLibPath(source).toStdString()) !=
            common::E_OK)
        {
            if (errorText)
                *errorText = QStringLiteral("open failed: %1").arg(source);
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

        bool hn = false;
        // Fixed-precision formatting, no scientific notation: values are
        // formatted with up to 17 significant digits in plain decimal and
        // trailing zeros trimmed.
        while (result->next(hn) == common::E_OK && hn)
        {
            storage::RowRecord* row = result->get_row_record();
            if (row == nullptr) continue;
            const int64_t ts =
                normalizeTsToMs(row->get_field(0)->get_value<int64_t>());
            double v = std::numeric_limits<double>::quiet_NaN();
            QString text;
            fieldToValues(row->get_field(1), v, text);
            // Seconds with millisecond precision, trailing zeros trimmed
            // (matches the table's Time column), then the CSV separator.
            char tbuf[48];
            std::snprintf(tbuf, sizeof(tbuf), "%.3f",
                          static_cast<double>(ts) / 1e3);
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
        }
        reader.destroy_query_data_set(result);
        reader.close();
        return true;
    }
    QString path_;                   // single-file mode: the file
    QStringList files_;              // directory mode: the loaded files
    QHash<QString, ParamRoute> routes_;  // key: "device\x01measurement"
    QVector<FileInfoEntry> fileEntries_;  // per-file summary (dialog)
    qint64 overlappingParamCount_ = 0;  // params whose files overlap in time
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

void TsFileDocument::openFilesAsync(const QStringList& paths)
{
    // Multi-file open bypasses repair: unreadable files are skipped silently
    // and counted in MetaInfo::skippedFileCount.
    QMetaObject::invokeMethod(worker_, [worker = worker_, paths]
    {
        worker->setFiles(paths);
        worker->openFiles(paths);
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

void TsFileDocument::repairFileAsync(const QString& path)
{
    // Thread pool, not the doc worker: the self-check scan of a large file
    // takes minutes and must not block value queries queued on the worker
    // (same concurrency model as exportCsvBlocking).
    (void)QtConcurrent::run([this, path]
    {
        Worker::repairFile(
            path,
            [this, path](bool ok, const QString& message)
            {
                // Emitted from the pool thread; slots run queued on the
                // UI thread.
                emit repairFinished(path, ok, message);
            });
    });
}

void TsFileDocument::queryValuesAsync(const ParamInfo& param){
    // Mark the request so a running query can notice it at its next flush
    // point and abort; the invoke then delivers the new one. The lambda runs
    // on the worker thread (queued), so pendingParam_ stays single-threaded.
    emit supersedeRequested(param);
    QMetaObject::invokeMethod(worker_, [worker = worker_, param]
    {
        worker->query(param);
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
