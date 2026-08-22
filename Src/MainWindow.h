#pragma once

#include <QMainWindow>
#include <QNetworkAccessManager>

#include "TsFileDocument.h"

#include <qcustomplot.h>

#include <QFutureWatcher>

class QLabel;
class QLineEdit;
class QProgressBar;
class QTableView;
class QTreeView;

class ParamProxyModel;
class ParamTreeModel;
class ValueTableModel;
class LoadingOverlay;

// Layout:
//   toolbar  : metainfo bar (important fields inline, details in tooltip)
//   left     : search + param tree (device > measurement)
//   right    : values table (top) / plot (bottom)
//   statusbar: load progress (busy mode while a query streams)
//
// Queries start on explicit activation (double-click / Enter / context menu
// "Load values"); plain selection only highlights, so browsing the tree
// never fires a query.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void openFile(const QString& path);

protected:
    // File label in the status bar: left-click = reveal folder,
    // right-click = copy path.
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void setupUi();
    void updateMetaBar(const MetaInfo& meta);
    void onValuesChunk(const SeriesData& chunk, bool done);
    void rebuildPlot();
    // Size the sample markers by visible density (small when dense, large
    // when zoomed into <=~1000 samples).
    void applyScatterSize(class QCPGraph* graph, int totalRows);
    void onPlotXRangeChanged(const class QCPRange& range);
    // Export the CURRENT parameter's full series (all pages) to CSV:
    // header "Time,Value", no scientific notation.
    void exportCsv();
    void clearContent();
    // Double-click / Enter / context-menu action on the param tree.
    void onParamActivated();
    // Query the current parameter at a row-based page (0-based).
    void loadPage(qint64 page);
    // GitHub releases/latest check; updates versionLabel_ on a newer tag.
    void checkForUpdate();
    void updateVersionLabel();

    // widgets
    QLineEdit* searchEdit_ = nullptr;
    QTreeView* paramTree_ = nullptr;
    QTableView* valuesTable_ = nullptr;
    QCustomPlot* plot_ = nullptr;
    QProgressBar* busy_ = nullptr;
    LoadingOverlay* overlay_ = nullptr;  // translucent loading mask
    class QCPGraph* plotGraph_ = nullptr;  // current curve (marker retuning)
    class QCPItemTracer* tracer_ = nullptr;  // marks the table's selected row
    class QPushButton* prevPage_ = nullptr;
    class QPushButton* nextPage_ = nullptr;
    class QLabel* pageInfo_ = nullptr;

    // toolbar labels: important info inline, the rest in tooltips
    QLabel* fileLabel_ = nullptr;
    QLabel* devicesLabel_ = nullptr;
    QLabel* paramsLabel_ = nullptr;
    QLabel* rangeLabel_ = nullptr;
    QLabel* analysisLabel_ = nullptr;  // status bar: per-parameter analysis
    QLabel* versionLabel_ = nullptr;   // status bar: v0.1.0 (clickable on update)
    QString latestVersion_;            // newest tag from GitHub ("" unknown)
    bool updateAvailable_ = false;
    QNetworkAccessManager net_{this};  // update check
    QLabel* codecLabel_ = nullptr;     // toolbar: current param's codec
    QLabel* paramNameLabel_ = nullptr; // paging bar: current param name
    class QPushButton* exportBtn_ = nullptr;  // paging bar: CSV export

    // models / data
    TsFileDocument* doc_ = nullptr;
    ParamTreeModel* treeModel_ = nullptr;
    ParamProxyModel* proxy_ = nullptr;
    ValueTableModel* valueModel_ = nullptr;
    ParamInfo currentParam_;
    // Current file's original path (label shows native separators).
    QString currentPath_;
    bool loading_ = false;  // a query is streaming (guards re-trigger)
    qint64 page_ = 0;       // current row page of currentParam_
    // Fit axes to data on the next rebuildPlot (new page); cleared after,
    // so progressive chunks keep the user's zoom instead of snapping back.
    bool fitOnNextRebuild_ = true;
    QFutureWatcher<bool> exportWatcher_;  // background CSV export
};
