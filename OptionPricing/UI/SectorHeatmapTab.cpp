//
//  SectorHeatmapTab.cpp
//  OptionPricing
//

#include "SectorHeatmapTab.h"
#include "Formatting.h"

#include <QtCore/QStandardPaths>

#include <algorithm>
#include <cmath>

namespace {

constexpr const char* kPeriodKey = "sectorHeatmap/period";
constexpr const char* kViewKey = "sectorHeatmap/view";
constexpr const char* kUniverseKey = "sectorHeatmap/universe";
constexpr int kCapCacheDays = 14;

/// Built-in large-cap universe with sector and an approximate market cap (USD bn) used only
/// until the live figure arrives from ticker details.
struct UniverseEntry { const char* ticker; const char* name; const char* sector; double capBn; };
const std::vector<UniverseEntry>& universe()
{
    static const std::vector<UniverseEntry> entries{
        // Internet & media
        { "AAPL", "Apple Inc.", "Internet", 3400 }, { "GOOGL", "Alphabet Inc. Class A", "Internet", 2100 }, { "AMZN", "Amazon.com Inc", "Internet", 2000 },
        { "META", "Meta Platforms, Inc.", "Internet", 1300 }, { "NFLX", "Netflix, Inc.", "Internet", 300 }, { "DIS", "Walt Disney Co", "Internet", 200 },
        { "ABNB", "Airbnb, Inc.", "Internet", 80 }, { "MELI", "MercadoLibre, Inc.", "Internet", 90 }, { "T", "AT&T Inc.", "Internet", 130 },
        { "VZ", "Verizon Communications", "Internet", 170 }, { "TMUS", "T-Mobile US", "Internet", 260 }, { "CMCSA", "Comcast Corp", "Internet", 150 },
        { "UBER", "Uber Technologies", "Internet", 170 }, { "SPOT", "Spotify Technology", "Internet", 120 }, { "BKNG", "Booking Holdings", "Internet", 170 },
        // Software
        { "MSFT", "Microsoft Corp", "Software", 3100 }, { "ORCL", "Oracle Corp", "Software", 450 }, { "CRM", "Salesforce, Inc.", "Software", 260 },
        { "ADBE", "Adobe Inc.", "Software", 200 }, { "NOW", "ServiceNow, Inc.", "Software", 180 }, { "INTU", "Intuit Inc.", "Software", 180 },
        { "PLTR", "Palantir Technologies", "Software", 300 }, { "PANW", "Palo Alto Networks", "Software", 120 }, { "CRWD", "CrowdStrike Holdings", "Software", 100 },
        { "FTNT", "Fortinet, Inc.", "Software", 70 }, { "SNOW", "Snowflake Inc.", "Software", 70 }, { "DDOG", "Datadog, Inc.", "Software", 45 },
        { "ZS", "Zscaler, Inc.", "Software", 40 }, { "WDAY", "Workday, Inc.", "Software", 60 }, { "ADP", "Automatic Data Processing", "Software", 120 },
        { "ACN", "Accenture plc", "Software", 200 }, { "IBM", "International Business Machines", "Software", 220 }, { "SHOP", "Shopify Inc.", "Software", 130 },
        { "APP", "AppLovin Corp", "Software", 150 },
        // Semiconductors
        { "NVDA", "Nvidia Corp", "Semis", 4300 }, { "TSM", "Taiwan Semiconductor Manufacturing", "Semis", 1100 }, { "AVGO", "Broadcom Inc.", "Semis", 1300 },
        { "AMD", "Advanced Micro Devices", "Semis", 280 }, { "MU", "Micron Technology, Inc.", "Semis", 180 }, { "QCOM", "Qualcomm Inc.", "Semis", 170 },
        { "TXN", "Texas Instruments", "Semis", 190 }, { "INTC", "Intel Corp", "Semis", 100 }, { "ARM", "Arm Holdings plc", "Semis", 150 },
        { "AMAT", "Applied Materials", "Semis", 160 }, { "LRCX", "Lam Research Corp", "Semis", 130 }, { "KLAC", "KLA Corp", "Semis", 120 },
        { "ADI", "Analog Devices", "Semis", 120 }, { "MRVL", "Marvell Technology", "Semis", 70 }, { "ASML", "ASML Holding N.V.", "Semis", 300 },
        { "STX", "Seagate Technology", "Semis", 25 }, { "WDC", "Western Digital Corp", "Semis", 25 }, { "ON", "ON Semiconductor", "Semis", 20 },
        // Hardware & networking
        { "DELL", "Dell Technologies", "Hardware", 90 }, { "HPE", "Hewlett Packard Enterprise", "Hardware", 30 }, { "HPQ", "HP Inc.", "Hardware", 30 },
        { "CSCO", "Cisco Systems", "Hardware", 260 }, { "ANET", "Arista Networks", "Hardware", 130 }, { "SMCI", "Super Micro Computer", "Hardware", 30 },
        { "NTAP", "NetApp, Inc.", "Hardware", 20 },
        // Consumer
        { "TSLA", "Tesla, Inc.", "Consumer", 1300 }, { "WMT", "Walmart Inc.", "Consumer", 800 }, { "COST", "Costco Wholesale", "Consumer", 400 },
        { "PG", "Procter & Gamble", "Consumer", 380 }, { "HD", "Home Depot", "Consumer", 360 }, { "PEP", "PepsiCo, Inc.", "Consumer", 200 },
        { "KO", "Coca-Cola Co", "Consumer", 290 }, { "PM", "Philip Morris International", "Consumer", 240 }, { "MCD", "McDonald's Corp", "Consumer", 220 },
        { "LOW", "Lowe's Companies", "Consumer", 130 }, { "MNST", "Monster Beverage", "Consumer", 60 }, { "CMG", "Chipotle Mexican Grill", "Consumer", 60 },
        { "NKE", "Nike, Inc.", "Consumer", 100 }, { "SBUX", "Starbucks Corp", "Consumer", 110 }, { "TGT", "Target Corp", "Consumer", 45 },
        // Healthcare
        { "LLY", "Eli Lilly & Co.", "Healthcare", 800 }, { "UNH", "UnitedHealth Group", "Healthcare", 300 }, { "JNJ", "Johnson & Johnson", "Healthcare", 400 },
        { "ABBV", "AbbVie Inc.", "Healthcare", 350 }, { "MRK", "Merck & Co.", "Healthcare", 220 }, { "TMO", "Thermo Fisher Scientific", "Healthcare", 200 },
        { "AMGN", "Amgen Inc.", "Healthcare", 170 }, { "GILD", "Gilead Sciences", "Healthcare", 140 }, { "ABT", "Abbott Laboratories", "Healthcare", 230 },
        { "PFE", "Pfizer Inc.", "Healthcare", 140 }, { "ISRG", "Intuitive Surgical", "Healthcare", 180 }, { "BMY", "Bristol-Myers Squibb", "Healthcare", 100 },
        { "MRNA", "Moderna, Inc.", "Healthcare", 15 }, { "VRTX", "Vertex Pharmaceuticals", "Healthcare", 110 }, { "REGN", "Regeneron Pharmaceuticals", "Healthcare", 70 },
        { "ARGX", "argenx SE", "Healthcare", 40 }, { "ALNY", "Alnylam Pharmaceuticals", "Healthcare", 50 }, { "BIIB", "Biogen Inc.", "Healthcare", 25 },
        { "DHR", "Danaher Corp", "Healthcare", 140 }, { "SYK", "Stryker Corp", "Healthcare", 140 }, { "MDT", "Medtronic plc", "Healthcare", 120 },
        { "CVS", "CVS Health Corp", "Healthcare", 100 }, { "BSX", "Boston Scientific", "Healthcare", 150 },
        // Finance
        { "BRK.B", "Berkshire Hathaway Class B", "Finance", 1000 }, { "JPM", "JPMorgan Chase & Co.", "Finance", 800 }, { "V", "Visa Inc.", "Finance", 650 },
        { "MA", "Mastercard Inc.", "Finance", 500 }, { "BAC", "Bank of America", "Finance", 380 }, { "WFC", "Wells Fargo & Co", "Finance", 260 },
        { "GS", "Goldman Sachs Group", "Finance", 230 }, { "MS", "Morgan Stanley", "Finance", 220 }, { "AXP", "American Express", "Finance", 220 },
        { "BLK", "BlackRock, Inc.", "Finance", 170 }, { "SPGI", "S&P Global Inc.", "Finance", 160 }, { "C", "Citigroup Inc.", "Finance", 170 },
        { "SCHW", "Charles Schwab Corp", "Finance", 170 }, { "ICE", "Intercontinental Exchange", "Finance", 100 }, { "BX", "Blackstone Inc.", "Finance", 190 },
        { "KKR", "KKR & Co. Inc.", "Finance", 130 }, { "PYPL", "PayPal Holdings", "Finance", 70 }, { "COF", "Capital One Financial", "Finance", 80 },
        { "PGR", "Progressive Corp", "Finance", 150 }, { "CB", "Chubb Limited", "Finance", 120 },
        // Energy
        { "XOM", "Exxon Mobil Corp", "Energy", 500 }, { "CVX", "Chevron Corp", "Energy", 300 }, { "COP", "ConocoPhillips", "Energy", 130 },
        { "VLO", "Valero Energy Corp", "Energy", 50 }, { "WMB", "Williams Companies", "Energy", 70 }, { "SLB", "Schlumberger N.V.", "Energy", 50 },
        { "EOG", "EOG Resources", "Energy", 70 }, { "KMI", "Kinder Morgan", "Energy", 60 }, { "PSX", "Phillips 66", "Energy", 60 },
        { "MPC", "Marathon Petroleum", "Energy", 60 }, { "OKE", "Oneok, Inc.", "Energy", 60 }, { "OXY", "Occidental Petroleum", "Energy", 45 },
        { "DVN", "Devon Energy", "Energy", 25 }, { "TRGP", "Targa Resources", "Energy", 45 }, { "HAL", "Halliburton Co", "Energy", 25 },
        { "BKR", "Baker Hughes Co", "Energy", 45 }, { "FANG", "Diamondback Energy", "Energy", 45 }, { "CEG", "Constellation Energy", "Energy", 100 },
        { "VST", "Vistra Corp", "Energy", 60 },
        // Industrials
        { "CAT", "Caterpillar Inc.", "Industrials", 200 }, { "RTX", "RTX Corp", "Industrials", 200 }, { "GE", "GE Aerospace", "Industrials", 300 },
        { "ETN", "Eaton Corp plc", "Industrials", 150 }, { "HON", "Honeywell International", "Industrials", 140 }, { "UNP", "Union Pacific Corp", "Industrials", 140 },
        { "UPS", "United Parcel Service", "Industrials", 90 }, { "WM", "Waste Management", "Industrials", 90 }, { "NOC", "Northrop Grumman", "Industrials", 80 },
        { "GD", "General Dynamics", "Industrials", 80 }, { "LMT", "Lockheed Martin", "Industrials", 110 }, { "BA", "Boeing Co", "Industrials", 150 },
        { "DE", "Deere & Co", "Industrials", 140 }, { "PWR", "Quanta Services", "Industrials", 60 }, { "URI", "United Rentals", "Industrials", 60 },
        { "EMR", "Emerson Electric", "Industrials", 75 }, { "CARR", "Carrier Global", "Industrials", 60 }, { "PH", "Parker-Hannifin", "Industrials", 90 },
        { "TT", "Trane Technologies", "Industrials", 100 }, { "FDX", "FedEx Corp", "Industrials", 60 },
        // Utilities
        { "NEE", "NextEra Energy", "Utilities", 150 }, { "SO", "Southern Co", "Utilities", 100 }, { "DUK", "Duke Energy", "Utilities", 90 },
        { "AEP", "American Electric Power", "Utilities", 60 }, { "SRE", "Sempra", "Utilities", 50 }, { "D", "Dominion Energy", "Utilities", 45 },
        { "EXC", "Exelon Corp", "Utilities", 45 }, { "XEL", "Xcel Energy", "Utilities", 40 },
        // Materials
        { "LIN", "Linde plc", "Materials", 220 }, { "NEM", "Newmont Corp", "Materials", 60 }, { "SHW", "Sherwin-Williams", "Materials", 90 },
        { "APD", "Air Products & Chemicals", "Materials", 65 }, { "FCX", "Freeport-McMoRan", "Materials", 60 }, { "ECL", "Ecolab Inc.", "Materials", 70 },
        { "NUE", "Nucor Corp", "Materials", 30 }, { "SCCO", "Southern Copper", "Materials", 80 }, { "DOW", "Dow Inc.", "Materials", 25 },
        // Real estate
        { "PLD", "Prologis, Inc.", "Real Estate", 110 }, { "AMT", "American Tower", "Real Estate", 100 }, { "O", "Realty Income", "Real Estate", 50 },
        { "WELL", "Welltower Inc.", "Real Estate", 90 }, { "SPG", "Simon Property Group", "Real Estate", 55 }, { "DLR", "Digital Realty Trust", "Real Estate", 60 },
        { "PSA", "Public Storage", "Real Estate", 50 }, { "CCI", "Crown Castle", "Real Estate", 45 }, { "VICI", "VICI Properties", "Real Estate", 35 },
        { "EQIX", "Equinix, Inc.", "Real Estate", 80 },
    };
    return entries;
}

QColor sectorColor(const QString& sector, const Theme& theme)
{
    static const QHash<QString, QString> colors{
        { "Internet", "#a78bfa" }, { "Software", "#7c3aed" }, { "Semis", "#4f46e5" }, { "Hardware", "#8b5cf6" }, { "Consumer", "#f59e0b" },
        { "Healthcare", "#ec4899" }, { "Finance", "#3b82f6" }, { "Energy", "#f97316" }, { "Industrials", "#9ca3af" }, { "Utilities", "#84cc16" },
        { "Materials", "#d97706" }, { "Real Estate", "#14b8a6" },
    };
    const QString hex = colors.value(sector);
    if (!hex.isEmpty()) return QColor(hex);
    // Watchlist industries: a stable hue from the name.
    const uint h = qHash(sector) % 360u;
    return QColor::fromHsv(static_cast<int>(h), 140, 200);
}

QString formatCap(double usd)
{
    if (usd >= 1e12) return QStringLiteral("%1T").arg(QString::number(usd / 1e12, 'f', 2));
    if (usd >= 1e9) return QStringLiteral("%1B").arg(QString::number(usd / 1e9, 'f', usd >= 1e11 ? 0 : 1));
    if (usd >= 1e6) return QStringLiteral("%1M").arg(QString::number(usd / 1e6, 'f', 0));
    return usd > 0 ? QString::number(usd, 'f', 0) : QStringLiteral("–");
}

QString signedPct(double v)
{
    if (std::isnan(v)) return QStringLiteral("–");
    return QStringLiteral("%1%2%").arg(v >= 0 ? "+" : "").arg(QString::number(v, 'f', 2));
}

} // namespace

