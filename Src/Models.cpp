#include "Models.h"

#include "TsFileDocument.h"

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>

#include <cmath>

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
    for (const auto& p : params)
    {
        if (deviceItem == nullptr || p.device != currentDevice)
        {
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
        }
        auto* nameItem = new QStandardItem(leafIcon(), p.measurement);
        nameItem->setEditable(false);
        auto* typeItem = new QStandardItem(TsFileNames::dataType(p.dataType));
        typeItem->setEditable(false);
        typeItem->setToolTip(QStringLiteral("type=%1 encoding=%2 compression=%3")
                                 .arg(TsFileNames::dataType(p.dataType),
                                      TsFileNames::encoding(p.encoding),
                                      TsFileNames::compression(p.compression)));
        deviceItem->appendRow({nameItem, typeItem});
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
    const QModelIndex parent = measurementIndex.parent();
    if (!parent.isValid())
    {
        return {};
    }
    // Always resolve through column 0 (the name column): the index the view
    // hands us may be any column (double-click on the Type column etc.).
    const QModelIndex nameIndex = measurementIndex.sibling(measurementIndex.row(), 0);
    const QString measurement = data(nameIndex, Qt::DisplayRole).toString();
    for (const auto& p : params_)
    {
        // Compare the measurement plus the owning ParamInfo instead of the
        // displayed device label (table groups carry a "[table] " prefix).
        if (p.measurement == measurement && owns(p, parent.row()))
        {
            return p;
        }
    }
    return {};
}

// Row-based ownership check: the load() loop groups consecutive params by
// device in the same order, so top-level row r maps to the r-th device run.
bool ParamTreeModel::owns(const ParamInfo& p, int topLevelRow) const
{
    if (topLevelRow < 0)
    {
        return false;
    }
    QString device;
    for (const auto& q : params_)
    {
        if (q.device != device)
        {
            device = q.device;
            if (--topLevelRow < 0)
            {
                return device == p.device;
            }
        }
    }
    return false;
}

// ---- ValueTableModel --------------------------------------------------------

ValueTableModel::ValueTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void ValueTableModel::setSeries(const SeriesData& series)
{
    beginResetModel();
    series_ = std::make_unique<SeriesData>(series);
    endResetModel();
}

void ValueTableModel::appendChunk(const SeriesData& chunk)
{
    if (series_ == nullptr || series_->key() != chunk.key() ||
        series_->offset != chunk.offset)
    {
        setSeries(chunk);
        return;
    }
    const int first = series_->ts.size();
    const int added = chunk.ts.size();
    // Final chunk carries the page metadata.
    if (chunk.hasMore)
    {
        series_->hasMore = chunk.hasMore;
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
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role != Qt::DisplayRole)
    {
        return {};
    }

    switch (index.column())
    {
        case ColNo:
            return static_cast<qint64>(row + 1) +
                   (series_->offset > 0 ? series_->offset : 0);
        case ColTime:
            return QString::number(series_->ts[row]);
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
        case ColTime: return QStringLiteral("Time (us)");
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
    // Round-trip precision like TsFileStat's %.17g.
    return QString::number(v, 'g', 17);
}
