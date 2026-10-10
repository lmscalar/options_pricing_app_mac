//
//  SurfaceView.h
//  OptionPricing
//
//  A self-contained 3D surface widget (software projection with QPainter): a strike ×
//  expiry grid of implied vols drawn as depth-sorted shaded quads with a colour ramp, axes
//  and labels. Drag to rotate, scroll to zoom, hover a vertex for its figures. No Qt Quick
//  or OpenGL dependency, so it follows the application theme and renders in screenshots.
//

#pragma once

#include "QtHeaders.h"
#include "Theme.h"
#include "../Pricing/VolSurface.h"

#include <functional>
#include <vector>

class SurfaceView : public QWidget
{
public:
    explicit SurfaceView(QWidget* parent = nullptr);

    /// Replaces the grid; an empty grid shows the placeholder text.
    void setGrid(const pricing::SurfaceGrid& grid, double spot, const QString& title);
    void clear(const QString& placeholder = QString());
    void setTheme(const Theme& theme);
    /// Camera: yaw around the vertical axis, elevation above the strike-expiry plane (degrees).
    void setAngles(double yawDegrees, double elevationDegrees);
    double yaw() const { return m_yaw; }
    double elevation() const { return m_elevation; }
    bool hasGrid() const { return !m_grid.empty(); }
    /// One line describing the grid (expiries, strike range, vol range) for the assistant.
    QString summaryText() const;

    std::function<void(const QString& text)> onHover;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct Projected {
        QPointF screen;
        double depth = 0.0;
    };
    Projected project(double x, double y, double z) const;   ///< unit-cube coordinates
    QColor colourFor(double vol) const;
    QRectF plotRect() const;

    pricing::SurfaceGrid m_grid;
    double m_spot = 0.0;
    QString m_title;
    QString m_placeholder = "Load an option chain to see the fitted volatility surface.";
    Theme m_theme;
    double m_yaw = -35.0;
    double m_elevation = 28.0;
    double m_zoom = 1.0;
    bool m_dragging = false;
    QPointF m_dragStart;
    double m_dragYaw = 0.0, m_dragElevation = 0.0;
    QPointF m_hover;
    bool m_hovering = false;
    mutable std::vector<std::pair<QPointF, std::pair<int, int>>> m_vertices;   ///< projected vertices for hover lookup
};
