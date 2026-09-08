#pragma once

#include <QMainWindow>
#include <QNetworkAccessManager>

#include "TsFileDocument.h"

#include <qcustomplot.h>

#include <QElapsedTimer>
#include <QFutureWatcher>

#include <limits>
#include <optional>

class QLabel;
class QLineEdit;
class QProgressBar;
class QTableView;
class QTimer;
class QTreeView;

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
// "Load values") and stream the parameter's whole series; plain selection
// only highlights, so browsing the tree never fires a query.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void openFile(const QString& path);
    // Open several files at once: params aggregate across them.
    void openFiles(const QStringList& paths);

protected:
    // File label in the status bar: left-click = reveal folder,
    // right-click = copy path.
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void setupUi();
    void updateMetaBar(const MetaInfo& meta);
    void onValuesChunk(const SeriesData& chunk, bool done);
    // Load-end work once the final chunk arrived: stats, envelope, tooltip,
    // range label.
    void finishValuesLoad(const SeriesData& series);
    void rebuildPlot();
    // Show/hide the red sample markers for the current x range: hidden when
    // more than kScatterVisibleLimit points are visible (density makes them
    // a solid band, and skipping them saves a data walk per replot).
    void applyScatterSize(class QCPGraph* graph);
    void onPlotXRangeChanged(const class QCPRange& range);
    // Zoom bounds for the x axis (wheel zoom): out is capped at 1.2x the
    // data range, in stops when fewer than kMinVisiblePoints stay visible.
    // Adjusts r into bounds; returns true when the range changed.
    bool clampXRange(class QCPRange& r) const;
    // Export the CURRENT parameter's full series to CSV:
    // header "Time,Value", no scientific notation.
    void exportCsv();
    void clearContent();
    // Detach the graph/tracer and drop the plot data containers; shared by
    // clearContent() and loadValues().
    void clearPlot();
    // Double-click / Enter / context-menu action on the param tree.
    void onParamActivated();
    // Stream the current parameter's full series into the table and plot.
    void loadValues();
    // Run the tree filter now (debounce timer vs. immediate Enter).
    void applySearch(const QString& text);
    // GitHub releases/latest check; updates versionLabel_ on a newer tag.
    void checkForUpdate();
    void updateVersionLabel();
    // Plot data management for large series: the raw points live in a
    // container shared with the graph (chunk-appended, no per-chunk copy);
    // a decimated min/max envelope is built once at load end and substituted
    // while the view covers too many points to draw raw.
    void appendPlotData(const SeriesData& chunk, bool fresh);
    QSharedPointer<QCPGraphDataContainer> buildEnvelope() const;
    // Switch the graph between raw and envelope data for the current x range.
    void updateGraphData();
    // Auto-follow: refit the y axis to the finite samples inside the current
    // x range (y is not user-zoomable, so the view always hugs the visible
    // curve).
    void rescaleYToVisible();
    // Running statistics of the current series, accumulated per chunk so
    // 10M+ row queries stay cheap.
    void resetStats();
    void accumulateStats(const SeriesData& chunk);
    // Table row -> plot sync: pan so the selected sample stays inside the
    // 10%..90% band (no zoom change).
    void syncPlotToRow(const QModelIndex& idx);
    // Table row activation (double-click / Enter): zoom to the marker-visible
    // window centered on the row.
    void activateRow(const QModelIndex& idx);
    // Plot left-click near the curve: select the nearest sample's row.
    void onPlotMousePress(QMouseEvent* ev);
    // Sync the table to a source row (plot click / arrow-key navigation):
    // select it, center it in the table when it is off-view, place the
    // tracer on its sample.
    void selectSampleRow(int row);
    // Source row of the sample nearest to time t (bisection on the sorted
    // series; -1 when the series is empty).
    int nearestSampleRow(double t) const;
    // Move the crosshair tracer to a sample (row < 0 hides tracer and
    // readout) and refresh the top-right info text.
    void updateTracer(int row);
    // Arrow navigation: Up/Down move the table selection (plot syncs via
    // selectionChanged); Left/Right step the selected sample (table row
    // follows, plot pans to keep the point in the 10%..90% band).
    void stepSelection(int key);
    // Parameter column stretches, Type column fits its content. Re-applied
    // after every model rebuild: clear() drops per-section resize modes.
    void applyTreeHeader();
    // Toolbar file label left-click: dialog listing the loaded files with
    // per-file footer statistics (name, size, devices, params, chunks...).
    void showFilesDialog();

    // widgets
    QLineEdit* searchEdit_ = nullptr;
    QTimer* searchTimer_ = nullptr;  // debounce for the tree filter
    QTreeView* paramTree_ = nullptr;
    QTableView* valuesTable_ = nullptr;
    QCustomPlot* plot_ = nullptr;
    QProgressBar* busy_ = nullptr;
    LoadingOverlay* overlay_ = nullptr;         // translucent loading mask
    class QCPGraph* plotGraph_ = nullptr;       // current curve (marker retuning)
    class QCPItemTracer* tracer_ = nullptr;     // crosshair marking the selected row
    class QCPItemText* tracerInfo_ = nullptr;   // tracer readout (top-right of plot)

    // toolbar labels: important info inline, the rest in tooltips
    QLabel* fileLabel_ = nullptr;
    QLabel* devicesLabel_ = nullptr;
    QLabel* paramsLabel_ = nullptr;
    QLabel* rangeLabel_ = nullptr;

    MetaInfo lastMeta_;  // last successful open (file dialog data source)
    int activeRepairs_ = 0;       // queued file repairs (reload when 0)
    QLabel* analysisLabel_ = nullptr;  // status bar: per-parameter analysis
    QLabel* versionLabel_ = nullptr;   // status bar: v0.1.0 (clickable on update)
    QString latestVersion_;            // newest tag from GitHub ("" unknown)
    bool updateAvailable_ = false;
    QNetworkAccessManager net_{this};  // update check
    QLabel* paramCodecLabel_ = nullptr;  // values bar: codec of the current param
    QLabel* paramNameLabel_ = nullptr; // values bar: current param name
    QLabel* paramStatLabel_ = nullptr; // values bar: rows · span · rate · min/max/mean
    class QPushButton* exportBtn_ = nullptr;  // values bar: CSV export

    // models / data
    TsFileDocument* doc_ = nullptr;
    ParamTreeModel* treeModel_ = nullptr;
    ValueTableModel* valueModel_ = nullptr;
    ParamInfo currentParam_;
    // Current file's original path (label shows native separators).
    QString currentPath_;
    // Raw plot points (shared with the graph) and the decimated overview
    // for zoomed-out views of very large series.
    QSharedPointer<QCPGraphDataContainer> plotData_;
    QSharedPointer<QCPGraphDataContainer> envelopeData_;
    bool useEnvelope_ = false;
    bool loading_ = false;  // a query is streaming (guards re-trigger)
    // Fit axes to data on the next rebuildPlot (new query); cleared after,
    // so progressive chunks keep the user's zoom instead of snapping back.
    bool fitOnNextRebuild_ = true;
    // Set while the code itself changes the table's selection (plot click ->
    // table row): the selectionChanged-driven plot sync must not fire for
    // those and zoom away from what the user sees.
    bool suppressPlotSync_ = false;
    // Streaming replot throttle: last rebuildPlot time while a query runs
    // (ms on loadClock_; -1 = next chunk replots immediately).
    QElapsedTimer loadClock_;
    qint64 lastLoadReplotMs_ = -1;
    // Last x range that passed clampXRange (zoom-in floor restores it when
    // a wheel step would drop below the visible-point minimum).
    std::optional<QCPRange> lastGoodXRange_;
    QFutureWatcher<bool> exportWatcher_;  // background CSV export

    // Running statistics for the loaded series (display + O(1) y fit while
    // a query streams).
    double statVmin_ = std::numeric_limits<double>::quiet_NaN();
    double statVmax_ = std::numeric_limits<double>::quiet_NaN();
    double statSum_ = 0;
    qint64 statFinite_ = 0;
    qint64 statNa_ = 0;
};
