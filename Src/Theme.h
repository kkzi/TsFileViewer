// Flat monochrome theme for TsFileViewer, modeled on the songbird design
// system (github.com/o3ku/songbird): border-radius 0, no gradients,
// black/white minimal palette, unified control styling.
// Kept as code (not .qrc) so tokens are trivially adjustable.

#include <QApplication>

namespace theme
{

inline const char* kStyle = R"(
* { font-family: 'Segoe UI', 'Microsoft YaHei', sans-serif; }

QMainWindow, QDialog, QMessageBox { background: #f3f5f8; color: #1f2328; }
QWidget { color: #1f2328; }

/* ---- toolbar ---------------------------------------------------------- */
QToolBar { background: #ffffff; border: none; border-bottom: 1px solid #d8dee7; spacing: 2px; }
QToolBar::separator { width: 1px; margin: 4px 2px; background: #dde3ea; }
QToolBar QLabel { background: transparent; border: none; padding: 0 4px; color: #1f2328; }

/* ---- buttons: flat, square, 1px border -------------------------------- */
QToolButton, QPushButton {
    background: #f8fafc; border: 1px solid #c5ced8; border-radius: 0px;
    padding: 3px 10px; color: #1f2328;
}
QPushButton { min-width: 48px; }
QToolButton:hover, QPushButton:hover { background: #eef2f6; border-color: #8b98a8; }
QToolButton:pressed, QPushButton:pressed { background: #1f2328; border-color: #1f2328; color: #ffffff; }
QToolButton:disabled, QPushButton:disabled { background: #eef2f6; border-color: #dde3ea; color: #8a94a6; }
QPushButton:focus { border: 1px solid #1f2328; outline: none; }

/* ---- inputs ----------------------------------------------------------- */
QLineEdit, QComboBox, QSpinBox {
    background: #ffffff; border: 1px solid #c5ced8; border-radius: 0px;
    padding: 3px 8px; selection-background-color: #1f2328; selection-color: #ffffff;
}
QLineEdit:hover, QComboBox:hover { border-color: #8b98a8; }
QLineEdit:focus { border: 1px solid #1f2328; }

/* ---- item views ------------------------------------------------------- */
QAbstractItemView {
    background: #ffffff; border: 1px solid #d8dee7; border-radius: 0px;
    selection-background-color: #e6e8eb; selection-color: #1f2328; outline: 0;
    alternate-background-color: #f8fafc;
}
QTreeView { border: none; }
QTreeView::item { padding: 2px 1px; border: none; }
QTreeView::item:hover { background: #eef2f6; }
QTreeView::item:selected { background: #e6e8eb; color: #1f2328; }
QTreeView::branch { background: transparent; }

QTableView { border: 1px solid #d8dee7; gridline-color: #eef2f6; }
QTableView::item { padding: 1px 6px; }
QTableView::item:selected { background: #1f2328; color: #ffffff; }

QHeaderView::section {
    background: #eef2f6; color: #667085; padding: 4px 8px; border: none;
    border-bottom: 1px solid #c5ced8; border-right: 1px solid #dde3ea;
    font-weight: 600;
}
QHeaderView::section:last { border-right: none; }

/* ---- menus ------------------------------------------------------------ */
QMenu { background: #ffffff; border: 1px solid #c5ced8; padding: 4px; }
QMenu::item { padding: 5px 24px; border-radius: 0px; }
QMenu::item:selected { background: #1f2328; color: #ffffff; }
QMenu::separator { height: 1px; background: #e4e7ec; margin: 4px 8px; }

/* ---- status bar ------------------------------------------------------- */
QStatusBar { background: #ffffff; border-top: 1px solid #d8dee7; color: #1f2328; }
QStatusBar QLabel { padding: 0 6px; color: #667085; }
QStatusBar::item { border: none; }

/* ---- progress: flat bar, no chunk groove ------------------------------ */
QProgressBar {
    background: #eef2f6; border: 1px solid #c5ced8; border-radius: 0px;
    text-align: center; color: #1f2328;
}
QProgressBar::chunk { background: #1f2328; }

/* ---- splitter handles -------------------------------------------------- */
QSplitter::handle { background: #d8dee7; }
QSplitter::handle:hover { background: #8b98a8; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

/* ---- message boxes ----------------------------------------------------- */
QMessageBox { background: #ffffff; }
QMessageBox QLabel { color: #1f2328; }

/* ---- scrollbars: flat, square ------------------------------------------ */
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #c5ced8; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #8b98a8; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: #c5ced8; min-width: 24px; }
QScrollBar::handle:horizontal:hover { background: #8b98a8; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0px; width: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)";

inline void apply(QApplication& app)
{
    app.setStyleSheet(QString::fromUtf8(kStyle));
}

}  // namespace theme
