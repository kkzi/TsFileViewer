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
#include <QMessageBox>
#include <QProgressBar>
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
    paramTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    paramTree_->header()->setStretchLastSection(true);
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
    valuesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    valuesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    plot_ = new QCustomPlot(right);
    plot_->setInteraction(QCP::iRangeDrag, true);
    plot_->setInteraction(QCP::iRangeZoom, true);
    plot_->setNoAntialiasingOnDrag(true);
    plot_->xAxis->setLabel(tr("Relative time (s)"));
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
    connect(paramTree_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::onSelectionChanged);

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
        // Progressive load: show running row count; keep the busy indicator
        // visible until the final chunk.
        statusBar()->showMessage(
            tr("%1: loading... %2 rows").arg(chunk.key()).arg(
                valueModel_->series() ? valueModel_->series()->ts.size() : 0));
        rebuildPlot();
        return;
    }

    busy_->hide();
    const SeriesData* series = valueModel_->series();
    if (series == nullptr)
    {
        return;
    }
    statusBar()->showMessage(
        tr("%1: %2 points").arg(series->key()).arg(series->ts.size()), 5000);
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
        const qint64 t0 = series->ts.first();
        QVector<double> x(series->ts.size());
        for (int i = 0; i < series->ts.size(); ++i)
        {
            x[i] = (series->ts[i] - t0) / 1e6;
        }
        auto* graph = plot_->addGraph();
        graph->setData(x, series->value, true);
        graph->rescaleAxes();
        // Padding so the curve is not glued to the frame.
        const double pad = std::abs(plot_->yAxis->range().size()) * 0.05 + 1e-9;
        plot_->yAxis->setRange(plot_->yAxis->range().lower - pad,
                               plot_->yAxis->range().upper + pad);
    }
    if (series != nullptr)
    {
        plot_->xAxis->setLabel(tr("Relative time (s) — %1").arg(series->measurement));
    }
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

void MainWindow::clearContent()
{
    valueModel_->setSeries(SeriesData{});
    plot_->clearPlottables();
    plot_->replot();
}

void MainWindow::onSelectionChanged()
{
    const QModelIndex proxyIndex = paramTree_->currentIndex();
    if (!proxyIndex.isValid())
    {
        return;
    }
    const QModelIndex sourceIndex = proxy_->mapToSource(proxyIndex);
    if (!ParamTreeModel::isMeasurementRow(sourceIndex))
    {
        // Device/table group row: nothing to query, say so instead of
        // looking dead.
        statusBar()->showMessage(
            tr("Group selected — pick a parameter under it to load values"),
            3000);
        return;
    }
    const ParamInfo param = treeModel_->paramAt(sourceIndex);
    if (param.measurement.isEmpty())
    {
        statusBar()->showMessage(tr("No parameter at this row"), 3000);
        return;
    }
    currentParam_ = param;
    busy_->show();
    statusBar()->showMessage(tr("Querying %1...").arg(param.key()));
    doc_->queryValuesAsync(param);
}
