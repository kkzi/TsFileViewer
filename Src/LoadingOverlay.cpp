#include "LoadingOverlay.h"

#include <QPainter>
#include <QPainterPath>

void LoadingOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    // White translucent scrim, flat (no gradient).
    p.fillRect(rect(), QColor(255, 255, 255, 96));

    // Centered black spinner ring.
    const int r = 14;
    const QRectF circle = QRectF(QPointF(width() / 2.0 - r, height() / 2.0 - r),
                                 QSizeF(2 * r, 2 * r));
    QPen pen(QColor(0x11, 0x14, 0x18), 3);
    pen.setCapStyle(Qt::FlatCap);
    p.setPen(pen);
    p.translate(circle.center());
    p.rotate(angle_);
    p.translate(-circle.center());
    p.drawArc(circle, 30 * 16, 300 * 16);

    p.setPen(QPen(QColor(0x11, 0x14, 0x18)));
    p.setFont(font());
    p.drawText(rect().adjusted(0, 2 * r + 12, 0, 0),
               Qt::AlignHCenter | Qt::AlignTop, tr("Loading..."));
}