// MARK: - Treemap view

/// Paints the two-level treemap (sectors with a title band, stocks inside) or the
/// sector-level view, plus a colour legend; reports hover, click and double-click.
class TreemapView : public QWidget
{
public:
    struct Tile {
        QString ticker;      ///< empty for sector tiles
        QString title;       ///< ticker or sector name
        QString subtitle;    ///< company name or "N stocks"
        double weight = 0.0;
        double performance = std::numeric_limits<double>::quiet_NaN();
        QString tooltip;
        treemap::Rect rect;
    };
    struct Group {
        QString name;
        QColor color;
        double weight = 0.0;
        double performance = std::numeric_limits<double>::quiet_NaN();
        std::vector<Tile> tiles;
        treemap::Rect rect;
    };

    explicit TreemapView(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMouseTracking(true);
        setMinimumSize(320, 220);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

    void setTheme(const Theme& theme) { m_theme = theme; update(); }
    void setScale(double percent) { m_scale = std::max(0.5, percent); update(); }
    void setGroups(std::vector<Group> groups, bool sectorsOnly)
    {
        m_groups = std::move(groups);
        m_sectorsOnly = sectorsOnly;
        m_hover = nullptr;
        m_dirty = true;
        update();
    }
    void setEmptyText(const QString& text) { m_emptyText = text; update(); }

    std::function<void(const QString& ticker)> onTickerClicked;
    std::function<void(const QString& ticker)> onTickerDoubleClicked;
    /// A sector tile (Sectors view) or a sector's title band (Stocks view) was clicked.
    std::function<void(const QString& sector)> onGroupClicked;

protected:
    void resizeEvent(QResizeEvent*) override { m_dirty = true; }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QColor bg(m_theme.surface.isEmpty() ? "#121a2b" : m_theme.surface);
        p.fillRect(rect(), bg);
        if (m_dirty) layout();
        if (m_groups.empty()) {
            p.setPen(QColor(m_theme.textMuted.isEmpty() ? "#8294ad" : m_theme.textMuted));
            p.drawText(rect(), Qt::AlignCenter, m_emptyText.isEmpty() ? QStringLiteral("No data yet.") : m_emptyText);
            return;
        }
        for (const Group& g : m_groups) {
            if (g.rect.empty()) continue;
            if (m_sectorsOnly) {
                paintTile(p, g.tiles.empty() ? Tile{} : g.tiles.front(), g.rect, g.color, true);
                continue;
            }
            // Title band (brighter while hovered: it is clickable and expands the sector)
            const QRectF band(g.rect.x, g.rect.y, g.rect.w, kBand);
            p.fillRect(band, &g == m_hoverBand ? g.color.lighter(125) : g.color);
            p.setPen(QColor("#0b1020"));
            QFont f = font();
            f.setBold(true);
            f.setPixelSize(11);
            p.setFont(f);
            p.drawText(band.adjusted(5, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(g.name.toUpper(), Qt::ElideRight, static_cast<int>(band.width()) - 10));
            for (const Tile& t : g.tiles) {
                if (t.rect.empty()) continue;
                paintTile(p, t, t.rect, g.color, false);
            }
        }
        paintLegend(p);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const Tile* tile = tileAt(e->position());
        const Group* band = bandAt(e->position());
        if (tile != m_hover || band != m_hoverBand) {
            m_hover = tile;
            m_hoverBand = band;
            update();
        }
        setCursor(tile || band ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (tile) QToolTip::showText(e->globalPosition().toPoint(), tile->tooltip, this);
        else if (band) QToolTip::showText(e->globalPosition().toPoint(), QStringLiteral("Click to expand %1").arg(band->name), this);
        else QToolTip::hideText();
    }
    void leaveEvent(QEvent*) override { m_hover = nullptr; m_hoverBand = nullptr; update(); }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        if (const Group* band = bandAt(e->position()); band && onGroupClicked) { onGroupClicked(band->name); return; }
        const Tile* tile = tileAt(e->position());
        if (!tile) return;
        if (tile->ticker.isEmpty()) { if (onGroupClicked) onGroupClicked(tile->title); }   // sector tile
        else if (onTickerClicked) onTickerClicked(tile->ticker);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (const Tile* tile = tileAt(e->position()); tile && !tile->ticker.isEmpty() && onTickerDoubleClicked) onTickerDoubleClicked(tile->ticker);
    }

private:
    static constexpr double kBand = 18.0;
    static constexpr double kLegend = 44.0;
    static constexpr double kGap = 2.0;

    const Tile* tileAt(const QPointF& pos) const
    {
        for (const Group& g : m_groups) {
            if (!g.rect.contains(pos.x(), pos.y())) continue;
            if (m_sectorsOnly) return g.tiles.empty() ? nullptr : &g.tiles.front();
            for (const Tile& t : g.tiles) if (t.rect.contains(pos.x(), pos.y())) return &t;
        }
        return nullptr;
    }
    /// The sector whose title band is under `pos` (Stocks view only).
    const Group* bandAt(const QPointF& pos) const
    {
        if (m_sectorsOnly) return nullptr;
        for (const Group& g : m_groups) {
            if (g.rect.contains(pos.x(), pos.y()) && pos.y() < g.rect.y + kBand) return &g;
        }
        return nullptr;
    }

    void layout()
    {
        m_dirty = false;
        const treemap::Rect bounds{ 0, 0, std::max(0.0, width() - kLegend - 4.0), static_cast<double>(height()) };
        std::vector<double> weights;
        for (const Group& g : m_groups) weights.push_back(g.weight);
        const auto rects = treemap::squarify(weights, bounds);
        for (size_t i = 0; i < m_groups.size(); ++i) {
            Group& g = m_groups[i];
            g.rect = treemap::inset(rects[i], kGap / 2.0);
            if (m_sectorsOnly || g.rect.empty()) continue;
            const treemap::Rect body{ g.rect.x, g.rect.y + kBand, g.rect.w, std::max(0.0, g.rect.h - kBand) };
            std::vector<double> w;
            for (const Tile& t : g.tiles) w.push_back(t.weight);
            const auto tileRects = treemap::squarify(w, body);
            for (size_t k = 0; k < g.tiles.size(); ++k) g.tiles[k].rect = treemap::inset(tileRects[k], kGap / 2.0);
        }
    }

    QColor performanceColor(double perf) const
    {
        const QColor neutral(m_theme.surfaceAlt.isEmpty() ? "#2b3444" : m_theme.surfaceAlt);
        if (std::isnan(perf)) return neutral;
        const double t = std::clamp(perf / m_scale, -1.0, 1.0);
        const QColor end(t >= 0 ? (m_theme.up.isEmpty() ? "#22c55e" : m_theme.up) : (m_theme.down.isEmpty() ? "#ef4444" : m_theme.down));
        // Deep saturated end colours like the terminal look: blend towards a darker version of the theme colour.
        const QColor deep = end.darker(135);
        const double a = std::fabs(t);
        if (a < 0.02) return neutral;
        const double k = 0.22 + 0.78 * a;
        auto mix = [k](int from, int to) { return static_cast<int>(std::lround(from + (to - from) * k)); };
        return QColor(mix(neutral.red(), deep.red()), mix(neutral.green(), deep.green()), mix(neutral.blue(), deep.blue()));
    }

    void paintTile(QPainter& p, const Tile& t, const treemap::Rect& r, const QColor& accent, bool sectorTile)
    {
        const QRectF box(r.x, r.y, r.w, r.h);
        QColor fill = performanceColor(t.performance);
        if (&t == m_hover) fill = fill.lighter(118);
        p.fillRect(box, fill);
        p.setPen(QPen(QColor(m_theme.surface.isEmpty() ? "#121a2b" : m_theme.surface), 1));
        p.drawRect(box.adjusted(0, 0, -1, -1));
        if (&t == m_hover) { p.setPen(QPen(accent, 2)); p.drawRect(box.adjusted(1, 1, -2, -2)); }
        if (r.w < 22 || r.h < 14) return;

        const QString pct = signedPct(t.performance);
        const double usable = r.w - 6.0;
        QFont f = font();
        f.setBold(true);
        // Start from the height, then shrink until the whole ticker fits the width (down to 8 px)
        // so small tiles read "ORCL" rather than "OR…".
        // Size from the tile's area (denser, terminal-style type): about a ninth of the side
        // of an equivalent square, capped so large tiles stay restrained.
        int px = static_cast<int>(std::clamp(std::sqrt(r.w * r.h) / 9.0, 8.0, sectorTile ? 24.0 : 19.0));
        f.setPixelSize(px);
        QFontMetrics fm(f);
        while (px > 8 && fm.horizontalAdvance(t.title) > usable) {
            f.setPixelSize(--px);
            fm = QFontMetrics(f);
        }
        p.setFont(f);
        const QColor textColor("#f3f6fb");
        p.setPen(textColor);
        const bool showPct = r.h >= px * 2.3 && r.w >= 36;
        const bool showSub = r.h >= px * 3.8 && r.w >= 90 && !t.subtitle.isEmpty();
        QFont small = font();
        small.setPixelSize(std::max(8, static_cast<int>(std::lround(px * 0.72))));
        QFont tiny = font();
        tiny.setPixelSize(std::max(8, std::min(11, px / 2 + 2)));
        const double block = fm.height() + (showPct ? QFontMetrics(small).height() : 0) + (showSub ? QFontMetrics(tiny).height() + 2 : 0);
        double y = r.y + (r.h - block) / 2.0;
        p.drawText(QRectF(r.x + 3, y, usable, fm.height()), Qt::AlignHCenter | Qt::AlignVCenter, fm.elidedText(t.title, Qt::ElideRight, static_cast<int>(usable)));
        y += fm.height();
        if (showPct) {
            p.setFont(small);
            p.drawText(QRectF(r.x + 3, y, usable, QFontMetrics(small).height()), Qt::AlignHCenter | Qt::AlignVCenter, pct);
            y += QFontMetrics(small).height();
        }
        if (showSub) {
            p.setFont(tiny);
            p.setPen(QColor(230, 236, 245, 200));
            p.drawText(QRectF(r.x + 3, y + 2, usable, QFontMetrics(tiny).height()), Qt::AlignHCenter | Qt::AlignVCenter, QFontMetrics(tiny).elidedText(t.subtitle, Qt::ElideRight, static_cast<int>(usable)));
        }
    }

    void paintLegend(QPainter& p)
    {
        const double x = width() - kLegend + 14.0;
        const double top = 22.0, bottom = height() - 22.0;
        if (bottom <= top) return;
        QLinearGradient grad(QPointF(x, top), QPointF(x, bottom));
        grad.setColorAt(0.0, performanceColor(m_scale));
        grad.setColorAt(0.5, performanceColor(0.0));
        grad.setColorAt(1.0, performanceColor(-m_scale));
        p.fillRect(QRectF(x, top, 14, bottom - top), grad);
        QFont f = font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(m_theme.up.isEmpty() ? "#22c55e" : m_theme.up));
        p.drawText(QRectF(width() - kLegend, 2, kLegend, 18), Qt::AlignCenter, QStringLiteral("+%1%").arg(QString::number(m_scale, 'g', 3)));
        p.setPen(QColor(m_theme.down.isEmpty() ? "#ef4444" : m_theme.down));
        p.drawText(QRectF(width() - kLegend, height() - 20, kLegend, 18), Qt::AlignCenter, QStringLiteral("−%1%").arg(QString::number(m_scale, 'g', 3)));
    }

