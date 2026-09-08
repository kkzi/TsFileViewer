#include "MainWindow.h"

#include "Models.h"
#include "LoadingOverlay.h"
#include "Version.h"

#include <qcustomplot.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QKeyEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressBar>
#include <QPushButton>
#include <QtConcurrent>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace
{
QString humanSize(qint64 bytes)
{
    const double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0;
    if (bytes >= gb) return QString::number(bytes / gb, 'f', 2) + QStringLiteral(" GB");
    if (bytes >= mb) return QString::number(bytes / mb, 'f', 1) + QStringLiteral(" MB");
    if (bytes >= kb) return QString::number(bytes / kb, 'f', 1) + QStringLiteral(" KB");
    return QString::number(bytes) + QStringLiteral(" B");
}

// Plot performance for very large series: QCustomPlot's adaptive sampling
// still walks every visible point per replot, so multi-million-point views
// lag on drag/zoom. Above this many visible points the graph switches to a
// decimated envelope; below it, the raw points draw directly.
constexpr qint64 kRawVisibleLimit = 2000000;
// Envelope buckets (each contributes first/min/max/last, roughly 3 points).
constexpr qint64 kEnvelopeBuckets = 1 << 18;
// Above this many visible points the red sample markers are hidden: at that
// density they overlap into a solid band anyway, and dropping them skips
// the scatter pass's second O(visible) data walk per replot.
constexpr qint64 kScatterVisibleLimit = 2000;
// Zoom bounds: zooming out stops at data range * 1.2 (80%/120% of the data
// min/max, so the curve never shrinks to a sliver in empty space), zooming
// in stops when fewer than this many points stay on screen (viewing less
// than a handful of samples has no information left).
constexpr qint64 kMinVisiblePoints = 10;

// Timestamp (microseconds since epoch) -> "yyyy-MM-dd hh:mm:ss.zzzzzz":
// QDateTime resolves milliseconds, so the microsecond tail is appended from
// the raw value.
QString formatFullTimeUs(qint64 ts)
{
    const qint64 ms = ts / 1000;
    const int usRemainder = static_cast<int>(ts - ms * 1000);
    return QDateTime::fromMSecsSinceEpoch(ms).toString(
               QStringLiteral("yyyy-MM-dd hh:mm:ss")) +
           QStringLiteral(".%1").arg(usRemainder, 3, 10, QLatin1Char('0'));
}

// Sample rate for the values-bar stats; kHz above 10 kHz for readability.
// Rounded UP to the displayed precision: a nominally 100 Hz signal
// measures slightly below (e.g. 99.8), and the mean rate must still
// report the nominal 100.
QString formatRate(double hz)
{
    if (hz >= 10000.0)
    {
        return QString::number(std::ceil(hz / 100.0) / 10.0, 'f', 1) +
               QStringLiteral(" kHz");
    }
    return QString::number(std::ceil(hz)) + QStringLiteral(" Hz");
}

// Left-tree group-row tooltip: the file info behind a device/table group
// (files, rows, coverage), built from the footer statistics gathered at
// open. Mirrors TsFile Viewer's file-info view (files, counts, time range).
QString deviceToolTipText(const DeviceFileInfo& d)
{
    static const QLocale loc(QLocale::English, QLocale::UnitedStates);
    QStringList lines;
    lines << QStringLiteral("Files: %1").arg(d.files.size());
    constexpr int kMaxListed = 8;
    for (int i = 0; i < d.files.size() && i < kMaxListed; ++i)
    {
        lines << QDir::toNativeSeparators(d.files.at(i));
    }
    if (d.files.size() > kMaxListed)
    {
        lines << QStringLiteral("... and %1 more")
                     .arg(d.files.size() - kMaxListed);
    }
    if (d.totalRows > 0)
    {
        lines << QStringLiteral("Rows: %1").arg(loc.toString(d.totalRows));
    }
    if (d.haveRange)
    {
        lines << QStringLiteral("Range: %1 .. %2")
                     .arg(formatFullTimeUs(d.firstTs), formatFullTimeUs(d.lastTs));
    }
    return lines.join(QLatin1Char('\n'));
}

// Keep the sample at t (seconds) inside the 10%..90% band of the plot's x
// view: pan the window when the point drifted past a band edge — zoom
// unchanged, new data slides in on that side. Clamped at the series ends so
// the window never shows empty space. The one rule for every
// selected-sample sync (table row change, plot click, arrow stepping).
void panToBand(QCustomPlot* plot, const SeriesData& series, double t)
{
    const QCPRange r = plot->xAxis->range();
    const double span = r.size();
    if (span <= 0)
    {
        return;
    }
    const double frac = (t - r.lower) / span;
    if (frac < 0.1)
    {
        const double lower =
            qMax(t - 0.1 * span, static_cast<double>(series.ts.first()) / 1e6);
        plot->xAxis->setRange(lower, lower + span);
    }
    else if (frac > 0.9)
    {
        const double upper =
            qMin(t + 0.1 * span, static_cast<double>(series.ts.last()) / 1e6);
        plot->xAxis->setRange(upper - span, upper);
    }
}

// Point count in [lower, upper] of the shared raw container (bisection).
qint64 countInRange(const QCPGraphDataContainer* data, double lower, double upper)
{
    if (data == nullptr || data->isEmpty())
    {
        return 0;
    }
    const auto b = data->findBegin(lower);
    const auto e = data->findEnd(upper);
    return e - b;
}

// X-axis ticker: wall-clock labels ("hh:mm:ss.zzzzzz") for coordinates in
// seconds since epoch. QDateTime only resolves milliseconds, so the label is
// built the same way as ValueTableModel::formatTimeUs. The label width
// follows the view span: microseconds below 0.5 s, whole seconds up to
// ~1.5 days, date+time beyond (the day must stay visible).
class ClockTicker : public QCPAxisTicker
{
public:
    explicit ClockTicker(QCPAxis* axis) : axis_(axis) {}

protected:
    QString getTickLabel(double tick, const QLocale& locale, QChar formatChar,
                         int precision) override
    {
        Q_UNUSED(locale)
        Q_UNUSED(formatChar)
        Q_UNUSED(precision)
        const qint64 us = static_cast<qint64>(llround(tick * 1e6));
        const qint64 ms = us / 1000;
        const int usTail = static_cast<int>(us - ms * 1000);
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms);
        const double span = axis_->range().size();
        if (span < 0.5)
        {
            return dt.toString(QStringLiteral("hh:mm:ss")) +
                   QStringLiteral(".%1").arg(usTail, 3, 10, QLatin1Char('0'));
        }
        if (span < 86400.0 * 1.5)
        {
            return dt.toString(QStringLiteral("hh:mm:ss"));
        }
        return dt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    }

private:
    QCPAxis* axis_;  // owning axis: reads the live range for label width
};
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    doc_ = new TsFileDocument(this);
    setupUi();
    overlay_ = new LoadingOverlay(this);

    connect(doc_, &TsFileDocument::opened, this,
            [this](const MetaInfo& meta, const QVector<ParamInfo>& params)
    {
        {
            QHash<QString, QString> tips;
            for (auto it = meta.deviceInfo.cbegin();
                 it != meta.deviceInfo.cend(); ++it)
            {
                tips.insert(it.key(), deviceToolTipText(it.value()));
            }
            treeModel_->load(params, tips);
        }
        paramTree_->expandAll();
        applyTreeHeader();
        updateMetaBar(meta);
        clearContent();
        busy_->hide();
        overlay_->end();
        if (meta.fileCount > 1)
        {
            QStringList parts;
            parts << tr("%1 file(s)").arg(meta.fileCount);
            if (meta.skippedFileCount > 0)
            {
                parts << tr("%1 skipped").arg(meta.skippedFileCount);
            }
            parts << tr("%1 parameter(s)").arg(meta.paramCount);
            statusBar()->showMessage(
                tr("Loaded %1 files (%2)").arg(meta.fileCount).arg(parts.join(QStringLiteral(", "))),
                8000);
        }
        // Name the bad file(s) so the user knows what to kick out of the
        // set (footer statistics only; suspects are heuristic).
        if (!meta.corruptFiles.isEmpty() || !meta.suspectFiles.isEmpty())
        {
            QStringList bad;
            for (const QString& f : meta.corruptFiles)
            {
                bad << tr("corrupt: %1").arg(QFileInfo(f).fileName());
            }
            for (const QString& f : meta.suspectFiles)
            {
                bad << tr("suspect: %1").arg(QFileInfo(f).fileName());
            }
            statusBar()->showMessage(bad.join(QStringLiteral("; ")), 12000);
        }
        else if (meta.repaired)
        {
            statusBar()->showMessage(
                tr("Loaded %1 — repaired in place (%2 bytes truncated)")
                    .arg(meta.path)
                    .arg(meta.truncatedBytes),
                8000);
        }
        else
        {
            statusBar()->showMessage(tr("Loaded %1").arg(meta.path), 5000);
        }
    });
    connect(doc_, &TsFileDocument::repairFinished, this,
            [this](const QString& path, bool ok, const QString& message)
    {
        if (activeRepairs_ > 0)
        {
            --activeRepairs_;
        }
        if (!ok)
        {
            QMessageBox::warning(
                this, tr("Repair failed"),
                tr("%1:\n%2").arg(QDir::toNativeSeparators(path), message));
            return;
        }
        repairsSucceeded_ = true;
        statusBar()->showMessage(
            tr("Repaired %1 (%2)").arg(QFileInfo(path).fileName(), message),
            8000);
        // Once the whole batch finished, ask before reloading — the user
        // may be mid-analysis; repair-all queues several files and a
        // mid-batch reload would show partial state anyway.
        if (activeRepairs_ == 0)
        {
            repairsSucceeded_ = false;
            if (QMessageBox::question(
                    this, tr("Reload files"),
                    tr("Repair finished. Reload the file set now?")) !=
                QMessageBox::Yes)
            {
                return;
            }
            QStringList paths;
            for (const FileInfoEntry& fe : lastMeta_.fileEntries)
            {
                paths << fe.path;
            }
            paths << lastMeta_.skippedFiles;
            if (!paths.isEmpty())
            {
                openFiles(paths);
            }
        }
    });
    connect(doc_, &TsFileDocument::openFailed, this, [this](const QString& error)
    {
        busy_->hide();
        overlay_->end();
        statusBar()->showMessage(tr("Failed: %1").arg(error));
        QMessageBox::warning(this, tr("Open failed"), error);
    });
    connect(doc_, &TsFileDocument::repairConfirmRequested, this,
            [this](const QString& path, int code)
    {
        busy_->hide();
        overlay_->end();
        const auto answer = QMessageBox::question(
            this, tr("File appears incomplete"),
            tr("This TsFile failed to open (code %1) — its tail is likely "
               "truncated (e.g. a crash during recording).\n\n"
               "Repair it in place? The corrupted tail will be cut off and "
               "the file sealed; everything before it stays readable.")
                .arg(code),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        doc_->retryWithRepair(path, answer == QMessageBox::Yes);
    });
    connect(doc_, &TsFileDocument::valuesChunk, this, &MainWindow::onValuesChunk);
    connect(doc_, &TsFileDocument::queryFailed, this, [this](const QString& error)
    {
        busy_->hide();
        overlay_->end();
        loading_ = false;
        statusBar()->showMessage(tr("Error: %1").arg(error));
    });
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("TsFileViewer"));
    resize(1280, 800);

    // ---- toolbar: metainfo bar --------------------------------------------
    auto* toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setMovable(false);
    toolbar->setContextMenuPolicy(Qt::PreventContextMenu);  // no right-click menu
    auto* openAct = toolbar->addAction(tr("Open..."));
    openAct->setShortcut(QKeySequence::Open);  // Ctrl+O
    connect(openAct, &QAction::triggered, this, [this]
    {
        // Start where the current file lives; fall back to the desktop.
        QString startDir;
        if (!currentPath_.isEmpty())
        {
            startDir = QFileInfo(currentPath_).absolutePath();
        }
        if (startDir.isEmpty() || !QDir(startDir).exists())
        {
            startDir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        }
        // Multi-select: one file opens alone; several files aggregate into
        // one device->param tree (shared params concatenate in time order).
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, tr("Open TsFile"), startDir,
            tr("TsFile (*.tsfile);;All files (*.*)"));
        if (paths.isEmpty())
        {
            return;
        }
        if (paths.size() == 1)
        {
            openFile(paths.first());
        }
        else
        {
            openFiles(paths);
        }
    });
    // Inset the Open button from the toolbar's top-left corner.
    if (QWidget* openBtn = toolbar->widgetForAction(openAct))
    {
        openBtn->setStyleSheet(
            QStringLiteral("margin-left: 4px; margin-bottom: 4px;"));
    }
    // File path label right beside the Open button.
    // Left-click opens the containing folder; right-click copies the path.
    fileLabel_ = new QLabel(tr("File: -"), toolbar);
    fileLabel_->setCursor(Qt::PointingHandCursor);
    fileLabel_->installEventFilter(this);
    toolbar->addWidget(fileLabel_);
    // Push the rest of the metainfo to the right side of the toolbar.
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    auto addSep = [toolbar] { toolbar->addSeparator(); };

    codecLabel_ = new QLabel(tr("Codec: -"), toolbar);
    toolbar->addWidget(codecLabel_);
    addSep();

    // Single label: show tree devices OR table count per file's model.
    devicesLabel_ = new QLabel(tr("Devices: -"), toolbar);
    toolbar->addWidget(devicesLabel_);
    addSep();

    paramsLabel_ = new QLabel(tr("Params: -"), toolbar);
    toolbar->addWidget(paramsLabel_);
    addSep();

    rangeLabel_ = new QLabel(tr("Range: -"), toolbar);
    toolbar->addWidget(rangeLabel_);

    busy_ = new QProgressBar(toolbar);
    busy_->setRange(0, 0);
    busy_->setTextVisible(false);
    busy_->setMaximumWidth(110);
    busy_->setToolTip(tr("Working..."));
    busy_->hide();
    toolbar->addWidget(busy_);

    // ---- central: left params / right (table over plot) -------------------
    auto* central = new QSplitter(Qt::Horizontal, this);

    // Left: search + tree, whole panel inset by 4px, 4px spacing between.
    auto* left = new QWidget(central);
    searchEdit_ = new QLineEdit(left);
    searchEdit_->setPlaceholderText(tr("Search parameter..."));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->installEventFilter(this);  // Up/Down -> tree navigation
    // Deterministic height for the header-alignment math below.
    searchEdit_->setFixedHeight(25);
    treeModel_ = new ParamTreeModel(this);
    paramTree_ = new QTreeView(left);
    paramTree_->setModel(treeModel_);
    paramTree_->setSortingEnabled(false);
    paramTree_->setUniformRowHeights(true);
    applyTreeHeader();
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(4, 4, 4, 4);
    leftLayout->setSpacing(4);
    leftLayout->addWidget(searchEdit_);
    leftLayout->addWidget(paramTree_, 1);

    // Right side: vertical box (0 margins) holding the fixed paging bar on
    // top and the table/plot splitter below; the bar is outside the splitter.
    auto* rightPane = new QWidget(central);
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    auto* right = new QSplitter(Qt::Vertical, rightPane);

    // Values bar, fixed height, outside any splitter.
    // Height = left margin(4) + search(25) + spacing(4) so the values-table
    // header aligns with the parameter-tree header.
    // Layout: param name | stretch | stats (right-aligned) | export
    auto* valuesBar = new QWidget(rightPane);
    valuesBar->setFixedHeight(33);
    paramNameLabel_ = new QLabel(QString(), valuesBar);
    paramNameLabel_->setMinimumWidth(120);
    // Muted stats right next to the export button: rows · span · rate ·
    // min · max · mean (key:value form).
    paramStatLabel_ = new QLabel(QString(), valuesBar);
    paramStatLabel_->setStyleSheet(
        QStringLiteral("color: #667085; padding-left: 2px;"));
    exportBtn_ = new QPushButton(tr("Export"), valuesBar);
    exportBtn_->setFlat(true);
    exportBtn_->setEnabled(false);
    auto* valuesBarLayout = new QHBoxLayout(valuesBar);
    valuesBarLayout->setContentsMargins(4, 0, 4, 0);
    valuesBarLayout->setSpacing(4);
    valuesBarLayout->addWidget(paramNameLabel_);
    valuesBarLayout->addStretch(1);
    valuesBarLayout->addWidget(paramStatLabel_);
    valuesBarLayout->addWidget(exportBtn_);
    connect(exportBtn_, &QPushButton::clicked, this, &MainWindow::exportCsv);

    rightLayout->addWidget(valuesBar);
    // Table+plot inset by 4px; the paging bar stays full-width.
    auto* rightContent = new QWidget(rightPane);
    auto* rightContentLayout = new QVBoxLayout(rightContent);
    rightContentLayout->setContentsMargins(4, 0, 4, 4);
    rightContentLayout->setSpacing(0);
    rightContentLayout->addWidget(right);
    rightLayout->addWidget(rightContent, 1);

    valueModel_ = new ValueTableModel(this);
    valuesTable_ = new QTableView(right);
    valuesTable_->setModel(valueModel_);
    valuesTable_->horizontalHeader()->setStretchLastSection(true);
    valuesTable_->horizontalHeader()->resizeSection(1, 160);  // Time column
    valuesTable_->verticalHeader()->setDefaultSectionSize(20);
    valuesTable_->verticalHeader()->hide();  // No column shows row numbers (No column exists)
    valuesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    valuesTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    valuesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // User selection (click or keyboard navigation) drives the plot sync:
    // the tracer moves and the view pans so the selected sample stays in
    // the 10%..90% band (zoom unchanged). Programmatic selections (plot
    // click -> table) set suppressPlotSync_ and don't re-pan.
    connect(valuesTable_->selectionModel(),
            &QItemSelectionModel::selectionChanged, this, [this]
    {
        if (suppressPlotSync_)
        {
            suppressPlotSync_ = false;
            return;
        }
        syncPlotToRow(valuesTable_->currentIndex());
    });
    // Double-click a row (or Enter on it): zoom to the marker-visible window
    // centered on the sample and place the tracer.
    connect(valuesTable_, &QTableView::doubleClicked, this,
            [this](const QModelIndex& idx) { activateRow(idx); });
    auto* rowActivateAct = new QAction(tr("Zoom to row"), this);
    // Scoped to the table (with children), not the window: a window-wide
    // Return shortcut would shadow the tree's load action while the table
    // has focus.
    rowActivateAct->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    rowActivateAct->setShortcuts(
        QList<QKeySequence>{QKeySequence(Qt::Key_Return),
                            QKeySequence(Qt::Key_Enter)});
    valuesTable_->addAction(rowActivateAct);
    connect(rowActivateAct, &QAction::triggered, this,
            [this] { activateRow(valuesTable_->currentIndex()); });

    plot_ = new QCustomPlot(right);
    plot_->legend->setVisible(false);  // no legend
    // Flat monochrome plot matching the app theme: white plot surface,
    // light grid, black curve, red sample markers stay for data salience.
    plot_->setBackground(QColor(0xff, 0xff, 0xff));
    plot_->xAxis->grid()->setPen(QPen(QColor(0xee, 0xf2, 0xf6), 1));
    plot_->yAxis->grid()->setPen(QPen(QColor(0xee, 0xf2, 0xf6), 1));
    plot_->xAxis->setBasePen(QPen(QColor(0xc5, 0xce, 0xd8), 1));
    plot_->yAxis->setBasePen(QPen(QColor(0xc5, 0xce, 0xd8), 1));
    plot_->xAxis->setTickPen(QPen(QColor(0xc5, 0xce, 0xd8), 1));
    plot_->yAxis->setTickPen(QPen(QColor(0xc5, 0xce, 0xd8), 1));
    plot_->xAxis->setSubTickPen(QPen(QColor(0xdd, 0xe3, 0xea), 1));
    plot_->yAxis->setSubTickPen(QPen(QColor(0xdd, 0xe3, 0xea), 1));
    plot_->xAxis->setTickLabelColor(QColor(0x66, 0x70, 0x85));
    plot_->yAxis->setTickLabelColor(QColor(0x66, 0x70, 0x85));
    plot_->xAxis->setLabelColor(QColor(0x1f, 0x23, 0x28));
    plot_->yAxis->setLabelColor(QColor(0x1f, 0x23, 0x28));
    plot_->setInteraction(QCP::iRangeDrag, true);
    // Zoom the x axis only: y stays fit to the loaded data, so dense sawtooth
    // waveforms keep a stable amplitude scale while scrolling in time.
    plot_->setInteraction(QCP::iRangeZoom, true);
    plot_->axisRect()->setRangeZoomAxes(plot_->xAxis, nullptr);
    plot_->axisRect()->setRangeDragAxes(plot_->xAxis, nullptr);
    plot_->setNoAntialiasingOnDrag(true);
    // Plot -> table link: clicking near a curve sample centers that row in
    // the values table (finds the nearest sample by x, O(log) via bisection
    // since ts is sorted).
    connect(plot_, &QCustomPlot::mousePress, this,
            [this](QMouseEvent* ev) { onPlotMousePress(ev); });
    // X axis labels are wall-clock time (ClockTicker: hh:mm:ss[.zzzzzz],
    // date prefix when the view spans days). Fixed y labels: English locale
    // groups thousands and 'f' never produces scientific notation.
    plot_->setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
    QSharedPointer<ClockTicker> clockTicker(new ClockTicker(plot_->xAxis));
    plot_->xAxis->setTicker(clockTicker);
    plot_->yAxis->setNumberFormat("f");
    plot_->yAxis->setNumberPrecision(6);
    plot_->xAxis->ticker()->setTickCount(5);
    // No axis titles: tick values carry the units; the freed space goes to
    // the plot area. Context (param name / codec) lives in the paging bar.
    plot_->xAxis->setVisible(true);
    // rangeChanged is overloaded; take the single-arg form.
    connect(plot_->xAxis,
            static_cast<void (QCPAxis::*)(const QCPRange&)>(
                &QCPAxis::rangeChanged),
            this, [this](const QCPRange& r) { onPlotXRangeChanged(r); });
    // Left/Right on the focused plot walk the selected sample (same sync to
    // the table as clicking a point). QCustomPlot takes focus on click
    // (Qt::ClickFocus); its own key handling doesn't claim arrows, so an
    // event filter sees them first.
    plot_->installEventFilter(this);
    // Left/Right on the focused table step the sample too (Up/Down keep
    // their native row movement, which triggers the plot sync).
    valuesTable_->installEventFilter(this);
    // Splitter now has two panes: table and plot.
    right->setStretchFactor(0, 1);
    right->setStretchFactor(1, 1);
    right->setSizes({320, 320});
    right->setCollapsible(0, false);
    right->setCollapsible(1, false);
    right->setChildrenCollapsible(false);

    central->addWidget(left);
    central->addWidget(rightPane);
    // Window resize grows only the right pane; the parameter tree keeps
    // its width (stretch 0), and neither pane may be collapsed by dragging.
    central->setStretchFactor(0, 0);
    central->setStretchFactor(1, 1);
    central->setSizes({340, 940});
    central->setCollapsible(0, false);
    central->setCollapsible(1, false);
    central->setChildrenCollapsible(false);
    setCentralWidget(central);

    // Debounced filtering: each apply rebuilds the visible tree from the
    // master params, so per-keystroke application can stutter on large
    // schemas. Apply after typing pauses instead; Enter applies
    // immediately.
    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(300);
    connect(searchTimer_, &QTimer::timeout, this, [this]
    {
        applySearch(searchEdit_->text());
    });
    connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString& text)
    {
        searchTimer_->start();
        Q_UNUSED(text);
    });
    connect(searchEdit_, &QLineEdit::returnPressed, this, [this]
    {
        searchTimer_->stop();
        applySearch(searchEdit_->text());
    });
    // Query on explicit activation (double-click / Enter / context menu),
    // not on plain selection: browsing the tree must not fire queries.
    connect(paramTree_, &QTreeView::doubleClicked, this,
            &MainWindow::onParamActivated);
    auto* activateAct = new QAction(tr("Load values"), this);
    // Scoped to the tree (with children), not the window: a window-wide
    // Return shortcut would shadow the values table's row-activation action
    // while the table has focus.
    activateAct->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    activateAct->setShortcuts(
        QList<QKeySequence>{QKeySequence(Qt::Key_Return),
                            QKeySequence(Qt::Key_Enter)});
    paramTree_->addAction(activateAct);
    connect(activateAct, &QAction::triggered, this, &MainWindow::onParamActivated);
    // Ctrl+F focuses the search box (and selects its text for retyping).
    auto* findAct = new QAction(tr("Find parameter"), this);
    findAct->setShortcut(QKeySequence::Find);
    addAction(findAct);
    connect(findAct, &QAction::triggered, this, [this]
    {
        searchEdit_->setFocus();
        searchEdit_->selectAll();
    });
    paramTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(paramTree_, &QTreeView::customContextMenuRequested, this,
            [this, activateAct](const QPoint& pos)
    {
        const QModelIndex index = paramTree_->indexAt(pos);
        if (!index.isValid())
        {
            return;
        }
        paramTree_->setCurrentIndex(index);
        QMenu menu(this);
        menu.addAction(activateAct);
        menu.exec(paramTree_->viewport()->mapToGlobal(pos));
    });

    // ---- status bar --------------------------------------------------------
    // Left: transient messages (file path etc.). Right: version (turns
    // into an "up" link when a newer release exists) then analysis. All
    // inline on one row; permanent widgets never wrap with the message.
    versionLabel_ = new QLabel(
        QStringLiteral("v" APP_VERSION), this);
    versionLabel_->setCursor(Qt::PointingHandCursor);
    versionLabel_->installEventFilter(this);
    statusBar()->addPermanentWidget(versionLabel_);
    checkForUpdate();
    analysisLabel_ = new QLabel(QString(), this);
    statusBar()->addPermanentWidget(analysisLabel_);

    statusBar()->showMessage(tr("Ready — open a TsFile (*.tsfile) to begin, or double-click a parameter to load values."));
}

