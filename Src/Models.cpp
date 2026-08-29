#include "Models.h"

#include "TsFileDocument.h"

#include <QDateTime>
#include <QIcon>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>

#include <cmath>
#include <limits>

namespace
{
// Flat monochrome runtime-painted icons (no asset files): match the app
// theme, distinguish folder (device/table group) from leaf (parameter).
// Cached once per size.
QIcon folderIcon()
{
    static const QIcon icon = [] {
        QPixmap pm(16, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, false);
        // Solid dark folder silhouette: tab + body.
        p.setPen(QPen(QColor(0x11, 0x14, 0x18), 1));
        p.setBrush(QBrush(QColor(0x11, 0x14, 0x18)));
        p.drawRect(1, 3, 5, 3);          // tab
        p.drawRect(1, 5, 14, 8);         // body
        p.setBrush(QBrush(QColor(0xff, 0xff, 0xff)));
        p.drawRect(2, 6, 12, 6);         // hollow interior
        p.end();
        return QIcon(pm);
    }();
    return icon;
}

QIcon leafIcon()
{
    static const QIcon icon = [] {
        QPixmap pm(16, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, false);
        // Leaf: small filled square marker (a "value point"), centered.
        p.setPen(Qt::NoPen);
        p.setBrush(QBrush(QColor(0x66, 0x70, 0x85)));
        p.drawRect(5, 5, 6, 6);
        p.setBrush(QBrush(QColor(0xff, 0xff, 0xff)));
        p.drawRect(6, 6, 4, 4);
        p.end();
        return QIcon(pm);
    }();
    return icon;
}
// Data-integrity warning color for tree rows: params whose sources carry
// contradictory footer statistics (likely a corrupted file in the set), or
// that come from a repaired file. Red stays readable on the white theme.
const QColor kSuspiciousColor(0xd9, 0x30, 0x30);
}  // namespace

// ---- ParamProxyModel --------------------------------------------------------

void ParamProxyModel::setFilter(const QString& text)
{
    filter_ = text;
    invalidateFilter();
}

bool ParamProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (filter_.isEmpty())
    {
        return true;
    }

    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    // Measurement rows match on their own name; device rows match when they
    // match themselves or any child matches.
    const QString name = sourceModel()->data(index, Qt::DisplayRole).toString();
    if (name.contains(filter_, Qt::CaseInsensitive))
    {
        return true;
    }

    const int childCount = sourceModel()->rowCount(index);
    for (int i = 0; i < childCount; ++i)
    {
        if (filterAcceptsRow(i, index))
        {
            return true;
        }
    }
    return false;
}

// ---- ParamTreeModel ---------------------------------------------------------

ParamTreeModel::ParamTreeModel(QObject* parent) : QStandardItemModel(parent)
{
    setHorizontalHeaderLabels({QStringLiteral("Parameter"), QStringLiteral("Type")});
}