    Theme m_theme;
    std::vector<Group> m_groups;
    bool m_sectorsOnly = false;
    bool m_dirty = true;
    double m_scale = 3.0;
    const Tile* m_hover = nullptr;
    const Group* m_hoverBand = nullptr;
    QString m_emptyText;
};

// MARK: - Tab

const std::vector<SectorHeatmapTab::Period>& SectorHeatmapTab::periods()
{
    static const std::vector<Period> list{ { "Daily", 0, 3.0 }, { "1W", 7, 6.0 }, { "30D", 30, 12.0 }, { "90D", 90, 20.0 }, { "YTD", -1, 40.0 } };
    return list;
}

SectorHeatmapTab::SectorHeatmapTab(QWidget* parent) : QWidget(parent)
{
    buildUi();
    wire();
    loadCapCache();
    buildUniverse();
}

void SectorHeatmapTab::buildUi()
{
    auto* viewLabel = new QLabel("View", this);
    viewLabel->setObjectName("muted");
    m_viewGroup = new QButtonGroup(this);
    m_viewGroup->setExclusive(true);
    auto* viewRow = new QHBoxLayout;
    viewRow->setSpacing(4);
    int index = 0;
    for (const char* name : { "Stocks", "Sectors" }) {
        auto* button = new QToolButton(this);
        button->setText(name);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(index == 0 ? "Every stock as a tile inside its sector, sized by market cap" : "One cap-weighted tile per sector");
        m_viewGroup->addButton(button, index++);
        viewRow->addWidget(button);
    }
    m_periodGroup = new QButtonGroup(this);
    m_periodGroup->setExclusive(true);
    auto* periodRow = new QHBoxLayout;
    periodRow->setSpacing(4);
    index = 0;
    for (const Period& period : periods()) {
        auto* button = new QToolButton(this);
        button->setText(period.label);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(period.lookbackDays == 0 ? "Change versus the previous close" : (period.lookbackDays < 0 ? "Change since the last close of the previous year" : QStringLiteral("Change over the last %1 calendar days").arg(period.lookbackDays)));
        m_periodGroup->addButton(button, index++);
        periodRow->addWidget(button);
    }
    m_universe = new QComboBox(this);
    m_universe->addItem(QStringLiteral("Large caps (%1)").arg(universe().size()), "builtin");
    m_universe->addItem("Watchlist", "watchlist");
    m_universe->setToolTip("Which stocks to show: the built-in large-cap universe grouped by sector, or the active watchlist grouped by industry");
    m_summary = new QLabel(this);
    m_summary->setObjectName("muted");
    m_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_refresh = ui::makeButton(this, "Refresh", "secondary", "Download the latest prices");
    m_reset = ui::makeButton(this, "Reset", "secondary", "Back to the full sector map");
    m_reset->setVisible(false);

    auto* controls = new QHBoxLayout;
    controls->setSpacing(10);
    controls->addWidget(viewLabel);
    controls->addLayout(viewRow);
    controls->addSpacing(8);
    controls->addLayout(periodRow);
    controls->addSpacing(8);
    controls->addWidget(m_universe);
    controls->addWidget(m_summary, 1);
    controls->addWidget(m_reset);
    controls->addWidget(m_refresh);

    m_view = new TreemapView(this);
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* pane = new QFrame(this);
    pane->setObjectName("pane");
    auto* paneLayout = new QVBoxLayout(pane);
    paneLayout->setContentsMargins(12, 10, 12, 10);
    paneLayout->setSpacing(8);
    paneLayout->addLayout(controls);
    paneLayout->addWidget(m_view, 1);
    paneLayout->addWidget(m_status);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 10);
    root->addWidget(pane);

    const QSettings settings;
    m_periodIndex = std::clamp(settings.value(kPeriodKey, 0).toInt(), 0, static_cast<int>(periods().size()) - 1);
    m_sectorsView = settings.value(kViewKey, "stocks").toString() == "sectors";
    m_watchlistUniverse = settings.value(kUniverseKey, "builtin").toString() == "watchlist";
    if (QAbstractButton* b = m_periodGroup->button(m_periodIndex)) b->setChecked(true);
    if (QAbstractButton* b = m_viewGroup->button(m_sectorsView ? 1 : 0)) b->setChecked(true);
    m_universe->setCurrentIndex(m_watchlistUniverse ? 1 : 0);
    m_view->setScale(periods()[static_cast<size_t>(m_periodIndex)].colorScale);
    m_view->setEmptyText("Press Refresh to load sector performance.");
}