void MainWindow::checkForUpdate()
{
    QNetworkReply* reply = net_.get(
        QNetworkRequest(QUrl(QStringLiteral(APP_RELEASES_API))));
    connect(reply, &QNetworkReply::finished, this, [this, reply]
    {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            return;  // offline / rate-limited: stay silent
        }
        const QByteArray body = reply->readAll();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
        {
            return;
        }
        latestVersion_ = doc.object()
                             .value(QStringLiteral("tag_name"))
                             .toString()
                             .remove(QLatin1Char('v'));
        updateVersionLabel();
    });
}

void MainWindow::updateVersionLabel()
{
    const QString current = QStringLiteral(APP_VERSION);
    if (!latestVersion_.isEmpty() && latestVersion_ != current)
    {
        updateAvailable_ = true;
        versionLabel_->setText(
            tr("v%1 ↑").arg(current));
        versionLabel_->setToolTip(
            tr("Version %1 is available — click to open the release page.")
                .arg(latestVersion_));
    }
    else
    {
        updateAvailable_ = false;
        versionLabel_->setText(QStringLiteral("v" APP_VERSION));
        versionLabel_->setToolTip(QString());
    }
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    // Version label: click opens the release page when an update exists.
    if (obj == versionLabel_ && updateAvailable_ &&
        event->type() == QEvent::MouseButtonPress)
    {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton)
        {
            QDesktopServices::openUrl(QUrl(QStringLiteral(APP_RELEASES_URL)));
            return true;
        }
    }
    // Search box: Up/Down move the tree selection (type-to-filter, arrows
    // to pick, Enter loads — no mouse needed).
    if (obj == searchEdit_ && event->type() == QEvent::KeyPress)
    {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down)
        {
            QApplication::postEvent(paramTree_,
                                    new QKeyEvent(ke->type(), ke->key(),
                                                  ke->modifiers()));
            return true;
        }
    }
    // Focused plot: all four arrows navigate the selection. Up/Down go
    // through the table's selection (plot syncs), Left/Right step the
    // sample (table syncs).
    if (obj == plot_ && event->type() == QEvent::KeyPress)
    {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right ||
            ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down)
        {
            stepSelection(ke->key());
            return true;  // the plot has no built-in arrow use
        }
    }
    // Focused table: Left/Right would only shuffle the current column —
    // redirect them to sample stepping (Up/Down keep their native row
    // movement, which already triggers the plot sync).
    if (obj == valuesTable_ && event->type() == QEvent::KeyPress)
    {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right)
        {
            stepSelection(ke->key());
            return true;
        }
    }
    if (obj == fileLabel_ && !currentPath_.isEmpty())
    {
        if (event->type() == QEvent::MouseButtonPress)
        {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton)
            {
                showFilesDialog();
                return true;
            }
            if (me->button() == Qt::RightButton)
            {
                QApplication::clipboard()->setText(QDir::toNativeSeparators(currentPath_));
                statusBar()->showMessage(tr("Path copied to clipboard"), 2500);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::openFile(const QString& path)
{
    clearContent();
    treeModel_->load({});
    applyTreeHeader();  // the clear/rebuild drops per-section modes
    lastMeta_ = MetaInfo();
    currentPath_ = path;
    fileLabel_->setText(tr("File: %1").arg(QDir::toNativeSeparators(path)));
    devicesLabel_->setText(tr("Devices: -"));
    paramsLabel_->setText(tr("Params: -"));
    rangeLabel_->setText(tr("Range: -"));
    busy_->show();
    overlay_->begin();
    statusBar()->showMessage(tr("Opening %1...").arg(QDir::toNativeSeparators(path)));
    doc_->openAsync(path);
}

void MainWindow::openFiles(const QStringList& paths)
{
    clearContent();
    treeModel_->load({});
    applyTreeHeader();  // the clear/rebuild drops per-section modes
    lastMeta_ = MetaInfo();
    // Keep a representative path so the next Open dialog starts in the same
    // folder (labels show the count, not this path).
    currentPath_ = paths.first();
    fileLabel_->setText(tr("Files: %1").arg(paths.size()));
    devicesLabel_->setText(tr("Devices: -"));
    paramsLabel_->setText(tr("Params: -"));
    rangeLabel_->setText(tr("Range: -"));
    busy_->show();
    overlay_->begin();
    statusBar()->showMessage(
        tr("Opening %1 file(s) (aggregating)...").arg(paths.size()));
    doc_->openFilesAsync(paths);
}

void MainWindow::updateMetaBar(const MetaInfo& meta)
{
    lastMeta_ = meta;
    // Multi-file: keep the existing currentPath_ (a representative file);
    // clearing it would send the next Open dialog to the desktop.
    fileLabel_->setText(
        meta.fileCount > 1
            ? tr("Files: %1").arg(meta.fileCount)
            : tr("File: %1").arg(QDir::toNativeSeparators(meta.path)));
    if (meta.fileCount == 1)
    {
        currentPath_ = meta.path;
    }
    if (meta.deviceCount > 0)
    {
        devicesLabel_->setText(tr("Devices: %1").arg(meta.deviceCount));
    }
    else
    {
        devicesLabel_->setText(tr("Tables: %1").arg(meta.tableCount));
    }
    paramsLabel_->setText(tr("Params: %1").arg(meta.paramCount));
    rangeLabel_->setText(
        meta.haveTimeRange
            ? tr("Range: %1 .. %2")
                  .arg(formatFullTimeUs(meta.firstTs),
                       formatFullTimeUs(meta.lastTs))
            : tr("Range: -"));

    // Secondary details live in the file label's tooltip.
    QStringList tip;
    if (meta.fileCount > 1)
    {
        tip << tr("Files: %1 (%2 skipped)").arg(meta.fileCount).arg(meta.skippedFileCount);
        tip << tr("Total size: %1").arg(humanSize(meta.fileSize));
        if (meta.skippedFileCount > 0)
        {
            tip << tr("Skipped files could not be opened (corrupted tail?):");
            for (int i = 0; i < meta.skippedFiles.size(); ++i)
            {
                tip << QStringLiteral("  ") +
                           QDir::toNativeSeparators(meta.skippedFiles.at(i)) +
                           (i < meta.skippedErrors.size()
                                ? QStringLiteral(" — ") +
                                      meta.skippedErrors.at(i)
                                : QString());
            }
        }
        if (!meta.corruptFiles.isEmpty())
        {
            tip << tr("Corrupt files (footer statistics contradict themselves):");
            for (const QString& f : meta.corruptFiles)
            {
                tip << QStringLiteral("  ") + QDir::toNativeSeparators(f);
            }
        }
        if (!meta.suspectFiles.isEmpty())
        {
            tip << tr("Suspect files (statistics contradict the majority of files — heuristic):");
            for (const QString& f : meta.suspectFiles)
            {
                tip << QStringLiteral("  ") + QDir::toNativeSeparators(f);
            }
        }
    }
    else
    {
        tip << tr("Path: %1").arg(QDir::toNativeSeparators(meta.path));
        tip << tr("Size: %1 (%2 bytes)").arg(humanSize(meta.fileSize)).arg(meta.fileSize);
        tip << tr("Layout: %1").arg(meta.tableCount > 0
                                        ? (meta.deviceCount > 0
                                               ? QStringLiteral("tree-device + table")
                                               : QStringLiteral("table"))
                                        : QStringLiteral("tree-device"));
    }
    if (meta.deviceCount > 0 && meta.deviceCount <= 20)
    {
        tip << tr("Devices: %1").arg(meta.devices.join(QStringLiteral(", ")));
    }
    if (meta.tableCount > 0 && meta.tableCount <= 20)
    {
        tip << tr("Tables: %1").arg(meta.tables.join(QStringLiteral(", ")));
    }
    if (meta.overlappingParamCount > 0)
    {
        tip << tr("Note: %1 parameter(s) have overlapping time ranges across "
                  "files; their concatenated values are not globally "
                  "time-sorted").arg(meta.overlappingParamCount);
    }
    tip << tr("Left-click: file details");
    tip << tr("Right-click: copy path");
    fileLabel_->setToolTip(tip.join(QLatin1Char('\n')));
}

void MainWindow::showFilesDialog()
{
    const auto dash = [](qint64 v)
    { return v < 0 ? QStringLiteral("-") : QString::number(v); };
    // Distinct codec names of one file: "TS_2DIFF, PLAIN" style.
    const auto codecNames = [](const QVector<int>& vals)
    {
        QStringList names;
        for (int v : vals)
        {
            names << TsFileNames::encoding(v);
        }
        return names.isEmpty() ? QStringLiteral("-")
                                : names.join(QStringLiteral(", "));
    };
    const auto compNames = [](const QVector<int>& vals)
    {
        QStringList names;
        for (int v : vals)
        {
            names << TsFileNames::compression(v);
        }
        return names.isEmpty() ? QStringLiteral("-")
                                : names.join(QStringLiteral(", "));
    };

    // Row data for the damaged table (declared before the dialog so the
    // connect-lambdas below can never outlive it).
    QStringList badPaths;    // parallel to badTable rows
    QVector<bool> badSkipped;  // true = unreadable (repair candidate)

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Loaded Files"));
    auto* lay = new QVBoxLayout(&dlg);

    // ---- normal (loaded, clean) files --------------------------------
    auto* tableLabel = new QLabel(tr("Loaded files"), &dlg);
    QFont boldFont = tableLabel->font();
    boldFont.setBold(true);
    tableLabel->setFont(boldFont);
    auto* table = new QTableWidget(&dlg);
    table->setColumnCount(11);
    table->setHorizontalHeaderLabels(
        {tr("No."), tr("Name"), tr("Size"), tr("Devices"), tr("Tables"),
         tr("Params"), tr("Chunks"), tr("Rows"), tr("Time Range"),
         tr("Encoding"), tr("Compression")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(false);
    auto addRow = [&](int row, const QString& path, const QString& name,
                      const QStringList& cells)
    {
        QStringList all;
        all << QString::number(row + 1) << name << cells;
        for (int c = 0; c < all.size(); ++c)
        {
            auto* it = new QTableWidgetItem(all.at(c));
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            if (c == 1)
            {
                it->setToolTip(QDir::toNativeSeparators(path));
            }
            else
            {
                it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            }
            table->setItem(row, c, it);
        }
    };

    // ---- damaged / unreadable files ----------------------------------
    auto* badLabel = new QLabel(tr("Damaged / unreadable files"), &dlg);
    badLabel->setFont(boldFont);
    auto* badTable = new QTableWidget(&dlg);
    badTable->setColumnCount(4);
    badTable->setHorizontalHeaderLabels(
        {tr("No."), tr("Name"), tr("Size"), tr("Status")});
    badTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    badTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    badTable->setSelectionMode(QAbstractItemView::SingleSelection);
    badTable->setAlternatingRowColors(true);
    badTable->verticalHeader()->setVisible(false);
    // Red = damaged, mirroring the red tree labels.
    const QColor bad(0xd9, 0x30, 0x30);
    auto addBadRow = [&](const QString& path, qint64 size,
                         const QString& status, bool skipped)
    {
        const int row = badPaths.size();
        badPaths << path;
        badSkipped << skipped;
        badTable->setRowCount(row + 1);
        auto* num = new QTableWidgetItem(QString::number(row + 1));
        num->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* name = new QTableWidgetItem(QFileInfo(path).fileName());
        name->setToolTip(QDir::toNativeSeparators(path));
        auto* sz = new QTableWidgetItem(humanSize(size));
        sz->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* st = new QTableWidgetItem(status);
        QTableWidgetItem* cells[] = {num, name, sz, st};
        for (int c = 0; c < 4; ++c)
        {
            cells[c]->setFlags(cells[c]->flags() & ~Qt::ItemIsEditable);
            cells[c]->setForeground(QBrush(bad));
            badTable->setItem(row, c, cells[c]);
        }
    };
    auto setBadStatus = [&](const QString& path, const QString& text)
    {
        const int i = badPaths.indexOf(path);
        if (i >= 0 && badTable->item(i, 3) != nullptr)
        {
            badTable->item(i, 3)->setText(text);
        }
    };

    // ---- fill / refresh both tables from lastMeta_ --------------------
    auto fill = [&]()
    {
        table->setRowCount(0);
        int row = 0;
        for (const FileInfoEntry& fe : lastMeta_.fileEntries)
        {
            if (lastMeta_.corruptFiles.contains(fe.path) ||
                lastMeta_.suspectFiles.contains(fe.path))
            {
                continue;  // damaged files live in the lower table
            }
            table->setRowCount(row + 1);
            addRow(row++, fe.path, QFileInfo(fe.path).fileName(),
                   {humanSize(fe.size), QString::number(fe.deviceCount),
                    QString::number(fe.tableCount),
                    QString::number(fe.paramCount), dash(fe.chunkCount),
                    dash(fe.rowCount),
                    fe.haveRange
                        ? QStringLiteral("%1 .. %2")
                              .arg(formatFullTimeUs(fe.firstTs),
                                   formatFullTimeUs(fe.lastTs))
                        : QStringLiteral("-"),
                    codecNames(fe.encodings), compNames(fe.compressions)});
        }
        badPaths.clear();
        badSkipped.clear();
        badTable->setRowCount(0);
        for (const FileInfoEntry& fe : lastMeta_.fileEntries)
        {
            if (lastMeta_.corruptFiles.contains(fe.path))
            {
                addBadRow(fe.path, fe.size,
                          QStringLiteral(
                              "corrupt: footer statistics self-contradictory"),
                          false);
            }
            else if (lastMeta_.suspectFiles.contains(fe.path))
            {
                addBadRow(fe.path, fe.size,
                          QStringLiteral("suspect: statistics contradict the "
                                          "majority (heuristic)"),
                          false);
            }
        }
        for (int i = 0; i < lastMeta_.skippedFiles.size(); ++i)
        {
            const QString& p = lastMeta_.skippedFiles.at(i);
            addBadRow(
                p, QFileInfo(p).size(),
                i < lastMeta_.skippedErrors.size()
                    ? lastMeta_.skippedErrors.at(i)
                    : QString(),
                true);
        }
        badTable->resizeColumnsToContents();
        badTable->horizontalHeader()->setStretchLastSection(true);
        const bool any = !badPaths.isEmpty();
        badLabel->setVisible(any);
        badTable->setVisible(any);
        badTable->setMaximumHeight(
            any ? qMin(220, 50 + badPaths.size() * 28) : 0);
    };
    fill();

    // ---- repair dispatch ---------------------------------------------
    const auto dispatchRepair = [&](const QStringList& paths)
    {
        for (const QString& p : paths)
        {
            setBadStatus(p, tr("Repairing…"));
            ++activeRepairs_;
            doc_->repairFileAsync(p);
        }
    };

    // Right-click the damaged table: repair the clicked unreadable file or
    // all of them (truncate the damaged tail and seal, same as the
    // single-file repair flow). Statistics-level corrupt/suspect files are
    // openable — truncation is not their remedy, so no repair for them.
    badTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(badTable, &QTableWidget::customContextMenuRequested, &dlg,
            [this, badTable, &badPaths, &badSkipped, &dispatchRepair](
                const QPoint& pos)
    {
        const int row = badTable->rowAt(pos.y());
        QStringList repairable;
        for (int i = 0; i < badPaths.size(); ++i)
        {
            if (badSkipped.at(i))
            {
                repairable << badPaths.at(i);
            }
        }
        QMenu menu(badTable);
        QAction* one = nullptr;
        if (row >= 0 && row < badPaths.size() && badSkipped.at(row))
        {
            one = menu.addAction(tr("Repair (truncate damaged tail)…"));
        }
        QAction* all = repairable.size() > 1
                           ? menu.addAction(tr("Repair all %1 unreadable file(s)…")
                                                .arg(repairable.size()))
                           : nullptr;
        if (one == nullptr && all == nullptr)
        {
            return;
        }
        QAction* chosen = menu.exec(badTable->viewport()->mapToGlobal(pos));
        QStringList targets;
        if (chosen == one && one != nullptr)
        {
            targets << badPaths.at(row);
        }
        else if (chosen == all && all != nullptr)
        {
            targets = repairable;
        }
        else
        {
            return;
        }
        if (QMessageBox::question(
                badTable, tr("Repair files"),
                tr("Truncate the damaged tail of %1 file(s) and seal them in "
                   "place?\nThe files will be modified, then the whole set "
                   "is reloaded.")
                    .arg(targets.size())) != QMessageBox::Yes)
        {
            return;
        }
        dispatchRepair(targets);
    });

    // ---- live updates while the dialog is open ------------------------
    // Repair outcome lands in the Status column; on user-confirmed reload
    // `opened` refills both tables — a repaired file then appears as a
    // normal row.
    connect(doc_, &TsFileDocument::repairFinished, &dlg,
            [&setBadStatus](const QString& path, bool ok, const QString& message)
    {
        setBadStatus(path,
                     ok ? QObject::tr("repaired — reload to apply")
                        : QObject::tr("repair failed: %1").arg(message));
    });
    connect(doc_, &TsFileDocument::opened, &dlg, [&]() { fill(); });

    table->resizeColumnsToContents();
    table->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(tableLabel);
    lay->addWidget(table, 1);
    lay->addWidget(badLabel);
    lay->addWidget(badTable);
    const int w = qMin(1400, table->horizontalHeader()->length() + 70);
    const int h = qMin(640, 80 + table->horizontalHeader()->height() +
                                 table->rowCount() * 30);
    dlg.resize(w, h);
    dlg.exec();
}

void MainWindow::onValuesChunk(const SeriesData& chunk, bool done)
{
    const SeriesData* before = valueModel_->series();
    // A model reset (new query) desyncs plotData_ from the rows: start a
    // fresh container. Detected by series identity, not row counts — a
    // count match between different series would silently append the new
    // chunk onto the old container. Computed BEFORE appendChunk: a reset
    // (different key) destroys the old series, leaving `before` dangling.
    const bool fresh = before == nullptr || before->key() != chunk.key();
    accumulateStats(chunk);
    valueModel_->appendChunk(chunk);
    appendPlotData(chunk, fresh);
    const SeriesData* series = valueModel_->series();
    // TEXT and other non-numeric series have no curve to draw: hide the
    // plot pane entirely (empty axes behind the table would read as broken
    // rendering). The table and CSV export still carry the data.
    plot_->setVisible(series == nullptr || series->numeric);
    if (!done)
    {
        // Progressive load: running row count in the status bar. The total
        // row count is unknown until the query completes, so the progress
        // bar stays in busy mode (no fake percentage).
        statusBar()->showMessage(
            tr("%1: loading... %2 rows")
                .arg(chunk.key())
                .arg(series ? series->ts.size() : chunk.ts.size()));
        // Replot throttle: every chunk's replot walks the whole visible
        // range; on a multi-million-row series that dwarfs the query
        // itself. The curve still grows in view, just at >=5 fps instead
        // of once per 500k rows.
        const qint64 now = loadClock_.elapsed();
        if (lastLoadReplotMs_ < 0 || now - lastLoadReplotMs_ >= 200)
        {
            lastLoadReplotMs_ = now;
            rebuildPlot();
        }
        return;
    }

    busy_->hide();
    overlay_->end();
    loading_ = false;
    if (series == nullptr)
    {
        return;
    }
    // Release the per-row text vector's slack: numeric series keep none of
    // it (a pointer-sized slot per row adds up on large series).
    valueModel_->compactText();
    finishValuesLoad(*series);
}

// Load-end work once the final chunk arrived.
void MainWindow::finishValuesLoad(const SeriesData& series)
{
    // Model resets restore default column widths; re-apply.
    valuesTable_->horizontalHeader()->resizeSection(1, 160);
    statusBar()->showMessage(
        tr("%1: %2 rows").arg(series.key()).arg(series.ts.size()), 5000);
    // The 200ms replot throttle means the last chunks may sit beyond the x
    // range of the previous rebuild: fit once more to the complete series
    // (the loading overlay blocked interaction, so no user zoom is lost).
    fitOnNextRebuild_ = true;
    rebuildPlot();
    // Large numeric series: build the decimated overview once, then let the
    // current view decide raw vs envelope.
    if (series.numeric && series.ts.size() > kRawVisibleLimit)
    {
        envelopeData_ = buildEnvelope();
        updateGraphData();
    }
    exportBtn_->setEnabled(true);

    // ---- values-bar stats: rows · span · rate · min · max · mean ----------
    // Span is the actual coverage (max ts - min ts), NOT last-first: a
    // corrupted file's footer statistics can order the concatenation
    // non-monotonically (last < first), and the span must stay a factual
    // coverage measure in that case, not go negative. rate =
    // (rows-1)/span, the mean sampling frequency across the series.
    // Min/max/mean come from the running stats (finite rows only).
    if (!series.ts.isEmpty())
    {
        qint64 minTs = series.ts.first();
        qint64 maxTs = minTs;
        for (qint64 t : series.ts)
        {
            if (t < minTs) minTs = t;
            if (t > maxTs) maxTs = t;
        }
        const double spanS =
            static_cast<double>(maxTs - minTs) / 1e6;
        const int n = series.ts.size();
        QString rate = tr("-");
        if (n >= 2 && spanS > 0.0)
        {
            rate = formatRate((n - 1) / spanS);
        }
        QString stats = tr("rows: %1  span: %2 s  rate: %3")
                            .arg(n)
                            .arg(QString::number(spanS, 'f', 3), rate);
        if (statFinite_ > 0)
        {
            stats += tr("  min: %1  max: %2  mean: %3")
                         .arg(ValueTableModel::formatValue(statVmin_),
                              ValueTableModel::formatValue(statVmax_),
                              ValueTableModel::formatValue(statSum_ / statFinite_));
        }
        paramStatLabel_->setText(stats);
        paramStatLabel_->setToolTip(
            tr("Rows: %1\nDuration: %2 s (max - min of the "
               "Time column)\nRate: %3 ((rows - 1) / duration)\n"
               "Min/Max/Mean: finite rows only")
                .arg(n)
                .arg(QString::number(spanS, 'f', 6), rate)
                .arg(statFinite_));
    }
    else
    {
        paramStatLabel_->setText(QString());
        paramStatLabel_->setToolTip(QString());
    }

    // ---- stats: running accumulators (O(1) here), plot tooltip -----------
    // + status-bar analysis --------------------------------------------
    if (series.measurement.endsWith(QLatin1String("SFID"), Qt::CaseSensitive))
    {
        // SFID counters report the wrap behavior: one pass over the loaded
        // values (the running stats carry min/max/finite already).
        qint64 inc1 = 0, wraps = 0, violations = 0;
        double prev = std::numeric_limits<double>::quiet_NaN();
        for (double v : series.value)
        {
            if (std::isnan(v)) continue;
            if (!std::isnan(prev))
            {
                const double d = v - prev;
                if (d == 1.0)
                {
                    ++inc1;
                }
                else if (v < prev)  // wrap: decreases -> restart from min
                {
                    ++wraps;
                }
                else
                {
                    ++violations;
                }
            }
            prev = v;
        }
        analysisLabel_->setText(
            tr("SFID: %1/%2 steps +1, %3 wrap(s) to min, %4 other step(s)"
               "  |  range %5..%6")
                .arg(inc1)
                .arg(statFinite_ - (statFinite_ > 0 ? 1 : 0))
                .arg(wraps)
                .arg(violations)
                .arg(ValueTableModel::formatValue(statVmin_))
                .arg(ValueTableModel::formatValue(statVmax_)));
    }
    else if (statFinite_ > 0)
    {
        analysisLabel_->setText(
            tr("min=%1  max=%2  mean=%3  n=%4")
                .arg(ValueTableModel::formatValue(statVmin_),
                     ValueTableModel::formatValue(statVmax_),
                     ValueTableModel::formatValue(statSum_ / statFinite_))
                .arg(statFinite_));
    }
    else
    {
        analysisLabel_->setText(QString());
    }

    QStringList tip;
    tip << tr("Device: %1").arg(series.device);
    tip << tr("Points: %1").arg(series.ts.size());
    if (statFinite_ > 0)
    {
        tip << tr("Min: %1").arg(ValueTableModel::formatValue(statVmin_));
        tip << tr("Max: %1").arg(ValueTableModel::formatValue(statVmax_));
        tip << tr("Mean: %1").arg(ValueTableModel::formatValue(statSum_ / statFinite_));
    }
    if (statNa_ > 0)
    {
        tip << tr("Non-numeric rows: %1").arg(statNa_);
    }
    plot_->setToolTip(tip.join(QLatin1Char('\n')));
    if (!series.ts.isEmpty())
    {
        rangeLabel_->setText(tr("Range: %1 .. %2")
                                 .arg(formatFullTimeUs(series.ts.first()),
                                      formatFullTimeUs(series.ts.last())));
    }
    else
    {
        rangeLabel_->setText(tr("Range: -"));
    }
}

void MainWindow::rebuildPlot()
{
    const SeriesData* series = valueModel_->series();
    plot_->clearPlottables();
    plotGraph_ = nullptr;
    if (series != nullptr && !series->ts.isEmpty() && series->numeric)
    {
        auto* graph = plot_->addGraph();
        // X axis in seconds (ts / 1e6, same base as the Time column). Raw
        // points live in plotData_ (shared container, chunk-appended);
        // rebuildPlot only re-attaches, so progressive chunks don't copy.
        if (!plotData_.isNull())
        {
            graph->setData(plotData_);
        }
        // Adaptive sampling keeps the per-pixel min/max envelope for
        // medium-density data; multi-million-point views switch to
        // envelopeData_.
        graph->setAdaptiveSampling(true);
        // Straight lines between samples: values connect directly (a 0..7
        // counter shows as diagonal ramps), rather than stepped hold levels.
        graph->setLineStyle(QCPGraph::lsLine);
        applyScatterSize(graph);
        plotGraph_ = graph;
        // Selection tracer: marks the table's current row on the curve.
        if (tracer_ == nullptr)
        {
            tracer_ = new QCPItemTracer(plot_);
            tracer_->setStyle(QCPItemTracer::tsCrosshair);
            tracer_->setSize(12);
            tracer_->setPen(QPen(QColor(0x1e, 0x8e, 0x3e), 1));
            // Crosshair line only: no dot fill.
            tracer_->setBrush(Qt::NoBrush);
            // Readout of the traced sample: top-right corner of the plot.
            tracerInfo_ = new QCPItemText(plot_);
            tracerInfo_->setPositionAlignment(Qt::AlignTop | Qt::AlignRight);
            tracerInfo_->position->setType(QCPItemPosition::ptAxisRectRatio);
            tracerInfo_->position->setCoords(0.98, 0.02);
            tracerInfo_->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
            tracerInfo_->setFont(QFont(QStringLiteral("Consolas"), 9));
            tracerInfo_->setColor(QColor(0x11, 0x14, 0x18));
            tracerInfo_->setPadding(QMargins(4, 4, 4, 4));
            tracerInfo_->setVisible(false);
        }
        tracer_->setGraph(graph);
        // While a query streams, keep following the data (the loading
        // overlay blocks interaction, so there is no user zoom to preserve)
        // — a fit on the first chunk alone would clip the global min/max
        // that later chunks bring in. y comes from the running stats (O(1)),
        // x from the sorted container's key range. After the query completes
        // the view keeps the user's zoom.
        if (fitOnNextRebuild_ || loading_)
        {
            graph->rescaleKeyAxis();
            if (statFinite_ > 0)
            {
                double pad = (statVmax_ - statVmin_) * 0.05;
                if (pad <= 0)
                {
                    pad = std::abs(statVmax_) * 0.05 + 1e-9;
                }
                if (pad <= 0)
                {
                    pad = 1.0;  // flat line at zero
                }
                plot_->yAxis->setRange(statVmin_ - pad, statVmax_ + pad);
            }
            fitOnNextRebuild_ = false;
        }
    }
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

void MainWindow::applyScatterSize(QCPGraph* graph)
{
    if (graph == nullptr)
    {
        return;
    }
    // Visible point count decides marker visibility: dense views
    // (>kScatterVisibleLimit) hide the red markers (they'd overlap into a
    // band anyway, and skipping them saves the scatter pass's extra data
    // walk per replot); zoomed-in views show them so individual samples
    // stand out.
    const QCPRange range = plot_->xAxis->range();
    const qint64 visible =
        countInRange(plotData_.data(), range.lower, range.upper);
    const bool showMarkers = visible <= kScatterVisibleLimit;
    // 4px red dot, no outline.
    graph->setScatterStyle(showMarkers
                               ? QCPScatterStyle(QCPScatterStyle::ssCircle,
                                                 QPen(Qt::NoPen),
                                                 QBrush(QColor(Qt::red)),
                                                 4)
                               : QCPScatterStyle(QCPScatterStyle::ssNone));
    // Black curve on white; matches the monochrome theme.
    graph->setPen(QPen(QColor(0x1f, 0x23, 0x28), 1));
}

// Clamp the x view into the zoom bounds derived from the loaded data:
// - zoom-out cap: [0.8*dataMin, 1.2*dataMax] — beyond that the plot shows
//   mostly empty space, so the range is pulled back to the cap (keeping the
//   wheel center where the user zoomed);
// - zoom-in floor: a range showing fewer than kMinVisiblePoints is refused;
//   the previous (still legal) range is restored.
// Returns the (possibly adjusted) range to set, and false when nothing needs
// changing. Returns true with an unchanged range when no data is loaded
// (nothing to clamp against).
bool MainWindow::clampXRange(QCPRange& r) const
{
    if (plotData_.isNull() || plotData_->isEmpty())
    {
        return false;
    }
    bool foundRange = false;
    const QCPRange data = plotData_->keyRange(foundRange);
    if (!foundRange)
    {
        return false;  // e.g. all-NaN values: nothing sane to clamp against
    }
    // Zoom-in floor: fewer visible points than the floor — restore the last
    // legal range (lastGoodXRange_ is updated only through this gate).
    if (countInRange(plotData_.data(), r.lower, r.upper) < kMinVisiblePoints)
    {
        if (lastGoodXRange_.has_value() &&
            lastGoodXRange_->lower != r.lower &&
            lastGoodXRange_->upper != r.upper)
        {
            r = *lastGoodXRange_;
            return true;
        }
        // No legal predecessor (e.g. the whole series has fewer points than
        // the floor): keep the full data range.
        r = data;
        return true;
    }
    // Zoom-out cap: 20% padding on each side of the data range.
    const double loCap = data.lower - 0.2 * data.size();
    const double hiCap = data.upper + 0.2 * data.size();
    if (r.lower < loCap || r.upper > hiCap)
    {
        r.lower = qMax(r.lower, loCap);
        r.upper = qMin(r.upper, hiCap);
        if (r.lower >= r.upper)
        {
            r = data;  // degenerate after clamping: fall back to the data
        }
        return true;
    }
    return false;
}

void MainWindow::onPlotXRangeChanged(const QCPRange&)
{
    // Zoom bounds enforcement happens here (wheel zoom goes through
    // scaleRange -> setRange -> rangeChanged). setRange from inside a
    // rangeChanged handler is safe (no recursion: the clamped value is
    // already inside the bounds).
    QCPRange r = plot_->xAxis->range();
    if (clampXRange(r))
    {
        plot_->xAxis->setRange(r);
    }
    else
    {
        lastGoodXRange_ = r;
    }
    // Re-decide marker visibility for the new density (show only when the
    // view is zoomed in enough); queued replot avoids storms during wheel
    // interaction.
    applyScatterSize(plotGraph_);
    // Raw vs envelope switch for the new visible density.
    updateGraphData();
    // Refit y to the visible curve. Not while a query streams: the rebuild
    // that triggered this fits y from the O(1) running stats right after.
    if (!loading_)
    {
        rescaleYToVisible();
    }
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

// Append a chunk to the shared raw container (created empty on fresh=true).
void MainWindow::appendPlotData(const SeriesData& chunk, bool fresh)
{
    if (fresh || plotData_.isNull())
    {
        plotData_ = QSharedPointer<QCPGraphDataContainer>::create();
    }
    QVector<QCPGraphData> pts;
    pts.reserve(chunk.ts.size());
    for (int i = 0; i < chunk.ts.size(); ++i)
    {
        pts.push_back({static_cast<double>(chunk.ts.at(i)) / 1e6,
                       chunk.value.at(i)});
    }
    // Rows stream in file order and ts is monotonic within a file: sorted.
    // (Multi-file concatenations with overlapping time ranges are flagged
    // in the file tooltip; they keep the same sorted assumption the table
    // click-bisection always made.)
    plotData_->add(pts, true);
}

// One-pass min/max envelope over the loaded series: kEnvelopeBuckets buckets
// across the time span, each contributing its first/min/max/last sample. NaN
// rows stay NaN so line breaks survive decimation.
QSharedPointer<QCPGraphDataContainer> MainWindow::buildEnvelope() const
{
    const SeriesData* series = valueModel_->series();
    if (series == nullptr || series->ts.isEmpty() || plotData_.isNull())
    {
        return {};
    }
    const qint64 n = series->ts.size();
    const double t0 = static_cast<double>(series->ts.first()) / 1e6;
    const double t1 = static_cast<double>(series->ts.last()) / 1e6;
    if (t1 <= t0)
    {
        return {};  // degenerate span: raw data is small anyway
    }
    const double scale = kEnvelopeBuckets / (t1 - t0);
    QVector<QCPGraphData> pts;
    pts.reserve(int(kEnvelopeBuckets * 3));
    double bucketT0 = 0, bucketT1 = 0;
    double first = 0, last = 0, vMin = 0, vMax = 0, vMinT = 0, vMaxT = 0;
    bool hasFinite = false, bucketOpen = false;
    qint64 bucket = -1;
    auto closeBucket = [&]
    {
        pts.push_back({bucketT0, first});
        if (hasFinite)
        {
            // Min/max in chronological order: the container expects
            // non-decreasing keys.
            if (vMinT <= vMaxT)
            {
                pts.push_back({vMinT, vMin});
                pts.push_back({vMaxT, vMax});
            }
            else
            {
                pts.push_back({vMaxT, vMax});
                pts.push_back({vMinT, vMin});
            }
        }
        pts.push_back({bucketT1, last});
    };
    qint64 prevTs = std::numeric_limits<qint64>::min();
    for (qint64 i = 0; i < n; ++i)
    {
        const qint64 ts = series->ts.at(int(i));
        // Overlapping multi-file concatenations are not globally
        // time-sorted: bucketing and the sorted-container logic assume
        // monotonic keys, so give up (raw + adaptive sampling still draws).
        if (ts < prevTs)
        {
            return {};
        }
        prevTs = ts;
        const double t = static_cast<double>(ts) / 1e6;
        const double v = series->value.at(int(i));
        const qint64 b = qMin(kEnvelopeBuckets - 1,
                              static_cast<qint64>((t - t0) * scale));
        if (!bucketOpen || b != bucket)
        {
            if (bucketOpen)
            {
                // Close the bucket: first, min, max, last (NaN-only buckets
                // collapse to one NaN point so line breaks survive).
                closeBucket();
            }
            bucket = b;
            bucketT0 = t;
            first = v;
            vMin = v;
            vMax = v;
            vMinT = t;
            vMaxT = t;
            hasFinite = !std::isnan(v);
            bucketT1 = t;
            last = v;
            bucketOpen = true;
            continue;
        }
        if (!std::isnan(v))
        {
            if (!hasFinite || v < vMin)
            {
                vMin = v;
                vMinT = t;
            }
            if (!hasFinite || v > vMax)
            {
                vMax = v;
                vMaxT = t;
            }
            hasFinite = true;
        }
        bucketT1 = t;
        last = v;
    }
    if (bucketOpen)
    {
        closeBucket();
    }
    auto out = QSharedPointer<QCPGraphDataContainer>::create();
    out->add(pts, true);
    return out;
}

// Swap the graph between the raw container and the envelope for the current
// x range, so pan/zoom replots stay interactive on multi-million-point
// series.
void MainWindow::updateGraphData()
{
    if (plotGraph_ == nullptr)
    {
        return;
    }
    const QCPRange r = plot_->xAxis->range();
    const bool wantEnvelope =
        !envelopeData_.isNull() &&
        countInRange(plotData_.data(), r.lower, r.upper) > kRawVisibleLimit;
    if (wantEnvelope == useEnvelope_)
    {
        return;
    }
    useEnvelope_ = wantEnvelope;
    plotGraph_->setData(wantEnvelope ? envelopeData_ : plotData_);
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

// Auto-follow y: fit the value axis to the finite samples inside the current
// x range. The value axis is not user-zoomable (drag/zoom act on x only), so
// refitting on every x change never fights the user — panning to a quieter
// region tightens the view, zooming out brings the spikes back.
void MainWindow::rescaleYToVisible()
{
    if (plotGraph_ == nullptr)
    {
        return;
    }
    // inKeyRange=true: binary-search bounds + NaN-skipping walk over the
    // graph's ACTIVE container (raw or envelope — the envelope preserves the
    // visible min/max exactly), so the cost tracks the visible points, not
    // the series length.
    const QCPRange before = plot_->yAxis->range();
    plotGraph_->rescaleValueAxis(false, true);
    const QCPRange after = plot_->yAxis->range();
    if (after == before)
    {
        return;  // no finite samples in view (all NaN): keep the range
    }
    // Same 5% breathing room the load-end fit uses.
    const double pad = after.size() * 0.05;
    plot_->yAxis->setRange(after.lower - pad, after.upper + pad);
}

void MainWindow::resetStats()
{
    statVmin_ = std::numeric_limits<double>::quiet_NaN();
    statVmax_ = std::numeric_limits<double>::quiet_NaN();
    statSum_ = 0;
    statFinite_ = 0;
    statNa_ = 0;
}

void MainWindow::accumulateStats(const SeriesData& chunk)
{
    for (double v : chunk.value)
    {
        if (std::isnan(v))
        {
            ++statNa_;
            continue;
        }
        if (std::isnan(statVmin_) || v < statVmin_) statVmin_ = v;
        if (std::isnan(statVmax_) || v > statVmax_) statVmax_ = v;
        statSum_ += v;
        ++statFinite_;
    }
}

// Move the crosshair tracer to a sample and refresh the top-right readout
// (row < 0 hides both).
void MainWindow::updateTracer(int row)
{
    if (tracer_ == nullptr)
    {
        return;
    }
    const SeriesData* series = valueModel_->series();
    const bool valid = plotGraph_ != nullptr && series != nullptr &&
                       row >= 0 && row < series->ts.size();
    tracer_->setVisible(valid);
    if (tracerInfo_ != nullptr)
    {
        tracerInfo_->setVisible(valid);
    }
    if (!valid)
    {
        plot_->replot(QCustomPlot::rpQueuedReplot);
        return;
    }
    tracer_->setGraphKey(static_cast<double>(series->ts[row]) / 1e6);
    if (tracerInfo_ != nullptr)
    {
        // Readout: wall-clock timestamp (same format as the Time column) +
        // param: value. Text rows keep their string; numeric cells render
        // via the shared formatter.
        QString value = row < series->text.size() && !series->text[row].isEmpty()
                            ? series->text[row]
                            : ValueTableModel::formatValue(series->value[row]);
        tracerInfo_->setText(
            QStringLiteral("%1\n%2: %3")
                .arg(ValueTableModel::formatTimeUs(series->ts[row]),
                     series->measurement, value));
    }
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

// Sync the table to a source row (plot click / arrow-key navigation):
// select it, center it in the table when it is off-view, keep the sample
// in the plot's 10%..90% band, place the tracer.
void MainWindow::selectSampleRow(int row)
{
    const SeriesData* series = valueModel_->series();
    if (series == nullptr || row < 0 || row >= series->ts.size())
    {
        return;
    }
    const QModelIndex idx = valueModel_->index(row, 1);
    // Programmatic selection: don't let it re-trigger the plot sync
    // (that would zoom away from the spot the user just clicked).
    suppressPlotSync_ = true;
    valuesTable_->setCurrentIndex(idx);
    // Center the row only when it is off-view; visible rows stay put so
    // consecutive arrow steps don't scroll the table around.
    if (!valuesTable_->viewport()->rect()
             .contains(valuesTable_->visualRect(idx)))
    {
        valuesTable_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    }
    // Same for the plot: the selected sample must always sit in the middle
    // 80% of the view (10%..90%) — shared rule with the table-driven sync.
    panToBand(plot_, *series, static_cast<double>(series->ts[row]) / 1e6);
    updateTracer(row);
}

// Table row -> plot sync: pan so the selected sample stays inside the
// 10%..90% band (zoom unchanged).
void MainWindow::syncPlotToRow(const QModelIndex& idx)
{
    const SeriesData* series = valueModel_->series();
    const int row = idx.isValid() ? idx.row() : -1;
    if (series == nullptr || row < 0 || row >= series->ts.size())
    {
        return;
    }
    panToBand(plot_, *series, static_cast<double>(series->ts.at(row)) / 1e6);
    plot_->replot();
    updateTracer(row);
}

// Table row activation (double-click / Enter): zoom to the marker-visible
// window centered on the row.
void MainWindow::activateRow(const QModelIndex& idx)
{
    const SeriesData* series = valueModel_->series();
    const int row = idx.isValid() ? idx.row() : -1;
    if (series == nullptr || row < 0 || row >= series->ts.size())
    {
        return;
    }
    const int n = series->ts.size();
    if (n > kScatterVisibleLimit)
    {
        // Window of kScatterVisibleLimit consecutive samples centered on
        // the row — the widest span where red markers still show, with the
        // selected sample exactly in the middle. ts is sorted, so the row's
        // neighbors bound it directly.
        const int half = static_cast<int>(kScatterVisibleLimit / 2 - 1);
        const int lo = qMax(0, row - half);
        const int hi = qMin(n - 1, row + half);
        double a = static_cast<double>(series->ts.at(lo)) / 1e6;
        double b = static_cast<double>(series->ts.at(hi)) / 1e6;
        const double t = static_cast<double>(series->ts.at(row)) / 1e6;
        // Duplicate timestamps can pack more points into the value range
        // than the window's row count — the marker visibility check counts
        // by value. Shrink symmetrically around the selected sample until
        // it actually fits the threshold.
        while (b > a && countInRange(plotData_.data(), a, b) > kScatterVisibleLimit)
        {
            a = t + (a - t) * 0.9;
            b = t + (b - t) * 0.9;
        }
        if (b > a)
        {
            plot_->xAxis->setRange(a, b);
        }
        else
        {
            // Degenerate (identical timestamps): small window around the
            // sample.
            plot_->xAxis->setRange(t - 0.5, t + 0.5);
        }
    }
    else
    {
        // Whole series already fits under the marker threshold.
        plot_->xAxis->setRange(static_cast<double>(series->ts.first()) / 1e6,
                               static_cast<double>(series->ts.last()) / 1e6);
    }
    plot_->replot();
    updateTracer(row);
}

// Source row of the sample nearest to time t (seconds); bisection on the
// sorted ts vector (-1 when the series is empty).
int MainWindow::nearestSampleRow(double t) const
{
    const SeriesData* series = valueModel_->series();
    if (series == nullptr || series->ts.isEmpty())
    {
        return -1;
    }
    const QVector<qint64>& ts = series->ts;
    const double tx = t * 1e6;  // back to us
    // First sample >= t by bisection (ts sorted ascending)...
    int lo = 0, hi = ts.size() - 1;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (static_cast<double>(ts[mid]) < tx)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    // ...then lo is the first >= t; check lo-1 too for the true nearest.
    int row = lo;
    if (lo > 0 &&
        std::abs(static_cast<double>(ts[lo - 1]) - tx) <
            std::abs(static_cast<double>(ts[lo]) - tx))
    {
        row = lo - 1;
    }
    return row;
}

void MainWindow::onPlotMousePress(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton)
    {
        return;
    }
    const SeriesData* series = valueModel_->series();
    if (series == nullptr || series->ts.isEmpty() || plotGraph_ == nullptr)
    {
        return;
    }
    // Only when the click is on the graph (its selectable scatter).
    // selectTest returns the pixel distance to the curve; treat
    // <=8px as "on the curve" so plain background drags don't jump.
    if (plotGraph_->selectTest(ev->pos(), false) > 8.0)
    {
        return;  // click not on the curve: plain drag, no table jump
    }
    const double x = plot_->xAxis->pixelToCoord(ev->pos().x());
    selectSampleRow(nearestSampleRow(x));
}

// Arrow navigation shared by the table and the plot: Up/Down move the
// table's selected row (the selectionChanged handler then syncs the plot —
// the usual table-driven path); Left/Right step the selected sample
// (plot-driven path: table row follows, view pans to keep the point in the
// 10%..90% band).
void MainWindow::stepSelection(int key)
{
    const SeriesData* series = valueModel_->series();
    if (series == nullptr || series->ts.isEmpty())
    {
        return;
    }
    if (key == Qt::Key_Up || key == Qt::Key_Down)
    {
        const int rows = valueModel_->rowCount();
        if (rows <= 0)
        {
            return;
        }
        const QModelIndex cur = valuesTable_->currentIndex();
        const int r = cur.isValid() ? cur.row() : 0;
        const int next = key == Qt::Key_Up
                             ? qMax(0, r - 1)
                             : qMin(rows - 1, r + 1);
        // Plain selection change (NOT suppressed): the plot sync fires.
        const QModelIndex idx =
            valueModel_->index(next, cur.isValid() ? cur.column() : 1);
        valuesTable_->setCurrentIndex(idx);
        valuesTable_->scrollTo(idx, QAbstractItemView::EnsureVisible);
        return;
    }
    // Left/Right: step the selected sample.
    const QModelIndex cur = valuesTable_->currentIndex();
    const int row = cur.isValid() ? cur.row() : -1;
    if (row < 0)
    {
        return;
    }
    const int next = key == Qt::Key_Left
                         ? qMax(0, row - 1)
                         : qMin(static_cast<int>(series->ts.size()) - 1,
                                row + 1);
    if (next != row)
    {
        selectSampleRow(next);
    }
}

void MainWindow::exportCsv()
{
    if (currentParam_.measurement.isEmpty() || loading_)
    {
        return;
    }
    if (exportWatcher_.isRunning())
    {
        statusBar()->showMessage(tr("An export is already in progress — please wait for it to finish."), 4000);
        return;
    }
    const QString suggested =
        currentParam_.measurement + QLatin1Char('_') +
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd hhmmss")) +
        QStringLiteral(".csv");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export CSV"), suggested, tr("CSV (*.csv);;All files (*.*)"));
    if (path.isEmpty())
    {
        return;
    }
    statusBar()->showMessage(tr("Exporting %1 ...").arg(path));
    exportBtn_->setEnabled(false);
    busy_->show();
    overlay_->begin();
    // BlockingQueuedConnection inside runs on the worker thread; QtConcurrent
    // keeps the UI thread free while that happens.
    const ParamInfo param = currentParam_;
    auto* future = new QFuture<bool>();
    *future = QtConcurrent::run([this, param, path]() -> bool
    {
        QString err;
        const bool ok = doc_->exportCsvBlocking(param, path, &err);
        if (!ok)
        {
            QMetaObject::invokeMethod(this, [this, err]
            {
                statusBar()->showMessage(tr("Export failed: %1").arg(err));
            }, Qt::QueuedConnection);
        }
        return ok;
    });
    exportWatcher_.setFuture(*future);
    connect(&exportWatcher_, &QFutureWatcher<bool>::finished, this, [this, future]
    {
        const bool ok = exportWatcher_.result();
        delete future;
        busy_->hide();
        overlay_->end();
        exportBtn_->setEnabled(true);
        if (ok)
        {
            statusBar()->showMessage(tr("Export finished — CSV saved."), 5000);
        }
    });
}

void MainWindow::clearPlot()
{
    plot_->clearPlottables();
    plotGraph_ = nullptr;
    if (tracer_ != nullptr)
    {
        tracer_->setGraph(nullptr);
    }
    updateTracer(-1);
    plot_->replot();
    plotData_.clear();
    envelopeData_.clear();
    useEnvelope_ = false;
    lastGoodXRange_.reset();
}

void MainWindow::clearContent()
{
    valueModel_->setSeries(SeriesData{});
    clearPlot();
    plot_->setVisible(true);  // a TEXT series hides it; restore on clear
    paramNameLabel_->setText(QString());
    paramStatLabel_->setText(QString());
    paramStatLabel_->setToolTip(QString());
    codecLabel_->setVisible(true);
    codecLabel_->setText(tr("Codec: -"));
    analysisLabel_->setText(QString());
    plot_->setToolTip(QString());
    exportBtn_->setEnabled(false);
    resetStats();
}

void MainWindow::applyTreeHeader()
{
    // Parameter names take all remaining width (a fixed width truncates
    // them); the short Type labels fit their content.
    paramTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    paramTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    paramTree_->header()->setStretchLastSection(false);
}

void MainWindow::applySearch(const QString& text)
{
    treeModel_->setFilter(text);
    // The rebuild starts devices collapsed; re-expand so matched leaves
    // stay visible. Also auto-select the first match for quick
    // Enter-loading.
    paramTree_->expandAll();
    applyTreeHeader();
    if (!text.isEmpty())
    {
        const QModelIndex first = treeModel_->index(0, 0);
        if (first.isValid())
        {
            paramTree_->setCurrentIndex(first);
        }
    }
}

void MainWindow::onParamActivated()
{
    if (loading_)
    {
        statusBar()->showMessage(
            tr("Still loading %1 — please wait for the current query to finish.")
                .arg(currentParam_.key()),
            4000);
        return;
    }
    const QModelIndex proxyIndex = paramTree_->currentIndex();
    if (!proxyIndex.isValid())
    {
        return;
    }
    const QModelIndex sourceIndex = proxyIndex;  // no proxy since the tree filters itself
    if (!ParamTreeModel::isMeasurementRow(sourceIndex))
    {
        statusBar()->showMessage(
            tr("This is a group heading — expand it and double-click a parameter to load values."), 4000);
        return;
    }
    const ParamInfo param = treeModel_->paramAt(sourceIndex);
    if (param.measurement.isEmpty())
    {
        statusBar()->showMessage(tr("Nothing to load here — double-click a parameter (leaf row), not a group heading."), 4000);
        return;
    }
    // Same parameter again: the data is already loaded and displayed —
    // re-querying would stream it from disk for no visible change.
    if (param == currentParam_ && exportBtn_->isEnabled())
    {
        statusBar()->showMessage(
            tr("%1 is already loaded.").arg(param.key()), 2500);
        return;
    }
    currentParam_ = param;
    paramNameLabel_->setText(param.measurement);
    // Stats from the previous series are stale until the query completes.
    paramStatLabel_->setText(QString());
    paramStatLabel_->setToolTip(QString());
    // Multi-file: one param spans several files whose codecs may differ,
    // so no single toolbar value is honest — the files dialog lists the
    // per-file codecs instead.
    codecLabel_->setVisible(lastMeta_.fileCount <= 1);
    codecLabel_->setText(tr("Codec: %1 / %2")
                             .arg(TsFileNames::encoding(param.encoding),
                                  TsFileNames::compression(param.compression)));
    loadValues();
}

void MainWindow::loadValues()
{
    if (loading_ || currentParam_.measurement.isEmpty())
    {
        return;
    }
    loading_ = true;
    overlay_->begin();
    fitOnNextRebuild_ = true;  // new query: fit the view once, then keep zoom
    busy_->show();
    loadClock_.start();
    lastLoadReplotMs_ = -1;  // first chunk replots immediately
    // Drop the previous series immediately so a same-named parameter from
    // another file cannot append onto the leftover rows, and the table does
    // not keep showing stale data while the new series streams in.
    valueModel_->setSeries(SeriesData{});
    clearPlot();
    resetStats();
    analysisLabel_->setText(QString());
    plot_->setToolTip(QString());
    exportBtn_->setEnabled(false);
    statusBar()->showMessage(tr("Querying %1...").arg(currentParam_.key()));
    doc_->queryValuesAsync(currentParam_);
}
