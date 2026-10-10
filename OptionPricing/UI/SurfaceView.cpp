//
//  SurfaceView.cpp
//  OptionPricing
//

#include "SurfaceView.h"
#include "Formatting.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

/// Blue → cyan → green → yellow → red, like a heat ramp.
QColor ramp(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    struct Stop { double at; int r, g, b; };
    static const Stop stops[] = { { 0.0, 59, 130, 246 }, { 0.25, 34, 211, 238 }, { 0.5, 34, 197, 94 }, { 0.75, 245, 158, 11 }, { 1.0, 239, 68, 68 } };
    for (size_t i = 1; i < 5; ++i) {
        if (t <= stops[i].at) {
            const double u = (t - stops[i - 1].at) / (stops[i].at - stops[i - 1].at);
            return QColor(static_cast<int>(stops[i - 1].r + u * (stops[i].r - stops[i - 1].r)), static_cast<int>(stops[i - 1].g + u * (stops[i].g - stops[i - 1].g)),
                          static_cast<int>(stops[i - 1].b + u * (stops[i].b - stops[i - 1].b)));
        }
    }
    return QColor(stops[4].r, stops[4].g, stops[4].b);
}

} // namespace

SurfaceView::SurfaceView(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(220, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setToolTip("Drag to rotate, scroll to zoom, double-click to reset the view; hover a point for its strike, expiry and implied vol");
}

void SurfaceView::setGrid(const pricing::SurfaceGrid& grid, double spot, const QString& title)
{
    m_grid = grid;
    m_spot = spot;
    m_title = title;
    update();
}

void SurfaceView::clear(const QString& placeholder)
{
    m_grid = pricing::SurfaceGrid{};
    if (!placeholder.isEmpty()) m_placeholder = placeholder;
    update();
}

void SurfaceView::setTheme(const Theme& theme)
{
    m_theme = theme;
    update();
}

void SurfaceView::setAngles(double yawDegrees, double elevationDegrees)
{
    m_yaw = yawDegrees;
    m_elevation = std::clamp(elevationDegrees, 5.0, 85.0);
    update();
}

QString SurfaceView::summaryText() const
{
    if (m_grid.empty()) return "No volatility surface.";
    return QStringLiteral("Surface: %1 expiries (%2 to %3 days), strikes %4 to %5, implied vol %6 to %7.")
        .arg(m_grid.days.size()).arg(m_grid.days.front()).arg(m_grid.days.back())
        .arg(ui::number(m_grid.strikes.front(), 0), ui::number(m_grid.strikes.back(), 0), ui::percent(m_grid.minVol, 1), ui::percent(m_grid.maxVol, 1));
}

QRectF SurfaceView::plotRect() const
{
    return QRectF(rect()).adjusted(12, 26, -12, -14);
}

SurfaceView::Projected SurfaceView::project(double x, double y, double z) const
{
    // World: X strikes (right), Y expiries (into the screen), Z vol (up); the camera yaws
    // around Z and looks down at `elevation`. Orthographic; scale fits the unit cube.
    const double yaw = m_yaw * kPi / 180.0, el = m_elevation * kPi / 180.0;
    const double cx = x - 0.5, cy = y - 0.5, cz = z - 0.5;
    const double rx = cx * std::cos(yaw) - cy * std::sin(yaw);
    const double ry = cx * std::sin(yaw) + cy * std::cos(yaw);
    const double screenY = cz * std::cos(el) + ry * std::sin(el);
    const double depth = ry * std::cos(el) - cz * std::sin(el);
    const QRectF r = plotRect();
    const double scale = std::min(r.width(), r.height()) * 0.62 * m_zoom;
    Projected p;
    p.screen = QPointF(r.center().x() + rx * scale, r.center().y() + 8.0 - screenY * scale);
    p.depth = depth;
    return p;
}

QColor SurfaceView::colourFor(double vol) const
{
    const double span = std::max(m_grid.maxVol - m_grid.minVol, 1e-6);
    return ramp((vol - m_grid.minVol) / span);
}

void SurfaceView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor surface(m_theme.surface.isEmpty() ? "#0f172a" : m_theme.surface);
    const QColor text(m_theme.text.isEmpty() ? "#c7d2e3" : m_theme.text);
    const QColor muted(m_theme.textMuted.isEmpty() ? "#8294ad" : m_theme.textMuted);
    const QColor strong(m_theme.textStrong.isEmpty() ? "#f3f6fb" : m_theme.textStrong);
    const QColor border(m_theme.border.isEmpty() ? "#1f2a3d" : m_theme.border);
    p.fillRect(rect(), surface);
    p.setPen(QPen(border, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);

    QFont titleFont = font();
    titleFont.setBold(true);
    titleFont.setPointSize(12);
    p.setFont(titleFont);
    p.setPen(strong);
    p.drawText(QRectF(12, 4, width() - 24, 20), Qt::AlignCenter, m_grid.empty() ? QStringLiteral("Volatility surface") : m_title);
    QFont small = font();
    small.setPointSize(std::max(8, font().pointSize() - 2));
    p.setFont(small);

    m_vertices.clear();
    if (m_grid.empty()) {
        p.setPen(muted);
        p.drawText(rect().adjusted(20, 30, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        return;
    }

    const size_t rows = m_grid.days.size(), cols = m_grid.strikes.size();
    const double kLo = m_grid.strikes.front(), kHi = std::max(m_grid.strikes.back(), kLo + 1e-9);
    const double vLo = std::max(0.0, std::floor(m_grid.minVol * 20.0) / 20.0 - 0.025), vHi = std::ceil(m_grid.maxVol * 20.0) / 20.0 + 0.025;
    auto nx = [&](double k) { return (k - kLo) / (kHi - kLo); };
    // Expiries are evenly spaced rows (a linear day axis squashes weeklies next to LEAPs).
    auto ny = [&](size_t r) { return rows > 1 ? static_cast<double>(r) / (rows - 1.0) : 0.5; };
    auto nz = [&](double v) { return (v - vLo) / std::max(vHi - vLo, 1e-9); };

    // Floor grid (z = 0) and the three visible axes.
    p.setPen(QPen(border, 1.0));
    for (int i = 0; i <= 4; ++i) {
        const double t = i / 4.0;
        const Projected a = project(t, 0, 0), b = project(t, 1, 0), c = project(0, t, 0), d = project(1, t, 0);
        p.drawLine(a.screen, b.screen);
        p.drawLine(c.screen, d.screen);
    }
    // Vertical axis at the back-left corner (the corner farthest from the camera on the left).
    {
        const Projected c00 = project(0, 0, 0), c01 = project(0, 1, 0), c10 = project(1, 0, 0), c11 = project(1, 1, 0);
        const Projected* corners[] = { &c00, &c01, &c10, &c11 };
        const double xs[] = { 0, 0, 1, 1 }, ys[] = { 0, 1, 0, 1 };
        int back = 0;
        for (int i = 1; i < 4; ++i) if (corners[i]->depth > corners[back]->depth) back = i;
        p.setPen(QPen(muted, 1.0));
        const Projected bottom = project(xs[back], ys[back], 0), top = project(xs[back], ys[back], 1);
        p.drawLine(bottom.screen, top.screen);
        p.setPen(muted);
        for (int i = 0; i <= 4; ++i) {
            const double v = vLo + (vHi - vLo) * i / 4.0;
            const Projected tick = project(xs[back], ys[back], i / 4.0);
            p.drawLine(tick.screen, tick.screen + QPointF(-5, 0));
            p.drawText(QRectF(tick.screen.x() - 52, tick.screen.y() - 8, 44, 16), Qt::AlignRight | Qt::AlignVCenter, ui::percent(v, 0));
        }
    }

    // Quads, farthest first.
    struct Quad { QPolygonF poly; double depth; QColor fill; };
    std::vector<Quad> quads;
    quads.reserve((rows - 1) * (cols - 1));
    std::vector<std::vector<Projected>> pts(rows, std::vector<Projected>(cols));
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            const double v = m_grid.vols[r][c];
            pts[r][c] = project(nx(m_grid.strikes[c]), ny(r), v > 0.0 ? nz(v) : 0.0);
            m_vertices.emplace_back(pts[r][c].screen, std::make_pair(static_cast<int>(r), static_cast<int>(c)));
        }
    }
    for (size_t r = 0; r + 1 < rows; ++r) {
        for (size_t c = 0; c + 1 < cols; ++c) {
            const double v = 0.25 * (m_grid.vols[r][c] + m_grid.vols[r][c + 1] + m_grid.vols[r + 1][c] + m_grid.vols[r + 1][c + 1]);
            if (v <= 0.0) continue;
            Quad q;
            q.poly << pts[r][c].screen << pts[r][c + 1].screen << pts[r + 1][c + 1].screen << pts[r + 1][c].screen;
            q.depth = 0.25 * (pts[r][c].depth + pts[r][c + 1].depth + pts[r + 1][c + 1].depth + pts[r + 1][c].depth);
            q.fill = colourFor(v);
            quads.push_back(q);
        }
    }
    std::sort(quads.begin(), quads.end(), [](const Quad& a, const Quad& b) { return a.depth > b.depth; });
    for (const Quad& q : quads) {
        QColor fill = q.fill;
        fill.setAlpha(205);
        p.setBrush(fill);
        p.setPen(QPen(fill.darker(135), 0.8));
        p.drawPolygon(q.poly);
    }

    // Axis labels: strikes along the front edge, expiries along the side.
    p.setPen(text);
    const int strikeLabels = std::min<int>(6, static_cast<int>(cols));
    for (int i = 0; i < strikeLabels; ++i) {
        const size_t c = static_cast<size_t>(std::lround(i * (cols - 1.0) / (strikeLabels - 1)));
        const Projected at = project(nx(m_grid.strikes[c]), 0, 0);
        p.drawText(QRectF(at.screen.x() - 30, at.screen.y() + 2, 60, 14), Qt::AlignCenter, ui::number(m_grid.strikes[c], 0));
    }
    const size_t labelStep = std::max<size_t>(1, (rows + 7) / 8);   // at most ~8 expiry labels
    for (size_t r = 0; r < rows; ++r) {
        if (r % labelStep != 0 && r + 1 != rows) continue;
        const Projected at = project(1, ny(r), 0);
        p.drawText(QRectF(at.screen.x() + 4, at.screen.y() - 7, 48, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("%1d").arg(static_cast<int>(m_grid.days[r])));
    }
    // Spot marker along the strike axis.
    if (m_spot > kLo && m_spot < kHi) {
        const Projected a = project(nx(m_spot), 0, 0), b = project(nx(m_spot), 1, 0);
        p.setPen(QPen(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2), 1.2, Qt::DashLine));
        p.drawLine(a.screen, b.screen);
        p.setPen(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2));
        p.drawText(QRectF(a.screen.x() - 40, a.screen.y() + 14, 80, 14), Qt::AlignCenter, QStringLiteral("spot %1").arg(ui::number(m_spot, 2)));
    }
    // Colour legend.
    {
        const QRectF bar(width() - 26.0, 40.0, 10.0, std::max(40.0, height() - 90.0));
        QLinearGradient g(bar.topLeft(), bar.bottomLeft());
        for (int i = 0; i <= 8; ++i) g.setColorAt(i / 8.0, ramp(1.0 - i / 8.0));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRect(bar);
        p.setPen(muted);
        p.drawText(QRectF(bar.left() - 46, bar.top() - 8, 42, 16), Qt::AlignRight | Qt::AlignVCenter, ui::percent(m_grid.maxVol, 0));
        p.drawText(QRectF(bar.left() - 46, bar.bottom() - 8, 42, 16), Qt::AlignRight | Qt::AlignVCenter, ui::percent(m_grid.minVol, 0));
    }

    // Hover readout: the nearest vertex within 14 px.
    if (m_hovering) {
        int bestR = -1, bestC = -1;
        double bestD = 14.0 * 14.0;
        for (const auto& [screen, rc] : m_vertices) {
            const double dx = screen.x() - m_hover.x(), dy = screen.y() - m_hover.y();
            const double d = dx * dx + dy * dy;
            if (d < bestD) { bestD = d; bestR = rc.first; bestC = rc.second; }
        }
        if (bestR >= 0) {
            const double v = m_grid.vols[static_cast<size_t>(bestR)][static_cast<size_t>(bestC)];
            const QPointF at = pts[static_cast<size_t>(bestR)][static_cast<size_t>(bestC)].screen;
            p.setPen(QPen(strong, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(at, 4.5, 4.5);
            const QString line1 = QStringLiteral("K %1 · %2d").arg(ui::number(m_grid.strikes[static_cast<size_t>(bestC)], 2)).arg(static_cast<int>(m_grid.days[static_cast<size_t>(bestR)]));
            const QString line2 = QStringLiteral("IV %1 · %2% moneyness").arg(ui::percent(v, 2), ui::number(m_spot > 0 ? m_grid.strikes[static_cast<size_t>(bestC)] / m_spot * 100.0 : 0.0, 1));
            const QFontMetricsF fm(small);
            const double w = std::max(fm.horizontalAdvance(line1), fm.horizontalAdvance(line2)) + 16, h = fm.height() * 2 + 10;
            QRectF box(at.x() + 12, at.y() - h / 2, w, h);
            if (box.right() > width() - 30) box.moveLeft(at.x() - 12 - w);
            box.moveTop(std::clamp(box.top(), 4.0, height() - h - 4.0));
            QColor bg = surface; bg.setAlpha(235);
            p.setBrush(bg);
            p.setPen(QPen(border, 1.0));
            p.drawRoundedRect(box, 5, 5);
            p.setPen(strong);
            p.drawText(QPointF(box.left() + 8, box.top() + 5 + fm.ascent()), line1);
            p.setPen(text);
            p.drawText(QPointF(box.left() + 8, box.top() + 5 + fm.ascent() + fm.height()), line2);
            if (onHover) onHover(line1 + " · " + line2);
        }
    }
}

void SurfaceView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_dragStart = event->position();
    m_dragYaw = m_yaw;
    m_dragElevation = m_elevation;
    setCursor(Qt::ClosedHandCursor);
}

void SurfaceView::mouseMoveEvent(QMouseEvent* event)
{
    m_hover = event->position();
    m_hovering = true;
    if (m_dragging) {
        const QPointF d = event->position() - m_dragStart;
        m_yaw = m_dragYaw + d.x() * 0.5;
        m_elevation = std::clamp(m_dragElevation + d.y() * 0.4, 5.0, 85.0);
    }
    update();
}

void SurfaceView::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
}

void SurfaceView::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    m_zoom = std::clamp(m_zoom * std::pow(1.1, steps), 0.5, 2.5);
    update();
}

void SurfaceView::leaveEvent(QEvent*)
{
    m_hovering = false;
    update();
}

void SurfaceView::mouseDoubleClickEvent(QMouseEvent*)
{
    m_yaw = -35.0;
    m_elevation = 28.0;
    m_zoom = 1.0;
    update();
}