void SectorHeatmapTab::wire()
{
    connect(m_viewGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_sectorsView = id == 1;
        QSettings().setValue(kViewKey, m_sectorsView ? "sectors" : "stocks");
        relayout();
    });
    connect(m_periodGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_periodIndex = id;
        QSettings().setValue(kPeriodKey, id);
        m_view->setScale(periods()[static_cast<size_t>(id)].colorScale);
        if (m_loaded) ensureReferenceCloses();
        else relayout();
    });
    connect(m_universe, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        m_watchlistUniverse = m_universe->currentData().toString() == "watchlist";
        QSettings().setValue(kUniverseKey, m_watchlistUniverse ? "watchlist" : "builtin");
        buildUniverse();
        refresh();
    });
    connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_reset, &QPushButton::clicked, this, [this] { clearFocus(); });
    m_view->onTickerClicked = [this](const QString& ticker) { if (onTickerSelected) onTickerSelected(ticker); };
    m_view->onTickerDoubleClicked = [this](const QString& ticker) { if (onOpenChain) onOpenChain(ticker); };
    m_view->onGroupClicked = [this](const QString& sector) { focusSector(sector); };
}

bool SectorHeatmapTab::focusSector(const QString& sector)
{
    const QString wanted = sector.trimmed();
    QString match;
    for (const Stock& s : m_stocks) {
        if (s.sector.compare(wanted, Qt::CaseInsensitive) == 0) { match = s.sector; break; }
    }
    if (match.isEmpty()) return false;
    m_focusSector = match;
    m_reset->setVisible(true);
    m_reset->setText(QStringLiteral("Reset ‹ %1").arg(match));
    relayout();
    return true;
}

