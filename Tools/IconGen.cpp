// Icon generator (dev tool, not part of the app): renders the app icon with
// QPainter and writes the PNG set + Windows .ico into Res/.
//
// Design: a white "document" card with text lines, out of which rises the
// app's signature waveform — the product in one glyph (txt file -> curve).
// Palette matches Theme.h: dark slate card header, monochrome lines, one
// red accent dot (the plot's sample marker).
//
// Run: Build/<config>/bin/IconGen  (no arguments; writes ../../.. relative
// paths are avoided — the tool writes to Res/ under the source tree via a
// compile-time flag passed by CMake).
#include <QApplication>
#include <QBuffer>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QRect>
#include <QRgb>
#include <QSize>
#include <QString>
#include <QSvgRenderer>
#include <QVector>
#include <QtMath>

#include <cstdio>

namespace
{
// Palette (kept in lockstep with Theme.h).
const QColor kCard(0xf6, 0xf7, 0xf9);        // card surface
const QColor kCardEdge(0xd4, 0xda, 0xe2);    // card border
const QColor kHeader(0x2a, 0x33, 0x40);      // dark slate header band
const QColor kTextLine(0xb7, 0xc0, 0xcb);    // faint "text" lines
const QColor kCurve(0x1f, 0x23, 0x28);      // near-black curve
const QColor kCurveAccent(0xd9, 0x30, 0x30); // red sample dot

// Render the icon into an RGBA image of size s x s (works from 16px up;
// strokes are scaled from the 256px design).
QImage renderIcon(int s)
{
    QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    const double u = s / 256.0;  // design-unit scale

    // ---- document card -------------------------------------------------
    // Rounded card, slightly taller than wide, with a small top-left fold.
    const QRectF card(34 * u, 20 * u, 188 * u, 216 * u);
    const double radius = 24 * u;
    QPainterPath cardPath;
    cardPath.addRoundedRect(card, radius, radius);
    p.fillPath(cardPath, kCard);
    p.setPen(QPen(kCardEdge, qMax(1.0, 2 * u)));
    p.drawPath(cardPath);

    // ---- header band -----------------------------------------------------
    // Dark slate band across the top third: the "file header" of the format.
    QPainterPath band;
    band.addRect(card.left(), card.top(), card.width(), 66 * u);
    QPainterPath rounded = cardPath.intersected(band);
    p.fillPath(rounded, kHeader);

    // ---- text lines (in the header) --------------------------------------
    // Faint lines like a text file's first rows.
    p.setPen(QPen(QColor(0x8f, 0x9a, 0xa8), qMax(1.0, 6 * u)));
    for (int i = 0; i < 2; ++i)
    {
        const double y = card.top() + (18 + i * 24) * u;
        const double w = (i == 0 ? 90 : 120) * u;
        p.drawLine(QPointF(card.left() + 22 * u, y),
                   QPointF(card.left() + 22 * u + w, y));
    }

    // ---- the curve --------------------------------------------------------
    // Rises from the header line and draws the waveform across the card's
    // lower two thirds: the loaded parameter's plot.
    QPolygonF pts;
    const double x0 = card.left() + 14 * u;
    const double x1 = card.right() - 14 * u;
    const double yMid = card.top() + 66 * u;   // boundary: header -> body
    const double yBody = card.bottom() - 26 * u;
    // Anchors (design units, relative to yMid): flat start, dip, rise, peak,
    // settle, dip, rise to the exit.
    const double xs[] = {0, 30, 62, 94, 126, 158, 174};
    const double ys[] = {0, 34, -8, 62, 14, 78, 30};
    const int n = sizeof(xs) / sizeof(xs[0]);
    for (int i = 0; i < n; ++i)
    {
        pts.append(QPointF(x0 + xs[i] * u, yMid + ys[i] * u));
    }
    // Clamp: the waveform stays inside the card body.
    QPainterPath curve;
    curve.moveTo(pts.first());
    for (int i = 1; i < n; ++i)
    {
        // Smooth segments: simple line joins read fine at icon sizes.
        curve.lineTo(pts[i]);
    }
    // Exit point flush with the card's right padding.
    curve.lineTo(x1, yMid + 30 * u);
    p.setPen(QPen(kCurve, qMax(1.5, 7 * u), Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    p.drawPath(curve);

    // Red sample dot on the peak: the plot's selected-sample marker.
    p.setPen(Qt::NoPen);
    p.setBrush(kCurveAccent);
    p.drawEllipse(pts[3], 11 * u, 11 * u);

    p.end();
    return img;
}

// Simplified small-size twin of the SVG design (gemini-svg.svg): dark navy
// rounded square, cyan document card, and the rising analysis curve with its
// bright end dot — the elements that still read at 16..32px (filters,
// gradients and thin grid lines of the SVG wash out at those sizes).
QImage renderMini(int s)
{
    QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double u = s / 256.0;  // design-unit scale

    // Background: dark navy rounded square with a subtle radial lift.
    QRadialGradient bg(QPointF(s * 0.5, s * 0.45), s * 0.75);
    bg.setColorAt(0, QColor(0x0b, 0x19, 0x2e));
    bg.setColorAt(1, QColor(0x03, 0x07, 0x12));
    QPainterPath card;
    card.addRoundedRect(QRectF(6 * u, 6 * u, 244 * u, 244 * u), 48 * u, 48 * u);
    p.fillPath(card, bg);
    p.setPen(QPen(QColor(0x1e, 0x3a, 0x5f), qMax(1.0, 3 * u)));
    p.drawPath(card);

    // Document card outline (dropped at 16px: it reads as noise there).
    if (s >= 24)
    {
        const QRectF doc(48 * u, 52 * u, 78 * u, 102 * u);
        p.setPen(QPen(QColor(0x38, 0xbd, 0xf8), qMax(1.5, 8 * u)));
        p.setBrush(QColor(0x06, 0x12, 0x24));
        p.drawRoundedRect(doc, 8 * u, 8 * u);
        p.setPen(QPen(QColor(0x02, 0x84, 0xc7), qMax(1.0, 6 * u)));
        for (int i = 0; i < 3; ++i)
        {
            const double y = doc.top() + (22 + i * 18) * u;
            const double w = (i == 1 ? 36.0 : 46.0) * u;
            p.drawLine(QPointF(doc.left() + 12 * u, y),
                       QPointF(doc.left() + 12 * u + w, y));
        }
    }

    // The signature analysis curve: rises left-to-right with a dip.
    const QPointF pts[] = {
        QPointF(40 * u, 196 * u), QPointF(80 * u, 212 * u),
        QPointF(120 * u, 150 * u), QPointF(160 * u, 170 * u),
        QPointF(200 * u, 100 * u), QPointF(224 * u, 70 * u)};
    QPolygonF curve;
    for (const QPointF& pt : pts)
    {
        curve.append(pt);
    }
    p.setPen(QPen(QColor(0x38, 0xbd, 0xf8), qMax(1.5, 10 * u), Qt::SolidLine,
                  Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(curve);
    // Bright end dot (the SVG's end node).
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xa5, 0xf3, 0xfc));
    p.drawEllipse(pts[5], qMax(2.0, 9 * u), qMax(2.0, 9 * u));

    p.end();
    return img;
}

// Minimal multi-size .ico writer: ICONDIR + ICONDIRENTRY per image, each
// stored as a 32-bit BMP (AND mask of 0). Uncompressed BMP keeps the legacy
// RC tool happy (it rejects PNG-compressed layers).
bool writeIco(const QString& path, const QVector<QImage>& images)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
    {
        return false;
    }
    QDataStream out(&f);
    out.setByteOrder(QDataStream::LittleEndian);
    out << quint16(0) << quint16(1) << quint16(images.size());  // header

    // Pass 1: entry directory (offsets need the data sizes first).
    QVector<QByteArray> blobs;
    qint64 offset = 6 + 16 * images.size();
    for (const QImage& img : images)
    {
        const QImage rgba = img.convertToFormat(QImage::Format_ARGB32);
        const int w = rgba.width();
        const int h = rgba.height();
        const int stride = ((w * 32 + 31) / 32) * 4;  // DWORD-aligned
        // BITMAPINFOHEADER (height doubled: XOR + AND masks) + XOR mask.
        // AND mask is all-zero (alpha channel carries transparency).
        const quint32 xorSize = stride * h;
        const quint32 andSize = ((w + 31) / 32) * 4 * h;
        QByteArray b;
        // Note: QDataStream(QByteArray&) in write mode TRUNCATES the array,
        // so the header goes through a QBuffer instead.
        QBuffer head(&b);
        head.open(QIODevice::WriteOnly);
        QDataStream h2(&head);
        h2.setByteOrder(QDataStream::LittleEndian);
        h2 << quint32(40) << qint32(w) << qint32(h * 2) << quint16(1)
           << quint16(32) << quint32(0) << quint32(xorSize)
           << qint32(0) << qint32(0) << quint32(0) << quint32(0);
        head.close();
        for (int y = h - 1; y >= 0; --y)  // bottom-up rows
        {
            const QRgb* row = reinterpret_cast<const QRgb*>(rgba.scanLine(y));
            for (int x = 0; x < w; ++x)
            {
                const QRgb px = row[x];
                b.append(char(px & 0xff))          // B
                    .append(char((px >> 8) & 0xff))   // G
                    .append(char((px >> 16) & 0xff))  // R
                    .append(char((px >> 24) & 0xff)); // A
            }
        }
        b.append(QByteArray(andSize, 0));
        blobs.push_back(b);
        const int iw = w == 256 ? 0 : w;
        out << quint8(iw) << quint8(iw)   // width, height (0 = 256)
            << quint8(0) << quint8(0)     // colorCount, reserved
            << quint16(1) << quint16(32)  // planes, bit count
            << quint32(b.size()) << quint32(offset);
        offset += b.size();
    }
    for (const QByteArray& b : blobs)
    {
        f.write(b);
    }
    return true;
}
}  // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addPositionalArgument(
        QStringLiteral("outdir"),
        QStringLiteral("directory to write icon.png / icon.ico into"));
    // --svg <file>: rasterize that SVG at each size instead of the built-in
    // QPainter design (fonts/gradient filters depend on the renderer).
    QCommandLineOption svgOption(
        QStringList{QStringLiteral("s"), QStringLiteral("svg")},
        QStringLiteral("render this SVG file instead of the built-in design"),
        QStringLiteral("path"));
    parser.addOption(svgOption);
    parser.process(app);
    const QString outDir = parser.positionalArguments().value(0);
    if (outDir.isEmpty())
    {
        std::fprintf(stderr, "usage: IconGen [--svg <file>] <outdir>\n");
        return 2;
    }
    QDir().mkpath(outDir);

    QSvgRenderer svg;
    const bool useSvg = parser.isSet(svgOption);
    if (useSvg && !svg.load(parser.value(svgOption)))
    {
        std::fprintf(stderr, "cannot load SVG: %s\n",
                     qPrintable(parser.value(svgOption)));
        return 2;
    }

    // Small sizes get the simplified twin (the SVG's filters/gradient wash
    // out below 48px); large sizes rasterize the SVG itself, supersampled
    // (2x then downscale) so hairlines survive.
    QVector<QImage> images;
    for (int s : {16, 24, 32, 48, 64, 128, 256})
    {
        if (useSvg && s >= 48)
        {
            QImage hi(s * 2, s * 2, QImage::Format_ARGB32_Premultiplied);
            hi.fill(Qt::transparent);
            QPainter p(&hi);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::TextAntialiasing, true);
            svg.render(&p);
            p.end();
            images.push_back(
                hi.scaled(s, s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        }
        else if (useSvg)
        {
            images.push_back(renderMini(s));
        }
        else
        {
            images.push_back(renderIcon(s));
        }
    }
    bool ok = true;
    for (const QImage& img : images)
    {
        ok = ok && img.save(QString("%1/icon%2.png").arg(outDir).arg(img.width()));
    }
    ok = ok && writeIco(outDir + QStringLiteral("/icon.ico"), images);
    std::printf(ok ? "icons written to %s\n" : "FAILED writing to %s\n",
                qPrintable(outDir));
    return ok ? 0 : 1;
}
