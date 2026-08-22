#include "TsFileDocument.h"

#include "common/record.h"
#include "common/tsfile_common.h"
#include "reader/tsfile_reader.h"
#include "reader/tsfile_tree_reader.h"

#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>
#include <limits>

namespace
{
QString dataTypeName(int t)
{
    using namespace common;
    switch (t)
    {
        case BOOLEAN: return QStringLiteral("BOOLEAN");
        case INT32: return QStringLiteral("INT32");
        case INT64: return QStringLiteral("INT64");
        case FLOAT: return QStringLiteral("FLOAT");
        case DOUBLE: return QStringLiteral("DOUBLE");
        case TEXT: return QStringLiteral("TEXT");
        default: return QStringLiteral("type(%1)").arg(t);
    }
}

QString encodingName(int e)
{
    using namespace common;
    switch (e)
    {
        case PLAIN: return QStringLiteral("PLAIN");
        case TS_2DIFF: return QStringLiteral("TS_2DIFF");
        case RLE: return QStringLiteral("RLE");
        case GORILLA: return QStringLiteral("GORILLA");
        default: return QStringLiteral("enc(%1)").arg(e);
    }
}

QString compressionName(int c)
{
    using namespace common;
    switch (c)
    {
        case UNCOMPRESSED: return QStringLiteral("UNCOMPRESSED");
        case SNAPPY: return QStringLiteral("SNAPPY");
        case GZIP: return QStringLiteral("GZIP");
        case LZ4: return QStringLiteral("LZ4");
        default: return QStringLiteral("cmp(%1)").arg(c);
    }
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
    void open(const QString& path)
    {
        const std::string utf8Path = path.toStdString();

        // Two-stage open mirrors TsFileStat: plain reader validates the file
        // (footer present), then the tree reader walks devices and schemas.
        // Read-only: the viewer never repairs.
        storage::TsFileReader probe;
        const int r = probe.open(utf8Path);
        if (r != common::E_OK)
        {
            emit openFailed(QStringLiteral("open failed (code %1); file was not modified")
                                .arg(r));
            return;
        }
        probe.close();

        storage::TsFileTreeReader tree;
        if (tree.open(utf8Path) != common::E_OK)
        {
            emit openFailed(QStringLiteral("TsFileTreeReader could not open file"));
            return;
        }

        MetaInfo meta;
        meta.path = path;
        QVector<ParamInfo> params;
        auto devices = tree.get_all_device_ids();
        meta.deviceCount = static_cast<int>(devices.size());
        for (const auto& device : devices)
        {
            meta.devices << QString::fromStdString(device);
            auto schemas = tree.get_device_schema(device);
            for (const auto& s : schemas)
            {
                ParamInfo p;
                p.device = QString::fromStdString(device);
                p.measurement = QString::fromStdString(s.measurement_name_);
                p.dataType = static_cast<int>(s.data_type_);
                p.encoding = static_cast<int>(s.encoding_);
                p.compression = static_cast<int>(s.compression_type_);
                params.push_back(p);
            }
        }
        tree.close();

        if (meta.deviceCount == 0)
        {
            emit openFailed(QStringLiteral("file has no devices (no tree-model data)"));
            return;
        }

        meta.paramCount = static_cast<int>(params.size());
        const QFileInfo fi(path);
        meta.fileSize = fi.size();

        emit opened(meta, params);
    }

    void query(const ParamInfo& param)
    {
        if (path_.isEmpty())
        {
            emit queryFailed(QStringLiteral("no file open"));
            return;
        }

        storage::TsFileTreeReader tree;
        if (tree.open(path_.toStdString()) != common::E_OK)
        {
            emit queryFailed(QStringLiteral("TsFileTreeReader could not open file"));
            return;
        }

        SeriesData out;
        out.device = param.device;
        out.measurement = param.measurement;

        storage::ResultSet* result = nullptr;
        const int q = tree.query({param.device.toStdString()},
                                 {param.measurement.toStdString()},
                                 std::numeric_limits<int64_t>::min(),
                                 std::numeric_limits<int64_t>::max(), result);
        if (q != common::E_OK || result == nullptr)
        {
            tree.close();
            emit queryFailed(QStringLiteral("query failed (code %1)").arg(q));
            return;
        }

        out.numeric = param.dataType != common::TEXT;
        bool haveData = false;
        int64_t minTs = std::numeric_limits<int64_t>::max();
        int64_t maxTs = std::numeric_limits<int64_t>::min();
        bool hasNext = false;
        while (true)
        {
            const int nextRet = result->next(hasNext);
            if (nextRet != common::E_OK)
            {
                tree.destroy_query_data_set(result);
                tree.close();
                emit queryFailed(
                    QStringLiteral("query iteration failed (code %1)").arg(nextRet));
                return;
            }
            if (!hasNext)
            {
                break;
            }
            // Single-measurement tree result: field 0 = time, field 1 = value
            // (ResultSet columns are 1-based; RowRecord fields are 0-based).
            storage::RowRecord* row = result->get_row_record();
            if (row == nullptr)
            {
                continue;
            }
            const int64_t ts = row->get_field(0)->get_value<int64_t>();
            double v = std::numeric_limits<double>::quiet_NaN();
            storage::Field* f = row->get_field(1);
            if (f != nullptr && f->type_ != common::NULL_TYPE)
            {
                switch (f->type_)
                {
                    case common::BOOLEAN:
                        v = f->value_.bval_ ? 1.0 : 0.0;
                        out.numeric = false;
                        break;
                    case common::INT32: v = static_cast<double>(f->value_.ival_); break;
                    case common::INT64:
                    case common::TIMESTAMP: v = static_cast<double>(f->value_.lval_); break;
                    case common::FLOAT: v = static_cast<double>(f->value_.fval_); break;
                    case common::DOUBLE: v = f->value_.dval_; break;
                    default: break;  // text and anything else stay NaN
                }
            }
            out.ts.push_back(ts);
            out.value.push_back(v);
            if (!haveData || ts < minTs) minTs = ts;
            if (!haveData || ts > maxTs) maxTs = ts;
            haveData = true;
        }
        tree.destroy_query_data_set(result);
        tree.close();

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

        emit valuesReady(out);
        emit timeRangeKnown(firstTs_, lastTs_, haveRange_);
    }

    void setFile(const QString& path)
    {
        path_ = path;
        haveRange_ = false;
        firstTs_ = 0;
        lastTs_ = 0;
    }

signals:
    void opened(const MetaInfo& meta, const QVector<ParamInfo>& params);
    void openFailed(const QString& error);
    void valuesReady(const SeriesData& series);
    void queryFailed(const QString& error);
    void timeRangeKnown(qint64 firstTs, qint64 lastTs, bool haveRange);

private:
    QString path_;
    bool haveRange_ = false;
    qint64 firstTs_ = 0;
    qint64 lastTs_ = 0;
};

#include "TsFileDocument.moc"

TsFileDocument::TsFileDocument(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<ParamInfo>("ParamInfo");
    qRegisterMetaType<MetaInfo>("MetaInfo");
    qRegisterMetaType<SeriesData>("SeriesData");
    qRegisterMetaType<QVector<ParamInfo>>("QVector<ParamInfo>");

    static bool connected = false;  // worker signals need the meta types above
    Q_UNUSED(connected);

    worker_ = new Worker();
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    // Forward worker signals to the document's own signals (queued because
    // the worker lives on thread_).
    connect(worker_, &Worker::opened, this, &TsFileDocument::opened);
    connect(worker_, &Worker::openFailed, this, &TsFileDocument::openFailed);
    connect(worker_, &Worker::valuesReady, this, &TsFileDocument::valuesReady);
    connect(worker_, &Worker::queryFailed, this, &TsFileDocument::queryFailed);
    thread_.start();
}

TsFileDocument::~TsFileDocument()
{
    shutdown();
}

void TsFileDocument::openAsync(const QString& path)
{
    QMetaObject::invokeMethod(worker_, [worker = worker_, path]
    {
        worker->setFile(path);
        worker->open(path);
    });
}

void TsFileDocument::queryValuesAsync(const ParamInfo& param)
{
    QMetaObject::invokeMethod(worker_, [worker = worker_, param] { worker->query(param); });
}

void TsFileDocument::shutdown()
{
    if (thread_.isRunning())
    {
        thread_.quit();
        thread_.wait();
    }
}