void SectorHeatmapTab::clearFocus()
{
    if (m_focusSector.isEmpty()) return;
    m_focusSector.clear();
    m_reset->setVisible(false);
    relayout();
}

void SectorHeatmapTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!m_loaded && !m_loadingQuotes) refresh();
}

void SectorHeatmapTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    m_view->setTheme(theme);
    relayout();
}

// MARK: - Universe and data

QString SectorHeatmapTab::sectorForWatchlistTicker(const QString& ticker, QString* name) const
{
    // The Option Chain tab caches ticker details (name, industry) next to the logos.
    const QString path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/logos/" + ticker + ".json";
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
        if (name) *name = o.value("name").toString();
        QString industry = o.value("industry").toString().trimmed();
        if (!industry.isEmpty()) {
            industry = industry.toLower();
            industry[0] = industry[0].toUpper();
            return industry;
        }
    }
    for (const UniverseEntry& e : universe()) {
        if (ticker == e.ticker) { if (name && name->isEmpty()) *name = e.name; return e.sector; }
    }
    return QStringLiteral("Other");
}

void SectorHeatmapTab::buildUniverse()
{
    m_stocks.clear();
    if (m_watchlistUniverse) {
        const QStringList tickers = watchlistProvider ? watchlistProvider() : QStringList();
        for (const QString& t : tickers) {
            Stock s;
            s.ticker = t.toUpper();
            s.sector = sectorForWatchlistTicker(s.ticker, &s.name);
            const auto cap = m_capCache.find(s.ticker);
            if (cap != m_capCache.end()) { s.marketCap = cap->second.marketCap; if (s.name.isEmpty()) s.name = cap->second.name; }
            if (s.name.isEmpty()) s.name = s.ticker;
            m_stocks.push_back(s);
        }
    } else {
        for (const UniverseEntry& e : universe()) {
            Stock s;
            s.ticker = e.ticker;
            s.name = e.name;
            s.sector = e.sector;
            s.marketCap = e.capBn * 1e9;
            const auto cap = m_capCache.find(s.ticker);
            if (cap != m_capCache.end() && cap->second.marketCap > 0.0) s.marketCap = cap->second.marketCap;
            m_stocks.push_back(s);
        }
    }
    m_loaded = false;
    relayout();
}

