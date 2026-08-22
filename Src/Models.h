#pragma once

#include <QSortFilterProxyModel>
#include <QStandardItemModel>

#include "TsFileDocument.h"

// Proxy that keeps device rows when a child measurement matches the filter
// (case-insensitive substring). Explicit recursion instead of Qt 5.10's
// recursive filtering, keeps behavior obvious.
class ParamProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setFilter(const QString& text);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString filter_;
};

// Tree model: top-level rows = devices, children = measurements.
class ParamTreeModel : public QStandardItemModel
{
    Q_OBJECT
public:
    enum Columns
    {
        ColName = 0,
        ColType,
        ColCount
    };

    explicit ParamTreeModel(QObject* parent = nullptr);

    void load(const QVector<ParamInfo>& params);
    // Measurement row for a param, or an invalid index.
    QModelIndex indexOfParam(const ParamInfo& param) const;
    ParamInfo paramAt(const QModelIndex& measurementIndex) const;
    static bool isMeasurementRow(const QModelIndex& index)
    {
        return index.isValid() && index.parent().isValid();
    }

private:
    // True when the top-level row r is the device group owning param p
    // (params_ groups consecutive entries by device in load order).
    bool owns(const ParamInfo& p, int topLevelRow) const;

    QVector<ParamInfo> params_;
};

// Table model for the selected series: No | Time (us) | Rel (s) | Value.
class ValueTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Columns
    {
        ColNo = 0,
        ColTime,
        ColRel,
        ColValue,
        ColTotal
    };

    explicit ValueTableModel(QObject* parent = nullptr);

    void setSeries(const SeriesData& series);
    // Append rows from a chunk (progressive loading). The chunk must belong
    // to the same series; a different key resets first.
    void appendChunk(const SeriesData& chunk);
    const SeriesData* series() const { return series_.get(); }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    static QString formatValue(double v);

    std::unique_ptr<SeriesData> series_;
};
