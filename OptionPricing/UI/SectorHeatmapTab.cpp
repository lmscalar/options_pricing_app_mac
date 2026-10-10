//
//  SectorHeatmapTab.cpp
//  OptionPricing
//

#include "SectorHeatmapTab.h"
#include "Formatting.h"

#include <QtCore/QSet>
#include <QtCore/QStandardPaths>

#include <algorithm>
#include <cmath>

namespace {

constexpr const char* kPeriodKey = "sectorHeatmap/period";
constexpr const char* kViewKey = "sectorHeatmap/view";
constexpr const char* kUniverseKey = "sectorHeatmap/universe";   ///< "builtin", "etfs", "futures" or "watchlist"
constexpr const char* kSizingKey = "sectorHeatmap/sizing";       ///< 0 market cap, 1 $ volume, 2 equal
constexpr int kCapCacheDays = 14;

/// Curated ETF universe grouped by asset class (sized by dollar volume: ETFs carry no market cap).
struct EtfEntry { const char* ticker; const char* name; const char* group; };
const std::vector<EtfEntry>& etfUniverse()
{
    static const std::vector<EtfEntry> list = {
        { "SPY", "S&P 500", "US equity" }, { "QQQ", "Nasdaq-100", "US equity" }, { "DIA", "Dow Jones Industrial", "US equity" },
        { "IWM", "Russell 2000", "US equity" }, { "MDY", "S&P MidCap 400", "US equity" }, { "RSP", "S&P 500 Equal Weight", "US equity" },
        { "XLK", "Technology", "US sectors" }, { "XLF", "Financials", "US sectors" }, { "XLE", "Energy", "US sectors" }, { "XLV", "Health Care", "US sectors" },
        { "XLY", "Consumer Discretionary", "US sectors" }, { "XLP", "Consumer Staples", "US sectors" }, { "XLI", "Industrials", "US sectors" },
        { "XLB", "Materials", "US sectors" }, { "XLU", "Utilities", "US sectors" }, { "XLRE", "Real Estate", "US sectors" }, { "XLC", "Communication Services", "US sectors" },
        { "SMH", "Semiconductors", "US sectors" }, { "XBI", "Biotech", "US sectors" }, { "KRE", "Regional Banks", "US sectors" }, { "XHB", "Homebuilders", "US sectors" },
        { "EFA", "EAFE developed", "International" }, { "EEM", "Emerging markets", "International" }, { "FXI", "China large caps", "International" },
        { "EWJ", "Japan", "International" }, { "EWZ", "Brazil", "International" }, { "INDA", "India", "International" }, { "EWG", "Germany", "International" }, { "EWY", "South Korea", "International" },
        { "TLT", "20+ Year Treasuries", "Bonds" }, { "IEF", "7-10 Year Treasuries", "Bonds" }, { "SHY", "1-3 Year Treasuries", "Bonds" }, { "LQD", "Investment-grade corporates", "Bonds" },
        { "HYG", "High-yield corporates", "Bonds" }, { "AGG", "US aggregate bonds", "Bonds" }, { "TIP", "TIPS", "Bonds" }, { "EMB", "Emerging-market bonds", "Bonds" },
        { "GLD", "Gold", "Commodities" }, { "SLV", "Silver", "Commodities" }, { "USO", "Crude oil", "Commodities" }, { "UNG", "Natural gas", "Commodities" },
        { "DBC", "Broad commodities", "Commodities" }, { "GDX", "Gold miners", "Commodities" }, { "CPER", "Copper", "Commodities" }, { "DBA", "Agriculture", "Commodities" },
        { "IBIT", "Bitcoin", "Crypto & thematic" }, { "ETHA", "Ether", "Crypto & thematic" }, { "ARKK", "ARK Innovation", "Crypto & thematic" },
        { "VXX", "Short-term VIX futures", "Crypto & thematic" }, { "UUP", "US Dollar", "Crypto & thematic" },
    };
    return list;
}

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
        // ETF asset classes and futures product groups
        { "US equity", "#3b82f6" }, { "US sectors", "#7c3aed" }, { "International", "#14b8a6" }, { "Bonds", "#84cc16" },
        { "Commodities", "#f59e0b" }, { "Crypto & thematic", "#ec4899" }, { "Equity index", "#3b82f6" }, { "Metals", "#d97706" },
        { "Rates", "#84cc16" }, { "Grains", "#65a30d" }, { "Meats", "#f43f5e" }, { "FX", "#06b6d4" }, { "Crypto", "#ec4899" },
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
    for (const char* name : { "Stocks", "Sectors", "Rotation" }) {
        auto* button = new QToolButton(this);
        button->setText(name);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(index == 0 ? "Every instrument as a tile inside its group" : index == 1 ? "One weighted tile per group"
                                      : "Each group as a point: relative strength over the period (x) against one-week momentum (y), versus the universe");
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
    m_universe->addItem(QStringLiteral("ETFs (%1)").arg(etfUniverse().size()), "etfs");
    m_universe->addItem("Futures", "futures");
    m_universe->addItem("Watchlist", "watchlist");
    m_universe->setToolTip("What to show: the built-in large caps by sector, ETFs by asset class, front-month futures by product group (real-time), or the active watchlist by industry");
    m_sizingBox = new QComboBox(this);
    m_sizingBox->addItem("Size: market cap", 0);
    m_sizingBox->addItem("Size: $ volume", 1);
    m_sizingBox->addItem("Size: equal", 2);
    m_sizingBox->setToolTip("Tile area: market capitalisation (ETFs and futures fall back to dollars traded today), dollars traded today, or equal tiles");
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
    controls->addWidget(m_sizingBox);
    controls->addWidget(m_summary, 1);
    controls->addWidget(m_reset);
    controls->addWidget(m_refresh);

    m_view = new TreemapView(this);
    // Rotation view: a scatter of the groups, drawn by buildRotation().
    m_rotation = new QChart;
    m_rotation->legend()->setAlignment(Qt::AlignRight);
    m_rotationChart = ui::makeChartView(this, m_rotation, 300);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_view);
    m_stack->addWidget(m_rotationChart);
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* pane = new QFrame(this);
    pane->setObjectName("pane");
    auto* paneLayout = new QVBoxLayout(pane);
    paneLayout->setContentsMargins(12, 10, 12, 10);
    paneLayout->setSpacing(8);
    paneLayout->addLayout(controls);
    paneLayout->addWidget(m_stack, 1);
    paneLayout->addWidget(m_status);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 10);
    root->addWidget(pane);

    const QSettings settings;
    m_periodIndex = std::clamp(settings.value(kPeriodKey, 0).toInt(), 0, static_cast<int>(periods().size()) - 1);
    const QString view = settings.value(kViewKey, "stocks").toString();
    m_sectorsView = view == "sectors";
    m_rotationView = view == "rotation";
    const QString universeName = settings.value(kUniverseKey, "builtin").toString();
    m_universeKind = universeName == "watchlist" ? Universe::Watchlist : universeName == "etfs" ? Universe::Etfs : universeName == "futures" ? Universe::Futures : Universe::LargeCaps;
    m_watchlistUniverse = m_universeKind == Universe::Watchlist;
    m_sizing = std::clamp(settings.value(kSizingKey, 0).toInt(), 0, 2);
    if (QAbstractButton* b = m_periodGroup->button(m_periodIndex)) b->setChecked(true);
    if (QAbstractButton* b = m_viewGroup->button(m_rotationView ? 2 : (m_sectorsView ? 1 : 0))) b->setChecked(true);
    m_universe->setCurrentIndex(std::max(0, m_universe->findData(universeName)));
    m_sizingBox->setCurrentIndex(m_sizing);
    m_view->setScale(periods()[static_cast<size_t>(m_periodIndex)].colorScale);
    m_view->setEmptyText("Press Refresh to load sector performance.");
}