void SectorHeatmapTab::refresh()
{
    if (m_stocks.empty()) {
        updateStatus(m_watchlistUniverse ? "The watchlist is empty." : "No stocks in the universe.", ui::StatusKind::Error);
        return;
    }
    fetchQuotes();
    fetchMissingCaps();
}

void SectorHeatmapTab::fetchQuotes()
{
    if (m_loadingQuotes) return;
    m_loadingQuotes = true;
    updateStatus(QStringLiteral("Loading prices for %1 stocks…").arg(m_stocks.size()), ui::StatusKind::Info);
    QStringList tickers;
    for (const Stock& s : m_stocks) tickers << s.ticker;
    // The snapshot endpoint takes a comma list; keep each request under ~60 symbols.
    auto remaining = std::make_shared<int>(0);
    auto failed = std::make_shared<QString>();
    const int chunk = 60;
    for (int i = 0; i < tickers.size(); i += chunk) ++*remaining;
    for (int i = 0; i < tickers.size(); i += chunk) {
        const QStringList part = tickers.mid(i, chunk);
        m_client.fetchQuotes(part, [this, remaining, failed](const std::vector<MarketDataClient::Quote>& quotes) {
            for (const MarketDataClient::Quote& q : quotes) {
                for (Stock& s : m_stocks) {
                    if (s.ticker != q.ticker) continue;
                    s.last = q.last;
                    s.previousClose = q.previousClose;
                    if (q.asOf.isValid() && q.asOf > m_pricesAsOf) m_pricesAsOf = q.asOf;
                }
            }
            if (--*remaining == 0) {
                m_loadingQuotes = false;
                m_loaded = true;
                if (!failed->isEmpty()) updateStatus(*failed, ui::StatusKind::Error);
                ensureReferenceCloses();
            }
        }, [this, remaining, failed](const QString& message) {
            *failed = message;
            if (--*remaining == 0) {
                m_loadingQuotes = false;
                m_loaded = true;
                updateStatus(message, ui::StatusKind::Error);
                ensureReferenceCloses();
            }
        });
    }
}

QDate SectorHeatmapTab::referenceDate() const
{
    const Period& period = periods()[static_cast<size_t>(m_periodIndex)];
    const QDate today = QDate::currentDate();
    if (period.lookbackDays < 0) return QDate(today.year() - 1, 12, 31);
    return today.addDays(-period.lookbackDays);
}

void SectorHeatmapTab::ensureReferenceCloses()
{
    const Period& period = periods()[static_cast<size_t>(m_periodIndex)];
    if (period.lookbackDays == 0) { recomputePerformance(); return; }
    const QString label = period.label;
    if (m_referenceSession.count(label)) { applyReferenceCloses(); return; }
    // Walk back from the target date to the most recent trading session (weekends, holidays).
    auto attempt = std::make_shared<std::function<void(QDate, int)>>();
    *attempt = [this, label, attempt](QDate date, int triesLeft) {
        updateStatus(QStringLiteral("Loading %1 reference closes (%2)…").arg(label, date.toString(Qt::ISODate)), ui::StatusKind::Info);
        m_client.fetchGroupedDaily(date, [this, label, date, triesLeft, attempt](const std::map<QString, double>& closes) {
            if (closes.empty() && triesLeft > 0) { (*attempt)(date.addDays(-1), triesLeft - 1); return; }
            m_referenceCloses[date.toString(Qt::ISODate)] = closes;
            m_referenceSession[label] = date;
            applyReferenceCloses();
        }, [this, date](const QString& message) {
            qWarning("[sector-heatmap] grouped daily failed (%s); falling back to per-ticker history", qPrintable(message));
            fetchReferenceFallback(date);
        });
    };
    (*attempt)(referenceDate(), 6);
}

void SectorHeatmapTab::fetchReferenceFallback(const QDate& target)
{
    // Per-ticker daily bars ending at the target date; a handful at a time.
    const QString label = periods()[static_cast<size_t>(m_periodIndex)].label;
    auto closes = std::make_shared<std::map<QString, double>>();
    auto queue = std::make_shared<QStringList>();
    for (const Stock& s : m_stocks) *queue << s.ticker;
    auto inFlight = std::make_shared<int>(0);
    auto session = std::make_shared<QDate>(target);
    auto pump = std::make_shared<std::function<void()>>();
    *pump = [this, closes, queue, inFlight, session, target, label, pump] {
        while (*inFlight < 4 && !queue->isEmpty()) {
            const QString ticker = queue->takeFirst();
            ++*inFlight;
            m_client.fetchAggregates(ticker, 1, "day", target.addDays(-10), target, [this, ticker, closes, inFlight, session, pump](const MarketDataClient::BarSeries& series) {
                if (!series.bars.empty()) {
                    (*closes)[ticker] = series.bars.back().close;
                    *session = QDateTime::fromMSecsSinceEpoch(series.bars.back().timeMs, QTimeZone("America/New_York")).date();
                }
                --*inFlight;
                (*pump)();
            }, [inFlight, pump](const QString&) { --*inFlight; (*pump)(); });
        }
        if (*inFlight == 0 && queue->isEmpty()) {
            m_referenceCloses[session->toString(Qt::ISODate)] = *closes;
            m_referenceSession[label] = *session;
            applyReferenceCloses();
        }
    };
    (*pump)();
}

