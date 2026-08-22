// Flat monochrome theme for TsFileViewer, modeled on the songbird design
// system (github.com/o3ku/songbird): border-radius 0, no gradients,
// black/white minimal palette, unified control styling.
// Kept as code (not .qrc) so tokens are trivially adjustable.

#include <QApplication>

namespace theme
{

inline const char* kStyle = R"(
* { font-family: 'Segoe UI', 'Microsoft YaHei', sans-serif; }

QMainWindow, QDialog, QMessageBox { background: #ffffff; color: #111418; }
QWidget { color: #111418; }

/* ---- toolbar ---------------------------------------------------------- */
QToolBar { background: #ffffff; border: none; border-bottom: 1px solid #111418; spacing: 2px; }
QToolBar::separator { width: 1px; margin: 4px 2px; background: #b7c0cb; }
QToolBar QLabel { background: transparent; border: none; padding: 0 5px; color: #111418; }

/* ---- buttons: flat, square, 1px border -------------------------------- */
QToolButton, QPushButton {
    background: #ffffff; border: 1px solid #111418; border-radius: 0px;
    padding: 3px 10px; color: #111418; font-weight: 600;
}
QPushButton { min-width: 48px; }
QToolButton:hover, QPushButton:hover { background: #e4e7ec; border-color: #111418; }
QToolButton:pressed, QPushButton:pressed { background: #111418; border-color: #111418; color: #ffffff; }
QToolButton:disabled, QPushButton:disabled { background: #eceef1; border-color: #b7c0cb; color: #8a94a6; }
QPushButton:focus { border: 1px solid #111418; outline: none; }

/* ---- inputs ----------------------------------------------------------- */
QLineEdit, QComboBox, QSpinBox {
    background: #ffffff; border: 1px solid #111418; border-radius: 0px;
    padding: 3px 8px; color: #111418;
    selection-background-color: #111418; selection-color: #ffffff;
}
QLineEdit:hover, QComboBox:hover { background: #f6f7f9; }
QLineEdit:focus { border: 1px solid #111418; }

/* ---- item views ------------------------------------------------------- */
QAbstractItemView {
    background: #ffffff; border: none; border-radius: 0px;
    selection-background-color: #c4ccd4; selection-color: #111418; outline: 0;
    alternate-background-color: #f2f4f6;
}
QTreeView { border: none; }
QTreeView::item { padding: 0px; border: none; }
QTreeView::item:hover { background: #dfe3e8; }
QTreeView::item:selected { background: #c4ccd4; color: #111418; }
QTreeView::branch { background: transparent; }

QTableView { border: none; gridline-color: #dfe3e8; }
QTableView::item { padding: 0px; }
QTableView::item:selected { background: #c4ccd4; color: #111418; }

/* One header height for every view (tree, table). */
QHeaderView::section {
    background: #111418; color: #ffffff; padding: 5px 8px; border: none;
    border-right: 1px solid #3a4048; font-weight: 600; min-height: 22px;
}
QHeaderView::section:last { border-right: none; }

/* ---- menus ------------------------------------------------------------ */
QMenu { background: #ffffff; border: 1px solid #111418; padding: 4px; }
QMenu::item { padding: 5px 24px; border-radius: 0px; }
QMenu::item:selected { background: #111418; color: #ffffff; }
QMenu::separator { height: 1px; background: #dfe3e8; margin: 4px 8px; }

/* ---- status bar ------------------------------------------------------- */
QStatusBar { background: #ffffff; border-top: 1px solid #111418; color: #111418; }
QStatusBar QLabel { padding: 0 6px; color: #111418; }
QStatusBar::item { border: none; }

/* ---- progress: flat bar, no chunk groove ------------------------------ */
QProgressBar {
    background: #ffffff; border: 1px solid #111418; border-radius: 0px;
    text-align: center; color: #111418;
}
QProgressBar::chunk { background: #111418; }

/* ---- splitter handles -------------------------------------------------- */
QSplitter::handle { background: #b7c0cb; }
QSplitter::handle:hover { background: #111418; }
QSplitter::handle:horizontal { width: 2px; }
QSplitter::handle:vertical { height: 2px; }

/* ---- message boxes ----------------------------------------------------- */
QMessageBox { background: #ffffff; }
QMessageBox QLabel { color: #111418; }

/* ---- scrollbars: flat, square ------------------------------------------ */
QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: #7a8694; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #111418; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle:horizontal { background: #7a8694; min-width: 24px; }
QScrollBar::handle:horizontal:hover { background: #111418; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0px; width: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)";

inline void apply(QApplication& app)
{
    app.setStyleSheet(QString::fromUtf8(kStyle));
}

}  // namespace theme