void SectorHeatmapTab::wire()
{
    connect(m_viewGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_sectorsView = id == 1;
        m_rotationView = id == 2;
        QSettings().setValue(kViewKey, m_rotationView ? "rotation" : (m_sectorsView ? "sectors" : "stocks"));
        if (m_rotationView && m_loaded) ensureShortReference();
        relayout();
    });
    connect(m_sizingBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_sizing = std::clamp(index, 0, 2);
        QSettings().setValue(kSizingKey, m_sizing);
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
        const QString key = m_universe->currentData().toString();
        m_universeKind = key == "watchlist" ? Universe::Watchlist : key == "etfs" ? Universe::Etfs : key == "futures" ? Universe::Futures : Universe::LargeCaps;
        m_watchlistUniverse = m_universeKind == Universe::Watchlist;
        QSettings().setValue(kUniverseKey, key);
        m_focusSector.clear();
        m_reset->setVisible(false);
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
    } else if (m_universeKind == Universe::Etfs) {
        for (const EtfEntry& e : etfUniverse()) {
            Stock s;
            s.ticker = e.ticker;
            s.name = e.name;
            s.sector = e.group;
            m_stocks.push_back(s);
        }
    } else if (m_universeKind == Universe::Futures) {
        // Front months of the standard-size contracts (micros duplicate their parent products).
        static const QSet<QString> micros = { "MES", "MNQ", "MCL", "MGC", "MBT", "MET" };
        for (const MarketDataClient::FuturesProduct& p : MarketDataClient::knownFuturesProducts()) {
            if (micros.contains(p.code)) continue;
            Stock s;
            s.ticker = QStringLiteral("/%1").arg(p.code);
            s.name = p.name;
            s.sector = p.category;
            s.multiplier = p.multiplier;
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

QString SectorHeatmapTab::universeTitle() const
{
    switch (m_universeKind) {
    case Universe::Etfs: return QStringLiteral("ETFs");
    case Universe::Futures: return QStringLiteral("Futures");
    case Universe::Watchlist: return QStringLiteral("Watchlist");
    case Universe::LargeCaps: break;
    }
    return QStringLiteral("Large caps");
}

bool SectorHeatmapTab::setUniverse(const QString& name)
{
    const QString wanted = name.trimmed().toLower();
    QString key;
    if (wanted.startsWith("etf")) key = "etfs";
    else if (wanted.startsWith("future")) key = "futures";
    else if (wanted.startsWith("watch")) key = "watchlist";
    else if (wanted.contains("cap") || wanted.contains("stock") || wanted.contains("built")) key = "builtin";
    if (key.isEmpty()) return false;
    const int index = m_universe->findData(key);
    if (index < 0) return false;
    if (index != m_universe->currentIndex()) m_universe->setCurrentIndex(index);   // the handler rebuilds and refreshes
    return true;
}

bool SectorHeatmapTab::setSizing(const QString& name)
{
    const QString wanted = name.trimmed().toLower();
    int index = -1;
    if (wanted.contains("cap")) index = 0;
    else if (wanted.contains("vol") || wanted.contains("dollar") || wanted.contains("$")) index = 1;
    else if (wanted.contains("equal") || wanted.contains("same")) index = 2;
    if (index < 0) return false;
    m_sizingBox->setCurrentIndex(index);
    return true;
}

QString SectorHeatmapTab::sizingLabel() const
{
    if (m_sizing == 1) return QStringLiteral("dollars traded today");
    if (m_sizing == 2 || m_universeKind == Universe::Futures) return QStringLiteral("equal tiles");
    return m_universeKind == Universe::Etfs ? QStringLiteral("dollars traded today") : QStringLiteral("market cap");
}

double SectorHeatmapTab::weightFor(const Stock& s) const
{
    const double dollarVolume = s.last * s.dayVolume * s.multiplier;
    if (m_sizing == 2) return 1.0;
    if (m_sizing == 1) return std::max(dollarVolume, 1.0);
    if (s.marketCap > 0.0) return s.marketCap;
    // No market cap: futures get equal tiles (notional traded would let the Treasuries and
    // index contracts swamp every other product); ETFs size by what traded today.
    if (m_universeKind == Universe::Futures) return 1.0;
    return std::max(dollarVolume, 1e8);
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
                    s.dayVolume = q.dayVolume;
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

void SectorHeatmapTab::ensureShortReference()
{
    // The rotation view's momentum axis uses the one-week window; load its reference closes
    // once (the period's own closes come through ensureReferenceCloses).
    static const QString label = QStringLiteral("1W");
    if (m_referenceSession.count(label) || m_stocks.empty()) { applyReferenceCloses(); return; }
    if (m_shortReferencePending) return;
    m_shortReferencePending = true;
    const QDate target = QDate::currentDate().addDays(-7);
    if (m_universeKind == Universe::Futures) {
        // Grouped daily closes cover stocks and ETFs only; futures take per-contract bars.
        auto closes = std::make_shared<std::map<QString, double>>();
        auto pending = std::make_shared<int>(static_cast<int>(m_stocks.size()));
        for (const Stock& s : m_stocks) {
            const QString ticker = s.ticker;
            auto done = [this, closes, pending, target] {
                if (--*pending > 0) return;
                m_referenceCloses[target.toString(Qt::ISODate) + "#1W"] = *closes;
                m_referenceSession[label] = target;
                m_shortReferencePending = false;
                applyReferenceCloses();
            };
            m_client.fetchAggregates(ticker, 1, "day", target.addDays(-10), target, [ticker, closes, done](const MarketDataClient::BarSeries& series) {
                if (!series.bars.empty()) (*closes)[ticker] = series.bars.back().close;
                done();
            }, [done](const QString&) { done(); });
        }
        return;
    }
    auto attempt = std::make_shared<std::function<void(QDate, int)>>();
    *attempt = [this, attempt](QDate date, int triesLeft) {
        m_client.fetchGroupedDaily(date, [this, date, triesLeft, attempt](const std::map<QString, double>& closes) {
            if (closes.empty() && triesLeft > 0) { (*attempt)(date.addDays(-1), triesLeft - 1); return; }
            m_referenceCloses[date.toString(Qt::ISODate)] = closes;
            m_referenceSession[label] = date;
            m_shortReferencePending = false;
            applyReferenceCloses();
        }, [this](const QString& message) {
            qWarning("[sector-heatmap] 1W reference failed: %s", qPrintable(message));
            m_shortReferencePending = false;
            applyReferenceCloses();
        });
    };
    (*attempt)(target, 6);
}

void SectorHeatmapTab::ensureReferenceCloses()
{
    const Period& period = periods()[static_cast<size_t>(m_periodIndex)];
    if (m_rotationView && !m_referenceSession.count(QStringLiteral("1W"))) ensureShortReference();
    if (period.lookbackDays == 0) { recomputePerformance(); return; }
    const QString label = period.label;
    if (m_referenceSession.count(label)) { applyReferenceCloses(); return; }
    if (m_universeKind == Universe::Futures) { fetchReferenceFallback(referenceDate()); return; }   // no grouped closes for futures
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
    // One-week closes for the rotation view first (futures keep theirs under a "#1W" key);
    // they matter even when the period itself needs no reference (Daily).
    const auto shortSession = m_referenceSession.find(QStringLiteral("1W"));
    if (shortSession != m_referenceSession.end()) {
        auto shortCloses = m_referenceCloses.find(shortSession->second.toString(Qt::ISODate) + "#1W");
        if (shortCloses == m_referenceCloses.end()) shortCloses = m_referenceCloses.find(shortSession->second.toString(Qt::ISODate));
        for (Stock& s : m_stocks) {
            s.shortReferenceClose = 0.0;
            if (shortCloses == m_referenceCloses.end()) continue;
            const auto it = shortCloses->second.find(s.ticker);
            if (it != shortCloses->second.end()) s.shortReferenceClose = it->second;
        }
    }
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
        // One-week performance for the rotation view (its axes are chosen in rotationAxes()).
        s.shortPerformance = (s.last > 0.0 && s.shortReferenceClose > 0.0) ? (s.last / s.shortReferenceClose - 1.0) * 100.0 : std::numeric_limits<double>::quiet_NaN();
        if (!std::isnan(s.performance)) ++priced;
    }
    const char* noun = m_universeKind == Universe::Futures ? "contracts" : m_universeKind == Universe::Etfs ? "ETFs" : "stocks";
    QString status = QStringLiteral("%1 of %2 %3 priced").arg(priced).arg(m_stocks.size()).arg(noun);
    if (m_pricesAsOf.isValid()) status += QStringLiteral(" · last prices %1").arg(m_pricesAsOf.toLocalTime().toString("yyyy-MM-dd HH:mm"));
    if (period.lookbackDays != 0 && m_referenceSessionDate.isValid()) status += QStringLiteral(" · versus the %1 close").arg(m_referenceSessionDate.toString(Qt::ISODate));
    status += m_universeKind == Universe::Futures ? " · real-time futures data from Massive.com" : " · 15-minute delayed data from Massive.com";
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
        t.weight = weightFor(s);
        t.performance = s.performance;
        const QString sizeLine = s.marketCap > 0.0 ? QStringLiteral("Market cap %1").arg(formatCap(s.marketCap))
                                                   : QStringLiteral("Traded today %1").arg(formatCap(s.last * s.dayVolume * s.multiplier));
        t.tooltip = QStringLiteral("<b>%1</b> · %2<br>%3<br>%4<br>Last %5 · %6: %7")
                        .arg(s.ticker, s.name, s.sector, sizeLine, s.last > 0 ? ui::number(s.last, 2) : QStringLiteral("–"),
                             periods()[static_cast<size_t>(m_periodIndex)].label, signedPct(s.performance));
        if (!std::isnan(s.shortPerformance) && periods()[static_cast<size_t>(m_periodIndex)].lookbackDays > 7) t.tooltip += QStringLiteral(" · 1W: %1").arg(signedPct(s.shortPerformance));
        g.weight += t.weight;
        g.tiles.push_back(t);
    }
    const char* noun = m_universeKind == Universe::Futures ? "contracts" : m_universeKind == Universe::Etfs ? "ETFs" : "stocks";
    const char* groupNoun = m_universeKind == Universe::LargeCaps || m_universeKind == Universe::Watchlist ? "sectors" : "groups";
    int upAll = 0, downAll = 0;
    std::vector<TreemapView::Group> list;
    for (const QString& name : order) {
        TreemapView::Group g = groups[name];
        // Weighted group performance over the priced members, and breadth (advancers / decliners).
        double wsum = 0.0, psum = 0.0;
        int up = 0, down = 0;
        for (const TreemapView::Tile& t : g.tiles) {
            if (std::isnan(t.performance)) continue;
            wsum += t.weight; psum += t.weight * t.performance;
            (t.performance >= 0 ? up : down)++;
        }
        upAll += up; downAll += down;
        g.performance = wsum > 0.0 ? psum / wsum : std::numeric_limits<double>::quiet_NaN();
        const QString breadth = up + down ? QStringLiteral("%1▲ %2▼").arg(up).arg(down) : QString();
        if (sectorsOnly) {
            TreemapView::Tile sectorTile;
            sectorTile.title = g.name;
            sectorTile.subtitle = QStringLiteral("%1 %2 · %3%4").arg(g.tiles.size()).arg(noun, breadth, m_sizing == 2 ? QString() : QStringLiteral(" · %1").arg(formatCap(g.weight)));
            sectorTile.weight = g.weight;
            sectorTile.performance = g.performance;
            sectorTile.tooltip = QStringLiteral("<b>%1</b><br>%2 %3 · breadth %4<br>%5: %6 (weighted by %7)")
                                     .arg(g.name).arg(g.tiles.size()).arg(noun, breadth, periods()[static_cast<size_t>(m_periodIndex)].label, signedPct(g.performance), sizingLabel());
            g.tiles.assign(1, sectorTile);
        } else {
            std::sort(g.tiles.begin(), g.tiles.end(), [](const TreemapView::Tile& a, const TreemapView::Tile& b) { return a.weight > b.weight; });
        }
        list.push_back(g);
    }
    std::sort(list.begin(), list.end(), [](const TreemapView::Group& a, const TreemapView::Group& b) { return a.weight > b.weight; });
    const size_t shown = list.empty() ? 0 : (focused ? list.front().tiles.size() : m_stocks.size());
    const QString breadthAll = upAll + downAll ? QStringLiteral(" · breadth %1▲ %2▼ (%3% up)").arg(upAll).arg(downAll).arg(qRound(100.0 * upAll / (upAll + downAll))) : QString();
    const QString sizeNote = QStringLiteral("Tile size is %1 and colour is the period change.").arg(sizingLabel());
    m_view->setGroups(std::move(list), sectorsOnly);
    if (m_rotationView && !focused) {
        if (m_loaded && !m_referenceSession.count(QStringLiteral("1W"))) ensureShortReference();   // momentum axis needs the week-ago closes
        buildRotation();
        m_stack->setCurrentWidget(m_rotationChart);
        const int lookback = periods()[static_cast<size_t>(m_periodIndex)].lookbackDays;
        const QString longLabel = lookback == 0 ? QStringLiteral("1W") : QString::fromLatin1(periods()[static_cast<size_t>(m_periodIndex)].label);
        const QString shortLabel = lookback == 0 || lookback == 7 ? QStringLiteral("Daily") : QStringLiteral("1W");
        m_summary->setText(QStringLiteral("%1 · %2 %3 · rotation: %4 relative strength vs %5 momentum%6").arg(universeTitle()).arg(order.size()).arg(groupNoun, longLabel, shortLabel, breadthAll));
        m_summary->setToolTip("Each point is a group. x: its period performance minus the universe's (relative strength); y: the same over the last week (momentum). "
                              "Leading = top right, Improving = top left, Weakening = bottom right, Lagging = bottom left. Click a point to expand that group.");
    } else {
        m_stack->setCurrentWidget(m_view);
        if (focused) {
            m_summary->setText(QStringLiteral("%1 · %2 %3 · %4 change%5").arg(m_focusSector).arg(shown).arg(noun, periods()[static_cast<size_t>(m_periodIndex)].label, breadthAll));
            m_summary->setToolTip(sizeNote + " Click a tile to load it; Reset returns to all groups.");
        } else {
            m_summary->setText(QStringLiteral("%1 · %2 %3 · %4 %5 · %6 change%7").arg(universeTitle()).arg(m_stocks.size()).arg(noun).arg(order.size()).arg(groupNoun, periods()[static_cast<size_t>(m_periodIndex)].label, breadthAll));
            m_summary->setToolTip(m_sectorsView ? sizeNote + " Click a group to expand it." : sizeNote);
        }
    }
}

namespace {
/// The rotation view's two windows for a stock: the long one (relative strength) and the
/// short one (momentum). Daily period: 1W against the day; 1W period: the week against the
/// day; longer periods: the period against 1W. Returns false when a figure is missing.
bool rotationAxes(const SectorHeatmapTab::Stock& s, int lookbackDays, double* longPct, double* shortPct)
{
    const double daily = (s.last > 0.0 && s.previousClose > 0.0) ? (s.last / s.previousClose - 1.0) * 100.0 : std::numeric_limits<double>::quiet_NaN();
    const double week = s.shortPerformance;
    if (lookbackDays == 0) { *longPct = week; *shortPct = daily; }
    else if (lookbackDays == 7) { *longPct = s.performance; *shortPct = daily; }
    else { *longPct = s.performance; *shortPct = week; }
    return !std::isnan(*longPct) && !std::isnan(*shortPct);
}
QString rotationLongLabel(int lookbackDays, const char* periodLabel) { return lookbackDays == 0 ? QStringLiteral("1W") : QString::fromLatin1(periodLabel); }
QString rotationShortLabel(int lookbackDays) { return lookbackDays == 0 || lookbackDays == 7 ? QStringLiteral("Daily") : QStringLiteral("1W"); }
} // namespace

void SectorHeatmapTab::buildRotation()
{
    // Weighted performance per group over the long window (x) and the short window (y), each
    // relative to the weighted universe, so the chart reads as a relative-rotation map.
    struct Agg { double w = 0, p = 0, ps = 0; int n = 0; };
    std::map<QString, Agg> groups;
    Agg all;
    std::vector<QString> order;
    const Period& period = periods()[static_cast<size_t>(m_periodIndex)];
    for (const Stock& s : m_stocks) {
        double longPct = 0.0, shortPct = 0.0;
        if (!rotationAxes(s, period.lookbackDays, &longPct, &shortPct)) continue;
        const double w = weightFor(s);
        if (!groups.count(s.sector)) order.push_back(s.sector);
        Agg& a = groups[s.sector];
        a.w += w; a.p += w * longPct; a.ps += w * shortPct; ++a.n;
        all.w += w; all.p += w * longPct; all.ps += w * shortPct; ++all.n;
    }
    m_rotation->removeAllSeries();
    for (QAbstractAxis* axis : m_rotation->axes()) m_rotation->removeAxis(axis);
    const QString longLabel = rotationLongLabel(period.lookbackDays, period.label);
    const QString shortLabel = rotationShortLabel(period.lookbackDays);
    const QColor text(m_theme.text.isEmpty() ? "#c7d2e3" : m_theme.text);
    m_rotation->setBackgroundBrush(QColor(m_theme.surface.isEmpty() ? "#121a2b" : m_theme.surface));
    m_rotation->setTitleBrush(text);
    m_rotation->legend()->setLabelColor(text);
    m_rotation->setTitle(all.w > 0.0
                             ? QStringLiteral("%1 rotation · %2 relative strength vs %3 momentum (weighted by %4)").arg(universeTitle(), longLabel, shortLabel, sizingLabel())
                             : QStringLiteral("%1 rotation · loading the week-ago closes…").arg(universeTitle()));
    if (all.w <= 0.0) return;
    const double benchLong = all.p / all.w, benchShort = all.ps / all.w;
    auto* x = new QValueAxis;
    x->setTitleText(QStringLiteral("Relative strength, %1 (pct points vs universe)").arg(longLabel));
    auto* y = new QValueAxis;
    y->setTitleText(QStringLiteral("Momentum, %1 (pct points vs universe)").arg(shortLabel));
    m_rotation->addAxis(x, Qt::AlignBottom);
    m_rotation->addAxis(y, Qt::AlignLeft);
    double maxX = 0.5, maxY = 0.5;
    for (const QString& name : order) {
        const Agg& a = groups[name];
        const double rx = a.p / a.w - benchLong, ry = a.ps / a.w - benchShort;
        maxX = std::max(maxX, std::fabs(rx)); maxY = std::max(maxY, std::fabs(ry));
        auto* series = new QScatterSeries;
        series->setName(QStringLiteral("%1 (%2, %3)").arg(name, signedPct(rx), signedPct(ry)));
        series->setMarkerSize(16.0);
        series->setColor(sectorColor(name, m_theme));
        series->setBorderColor(QColor(m_theme.border.isEmpty() ? "#273449" : m_theme.border));
        series->append(rx, ry);
        m_rotation->addSeries(series);
        series->attachAxis(x);
        series->attachAxis(y);
        const QString quadrant = rx >= 0 ? (ry >= 0 ? "Leading" : "Weakening") : (ry >= 0 ? "Improving" : "Lagging");
        const QString tip = QStringLiteral("%1 · %2\n%3 relative %4 · %5 momentum %6\n%7 members · click to expand").arg(name, quadrant, longLabel, signedPct(rx), shortLabel, signedPct(ry)).arg(a.n);
        connect(series, &QScatterSeries::hovered, this, [tip](const QPointF&, bool state) { if (state) QToolTip::showText(QCursor::pos(), tip); else QToolTip::hideText(); });
        connect(series, &QScatterSeries::clicked, this, [this, name](const QPointF&) { focusSector(name); if (QAbstractButton* b = m_viewGroup->button(0)) b->click(); });
    }
    // Quadrant axes through the universe (0, 0).
    const double rangeX = std::ceil(maxX * 1.25 * 2.0) / 2.0, rangeY = std::ceil(maxY * 1.25 * 2.0) / 2.0;
    x->setRange(-rangeX, rangeX);
    y->setRange(-rangeY, rangeY);
    for (int axis = 0; axis < 2; ++axis) {
        auto* zero = new QLineSeries;
        if (axis == 0) { zero->append(-rangeX, 0.0); zero->append(rangeX, 0.0); }
        else { zero->append(0.0, -rangeY); zero->append(0.0, rangeY); }
        QPen pen(QColor(m_theme.textMuted.isEmpty() ? "#8294ad" : m_theme.textMuted));
        pen.setStyle(Qt::DashLine);
        zero->setPen(pen);
        m_rotation->addSeries(zero);
        zero->attachAxis(x);
        zero->attachAxis(y);
        if (QLegendMarker* marker = m_rotation->legend()->markers(zero).value(0)) marker->setVisible(false);
    }
    // Axis colours
    for (QValueAxis* axis : { x, y }) {
        axis->setLabelsColor(text);
        axis->setTitleBrush(text);
        axis->setGridLineColor(QColor(m_theme.gridLine.isEmpty() ? "#1f2a3f" : m_theme.gridLine));
        axis->setLabelFormat("%+.1f");
    }
}

QString SectorHeatmapTab::rotationSummary() const
{
    struct Agg { double w = 0, p = 0, ps = 0; };
    std::map<QString, Agg> groups;
    Agg all;
    const Period& period = periods()[static_cast<size_t>(m_periodIndex)];
    for (const Stock& s : m_stocks) {
        double longPct = 0.0, shortPct = 0.0;
        if (!rotationAxes(s, period.lookbackDays, &longPct, &shortPct)) continue;
        const double w = weightFor(s);
        Agg& a = groups[s.sector];
        a.w += w; a.p += w * longPct; a.ps += w * shortPct;
        all.w += w; all.p += w * longPct; all.ps += w * shortPct;
    }
    if (all.w <= 0.0) return QStringLiteral("Rotation: no priced groups yet (the one-week reference closes may still be loading).");
    QStringList parts;
    for (const auto& [name, a] : groups) {
        const double rx = a.p / a.w - all.p / all.w, ry = a.ps / a.w - all.ps / all.w;
        const QString quadrant = rx >= 0 ? (ry >= 0 ? "leading" : "weakening") : (ry >= 0 ? "improving" : "lagging");
        parts << QStringLiteral("%1 %2 (RS %3, momentum %4)").arg(name, quadrant, signedPct(rx), signedPct(ry));
    }
    return QStringLiteral("Rotation versus the %1 universe (%2 relative strength, %3 momentum): %4.")
        .arg(universeTitle(), rotationLongLabel(period.lookbackDays, period.label), rotationShortLabel(period.lookbackDays), parts.join("; "));
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
    if (m_universeKind == Universe::Futures) return;   // contracts have no market cap; tiles size by dollars traded
    const QDateTime stale = QDateTime::currentDateTime().addDays(-kCapCacheDays);
    for (const Stock& s : m_stocks) {
        if (MarketDataClient::isFutures(s.ticker)) continue;
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
    m_universeKind = Universe::LargeCaps;
    {
        const QSignalBlocker blocker(m_universe);
        m_universe->setCurrentIndex(0);
    }
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
        s.shortReferenceClose = 100.0 * (1.0 - (drift[s.sector] * 1.5 + rnd() * 3.0) / 100.0);
        s.dayVolume = 1e6 * (1.5 + rnd());
    }
    m_referenceSession[QStringLiteral("1W")] = QDate::currentDate().addDays(-7);
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
    if (wanted.startsWith("sector") || wanted.startsWith("group")) { if (QAbstractButton* b = m_viewGroup->button(1)) b->click(); return true; }
    if (wanted.startsWith("stock") || wanted.startsWith("tile")) { if (QAbstractButton* b = m_viewGroup->button(0)) b->click(); return true; }
    if (wanted.startsWith("rotation") || wanted.startsWith("rrg")) { if (QAbstractButton* b = m_viewGroup->button(2)) b->click(); return true; }
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
        const double w = weightFor(s);
        a.w += w; a.p += w * s.performance; ++a.n;
    }
    if (priced.empty()) return QStringLiteral("Sector Heatmap (%1): prices not loaded yet.").arg(periodLabel());
    std::vector<std::pair<QString, double>> ranked;
    for (const auto& [name, a] : sectors) if (a.w > 0) ranked.emplace_back(name, a.p / a.w);
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::sort(priced.begin(), priced.end(), [](const Stock* a, const Stock* b) { return a->performance > b->performance; });
    QString out;
    QTextStream s(&out);
    s << "Sector Heatmap (" << universeTitle() << " universe, tiles sized by " << sizingLabel() << "), " << periodLabel() << " performance, "
      << priced.size() << " instruments priced (breadth " << up << " up, " << down << " down, " << qRound(100.0 * up / std::max<size_t>(1, priced.size())) << "% advancing)";
    if (!m_focusSector.isEmpty()) s << ", currently expanded to " << m_focusSector;
    if (m_rotationView) s << ", rotation view";
    if (m_referenceSessionDate.isValid() && periods()[static_cast<size_t>(m_periodIndex)].lookbackDays != 0) s << " versus the " << m_referenceSessionDate.toString(Qt::ISODate) << " close";
    s << ".\nGroups (weighted): ";
    QStringList parts;
    for (const auto& [name, perf] : ranked) parts << QStringLiteral("%1 %2").arg(name, signedPct(perf));
    s << parts.join(", ") << ".\nTop movers: ";
    parts.clear();
    for (size_t i = 0; i < std::min<size_t>(5, priced.size()); ++i) parts << QStringLiteral("%1 %2").arg(priced[i]->ticker, signedPct(priced[i]->performance));
    s << parts.join(", ") << ". Laggards: ";
    parts.clear();
    for (size_t i = priced.size(); i-- > 0 && parts.size() < 5;) parts << QStringLiteral("%1 %2").arg(priced[i]->ticker, signedPct(priced[i]->performance));
    s << parts.join(", ") << ".";
    if (m_rotationView) s << "\n" << rotationSummary();
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