void ParamTreeModel::load(const QVector<ParamInfo>& params)
{
    params_ = params;
    clear();
    setHorizontalHeaderLabels({QStringLiteral("Parameter"), QStringLiteral("Type")});

    QString currentDevice;
    QStandardItem* deviceItem = nullptr;
    bool deviceSuspicious = false;
    for (int i = 0; i < params.size(); ++i)
    {
        const ParamInfo& p = params.at(i);
        if (deviceItem == nullptr || p.device != currentDevice)
        {
            // Close out the previous device group: red label when any of its
            // params carries a data-integrity flag (whole-group hint).
            if (deviceItem != nullptr && deviceSuspicious)
            {
                deviceItem->setForeground(kSuspiciousColor);
                deviceItem->setToolTip(QStringLiteral(
                    "%1\nOne or more parameters below have conflicting "
                    "footer statistics across files (likely a corrupted "
                    "file in the set); their data is shown as-is.")
                    .arg(deviceItem->text()));
            }
            currentDevice = p.device;
            // Table params are marked so a mixed file stays readable.
            const QString label =
                p.source == ParamSource::Table
                    ? QStringLiteral("[table] ") + p.device
                    : p.device;
            deviceItem = new QStandardItem(folderIcon(), label);
            deviceItem->setEditable(false);
            if (p.source == ParamSource::Table)
            {
                deviceItem->setToolTip(QStringLiteral("table-model table: %1")
                                           .arg(p.device));
            }
            auto* empty = new QStandardItem();
            empty->setEditable(false);
            appendRow({deviceItem, empty});
            deviceSuspicious = false;
        }
        auto* nameItem = new QStandardItem(leafIcon(), p.measurement);
        nameItem->setEditable(false);
        if (p.suspicious)
        {
            nameItem->setForeground(kSuspiciousColor);
            nameItem->setToolTip(QStringLiteral(
                "%1\nThe footer statistics of this parameter's files "
                "conflict (overlapping time ranges; one file is likely "
                "corrupted) or the file was repaired after truncation. "
                "Values are shown as loaded.")
                .arg(p.measurement));
            deviceSuspicious = true;
        }
        // Index into params_: paramAt() resolves through this role instead
        // of the O(n) ownership walk.
        nameItem->setData(i, RoleParamIndex);
        auto* typeItem = new QStandardItem(TsFileNames::dataType(p.dataType));
        typeItem->setEditable(false);
        typeItem->setToolTip(QStringLiteral("type=%1 encoding=%2 compression=%3")
                                 .arg(TsFileNames::dataType(p.dataType),
                                      TsFileNames::encoding(p.encoding),
                                      TsFileNames::compression(p.compression)));
        deviceItem->appendRow({nameItem, typeItem});
    }
    // Last device group (loop closes groups on the NEXT device only).
    if (deviceItem != nullptr && deviceSuspicious)
    {
        deviceItem->setForeground(kSuspiciousColor);
        deviceItem->setToolTip(QStringLiteral(
            "%1\nOne or more parameters below have conflicting "
            "footer statistics across files (likely a corrupted "
            "file in the set); their data is shown as-is.")
            .arg(deviceItem->text()));
    }
}

QModelIndex ParamTreeModel::indexOfParam(const ParamInfo& param) const
{
    for (int r = 0; r < rowCount(); ++r)
    {
        const QModelIndex dev = index(r, 0);
        if (data(dev).toString() != param.device)
        {
            continue;
        }
        for (int c = 0; c < rowCount(dev); ++c)
        {
            const QModelIndex child = index(c, 0, dev);
            if (data(child).toString() == param.measurement)
            {
                return child;
            }
        }
    }
    return {};
}

ParamInfo ParamTreeModel::paramAt(const QModelIndex& measurementIndex) const
{
    if (!measurementIndex.isValid() || measurementIndex.model() != this)
    {
        return {};
    }
    // Always resolve through column 0 (the name column): the index the view
    // hands us may be any column (double-click on the Type column etc.).
    const QModelIndex nameIndex =
        measurementIndex.sibling(measurementIndex.row(), 0);
    // load() stored the index into params_ as a data role: O(1) resolution,
    // no ownership walk over the device runs.
    const QVariant v = data(nameIndex, RoleParamIndex);
    if (!v.isValid())
    {
        return {};
    }
    const int i = v.toInt();
    if (i < 0 || i >= params_.size())
    {
        return {};
    }
    return params_.at(i);
}

// ---- ValueTableModel --------------------------------------------------------

ValueTableModel::ValueTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void ValueTableModel::setSeries(const SeriesData& series)
{
    beginResetModel();
    series_ = std::make_unique<SeriesData>(series);
    // The final row count is usually known from the metadata statistic:
    // reserve once instead of growing (and briefly double-holding) all
    // three vectors.
    if (series.totalRows > 0)
    {
        const int n = static_cast<int>(qMin<qint64>(
            series.totalRows, std::numeric_limits<int>::max()));
        series_->ts.reserve(n);
        series_->value.reserve(n);
        series_->text.reserve(n);
    }
    endResetModel();
}