void SectorHeatmapTab::applyReferenceCloses()
{
    const QString label = periods()[static_cast<size_t>(m_periodIndex)].label;
    const auto session = m_referenceSession.find(label);
    if (session == m_referenceSession.end()) { recomputePerformance(); return; }
    m_referenceSessionDate = session->second;
    const auto closes = m_referenceCloses.find(session->second.toString(Qt::ISODate));
    for (Stock& s : m_stocks) {
        s.referenceClose = 0.0;
        if (closes == m_referenceCloses.end()) continue;
        const auto it = closes->second.find(s.ticker);
        if (it != closes->second.end()) s.referenceClose = it->second;
    }
    recomputePerformance();
}

void SectorHeatmapTab::recomputePerformance()
{
    const Period& period = periods()[static_cast<size_t>(m_periodIndex)];
    int priced = 0;
    for (Stock& s : m_stocks) {
        const double base = period.lookbackDays == 0 ? s.previousClose : s.referenceClose;
        s.performance = (s.last > 0.0 && base > 0.0) ? (s.last / base - 1.0) * 100.0 : std::numeric_limits<double>::quiet_NaN();
        if (!std::isnan(s.performance)) ++priced;
    }
    QString status = QStringLiteral("%1 of %2 stocks priced").arg(priced).arg(m_stocks.size());
    if (m_pricesAsOf.isValid()) status += QStringLiteral(" · last prices %1").arg(m_pricesAsOf.toLocalTime().toString("yyyy-MM-dd HH:mm"));
    if (period.lookbackDays != 0 && m_referenceSessionDate.isValid()) status += QStringLiteral(" · versus the %1 close").arg(m_referenceSessionDate.toString(Qt::ISODate));
    status += " · 15-minute delayed data from Massive.com";
    updateStatus(status, ui::StatusKind::Info);
    relayout();
}

void SectorHeatmapTab::relayout()
{
    // A focused sector fills the whole map with its stocks (band and tiles); otherwise the
    // Stocks view shows every sector with its members and the Sectors view one tile each.
    const bool focused = !m_focusSector.isEmpty();
    const bool sectorsOnly = m_sectorsView && !focused;
    std::map<QString, TreemapView::Group> groups;
    std::vector<QString> order;
    for (const Stock& s : m_stocks) {
        if (focused && s.sector != m_focusSector) continue;
        if (!groups.count(s.sector)) { order.push_back(s.sector); groups[s.sector].name = s.sector; groups[s.sector].color = sectorColor(s.sector, m_theme); }
        TreemapView::Group& g = groups[s.sector];
        TreemapView::Tile t;
        t.ticker = s.ticker;
        t.title = s.ticker;
        t.subtitle = s.name;
        t.weight = std::max(s.marketCap, 1e8);
        t.performance = s.performance;
        t.tooltip = QStringLiteral("<b>%1</b> · %2<br>%3<br>Market cap %4<br>Last %5 · %6: %7")
                        .arg(s.ticker, s.name, s.sector, formatCap(s.marketCap), s.last > 0 ? ui::number(s.last, 2) : QStringLiteral("–"),
                             periods()[static_cast<size_t>(m_periodIndex)].label, signedPct(s.performance));
        g.weight += t.weight;
        g.tiles.push_back(t);
    }
    std::vector<TreemapView::Group> list;
    for (const QString& name : order) {
        TreemapView::Group g = groups[name];
        // Cap-weighted sector performance over the priced members.
        double wsum = 0.0, psum = 0.0;
        for (const TreemapView::Tile& t : g.tiles) { if (!std::isnan(t.performance)) { wsum += t.weight; psum += t.weight * t.performance; } }
        g.performance = wsum > 0.0 ? psum / wsum : std::numeric_limits<double>::quiet_NaN();
        if (sectorsOnly) {
            TreemapView::Tile sectorTile;
            sectorTile.title = g.name;
            sectorTile.subtitle = QStringLiteral("%1 stocks · %2").arg(g.tiles.size()).arg(formatCap(g.weight));
            sectorTile.weight = g.weight;
            sectorTile.performance = g.performance;
            sectorTile.tooltip = QStringLiteral("<b>%1</b><br>%2 stocks · market cap %3<br>%4: %5 (cap-weighted)")
                                     .arg(g.name).arg(g.tiles.size()).arg(formatCap(g.weight), periods()[static_cast<size_t>(m_periodIndex)].label, signedPct(g.performance));
            g.tiles.assign(1, sectorTile);
        } else {
            std::sort(g.tiles.begin(), g.tiles.end(), [](const TreemapView::Tile& a, const TreemapView::Tile& b) { return a.weight > b.weight; });
        }
        list.push_back(g);
    }
    std::sort(list.begin(), list.end(), [](const TreemapView::Group& a, const TreemapView::Group& b) { return a.weight > b.weight; });
    const size_t shown = list.empty() ? 0 : (focused ? list.front().tiles.size() : m_stocks.size());
    m_view->setGroups(std::move(list), sectorsOnly);
    if (focused) {
        m_summary->setText(QStringLiteral("%1 · %2 stocks · %3 change")
                               .arg(m_focusSector).arg(shown).arg(periods()[static_cast<size_t>(m_periodIndex)].label));
        m_summary->setToolTip("Tile size is market cap and colour is the period change. Click a tile to load it; Reset returns to all sectors.");
    } else {
        m_summary->setText(QStringLiteral("%1 stocks · %2 sectors · %3 change")
                               .arg(m_stocks.size()).arg(order.size()).arg(periods()[static_cast<size_t>(m_periodIndex)].label));
        m_summary->setToolTip(m_sectorsView ? "Tile size is market cap and colour is the period change. Click a sector to expand it."
                                            : "Tile size is market cap and colour is the period change.");
    }
}

// MARK: - Market caps

QString SectorHeatmapTab::capCachePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir + "/sector-heatmap-caps.json";
}

void SectorHeatmapTab::loadCapCache()
{
    QFile file(capCachePath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        CapEntry e;
        e.marketCap = o.value("cap").toDouble();
        e.name = o.value("name").toString();
        e.asOf = QDateTime::fromString(o.value("asOf").toString(), Qt::ISODate);
        if (e.marketCap > 0.0) m_capCache[it.key()] = e;
    }
}

