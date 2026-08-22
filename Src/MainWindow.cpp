#include "MainWindow.h"

#include "Models.h"

#include <qcustomplot.h>

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QToolBar>
#include <QTreeView>
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
        updateMetaBar(meta);
        clearContent();
        busy_->hide();
        statusBar()->showMessage(tr("Loaded %1").arg(meta.path), 5000);
    });
    connect(doc_, &TsFileDocument::openFailed, this, [this](const QString& error)
    {
        busy_->hide();
        statusBar()->showMessage(tr("Error: %1").arg(error));
        QMessageBox::warning(this, tr("Open failed"), error);
    });
    connect(doc_, &TsFileDocument::valuesChunk, this, &MainWindow::onValuesChunk);
    connect(doc_, &TsFileDocument::queryFailed, this, [this](const QString& error)
    {
        busy_->hide();
        progress_->hide();
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
    connect(openAct, &QAction::triggered, this, [this]
    {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open TsFile"), QString(),
            tr("TsFile (*.tsfile);;All files (*.*)"));
        if (!path.isEmpty())
        {
            openFile(path);
        }
    });

    auto addSep = [toolbar] { toolbar->addSeparator(); };

    fileLabel_ = new QLabel(tr("File: -"), toolbar);
    fileLabel_->setMinimumWidth(140);
    toolbar->addWidget(fileLabel_);
    addSep();

    devicesLabel_ = new QLabel(tr("Devices: -"), toolbar);
    toolbar->addWidget(devicesLabel_);
    addSep();

    tablesLabel_ = new QLabel(tr("Tables: -"), toolbar);
    toolbar->addWidget(tablesLabel_);
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

    auto* left = new QWidget(central);
    searchEdit_ = new QLineEdit(left);
    searchEdit_->setPlaceholderText(tr("Search parameter..."));
    searchEdit_->setClearButtonEnabled(true);
    treeModel_ = new ParamTreeModel(this);
    proxy_ = new ParamProxyModel(this);
    proxy_->setSourceModel(treeModel_);
    paramTree_ = new QTreeView(left);
    paramTree_->setModel(proxy_);
    paramTree_->setSortingEnabled(false);
    // Parameter column stretches; Type column fixed width.
    paramTree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    paramTree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    paramTree_->header()->resizeSection(1, 80);
    paramTree_->setUniformRowHeights(true);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(4, 4, 4, 4);
    leftLayout->addWidget(searchEdit_);
    leftLayout->addWidget(paramTree_, 1);

    auto* right = new QSplitter(Qt::Vertical, central);
    valueModel_ = new ValueTableModel(this);
    valuesTable_ = new QTableView(right);
    valuesTable_->setModel(valueModel_);
    valuesTable_->horizontalHeader()->setStretchLastSection(true);
    valuesTable_->verticalHeader()->setDefaultSectionSize(20);
    valuesTable_->verticalHeader()->hide();  // No column shows row numbers (No column exists)
    valuesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    valuesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    plot_ = new QCustomPlot(right);
    plot_->setInteraction(QCP::iRangeDrag, true);
    // Zoom the x axis only: y stays fit to the loaded data, so dense sawtooth
    // waveforms keep a stable amplitude scale while scrolling in time.
    plot_->setInteraction(QCP::iRangeZoom, true);
    plot_->axisRect()->setRangeZoomAxes(plot_->xAxis, nullptr);
    plot_->axisRect()->setRangeDragAxes(plot_->xAxis, nullptr);
    plot_->setNoAntialiasingOnDrag(true);
    // Plain-integer tick labels for microsecond timestamps (no 2.32e9).
    plot_->xAxis->setNumberFormat("f");
    plot_->xAxis->setNumberPrecision(0);
    plot_->xAxis->setLabel(tr("Time (us)"));
    plot_->yAxis->setLabel(tr("Value"));
    right->setStretchFactor(0, 1);
    right->setStretchFactor(1, 1);
    right->setSizes({320, 320});

    central->addWidget(left);
    central->addWidget(right);
    central->setStretchFactor(0, 1);
    central->setStretchFactor(1, 3);
    central->setSizes({340, 940});
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

    // ---- status bar: paging + load progress -------------------------------
    prevPage_ = new QPushButton(tr("<< Prev"), this);
    nextPage_ = new QPushButton(tr("Next >>"), this);
    pageInfo_ = new QLabel(tr("rows 1-0"), this);
    prevPage_->setFlat(true);
    nextPage_->setFlat(true);
    prevPage_->setEnabled(false);
    nextPage_->setEnabled(false);
    statusBar()->addPermanentWidget(prevPage_);
    statusBar()->addPermanentWidget(pageInfo_);
    statusBar()->addPermanentWidget(nextPage_);
    connect(prevPage_, &QPushButton::clicked, this, [this] { loadPage(page_ - 1); });
    connect(nextPage_, &QPushButton::clicked, this, [this] { loadPage(page_ + 1); });

    // Busy-mode bar: the total row count is unknown until the query
    // finishes, so no percentage is shown — just an active pulse plus the
    // row count in the status bar text.
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 0);
    progress_->setTextVisible(false);
    progress_->setMaximumWidth(220);
    progress_->setToolTip(tr("Loading..."));
    progress_->hide();
    statusBar()->addPermanentWidget(progress_);

    statusBar()->showMessage(tr("Ready. Open a .tsfile to begin."));
}

