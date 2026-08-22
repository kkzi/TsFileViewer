#include "Models.h"

#include "TsFileDocument.h"

#include <cmath>

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
            deviceItem = new QStandardItem(label);
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
        auto* nameItem = new QStandardItem(p.measurement);
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
    const QString measurement = data(measurementIndex).toString();
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
    if (series_ == nullptr || series_->key() != chunk.key())
    {
        setSeries(chunk);
        return;
    }
    const int first = series_->ts.size();
    const int added = chunk.ts.size();
    series_->numeric = series_->numeric && chunk.numeric;
    series_->ts += chunk.ts;
    series_->value += chunk.value;
    series_->text += chunk.text;
    beginInsertRows(QModelIndex(), first, first + added - 1);
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
            return row + 1;
        case ColTime:
            return QString::number(series_->ts[row]);
        case ColRel:
            return QString::number((series_->ts[row] - series_->ts.first()) / 1e6, 'f', 6);
        case ColValue:
            if (row < series_->text.size())
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
        case ColRel: return QStringLiteral("Rel (s)");
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
