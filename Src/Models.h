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
    // Data role carrying the index into params_ (avoids the O(n) ownership
    // walk of paramAt on every activation).
    enum { RoleParamIndex = Qt::UserRole + 1 };
    static bool isMeasurementRow(const QModelIndex& index)
    {
        return index.isValid() && index.parent().isValid();
    }

private:
    QVector<ParamInfo> params_;
};

// Table model for the selected series: No | Time | Value.
class ValueTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Columns
    {
        ColNo = 0,
        ColTime,
        ColValue,
        ColTotal
    };

    explicit ValueTableModel(QObject* parent = nullptr);

    void setSeries(const SeriesData& series);
    // Append rows from a chunk (progressive loading). The chunk must belong
    // to the same series; a different key resets first.
    void appendChunk(const SeriesData& chunk);
    // Release the per-row text vector's slack after a load: numeric series
    // keep no text storage at all (~bytes/row otherwise).
    void compactText();
    const SeriesData* series() const { return series_.get(); }
    // Numeric cell formatter (NaN -> "-"): thousand separators, fixed 6
    // decimals, no scientific notation. Shared with the plot tracer readout
    // and the stats labels.
    static QString formatValue(double v);
    // Timestamp (microseconds since epoch) -> "hh:mm:ss.zzzzzz" (clock time,
    // date omitted). The microsecond tail is appended manually because
    // QDateTime only resolves milliseconds.
    static QString formatTimeUs(qint64 ts);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:

    std::unique_ptr<SeriesData> series_;
};
