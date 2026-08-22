#pragma once

#include <QMainWindow>

#include "TsFileDocument.h"

#include <qcustomplot.h>

class QLabel;
class QLineEdit;
class QProgressBar;
class QTableView;
class QTreeView;

class ParamProxyModel;
class ParamTreeModel;
class ValueTableModel;

// Layout per review:
//   toolbar  : metainfo bar (important fields inline, details in tooltip)
//   left     : search + param tree (device > measurement)
//   right    : values table (top) / plot (bottom)
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void openFile(const QString& path);

private:
    void setupUi();
    void updateMetaBar(const MetaInfo& meta);
    void showValues(const SeriesData& series);
    void clearContent();
    void onSelectionChanged();

    // widgets
    QLineEdit* searchEdit_ = nullptr;
    QTreeView* paramTree_ = nullptr;
    QTableView* valuesTable_ = nullptr;
    QCustomPlot* plot_ = nullptr;
    QProgressBar* busy_ = nullptr;

    // toolbar labels: important info inline, the rest in tooltips
    QLabel* fileLabel_ = nullptr;
    QLabel* devicesLabel_ = nullptr;
    QLabel* tablesLabel_ = nullptr;
    QLabel* paramsLabel_ = nullptr;
    QLabel* rangeLabel_ = nullptr;

    // models / data
    TsFileDocument* doc_ = nullptr;
    ParamTreeModel* treeModel_ = nullptr;
    ParamProxyModel* proxy_ = nullptr;
    ValueTableModel* valueModel_ = nullptr;
    ParamInfo currentParam_;
};