void MainWindow::openFile(const QString& path)
{
    clearContent();
    treeModel_->load({});
    fileLabel_->setText(tr("File: %1").arg(QDir::toNativeSeparators(path)));
    devicesLabel_->setText(tr("Devices: -"));
    tablesLabel_->setText(tr("Tables: -"));
    paramsLabel_->setText(tr("Params: -"));
    rangeLabel_->setText(tr("Range: -"));
    busy_->show();
    statusBar()->showMessage(tr("Opening %1...").arg(path));
    doc_->openAsync(path);
}

void MainWindow::updateMetaBar(const MetaInfo& meta)
{
    fileLabel_->setText(tr("File: %1").arg(QDir::toNativeSeparators(meta.path)));
    devicesLabel_->setText(tr("Devices: %1").arg(meta.deviceCount));
    tablesLabel_->setText(tr("Tables: %1").arg(meta.tableCount));
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
    progress_->hide();
    loading_ = false;
    const SeriesData* series = valueModel_->series();
    if (series == nullptr)
    {
        return;
    }
    // Page info + navigation. hasMore is set when the page came back full,
    // which means a further page likely exists (probing for the exact total
    // would require draining it — exactly what paging avoids).
    const qint64 first = series->offset + 1;
    const qint64 last = series->offset + series->ts.size();
    pageInfo_->setText(tr("rows %1-%2%3")
                           .arg(first)
                           .arg(last)
                           .arg(series->hasMore ? QStringLiteral("+") : QString()));
    prevPage_->setEnabled(series->offset > 0);
    nextPage_->setEnabled(series->hasMore);
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

    // ---- stats into the plot tooltip --------------------------------------
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
            x[i] = static_cast<double>(series->ts[i]);
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
        // Circle marker per visible sample: makes individual data points
        // obvious (sparse/event data especially). With adaptive sampling the
        // library auto-thins markers by pixel density, so dense pages don't
        // overdraw while zoomed-in pages show every sample.
        // Pen + brush: hollow circle with a solid red fill so markers stay
        // visible against the line at any zoom.
        graph->setScatterStyle(QCPScatterStyle(
            QCPScatterStyle::ssCircle,
            QPen(QColor(180, 30, 30), 1.5),          // outline
            QBrush(QColor(180, 30, 30, 200)),        // translucent fill
            5));                                     // diameter in px
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
        plot_->xAxis->setLabel(tr("Time (us) — %1").arg(series->measurement));
    }
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

void MainWindow::clearContent()
{
    valueModel_->setSeries(SeriesData{});
    plot_->clearPlottables();
    plot_->replot();
}

void MainWindow::onParamActivated()
{
    if (loading_)
    {
        statusBar()->showMessage(
            tr("Still loading %1 — wait for it to finish or use another view")
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
            tr("Group selected — pick a parameter under it to load values"), 3000);
        return;
    }
    const ParamInfo param = treeModel_->paramAt(sourceIndex);
    if (param.measurement.isEmpty())
    {
        statusBar()->showMessage(tr("No parameter at this row"), 3000);
        return;
    }
    currentParam_ = param;
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
    progress_->show();
    prevPage_->setEnabled(false);
    nextPage_->setEnabled(false);
    statusBar()->showMessage(
        tr("Querying %1 (page %2)...").arg(currentParam_.key()).arg(page_ + 1));
    doc_->queryValuesAsync(currentParam_, page_);
}
