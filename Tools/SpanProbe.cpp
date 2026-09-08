// Dev probe: dump per-file footer statistics vs actual first/last row
// timestamps for one measurement, to diagnose multi-file concat ordering
// (negative span reports). Usage:
//   SpanProbe <dir> <device> <measurement>        full row scan per file
//   SpanProbe --list <file>                       list device.measurement
//   SpanProbe --stats <dir> <device> <measurement>  footer stats only (fast)
//   SpanProbe --chunks <dir> <device> <measurement> per-chunk stats (fast)
//   SpanProbe --range <startMs> <endMs> <dir> <device> <measurement>
//                                                 rows inside a time window
//   SpanProbe --rows offset,limit <dir> <device> <measurement>
//                                                 row window via offset/limit
#include "reader/tsfile_reader.h"

#include "common/tsfile_common.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDebug>

#include <algorithm>
#include <cmath>
#include <limits>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument("dir", "directory with .tsfile files");
    parser.addPositionalArgument("device", "device name");
    parser.addPositionalArgument("measurement", "measurement name");
    QCommandLineOption listOpt(QStringLiteral("list"),
                               QStringLiteral("list device.measurement names and exit"));
    parser.addOption(listOpt);
    QCommandLineOption fileInfoOpt(QStringLiteral(
        "fileinfo"),
        QStringLiteral("per-file footer summary exactly as the viewer's "
                       "files dialog computes it (devices/tables/params/"
                       "chunks/rows/time range): fileinfo <file>..."));
    parser.addOption(fileInfoOpt);
    QCommandLineOption statsOpt(QStringLiteral("stats"),
                                QStringLiteral("footer statistics only (fast, no row scan)"));
    parser.addOption(statsOpt);
    QCommandLineOption chunksOpt(QStringLiteral("chunks"),
                                 QStringLiteral("per-chunk statistics (fast, no row decode)"));
    parser.addOption(chunksOpt);
    QCommandLineOption rangeOpt(
        QStringLiteral("range"),
        QStringLiteral("time-window row query: only chunks overlapping "
                       "[start,end] are decoded"),
        QStringLiteral("startMs,endMs"));
    parser.addOption(rangeOpt);
    QCommandLineOption rowsOpt(
        QStringLiteral("rows"),
        QStringLiteral("queryByRow offset/limit window; prints every row"),
        QStringLiteral("offset,limit"));
    parser.addOption(rowsOpt);
    parser.process(app);

    if (parser.isSet(fileInfoOpt))
    {
        // Mirrors TsFileDocument::appendFileToRoutes() field by field so
        // the dialog's numbers can be verified from the command line.
        for (const QString& path : parser.positionalArguments())
        {
            storage::TsFileReader reader;
            if (reader.open(path.toStdString()) != common::E_OK)
            {
                printf("%s: OPEN FAILED\n", qPrintable(path));
                continue;
            }
            const auto meta = reader.get_timeseries_metadata();
            int deviceCount = 0, tableCount = 0, paramCount = 0;
            qint64 chunkCount = 0, rowCount = 0;
            qint64 firstTs = 0, lastTs = 0;
            bool haveRange = false;
            for (const auto& kv : meta)
            {
                ++deviceCount;
                for (const auto& tsip : kv.second)
                {
                    ++paramCount;
                    if (const storage::Statistic* st = tsip->get_statistic())
                    {
                        rowCount += qMax<qint64>(st->get_count(), 0);
                        if (st->get_count() > 0)
                        {
                            if (!haveRange)
                            {
                                firstTs = st->start_time_;
                                lastTs = st->get_end_time();
                                haveRange = true;
                            }
                            else
                            {
                                firstTs = qMin(firstTs, st->start_time_);
                                lastTs = qMax(lastTs, st->get_end_time());
                            }
                        }
                    }
                    if (auto* list = tsip->is_aligned()
                                         ? tsip->get_time_chunk_meta_list()
                                         : tsip->get_chunk_meta_list())
                    {
                        chunkCount += list->size();
                    }
                }
            }
            for (const auto& ts : reader.get_all_table_schemas())
            {
                if (ts == nullptr || ts->is_virtual_table())
                {
                    continue;
                }
                ++tableCount;
                const auto categories = ts->get_column_categories();
                for (const auto c : categories)
                {
                    if (c == common::ColumnCategory::FIELD)
                    {
                        ++paramCount;
                    }
                }
            }
            printf("%s\n  devices=%d tables=%d params=%d chunks=%lld rows=%lld"
                   " range=%s\n",
                   qPrintable(path), deviceCount, tableCount, paramCount,
                   chunkCount, rowCount,
                   haveRange ? qPrintable(QString::number(firstTs) + ".." +
                                          QString::number(lastTs))
                             : "-");
            reader.close();
        }
        return 0;
    }

    if (parser.isSet(listOpt))
    {
        storage::TsFileReader reader;
        const QString path = parser.positionalArguments().isEmpty()
                                 ? QString()
                                 : parser.positionalArguments().first();
        if (reader.open(path.toStdString()) != common::E_OK)
        {
            printf("open failed\n");
            return 1;
        }
        const auto meta = reader.get_timeseries_metadata();
        for (const auto& kv : meta)
        {
            const QString dev = QString::fromStdString(
                kv.first->get_device_name());
            for (const auto& tsip : kv.second)
            {
                printf("root.%s.%s\n", qPrintable(dev),
                       qPrintable(QString::fromStdString(
                           tsip->get_measurement_name().to_std_string())));
            }
        }
        reader.close();
        return 0;
    }

    const QStringList args = parser.positionalArguments();
    if (args.size() < 3)
    {
        parser.showHelp(1);
    }
    const QString dir = args.at(0);
    const QString device = args.at(1);
    const QString measurement = args.at(2);
    const QString fullPath = device + "." + measurement;

    const QStringList files = QDir(dir).entryList(
        {QStringLiteral("*.tsfile")}, QDir::Files, QDir::Name);

    if (parser.isSet(rowsOpt))
    {
        // offset/limit pushdown: whole chunks are skipped without decode, so
        // a 16-row window at the tail costs one chunk.
        const QStringList parts = parser.value(rowsOpt).split(QLatin1Char(','));
        if (parts.size() != 2)
        {
            printf("--rows needs offset,limit\n");
            return 1;
        }
        const int offset = parts.at(0).toInt();
        const int limit = parts.at(1).toInt();
        for (const QString& name : files)
        {
            storage::TsFileReader reader;
            if (reader.open((dir + "/" + name).toStdString()) != common::E_OK)
            {
                printf("%s: OPEN FAILED\n", qPrintable(name));
                continue;
            }
            std::vector<std::string> pathList{fullPath.toStdString()};
            storage::ResultSet* result = nullptr;
            printf("%s  offset=%d limit=%d\n", qPrintable(name), offset, limit);
            if (reader.queryByRow(pathList, offset, limit, result) ==
                    common::E_OK &&
                result != nullptr)
            {
                bool hasNext = false;
                int i = 0;
                while (result->next(hasNext) == common::E_OK && hasNext)
                {
                    storage::RowRecord* row = result->get_row_record();
                    if (row == nullptr) continue;
                    const qint64 ts = row->get_field(0)->get_value<int64_t>();
                    const QDateTime dt =
                        QDateTime::fromMSecsSinceEpoch(ts);
                    printf("  row %-5d %s (%lld)\n", offset + i,
                           qPrintable(dt.toString(
                               QStringLiteral("hh:mm:ss.zzz"))),
                           static_cast<long long>(ts));
                    ++i;
                }
                reader.destroy_query_data_set(result);
            }
            reader.close();
            fflush(stdout);
        }
        return 0;
    }

    if (parser.isSet(rangeOpt))
    {
        // Time-window query: the reader prunes by chunk statistics, so a
        // narrow window costs one chunk decode instead of a full scan.
        // Used to tell a bogus chunk statistic from genuine out-of-order rows.
        const QStringList parts =
            parser.value(rangeOpt).split(QLatin1Char(','));
        if (parts.size() != 2)
        {
            printf("--range needs startMs,endMs\n");
            return 1;
        }
        const qint64 startMs = parts.at(0).toLongLong();
        const qint64 endMs = parts.at(1).toLongLong();
        for (const QString& name : files)
        {
            storage::TsFileReader reader;
            if (reader.open((dir + "/" + name).toStdString()) != common::E_OK)
            {
                printf("%s: OPEN FAILED\n", qPrintable(name));
                continue;
            }
            std::vector<std::string> pathList{fullPath.toStdString()};
            storage::ResultSet* result = nullptr;
            qint64 n = 0, first = 0, last = 0;
            if (reader.query(pathList, startMs, endMs, result) == common::E_OK &&
                result != nullptr)
            {
                bool hasNext = false;
                while (result->next(hasNext) == common::E_OK && hasNext)
                {
                    storage::RowRecord* row = result->get_row_record();
                    if (row == nullptr) continue;
                    const qint64 ts = row->get_field(0)->get_value<int64_t>();
                    if (n == 0) first = ts;
                    last = ts;
                    ++n;
                }
                reader.destroy_query_data_set(result);
            }
            reader.close();
            printf("%-52s rows in window=%lld", qPrintable(name),
                   static_cast<long long>(n));
            if (n > 0)
            {
                printf("  first=%lld last=%lld", static_cast<long long>(first),
                       static_cast<long long>(last));
            }
            printf("\n");
            fflush(stdout);
        }
        return 0;
    }

    if (parser.isSet(chunksOpt))
    {
        // Chunk-level statistics: the timeseries index holds one ChunkMeta
        // per chunk, each with its own [start, end, count]. Decoding nothing,
        // this shows the *internal* order of a file, which the aggregated
        // per-file statistic (a min/max merge) cannot reveal.
        auto fmt = [](qint64 ms)
        {
            const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms);
            return dt.toString(QStringLiteral("hh:mm:ss.zzz")).toStdString() +
                   QString(QStringLiteral(" (%1)")).arg(ms).toStdString();
        };
        for (const QString& name : files)
        {
            storage::TsFileReader reader;
            if (reader.open((dir + "/" + name).toStdString()) != common::E_OK)
            {
                printf("%s: OPEN FAILED\n", qPrintable(name));
                continue;
            }
            const auto meta = reader.get_timeseries_metadata();
            for (const auto& kv : meta)
            {
                if (QString::fromStdString(kv.first->get_device_name()) != device)
                {
                    continue;
                }
                for (const auto& tsip : kv.second)
                {
                    if (QString::fromStdString(
                            tsip->get_measurement_name().to_std_string()) != measurement)
                    {
                        continue;
                    }
                    printf("\n%s  (aligned=%d)\n", qPrintable(name),
                           tsip->is_aligned() ? 1 : 0);
                    if (const storage::Statistic* st = tsip->get_statistic())
                    {
                        printf("  index stat: %s .. %s  count=%d\n",
                               fmt(st->start_time_).c_str(),
                               fmt(st->get_end_time()).c_str(), st->get_count());
                    }
                    // Aligned series keep the timestamps in the time chunk
                    // list; plain series in the single chunk list.
                    auto* list = tsip->is_aligned()
                                     ? tsip->get_time_chunk_meta_list()
                                     : tsip->get_chunk_meta_list();
                    if (list == nullptr)
                    {
                        printf("  (no chunk meta list)\n");
                        continue;
                    }
                    int i = 0;
                    qint64 prevEnd = std::numeric_limits<qint64>::min();
                    for (auto it = list->begin(); it != list->end(); it++, ++i)
                    {
                        storage::ChunkMeta* cm = it.get();
                        if (cm == nullptr || cm->statistic_ == nullptr)
                        {
                            printf("  chunk %-3d (no statistic)\n", i);
                            continue;
                        }
                        const qint64 s = cm->statistic_->start_time_;
                        const qint64 e = cm->statistic_->get_end_time();
                        const bool back = prevEnd != std::numeric_limits<qint64>::min() &&
                                          s <= prevEnd;
                        printf("  chunk %-3d %s .. %s  count=%-7d offset=%lld%s\n",
                               i, fmt(s).c_str(), fmt(e).c_str(),
                               cm->statistic_->get_count(),
                               static_cast<long long>(cm->offset_of_chunk_header_),
                               back ? "  <== BACK JUMP" : "");
                        prevEnd = std::max(prevEnd, e);
                    }
                }
            }
            reader.close();
        }
        return 0;
    }

    if (parser.isSet(statsOpt))
    {
        // Footer statistics only: which files contain the param, and their
        // recorded [start, end, count]. No data decode, seconds per file.
        struct SRow
        {
            QString file;
            qint64 start = 0, end = 0, count = 0;
        };
        QVector<SRow> srows;
        for (const QString& name : files)
        {
            storage::TsFileReader reader;
            if (reader.open((dir + "/" + name).toStdString()) != common::E_OK)
            {
                printf("%s: OPEN FAILED\n", qPrintable(name));
                continue;
            }
            const auto meta = reader.get_timeseries_metadata();
            for (const auto& kv : meta)
            {
                if (QString::fromStdString(kv.first->get_device_name()) != device)
                {
                    continue;
                }
                for (const auto& tsip : kv.second)
                {
                    if (QString::fromStdString(
                            tsip->get_measurement_name().to_std_string()) != measurement)
                    {
                        continue;
                    }
                    SRow r;
                    r.file = name;
                    if (const storage::Statistic* st = tsip->get_statistic())
                    {
                        r.start = st->start_time_;
                        r.end = st->get_end_time();
                        r.count = st->get_count();
                    }
                    srows.push_back(r);
                }
            }
            reader.close();
            printf(".");
            fflush(stdout);
        }
        printf("\n\n");
        auto fmt = [](qint64 ms)
        {
            const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms);
            return dt.toString(QStringLiteral("hh:mm:ss.zzz")).toStdString() +
                   QString(QStringLiteral(" (%1)")).arg(ms).toStdString();
        };
        for (const SRow& r : srows)
        {
            printf("%-52s %s .. %s  count=%lld  span=%.6fs\n",
                   qPrintable(r.file),
                   fmt(r.start).c_str(), fmt(r.end).c_str(),
                   static_cast<long long>(r.count),
                   static_cast<double>(r.end - r.start) / 1e3);
        }
        // Sorted view (what routes_ uses for concat order) + overlap check.
        std::sort(srows.begin(), srows.end(),
                  [](const SRow& a, const SRow& b) { return a.start < b.start; });
        printf("\nconcat order by stat start:\n");
        for (int i = 0; i < srows.size(); ++i)
        {
            const bool back = i > 0 && srows[i].start <= srows[i - 1].end;
            printf("  %s%s\n", qPrintable(srows[i].file),
                   back ? "   <== overlaps previous" : "");
        }
        printf("\nfiles with param: %lld / %lld\n",
               static_cast<long long>(srows.size()),
               static_cast<long long>(files.size()));
        return 0;
    }

    struct Row
    {
        QString file;
        qint64 statStart = 0, statEnd = 0;
        qint64 statCount = 0;
        qint64 dataFirst = 0, dataLast = 0;
        qint64 dataCount = 0;
    };
    QVector<Row> rows;

    for (const QString& name : files)
    {
        const QString path = dir + "/" + name;
        Row r;
        r.file = name;

        storage::TsFileReader reader;
        if (reader.open(path.toStdString()) != common::E_OK)
        {
            printf("%s: OPEN FAILED\n", qPrintable(name));
            continue;
        }
        // Footer statistics (what openFiles/routing uses).
        const auto meta = reader.get_timeseries_metadata();
        bool found = false;
        for (const auto& kv : meta)
        {
            if (QString::fromStdString(kv.first->get_device_name()) != device)
            {
                continue;
            }
            for (const auto& tsip : kv.second)
            {
                if (QString::fromStdString(
                        tsip->get_measurement_name().to_std_string()) != measurement)
                {
                    continue;
                }
                found = true;
                if (const storage::Statistic* st = tsip->get_statistic())
                {
                    r.statStart = st->start_time_;
                    r.statEnd = st->get_end_time();
                    r.statCount = st->get_count();
                }
            }
        }
        if (!found)
        {
            reader.close();
            continue;  // file has no such param
        }
        // Actual endpoints (what the query concatenates): two 1-row point
        // queries, offset/limit pushed down to the chunk level — no full
        // series decode. statCount carries the row count for the last-row
        // offset.
        std::vector<std::string> pathList{fullPath.toStdString()};
        auto rowAt = [&](qint64 offset, qint64& outTs) -> bool
        {
            storage::ResultSet* result = nullptr;
            if (reader.queryByRow(pathList,
                                  static_cast<int>(qMin<qint64>(
                                      offset, std::numeric_limits<int>::max())),
                                  1, result) != common::E_OK ||
                result == nullptr)
            {
                return false;
            }
            bool ok = false;
            bool hasNext = false;
            if (result->next(hasNext) == common::E_OK && hasNext)
            {
                storage::RowRecord* row = result->get_row_record();
                if (row != nullptr && row->get_field(0) != nullptr)
                {
                    outTs = row->get_field(0)->get_value<int64_t>();
                    ok = true;
                }
            }
            reader.destroy_query_data_set(result);
            return ok;
        };
        r.dataCount = r.statCount;
        rowAt(0, r.dataFirst);
        if (r.statCount > 1)
        {
            rowAt(r.statCount - 1, r.dataLast);
        }
        else
        {
            r.dataLast = r.dataFirst;
        }
        reader.close();
        rows.push_back(r);
        printf(".");
        fflush(stdout);
    }
    printf("\n\n");

    auto fmt = [](qint64 ms)
    {
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms);
        return dt.toString(QStringLiteral("hh:mm:ss.zzz")).toStdString() +
               QString(QStringLiteral(" (%1)")).arg(ms).toStdString();
    };
    for (const Row& r : rows)
    {
        printf("%s\n  stat: %s .. %s count=%lld\n  data: %s .. %s count=%lld  delta first=%lld last=%lld\n",
               qPrintable(r.file),
               fmt(r.statStart).c_str(), fmt(r.statEnd).c_str(),
               static_cast<long long>(r.statCount),
               fmt(r.dataFirst).c_str(), fmt(r.dataLast).c_str(),
               static_cast<long long>(r.dataCount),
               static_cast<long long>(r.dataFirst - r.statStart),
               static_cast<long long>(r.dataLast - r.statEnd));
    }

    // Concatenation in stat-start order (what the viewer does): report any
    // place where the next file's data starts before the previous file's data
    // ended (=> non-monotonic concatenation, negative span if it crosses the
    // first file's first row... actually report back-jumps).
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.statStart < b.statStart; });
    printf("\nconcat order by stat start:\n");
    qint64 prevDataLast = std::numeric_limits<qint64>::min();
    qint64 globalFirst = std::numeric_limits<qint64>::max();
    qint64 globalLast = std::numeric_limits<qint64>::min();
    for (const Row& r : rows)
    {
        if (r.dataCount == 0) continue;
        const qint64 jump = r.dataFirst - prevDataLast;
        printf("  %s first=%lld %s\n", qPrintable(r.file),
               static_cast<long long>(r.dataFirst),
               prevDataLast == std::numeric_limits<qint64>::min()
                   ? "" : (jump < 0 ? "<== BACK JUMP" : ""));
        prevDataLast = std::max(prevDataLast, r.dataLast);
        globalFirst = std::min(globalFirst, r.dataFirst);
        globalLast = std::max(globalLast, r.dataLast);
    }
    printf("\nglobal span (min first .. max last) = %lld ms\n",
           static_cast<long long>(globalLast - globalFirst));
    return 0;
}
