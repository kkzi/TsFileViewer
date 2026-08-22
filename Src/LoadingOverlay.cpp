#include "LoadingOverlay.h"

#include <QFontMetrics>
#include <QPainter>

void LoadingOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    // White translucent scrim, flat (no gradient).
    p.fillRect(rect(), QColor(255, 255, 255, 96));

    // Text on a near-opaque white panel so it stays legible over any
    // underlying content. Panel hugs the text with symmetric padding.
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 1.15);
    f.setBold(true);
    const QString text = tr("Loading...");
    const QFontMetrics fm(f);
    const int textW = fm.horizontalAdvance(text);
    const int textH = fm.height();
    const int padX = 18, padY = 10;
    const QRect panel(0, 0, textW + 2 * padX, textH + 2 * padY);
    const QRect panelRect(width() / 2 - panel.width() / 2,
                          height() / 2 - panel.height() / 2,
                          panel.width(), panel.height());

    p.fillRect(panelRect, QColor(255, 255, 255, 235));
    p.setPen(QPen(QColor(0x11, 0x14, 0x18), 1));
    p.drawRect(panelRect);
    p.setFont(f);
    p.setPen(QColor(0x11, 0x14, 0x18));
    p.drawText(panelRect, Qt::AlignCenter, text);
}
