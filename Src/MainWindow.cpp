#include "MainWindow.h"

#include "Models.h"

#include <qcustomplot.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QtConcurrent>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
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

double firstFinite(const QVector<double>& v)
{
    for (double x : v)
    {
        if (!std::isnan(x)) return x;
    }
    return std::numeric_limits<double>::quiet_NaN();
}
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    doc_ = new TsFileDocument(this);
    setupUi();

    connect(doc_, &TsFileDocument::opened, this,
            [this](const MetaInfo& meta, const QVector<ParamInfo>& params)
    {
        treeModel_->load(params);
        proxy_->setFilter(QString());
        paramTree_->expandAll();
        // load() clears the model, which resets the header columns to default
        // widths; re-apply the Parameter column default after each load.
        paramTree_->header()->resizeSection(0, 220);
        updateMetaBar(meta);
        clearContent();
        busy_->hide();
        if (meta.repaired)
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
    connect(doc_, &TsFileDocument::openFailed, this, [this](const QString& error)
    {
        busy_->hide();
        statusBar()->showMessage(tr("Failed: %1").arg(error));
        QMessageBox::warning(this, tr("Open failed"), error);
    });
    connect(doc_, &TsFileDocument::repairConfirmRequested, this,
            [this](const QString& path, int code)
    {
        busy_->hide();
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
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open TsFile"), startDir,
            tr("TsFile (*.tsfile);;All files (*.*)"));
        if (!path.isEmpty())
        {
            openFile(path);
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
    // Deterministic height for the header-alignment math below.
    searchEdit_->setFixedHeight(25);
    treeModel_ = new ParamTreeModel(this);
    proxy_ = new ParamProxyModel(this);
    proxy_->setSourceModel(treeModel_);
    paramTree_ = new QTreeView(left);
    paramTree_->setModel(proxy_);
    paramTree_->setSortingEnabled(false);
    paramTree_->setUniformRowHeights(true);
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

    // Paging bar, fixed height, outside any splitter.
    // Height = left margin(4) + search(25) + spacing(4) so the values-table
    // header aligns with the parameter-tree header.
    // Layout: param name | stretch | export | <<prev rows next>> | progress
    auto* pagingBar = new QWidget(rightPane);
    pagingBar->setFixedHeight(33);
    paramNameLabel_ = new QLabel(QString(), pagingBar);
    paramNameLabel_->setMinimumWidth(120);
    exportBtn_ = new QPushButton(tr("Export"), pagingBar);
    exportBtn_->setFlat(true);
    prevPage_ = new QPushButton(tr("<< Prev"), pagingBar);
    pageInfo_ = new QLabel(tr("rows 1-0"), pagingBar);
    nextPage_ = new QPushButton(tr("Next >>"), pagingBar);
    prevPage_->setFlat(true);
    nextPage_->setFlat(true);
    exportBtn_->setEnabled(false);
    prevPage_->setEnabled(false);
    nextPage_->setEnabled(false);
    auto* pagingLayout = new QHBoxLayout(pagingBar);
    pagingLayout->setContentsMargins(4, 0, 4, 0);
    pagingLayout->setSpacing(4);
    pagingLayout->addWidget(paramNameLabel_);
    pagingLayout->addStretch(1);
    pagingLayout->addWidget(exportBtn_);
    pagingLayout->addWidget(prevPage_);
    pagingLayout->addWidget(pageInfo_);
    pagingLayout->addWidget(nextPage_);
    connect(prevPage_, &QPushButton::clicked, this, [this] { loadPage(page_ - 1); });
    connect(nextPage_, &QPushButton::clicked, this, [this] { loadPage(page_ + 1); });
    connect(exportBtn_, &QPushButton::clicked, this, &MainWindow::exportCsv);

    rightLayout->addWidget(pagingBar);
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
    valuesTable_->verticalHeader()->setDefaultSectionSize(20);
    valuesTable_->verticalHeader()->hide();  // No column shows row numbers (No column exists)
    valuesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    valuesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

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
    // Fixed-precision seconds on the x axis (no scientific notation). 6
    // decimals = microsecond resolution; when the view spans hundreds of
    // seconds the decimals are noise, so precision adapts to the zoom level.
    plot_->xAxis->setNumberFormat("f");
    connect(plot_->xAxis,
            static_cast<void (QCPAxis::*)(const QCPRange&)>(
                &QCPAxis::rangeChanged),
            this, [this](const QCPRange& r)
    {
        // Label precision follows the tick step: whole-second steps print
        // as integers, sub-second steps print their decimals.
        const double step = plot_->xAxis->ticker()->getTickStep(r);
        int precision = 0;
        if (step < 1.0)
        {
            precision = static_cast<int>(
                std::ceil(-std::log10(step)) + 0.5);
            precision = qBound(1, precision, 6);
        }
        if (plot_->xAxis->numberPrecision() != precision)
        {
            plot_->xAxis->setNumberPrecision(precision);
        }
    });
    plot_->xAxis->ticker()->setTickCount(5);
    // No axis titles: tick values carry the units; the freed space goes to
    // the plot area. Context (param name / codec) lives in the paging bar.
    plot_->xAxis->setVisible(true);
    // rangeChanged is overloaded; take the single-arg form.
    connect(plot_->xAxis,
            static_cast<void (QCPAxis::*)(const QCPRange&)>(
                &QCPAxis::rangeChanged),
            this, [this](const QCPRange& r) { onPlotXRangeChanged(r); });
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

    connect(searchEdit_, &QLineEdit::textChanged, proxy_, &ParamProxyModel::setFilter);
    // Query on explicit activation (double-click / Enter / context menu),
    // not on plain selection: browsing the tree must not fire queries.
    connect(paramTree_, &QTreeView::doubleClicked, this,
            &MainWindow::onParamActivated);
    auto* activateAct = new QAction(tr("Load values"), this);
    activateAct->setShortcut(Qt::Key_Return);
    addAction(activateAct);
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
    // Arrow keys page through the loaded series; they respect the same
    // enabled state as the Prev/Next buttons (disabled at bounds/loading).
    auto* prevAct = new QAction(tr("Previous page"), this);
    prevAct->setShortcut(Qt::Key_Left);
    addAction(prevAct);
    connect(prevAct, &QAction::triggered, this, [this]
    {
        if (prevPage_->isEnabled())
        {
            loadPage(page_ - 1);
        }
    });
    auto* nextAct = new QAction(tr("Next page"), this);
    nextAct->setShortcut(Qt::Key_Right);
    addAction(nextAct);
    connect(nextAct, &QAction::triggered, this, [this]
    {
        if (nextPage_->isEnabled())
        {
            loadPage(page_ + 1);
        }
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
    // Right side: parameter analysis. The file label lives in the toolbar,
    // right after the Open button (see setupUi toolbar section).
    analysisLabel_ = new QLabel(QString(), this);
    statusBar()->addPermanentWidget(analysisLabel_);

    statusBar()->showMessage(tr("Ready — open a TsFile (*.tsfile) to begin, or double-click a parameter to load values."));
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == fileLabel_ && !currentPath_.isEmpty())
    {
        if (event->type() == QEvent::MouseButtonPress)
        {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton)
            {
                const QDir dir = QFileInfo(currentPath_).absoluteDir();
                if (dir.exists())
                {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
                }
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
    currentPath_ = path;
    fileLabel_->setText(tr("File: %1").arg(QDir::toNativeSeparators(path)));
    devicesLabel_->setText(tr("Devices: -"));
    paramsLabel_->setText(tr("Params: -"));
    rangeLabel_->setText(tr("Range: -"));
    busy_->show();
    statusBar()->showMessage(tr("Opening %1...").arg(QDir::toNativeSeparators(path)));
    doc_->openAsync(path);
}

void MainWindow::updateMetaBar(const MetaInfo& meta)
{
    currentPath_ = meta.path;
    fileLabel_->setText(tr("File: %1").arg(QDir::toNativeSeparators(meta.path)));
    if (meta.deviceCount > 0)
    {
        devicesLabel_->setText(tr("Devices: %1").arg(meta.deviceCount));
    }
    else
    {
        devicesLabel_->setText(tr("Tables: %1").arg(meta.tableCount));
    }
    paramsLabel_->setText(tr("Params: %1").arg(meta.paramCount));
    rangeLabel_->setText(tr("Range: -"));

    // Secondary details live in the file label's tooltip.
    QStringList tip;
    tip << tr("Path: %1").arg(QDir::toNativeSeparators(meta.path));
    tip << tr("Size: %1 (%2 bytes)").arg(humanSize(meta.fileSize)).arg(meta.fileSize);
    tip << tr("Layout: %1").arg(meta.tableCount > 0
                                    ? (meta.deviceCount > 0
                                           ? QStringLiteral("tree-device + table")
                                           : QStringLiteral("table"))
                                    : QStringLiteral("tree-device"));
    if (meta.deviceCount > 0 && meta.deviceCount <= 20)
    {
        tip << tr("Devices: %1").arg(meta.devices.join(QStringLiteral(", ")));
    }
    if (meta.tableCount > 0)
    {
        tip << tr("Tables: %1").arg(meta.tables.join(QStringLiteral(", ")));
    }
    tip << tr("Left-click: open containing folder");
    tip << tr("Right-click: copy path");
    fileLabel_->setToolTip(tip.join(QLatin1Char('\n')));
}

void MainWindow::onValuesChunk(const SeriesData& chunk, bool done)
{
    valueModel_->appendChunk(chunk);
    if (!done)
    {
        // Progressive load: running row count in the status bar. The total
        // row count is unknown until the query completes, so the progress
        // bar stays in busy mode (no fake percentage).
        const SeriesData* series = valueModel_->series();
        statusBar()->showMessage(
            tr("%1: loading... %2 rows")
                .arg(chunk.key())
                .arg(series ? series->ts.size() : chunk.ts.size()));
        rebuildPlot();
        return;
    }

    busy_->hide();
    loading_ = false;
    const SeriesData* series = valueModel_->series();
    if (series == nullptr)
    {
        return;
    }
    // Page info + navigation. hasMore is set when the page came back full;
    // the series total (when the metadata provided it) yields the page count.
    const qint64 first = series->offset + 1;
    const qint64 last = series->offset + series->ts.size();
    if (series->totalRows > 0)
    {
        const qint64 totalPages =
            (series->totalRows + TsFileDocument::kPageSize - 1) /
            TsFileDocument::kPageSize;
        pageInfo_->setText(tr("page %1/%2  rows %3-%4")
                               .arg(page_ + 1)
                               .arg(totalPages)
                               .arg(first)
                               .arg(last));
    }
    else
    {
        pageInfo_->setText(tr("page %1+  rows %2-%3+")
                               .arg(page_ + 1)
                               .arg(first)
                               .arg(last));
    }
    prevPage_->setEnabled(series->offset > 0);
    nextPage_->setEnabled(series->hasMore);
    exportBtn_->setEnabled(true);
    if (series->hasMore)
    {
        statusBar()->showMessage(
            tr("%1: page %2, rows %3-%4 (page size %5M rows; use Next to "
               "continue)")
                .arg(series->key())
                .arg(page_ + 1)
                .arg(first)
                .arg(last)
                .arg(TsFileDocument::kPageSize / 1000000.0, 0, 'f', 1),
            8000);
    }
    else
    {
        statusBar()->showMessage(
            tr("%1: %2 rows (end of data)").arg(series->key()).arg(series->ts.size()),
            5000);
    }
    rebuildPlot();

    // ---- stats: plot tooltip + status-bar analysis -------------------------
    double vmin = std::numeric_limits<double>::quiet_NaN();
    double vmax = std::numeric_limits<double>::quiet_NaN();
    double vmean = 0;
    qint64 finite = 0;
    for (double v : series->value)
    {
        if (std::isnan(v)) continue;
        if (std::isnan(vmin) || v < vmin) vmin = v;
        if (std::isnan(vmax) || v > vmax) vmax = v;
        vmean += v;
        ++finite;
    }
    if (finite > 0) vmean /= finite;

    // Status-bar analysis: SFID counters report the wrap behavior, others
    // report min/max/mean.
    if (series->measurement.endsWith(QLatin1String("SFID"), Qt::CaseSensitive))
    {
        qint64 inc1 = 0, wraps = 0, violations = 0;
        double prev = std::numeric_limits<double>::quiet_NaN();
        for (double v : series->value)
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
                .arg(finite - (finite > 0 ? 1 : 0))
                .arg(wraps)
                .arg(violations)
                .arg(QString::number(vmin, 'g', 17))
                .arg(QString::number(vmax, 'g', 17)));
    }
    else if (finite > 0)
    {
        analysisLabel_->setText(
            tr("min=%1  max=%2  mean=%3  n=%4")
                .arg(QString::number(vmin, 'g', 17))
                .arg(QString::number(vmax, 'g', 17))
                .arg(QString::number(vmean, 'g', 17))
                .arg(finite));
    }
    else
    {
        analysisLabel_->setText(QString());
    }

    QStringList tip;
    tip << tr("Device: %1").arg(series->device);
    tip << tr("Points: %1").arg(series->ts.size());
    if (finite > 0)
    {
        tip << tr("Min: %1").arg(QString::number(vmin, 'g', 17));
        tip << tr("Max: %1").arg(QString::number(vmax, 'g', 17));
        tip << tr("Mean: %1").arg(QString::number(vmean, 'g', 17));
    }
    if (finite != series->ts.size())
    {
        tip << tr("Non-numeric rows: %1").arg(series->ts.size() - finite);
    }
    plot_->setToolTip(tip.join(QLatin1Char('\n')));
    if (!series->ts.isEmpty())
    {
        rangeLabel_->setText(tr("Range: %1 s .. %2 s")
                                 .arg(series->ts.first() / 1e6, 0, 'f', 3)
                                 .arg(series->ts.last() / 1e6, 0, 'f', 3));
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
    if (series != nullptr && !series->ts.isEmpty())
    {
        // X axis in raw microseconds timestamps (same as the Time column),
        // so plot and table agree across pages.
        QVector<double> x(series->ts.size());
        for (int i = 0; i < series->ts.size(); ++i)
        {
            x[i] = static_cast<double>(series->ts[i]) / 1e6;
        }
        auto* graph = plot_->addGraph();
        graph->setData(x, series->value, true);
        // Keep min/max of dense data visible at screen resolution: with
        // ~1M points over ~1k px, plain line drawing collapses shapes into
        // a band; adaptive sampling keeps the per-pixel min/max envelope.
        graph->setAdaptiveSampling(true);
        // Straight lines between samples: values connect directly (a 0..7
        // counter shows as diagonal ramps), rather than stepped hold levels.
        graph->setLineStyle(QCPGraph::lsLine);
        // Circle marker per visible sample; size scales with zoom (see
        // onPlotXRangeChanged): small (cheap) when dense, larger when the
        // view holds only a few hundred samples.
        applyScatterSize(graph, series->ts.size());
        plotGraph_ = graph;
        // Fit the view only while the page is still loading (first fit);
        // afterwards keep the user's zoom: re-fitting on every progressive
        // chunk would snap the view back to the full page range and make
        // zooming-in impossible (sawtooth stays compressed into bars).
        if (fitOnNextRebuild_)
        {
            graph->rescaleAxes();
            // Padding so the curve is not glued to the frame.
            const double pad =
                std::abs(plot_->yAxis->range().size()) * 0.05 + 1e-9;
            plot_->yAxis->setRange(plot_->yAxis->range().lower - pad,
                                   plot_->yAxis->range().upper + pad);
            fitOnNextRebuild_ = false;
        }
    }
    if (series != nullptr)
    {
        // No axis title (plot area stays maximal); current param already
        // shown in the paging bar.
        plot_->xAxis->setLabel(QString());
    }
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

void MainWindow::applyScatterSize(QCPGraph* graph, int totalRows)
{
    if (graph == nullptr)
    {
        return;
    }
    Q_UNUSED(totalRows);
    graph->setScatterStyle(QCPScatterStyle(
        QCPScatterStyle::ssCircle,
        Qt::NoPen,                                 // no outline
        QBrush(QColor(Qt::red)),                   // solid red fill
        2));
    // Black curve on white; matches the monochrome theme.
    graph->setPen(QPen(QColor(0x1f, 0x23, 0x28), 1));
}

void MainWindow::onPlotXRangeChanged(const QCPRange&)
{
    // Retune marker size for the new density; queued replot avoids storms
    // during wheel interaction.
    applyScatterSize(plotGraph_,
                     valueModel_->series() ? valueModel_->series()->ts.size()
                                           : 0);
    plot_->replot(QCustomPlot::rpQueuedReplot);
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
        exportBtn_->setEnabled(true);
        if (ok)
        {
            statusBar()->showMessage(tr("Export finished — CSV saved."), 5000);
        }
    });
}

void MainWindow::clearContent()
{
    valueModel_->setSeries(SeriesData{});
    plot_->clearPlottables();
    plotGraph_ = nullptr;
    plot_->replot();
}

void MainWindow::onParamActivated()
{
    if (loading_)
    {
        statusBar()->showMessage(
            tr("Still loading %1 — please wait for the current page to finish.")
                .arg(currentParam_.key()),
            4000);
        return;
    }
    const QModelIndex proxyIndex = paramTree_->currentIndex();
    if (!proxyIndex.isValid())
    {
        return;
    }
    const QModelIndex sourceIndex = proxy_->mapToSource(proxyIndex);
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
    currentParam_ = param;
    paramNameLabel_->setText(param.measurement);
    codecLabel_->setText(tr("Codec: %1 / %2")
                             .arg(TsFileNames::encoding(param.encoding),
                                  TsFileNames::compression(param.compression)));
    loadPage(0);
}

void MainWindow::loadPage(qint64 page)
{
    if (loading_ || currentParam_.measurement.isEmpty() || page < 0)
    {
        return;
    }
    page_ = page;
    loading_ = true;
    fitOnNextRebuild_ = true;  // new page: fit the view once, then keep zoom
    busy_->show();
    prevPage_->setEnabled(false);
    nextPage_->setEnabled(false);
    statusBar()->showMessage(
        tr("Querying %1 (page %2)...").arg(currentParam_.key()).arg(page_ + 1));
    doc_->queryValuesAsync(currentParam_, page_);
}