// Drop the per-row text vector when it carries nothing (numeric series):
// every empty QString still costs a pointer-sized slot per row.
void ValueTableModel::compactText()
{
    if (series_ == nullptr)
    {
        return;
    }
    const auto trim = [](QVector<QString>& v)
    {
        int last = v.size() - 1;
        while (last >= 0 && v.at(last).isEmpty())
        {
            --last;
        }
        if (last + 1 < v.size())
        {
            v.resize(last + 1);
        }
        v.squeeze();
    };
    trim(series_->text);
}

void ValueTableModel::appendChunk(const SeriesData& chunk)
{
    if (series_ == nullptr || series_->key() != chunk.key())
    {
        setSeries(chunk);
        return;
    }
    const int first = series_->ts.size();
    const int added = chunk.ts.size();
    // Final chunk carries the series metadata.
    if (chunk.totalRows > 0)
    {
        series_->totalRows = chunk.totalRows;
    }
    // Qt contract: beginInsertRows BEFORE the underlying data grows.
    beginInsertRows(QModelIndex(), first, first + added - 1);
    series_->numeric = series_->numeric && chunk.numeric;
    // text is sparse (only TEXT columns push entries); keep it row-aligned
    // with empty placeholders so data() can index it by row.
    if (series_->text.size() != static_cast<int>(series_->ts.size()))
    {
        series_->text.resize(series_->ts.size());
    }
    series_->ts += chunk.ts;
    series_->value += chunk.value;
    series_->text += chunk.text;
    if (series_->text.size() != static_cast<int>(series_->ts.size()))
    {
        series_->text.resize(series_->ts.size());
    }
    endInsertRows();
}

int ValueTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return series_ ? series_->ts.size() : 0;
}

int ValueTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColTotal;
}

QVariant ValueTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || !series_ || index.row() >= series_->ts.size())
    {
        return {};
    }
    const int row = index.row();
    if (role == Qt::TextAlignmentRole)
    {
        // No stays right-aligned; Time/Value center.
        if (index.column() == ColNo)
        {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
        return static_cast<int>(Qt::AlignHCenter | Qt::AlignVCenter);
    }
    if (role != Qt::DisplayRole)
    {
        return {};
    }

    switch (index.column())
    {
        case ColNo:
            return static_cast<qint64>(row + 1);
        case ColTime:
            return formatTimeUs(series_->ts[row]);
        case ColValue:
            if (row < series_->text.size() && !series_->text[row].isEmpty())
            {
                return series_->text[row];
            }
            return formatValue(series_->value[row]);
        default:
            return {};
    }
}

QVariant ValueTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
    {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section)
    {
        case ColNo: return QStringLiteral("No");
        case ColTime: return QStringLiteral("Time");
        case ColValue: return QStringLiteral("Value");
        default: return {};
    }
}

QString ValueTableModel::formatValue(double v)
{
    if (std::isnan(v))
    {
        return QStringLiteral("-");
    }
    // Thousand-separated plain decimal, fixed 6 decimals: no scientific
    // notation for large/small magnitudes. English locale = comma groups +
    // point decimal separator.
    static const QLocale loc(QLocale::English, QLocale::UnitedStates);
    return loc.toString(v, 'f', 6);
}

QString ValueTableModel::formatTimeUs(qint64 ts)
{
    // QDateTime resolves milliseconds only; the microsecond tail comes from
    // the raw value. Negative epochs floor toward -infinity so the fraction
    // stays in [0, 1000).
    const qint64 ms = ts / 1000;
    const int usRemainder = static_cast<int>(ts - ms * 1000);
    return QDateTime::fromMSecsSinceEpoch(ms).toString(
               QStringLiteral("hh:mm:ss")) +
           QStringLiteral(".%1").arg(usRemainder, 3, 10, QLatin1Char('0'));
}