void SectorHeatmapTab::saveCapCache() const
{
    QJsonObject root;
    for (const auto& [ticker, e] : m_capCache) root[ticker] = QJsonObject{ { "cap", e.marketCap }, { "name", e.name }, { "asOf", e.asOf.toString(Qt::ISODate) } };
    QFile file(capCachePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void SectorHeatmapTab::fetchMissingCaps()
{
    const QDateTime stale = QDateTime::currentDateTime().addDays(-kCapCacheDays);
    for (const Stock& s : m_stocks) {
        const auto it = m_capCache.find(s.ticker);
        if ((it == m_capCache.end() || it->second.asOf < stale) && !m_capQueue.contains(s.ticker)) m_capQueue << s.ticker;
    }
    auto pump = std::make_shared<std::function<void()>>();
    auto fetched = std::make_shared<int>(0);
    *pump = [this, pump, fetched] {
        while (m_capInFlight < 3 && !m_capQueue.isEmpty()) {
            const QString ticker = m_capQueue.takeFirst();
            ++m_capInFlight;
            m_client.fetchTickerDetails(ticker, [this, ticker, pump, fetched](const MarketDataClient::TickerDetails& details) {
                --m_capInFlight;
                if (details.marketCap > 0.0) {
                    m_capCache[ticker] = CapEntry{ details.marketCap, details.name, QDateTime::currentDateTime() };
                    for (Stock& s : m_stocks) if (s.ticker == ticker) { s.marketCap = details.marketCap; if (s.name.isEmpty() || s.name == s.ticker) s.name = details.name; }
                    if (++*fetched % 10 == 0) { saveCapCache(); relayout(); }
                }
                (*pump)();
            }, [this, pump](const QString&) { --m_capInFlight; (*pump)(); });
        }
        if (m_capInFlight == 0 && m_capQueue.isEmpty() && *fetched > 0) { saveCapCache(); relayout(); *fetched = 0; }
    };
    (*pump)();
}

// MARK: - Assistant and tests

void SectorHeatmapTab::loadSampleData()
{
    m_watchlistUniverse = false;
    m_universe->setCurrentIndex(0);
    buildUniverse();
    // Deterministic pseudo-random moves: sector drift plus a stock-specific wiggle.
    uint seed = 7;
    auto rnd = [&seed] { seed = seed * 1103515245u + 12345u; return ((seed >> 8) % 20001) / 10000.0 - 1.0; };
    std::map<QString, double> drift;
    for (Stock& s : m_stocks) {
        if (!drift.count(s.sector)) drift[s.sector] = rnd() * 1.5;
        s.previousClose = 100.0;
        s.last = 100.0 * (1.0 + (drift[s.sector] + rnd() * 2.0) / 100.0);
        s.referenceClose = 100.0 * (1.0 - (drift[s.sector] * 3.0 + rnd() * 6.0) / 100.0);
    }
    m_pricesAsOf = QDateTime::currentDateTime();
    m_loaded = true;
    recomputePerformance();
}

QStringList SectorHeatmapTab::periodLabels() const
{
    QStringList out;
    for (const Period& p : periods()) out << p.label;
    return out;
}

QString SectorHeatmapTab::periodLabel() const { return periods()[static_cast<size_t>(m_periodIndex)].label; }

bool SectorHeatmapTab::setPeriod(const QString& label)
{
    const QString wanted = label.trimmed().toLower();
    static const QHash<QString, int> aliases{
        { "daily", 0 }, { "day", 0 }, { "today", 0 }, { "1d", 0 }, { "1w", 1 }, { "week", 1 }, { "1 week", 1 }, { "weekly", 1 },
        { "30d", 2 }, { "month", 2 }, { "1m", 2 }, { "30 days", 2 }, { "monthly", 2 }, { "90d", 3 }, { "quarter", 3 }, { "3m", 3 }, { "90 days", 3 },
        { "ytd", 4 }, { "year to date", 4 }, { "year", 4 },
    };
    const int index = aliases.value(wanted, -1);
    if (index < 0 || index >= static_cast<int>(periods().size())) return false;
    if (QAbstractButton* b = m_periodGroup->button(index)) b->click();
    return true;
}

bool SectorHeatmapTab::setView(const QString& view)
{
    const QString wanted = view.trimmed().toLower();
    if (wanted.startsWith("sector")) { if (QAbstractButton* b = m_viewGroup->button(1)) b->click(); return true; }
    if (wanted.startsWith("stock")) { if (QAbstractButton* b = m_viewGroup->button(0)) b->click(); return true; }
    return false;
}

QString SectorHeatmapTab::summaryText() const
{
    if (m_stocks.empty()) return QStringLiteral("Sector Heatmap: no data loaded.");
    struct Agg { double w = 0, p = 0; int n = 0; };
    std::map<QString, Agg> sectors;
    int up = 0, down = 0;
    std::vector<const Stock*> priced;
    for (const Stock& s : m_stocks) {
        if (std::isnan(s.performance)) continue;
        priced.push_back(&s);
        (s.performance >= 0 ? up : down)++;
        Agg& a = sectors[s.sector];
        a.w += s.marketCap; a.p += s.marketCap * s.performance; ++a.n;
    }
    if (priced.empty()) return QStringLiteral("Sector Heatmap (%1): prices not loaded yet.").arg(periodLabel());
    std::vector<std::pair<QString, double>> ranked;
    for (const auto& [name, a] : sectors) if (a.w > 0) ranked.emplace_back(name, a.p / a.w);
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::sort(priced.begin(), priced.end(), [](const Stock* a, const Stock* b) { return a->performance > b->performance; });
    QString out;
    QTextStream s(&out);
    s << "Sector Heatmap, " << periodLabel() << " performance, " << priced.size() << " stocks priced (" << up << " up, " << down << " down)";
    if (!m_focusSector.isEmpty()) s << ", currently expanded to the " << m_focusSector << " sector";
    if (m_referenceSessionDate.isValid() && periods()[static_cast<size_t>(m_periodIndex)].lookbackDays != 0) s << " versus the " << m_referenceSessionDate.toString(Qt::ISODate) << " close";
    s << ".\nSectors (cap-weighted): ";
    QStringList parts;
    for (const auto& [name, perf] : ranked) parts << QStringLiteral("%1 %2").arg(name, signedPct(perf));
    s << parts.join(", ") << ".\nTop movers: ";
    parts.clear();
    for (size_t i = 0; i < std::min<size_t>(5, priced.size()); ++i) parts << QStringLiteral("%1 %2").arg(priced[i]->ticker, signedPct(priced[i]->performance));
    s << parts.join(", ") << ". Laggards: ";
    parts.clear();
    for (size_t i = priced.size(); i-- > 0 && parts.size() < 5;) parts << QStringLiteral("%1 %2").arg(priced[i]->ticker, signedPct(priced[i]->performance));
    s << parts.join(", ") << ".";
    return out;
}

QString SectorHeatmapTab::resultsCsv() const
{
    QString out = "sector,ticker,name,market_cap_bn,last,performance_pct\n";
    for (const Stock& s : m_stocks) {
        out += QStringLiteral("%1,%2,\"%3\",%4,%5,%6\n").arg(s.sector, s.ticker, QString(s.name).replace('"', '\''), QString::number(s.marketCap / 1e9, 'f', 1),
                                                           s.last > 0 ? QString::number(s.last, 'f', 2) : QString(), std::isnan(s.performance) ? QString() : QString::number(s.performance, 'f', 2));
    }
    return out;
}

void SectorHeatmapTab::updateStatus(const QString& text, ui::StatusKind kind)
{
    ui::setStatus(m_status, text, kind);
}
