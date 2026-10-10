//
//  MainWindowAssistant.cpp
//  OptionPricing
//
//  The assistant's integration with the application: the dock, the screen context sent
//  with each request, the tools the model may call (navigation, charting, drawings, data
//  lookups) and a local parser for simple spoken commands that works without an AI key.
//

#include "MainWindow.h"
#include "AssistantPanel.h"
#include "ChainTab.h"
#include "Formatting.h"
#include "HeatmapTab.h"
#include "SectorHeatmapTab.h"
#include "ScannerTab.h"
#include "PortfolioTab.h"
#include "AlertsTab.h"
#include "PricerTab.h"
#include "QuotesTab.h"
#include "ScenarioTab.h"
#include "StrategyTab.h"
#include "VolatilityTab.h"
#include "../Pricing/TechnicalAnalysis.h"

#include <QtWidgets/QDockWidget>

namespace {

QJsonObject prop(const QString& type, const QString& description, const QJsonArray& enumValues = {})
{
    QJsonObject p{ { "type", type }, { "description", description } };
    if (!enumValues.isEmpty()) p["enum"] = enumValues;
    return p;
}

QJsonObject schema(const QJsonObject& properties, const QStringList& required = {})
{
    QJsonObject s{ { "type", "object" }, { "properties", properties } };
    if (!required.isEmpty()) s["required"] = QJsonArray::fromStringList(required);
    return s;
}

QString clip(const QString& text, int maxChars)
{
    return text.size() <= maxChars ? text : text.left(maxChars) + QStringLiteral("\n…(truncated, %1 more characters)").arg(text.size() - maxChars);
}

/// Parses "2026-10-07", "2026-10-07 14:30", "2026-10-07T14:30" or epoch seconds into epoch seconds (UTC for dates).
qint64 epochSeconds(const QJsonValue& value, bool& ok)
{
    ok = true;
    if (value.isDouble()) return static_cast<qint64>(value.toDouble());
    const QString text = value.toString().trimmed();
    QDateTime dt = QDateTime::fromString(text, Qt::ISODate);
    if (!dt.isValid()) dt = QDateTime::fromString(text, "yyyy-MM-dd HH:mm");
    if (!dt.isValid()) {
        const QDate date = QDate::fromString(text, Qt::ISODate);
        if (date.isValid()) return QDateTime(date, QTime(0, 0), QTimeZone::UTC).toSecsSinceEpoch();
        ok = false;
        return 0;
    }
    return dt.toSecsSinceEpoch();
}

const char* kSystemPrompt =
    "You are the trading-desk assistant built into Option Pricer, a macOS options analytics workstation (option pricing, Greeks, "
    "implied-volatility surfaces, option chains from Massive.com, strategies, scenario grids, realized/GARCH volatility, and a "
    "TradingView-style price chart with drawing tools). Each user message begins with a [Screen context] block describing what the "
    "user is looking at right now (and, on the Quotes tab, an image of the chart). Ground your analysis in that context and in tool "
    "results; do not invent numbers.\n\n"
    "Tools let you act in the app: switch tabs, load a ticker everywhere (show_ticker), read bars, draw trend lines and shaded "
    "support/resistance zones on the chart, add or remove any TA-Lib technical indicator (list_indicators shows the catalogue by category), "
    "change the timeframe or chart style, read the option chain, volatility and strategy "
    "figures, set the market volatility, load a strategy preset, or set the pricer's contract. When asked to mark support and "
    "resistance: use the bars (get_bars if you need more history), identify 2-4 price areas where price repeatedly reversed or "
    "consolidated, and call draw_zone for each with a tight low-high range (typically 0.5%-1.5% of price), support below the current "
    "price and resistance above; then draw_trend_line through the dominant swing lows (uptrend) or swing highs (downtrend) using the "
    "bar dates. Clear earlier drawings first only if the user asks. After acting, report briefly what you drew and why, with prices.\n\n"
    "Format replies as a professional analyst note in Markdown (the panel renders headings, pipe tables, lists, bold and a highlighted "
    "'Bottom line'). For analysis requests use: a '# ' headline with ticker, price and change; '## Key levels' as a table with columns "
    "Level | Price | Type | Why it matters whenever price levels are involved (support, resistance, moving averages, breakeven, strikes); "
    "then short '## Technicals', '## Options and volatility' (when a chain or volatility data is loaded), '## Risks' and '## Next steps' "
    "sections using bullets; end with a one-sentence '**Bottom line:**'. Put numbers in tables rather than prose where possible, two decimals "
    "for prices, one or two for percentages, and use the data's own currency. Keep notes to 150-350 words; for simple commands reply in one "
    "line. Never mention internal tool names (show_ticker, draw_zone, …) or parameters to the user: describe actions in plain words such as "
    "'I opened the option chain' or offer them ('I can mark these zones on the chart'). "
    "Style: concise, numeric, trader-to-trader. Use percentages for volatilities. This is analysis of displayed data, not personalized "
    "investment advice; note key risks when relevant. If a request is ambiguous, make the most useful reasonable choice and say what you did.";

} // namespace

// MARK: - Dock

void MainWindow::buildAssistant()
{
    m_assistant = new AssistantPanel(this);
    m_assistantDock = new QDockWidget("Assistant", this);
    m_assistantDock->setObjectName("assistantDock");
    m_assistantDock->setWidget(m_assistant);
    m_assistantDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_assistantDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, m_assistantDock);
    resizeDocks({ m_assistantDock }, { 440 }, Qt::Horizontal);
    m_assistantDock->setVisible(QSettings().value("ai.panelVisible", true).toBool());
    connect(m_assistantDock, &QDockWidget::visibilityChanged, this, [this](bool) { updateCentralMargins(); });
    connect(m_assistantDock, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea) { updateCentralMargins(); });
    connect(m_assistantDock, &QDockWidget::topLevelChanged, this, [this](bool) { updateCentralMargins(); });
    updateCentralMargins();

    m_assistant->contextProvider = [this](std::function<void(const QString&, const QImage&)> done) { assistantContext(std::move(done)); };
    m_assistant->localCommandHandler = [this](const QString& text, QString& feedback) { return handleLocalCommand(text, feedback); };
    m_assistant->client().setSystemPrompt(QString::fromUtf8(kSystemPrompt));
    m_assistant->client().setTools(assistantTools());
    m_assistant->client().setToolExecutor([this](const QString& name, const QJsonObject& input, AssistantClient::ToolDone done) {
        executeAssistantTool(name, input, std::move(done));
    });
}

void MainWindow::showAlertBanner(const QString& ticker, const QString& message)
{
    if (!m_alertBanner) {
        m_alertBanner = new QFrame(centralWidget());
        m_alertBanner->setObjectName("alertBanner");
        m_alertBanner->setAttribute(Qt::WA_StyledBackground, true);
        m_alertBanner->setCursor(Qt::PointingHandCursor);
        auto* layout = new QHBoxLayout(m_alertBanner);
        layout->setContentsMargins(14, 10, 12, 10);
        layout->setSpacing(10);
        auto* icon = new QLabel("◆", m_alertBanner);
        icon->setObjectName("alertBannerIcon");
        m_alertBannerLabel = new QLabel(m_alertBanner);
        m_alertBannerLabel->setObjectName("alertBannerText");
        m_alertBannerLabel->setWordWrap(true);
        m_alertBannerLabel->setMaximumWidth(420);
        auto* close = new QToolButton(m_alertBanner);
        close->setText("✕");
        close->setAutoRaise(true);
        close->setToolTip("Dismiss");
        connect(close, &QToolButton::clicked, m_alertBanner, &QWidget::hide);
        layout->addWidget(icon);
        layout->addWidget(m_alertBannerLabel, 1);
        layout->addWidget(close);
        m_alertBanner->installEventFilter(this);
        m_alertBannerTimer = new QTimer(this);
        m_alertBannerTimer->setSingleShot(true);
        connect(m_alertBannerTimer, &QTimer::timeout, m_alertBanner, &QWidget::hide);
    }
    m_alertBannerTicker = ticker;
    m_alertBannerLabel->setText(QStringLiteral("<b>Alert · %1</b><br>%2<br><span style=\"opacity:0.75\">Click to open %1 everywhere</span>").arg(ticker, message.toHtmlEscaped()));
    m_alertBanner->adjustSize();
    m_alertBanner->move(centralWidget()->width() - m_alertBanner->width() - 28, 92);
    m_alertBanner->raise();
    m_alertBanner->show();
    m_alertBannerTimer->start(10000);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_alertBanner && event->type() == QEvent::MouseButtonRelease) {
        m_alertBanner->hide();
        if (!m_alertBannerTicker.isEmpty()) showTicker(m_alertBannerTicker, false);
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updateCentralMargins()
{
    if (!m_rootLayout) return;
    // 24 px of breathing room at the window edges, but only 6 px against a docked assistant:
    // together with the slim dock separator the chart and the chat then sit close together.
    const bool docked = m_assistantDock && m_assistantDock->isVisible() && !m_assistantDock->isFloating();
    const Qt::DockWidgetArea area = docked ? dockWidgetArea(m_assistantDock) : Qt::NoDockWidgetArea;
    const int left = area == Qt::LeftDockWidgetArea ? 6 : 24;
    const int right = area == Qt::RightDockWidgetArea ? 6 : 24;
    m_rootLayout->setContentsMargins(left, 16, right, 12);
}

QString MainWindow::tabNameOf(QWidget* tab) const
{
    const int index = m_tabs->indexOf(tab);
    return index >= 0 ? m_tabs->tabText(index) : QString();
}

QWidget* MainWindow::tabByName(const QString& name) const
{
    const QString wanted = name.trimmed().toLower();
    if (wanted.isEmpty()) return nullptr;
    for (int i = 0; i < m_tabs->count(); ++i) {
        const QString text = m_tabs->tabText(i).toLower();
        if (text == wanted || text.contains(wanted) || wanted.contains(text)) return m_tabs->widget(i);
    }
    if (wanted.contains("chain") || wanted.contains("option")) return m_chain;
    if (wanted.contains("vol")) return m_volatility;
    if (wanted.contains("sector") || wanted.contains("market map") || wanted.contains("treemap")) return m_sectorHeatmap;
    if (wanted.contains("idea") || wanted.contains("scan") || wanted.contains("screen")) return m_scanner;
    if (wanted.contains("portfolio") || wanted.contains("position") || wanted.contains("book") || wanted.contains("risk")) return m_portfolio;
    if (wanted.contains("alert")) return m_alerts;
    if (wanted.contains("heat")) return m_heatmap;
    if (wanted.contains("quote") || wanted.contains("chart") || wanted.contains("watch")) return m_quotes;
    if (wanted.contains("scenario") || wanted.contains("grid")) return m_scenario;
    if (wanted.contains("strat")) return m_strategy;
    if (wanted.contains("pric")) return m_pricer;
    return nullptr;
}

// MARK: - Context

QString MainWindow::chainSummary(int maxExpiries) const
{
    if (m_state.surface.empty()) return QStringLiteral("No option chain is loaded.\n");
    QString out;
    QTextStream s(&out);
    s << "Option chain for " << m_state.underlyingTicker << ": spot " << ui::number(m_state.market.spot, 2) << " (" << m_state.spotSource << "), "
      << m_state.chainQuotes.size() << " contracts, " << m_state.surface.slices().size() << " fitted expiries, downloaded "
      << m_state.chainTime.toString("yyyy-MM-dd HH:mm") << ".\n";
    s << "Per expiry (date, DTE, ATM implied vol %, forward, calls, puts, SVI fit RMSE vol pts):\n";
    int shown = 0;
    for (const pricing::ExpirySlice& slice : m_state.surface.slices()) {
        if (shown++ >= maxExpiries) { s << "…\n"; break; }
        s << QString::fromStdString(slice.expiryDate) << "," << slice.daysToExpiry << "," << ui::number(slice.atmVol() * 100.0, 2) << "," << ui::number(slice.forward, 2) << ","
          << slice.calls.size() << "," << slice.puts.size() << "," << ui::number(slice.fitRmse * 100.0, 2) << "\n";
    }
    return out;
}

void MainWindow::assistantContext(std::function<void(const QString&, const QImage&)> done)
{
    QWidget* current = m_tabs->currentWidget();
    QString text;
    QTextStream s(&text);
    s << "Active tab: " << tabNameOf(current) << ". Local time " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm") << ".\n";
    s << "Market state: ticker " << (m_state.underlyingTicker.isEmpty() ? QStringLiteral("none") : m_state.underlyingTicker);
    if (!m_state.companyName.isEmpty()) s << " (" << m_state.companyName << ")";
    s << ", spot " << ui::number(m_state.market.spot, 2) << (m_state.spotSource.isEmpty() ? QString() : " from " + m_state.spotSource)
      << ", r " << ui::number(m_state.rateFor(1.0) * 100.0, 2) << "%, q " << ui::number(m_state.market.dividendYield * 100.0, 2) << "%, sigma "
      << ui::number(m_state.market.volatility * 100.0, 2) << "%";
    if (m_state.hasDayChange()) s << ", day change " << ui::number(m_state.dayChange(), 2) << " (" << ui::number(m_state.dayChangePercent(), 2) << "%)";
    s << ".\n";
    if (!m_state.businessSummary().isEmpty()) s << "Business: " << m_state.businessSummary(400) << "\n";
    s << "Watchlist: " << m_quotes->watchlist().join(", ") << ". Option chains in memory: " << m_store.tickerCount() << " tickers.\n\n";

    if (current == m_quotes) {
        s << m_quotes->contextSummary(80);
    } else if (current == m_volatility) {
        s << "Volatility tab. " << m_volatility->summaryText() << "\n" << clip(m_volatility->resultsCsv(), 5000);
    } else if (current == m_chain) {
        s << chainSummary(30) << "\nVisible slice (CSV):\n" << clip(m_chain->resultsCsv(), 7000);
    } else if (current == m_heatmap) {
        s << chainSummary(12) << "\nHeatmap tab (CSV):\n" << clip(m_heatmap->resultsCsv(), 7000);
    } else if (current == m_alerts) {
        s << m_alerts->summaryText() << "\nAlerts (CSV):\n" << clip(m_alerts->rulesCsv(), 4000);
    } else if (current == m_portfolio) {
        s << m_portfolio->summaryText() << "\nPositions (CSV):\n" << clip(m_portfolio->resultsCsv(), 6000) << "\nRisk (CSV):\n" << clip(m_portfolio->riskCsv(), 3000);
    } else if (current == m_scanner) {
        s << m_scanner->summaryText() << "\nMarket scan (CSV):\n" << clip(m_scanner->metricsCsv(), 4000) << "\nIdeas (CSV):\n" << clip(m_scanner->resultsCsv(), 7000);
    } else if (current == m_sectorHeatmap) {
        s << m_sectorHeatmap->summaryText() << "\nSector Heatmap (CSV: sector, ticker, name, market cap bn, last, performance %):\n" << clip(m_sectorHeatmap->resultsCsv(), 9000);
    } else if (current == m_strategy) {
        s << "Strategy tab (CSV):\n" << clip(m_strategy->resultsCsv(), 7000) << "\n" << chainSummary(8);
    } else if (current == m_scenario) {
        s << "Scenarios tab (CSV):\n" << clip(m_scenario->resultsCsv(), 7000);
    } else if (current == m_pricer) {
        s << "Pricer tab (CSV):\n" << clip(m_pricer->resultsCsv(), 7000);
    }
    if (current != m_chain && current != m_heatmap && current != m_strategy && !m_state.surface.empty()) s << "\n" << chainSummary(8);

    if (current == m_quotes) {
        m_quotes->chartImage([text, done](const QImage& image) { done(text, image); });
    } else {
        done(text, QImage());
    }
}

// MARK: - Tools

std::vector<AssistantClient::Tool> MainWindow::assistantTools() const
{
    const QJsonArray tabs{ "Quotes", "Portfolio", "Pricer", "Strategy", "Scenarios", "Option Chain", "Heatmap", "Sector Heatmap", "Volatility", "Alerts" };
    const QJsonArray timeframes = QJsonArray::fromStringList(m_quotes->timeframeLabels());
    std::vector<AssistantClient::Tool> tools;
    tools.push_back({ "get_screen_context", "Returns a fresh text description of what is on screen now (same format as the [Screen context] block).", schema({}) });
    tools.push_back({ "switch_tab", "Shows one of the application's tabs.", schema({ { "tab", prop("string", "Tab to show", tabs) } }, { "tab" }) });
    tools.push_back({ "show_ticker",
                      "Makes a ticker the app-wide symbol: loads its option chain (from memory when available), chart, volatility and banner; every tab follows. "
                      "Optionally switches to the Option Chain tab.",
                      schema({ { "symbol", prop("string", "Stock or ETF ticker, e.g. AAPL") }, { "switch_to_chain", prop("boolean", "Also show the Option Chain tab (default false)") } }, { "symbol" }) });
    tools.push_back({ "get_bars",
                      "Loads the Quotes chart for a symbol/timeframe if needed and returns the chart description with the most recent bars as CSV (time,open,high,low,close,volume).",
                      schema({ { "symbol", prop("string", "Ticker (default: the charted symbol)") }, { "timeframe", prop("string", "Chart timeframe (default: current)", timeframes) },
                               { "limit", prop("integer", "Number of most recent bars to return (10-400, default 120)") } }) });
    tools.push_back({ "draw_trend_line",
                      "Draws a trend line on the Quotes chart between two (date, price) points; it extends to the right as a dashed ray. Dates are ISO (yyyy-MM-dd or yyyy-MM-dd HH:mm for intraday).",
                      schema({ { "from_date", prop("string", "Start date/time") }, { "from_price", prop("number", "Start price") }, { "to_date", prop("string", "End date/time") },
                               { "to_price", prop("number", "End price") }, { "extend", prop("boolean", "Extend to the right (default true)") } },
                             { "from_date", "from_price", "to_date", "to_price" }) });
    tools.push_back({ "draw_zone", "Shades a horizontal support or resistance zone across the Quotes chart between two prices.",
                      schema({ { "kind", prop("string", "Zone type", QJsonArray{ "support", "resistance" }) }, { "low", prop("number", "Lower price") }, { "high", prop("number", "Upper price") } },
                             { "kind", "low", "high" }) });
    tools.push_back({ "clear_drawings", "Removes every drawing from the charted symbol.", schema({}) });
    tools.push_back({ "list_drawings", "Returns the charted symbol's drawings as JSON.", schema({}) });
    tools.push_back({ "set_timeframe", "Changes the Quotes chart timeframe.", schema({ { "timeframe", prop("string", "Timeframe", timeframes) } }, { "timeframe" }) });
    tools.push_back({ "set_chart_type", "Changes the Quotes chart style.", schema({ { "type", prop("string", "Style", QJsonArray{ "candles", "bars", "heikin", "line" }) } }, { "type" }) });
    tools.push_back({ "list_indicators",
                      "Lists the technical-indicator catalogue (TA-Lib) by category: Overlap Studies, Momentum, Volume, Volatility, Price Transform, Cycle, Pattern Recognition, Statistics; each with its parameters and defaults.",
                      schema({ { "category", prop("string", "Only this category (default: all)") } }) });
    tools.push_back({ "add_indicator",
                      "Adds a TA-Lib technical indicator to the Quotes chart (kept as the default for future sessions). Overlap studies draw on the price, oscillators get their own pane, candlestick patterns mark bars. "
                      "func is the TA-Lib name (SMA, EMA, BBANDS, RSI, MACD, STOCH, ATR, ADX, OBV, SAR, CDLENGULFING …; see list_indicators); params use TA-Lib names or the aliases period, fast, slow, signal, nbdevup, nbdevdn, matype. "
                      "Re-adding a pane indicator replaces its parameters; at most three copies of one function.",
                      schema({ { "func", prop("string", "TA-Lib function name") }, { "params", QJsonObject{ { "type", "object" }, { "description", "Parameter values, e.g. {\"period\": 14} or {\"fast\": 12, \"slow\": 26, \"signal\": 9}" } } },
                               { "period", prop("integer", "Shorthand for params.period") } },
                             { "func" }) });
    tools.push_back({ "remove_indicator", "Removes indicators from the Quotes chart by TA-Lib function name (optionally only the one with a given period), or every indicator with func 'all'.",
                      schema({ { "func", prop("string", "TA-Lib function name, or 'all'") }, { "period", prop("integer", "Only the copy with this time period") } }, { "func" }) });
    tools.push_back({ "set_indicators", "Replaces the whole indicator set on the Quotes chart and optionally toggles the volume histogram.",
                      schema({ { "indicators", QJsonObject{ { "type", "array" }, { "description", "List of {func, params} objects, e.g. [{\"func\":\"SMA\",\"params\":{\"period\":20}},{\"func\":\"RSI\",\"params\":{\"period\":14}}]" },
                                                             { "items", QJsonObject{ { "type", "object" } } } } },
                               { "volume", prop("boolean", "Show the volume histogram") } },
                             { "indicators" }) });
    tools.push_back({ "get_option_chain",
                      "Returns the loaded option chain's per-expiry summary (ATM implied vol, forward, contract counts) and the visible strike slice. Use show_ticker first for another symbol.",
                      schema({ { "symbol", prop("string", "Ticker (default: the loaded chain)") } }) });
    tools.push_back({ "create_alert",
                      "Creates an alert on a ticker (saved; checked on every quote refresh and every minute). Conditions: price_above, price_below, change_above, change_below (day change, percent), iv_above, iv_below (ATM implied vol, percent), indicator_above, indicator_below (TA-Lib indicator such as RSI with a period). Fires a banner, a notification and optionally speech.",
                      schema({ { "symbol", prop("string", "Ticker") }, { "condition", prop("string", "Condition", QJsonArray{ "price_above", "price_below", "change_above", "change_below", "iv_above", "iv_below", "indicator_above", "indicator_below" }) },
                               { "threshold", prop("number", "Level (price, percent, or indicator value)") }, { "indicator", prop("string", "TA-Lib indicator for indicator conditions, e.g. RSI") },
                               { "period", prop("integer", "Indicator period (default 14)") }, { "repeat", prop("boolean", "Re-arm automatically when the condition clears") }, { "note", prop("string", "Optional note") } },
                             { "symbol", "condition", "threshold" }) });
    tools.push_back({ "list_alerts", "Lists every alert with its status and last observed value.", schema({}) });
    tools.push_back({ "delete_alert", "Deletes alerts by id, by ticker (all alerts on it), or 'all'.", schema({ { "target", prop("string", "Alert id, ticker, or 'all'") } }, { "target" }) });
    tools.push_back({ "get_portfolio",
                      "Returns the Portfolio tab's current view (one strategy or 'All strategies'): every position with mark, Greeks and P&L (CSV), exposure, and the latest VaR / CVaR, component VaR and stress results.",
                      schema({}) });
    tools.push_back({ "list_portfolios", "Lists the saved strategy books and which view is active ('All strategies' is the global portfolio).", schema({}) });
    tools.push_back({ "load_portfolio", "Switches the Portfolio tab to a strategy book, or to the global view with 'all'; marks and risk follow.",
                      schema({ { "name", prop("string", "Strategy name, or 'all'") } }, { "name" }) });
    tools.push_back({ "create_portfolio", "Creates a new strategy book (optionally copying the positions on screen) and switches to it.",
                      schema({ { "name", prop("string", "Strategy name") }, { "copy_current", prop("boolean", "Copy the positions currently shown (default false)") } }, { "name" }) });
    tools.push_back({ "delete_portfolio", "Deletes a strategy book and its positions (the last remaining book cannot be deleted).", schema({ { "name", prop("string", "Strategy name") } }, { "name" }) });
    tools.push_back({ "add_position",
                      "Adds a position to the Portfolio tab (saved between sessions). Stock: symbol, quantity (shares, negative = short), entry. Option: also type call|put, strike, expiry (YYYY-MM-DD) and optionally iv (percent).",
                      schema({ { "symbol", prop("string", "Underlying ticker") }, { "type", prop("string", "Position type", QJsonArray{ "stock", "call", "put" }) },
                               { "quantity", prop("number", "Shares or contracts; negative for short") }, { "strike", prop("number", "Option strike") },
                               { "expiry", prop("string", "Option expiry, ISO date") }, { "entry", prop("number", "Price paid per share / per contract unit") },
                               { "iv", prop("number", "Implied vol in percent to value the option when no stored chain quote matches") },
                               { "currency", prop("string", "ISO currency of the prices (default: the portfolio's reporting currency), e.g. USD, EUR, GBP") },
                               { "portfolio", prop("string", "Strategy book to add to (default: the one on screen; a new name creates a strategy)") },
                               { "trade_date", prop("string", "Date the trade was entered, ISO (default: today)") } },
                             { "symbol", "quantity" }) });
    tools.push_back({ "remove_position", "Removes positions from the Portfolio tab by symbol ('all' = every position), optionally only a type and strike.",
                      schema({ { "symbol", prop("string", "Ticker or 'all'") }, { "type", prop("string", "stock|call|put (default: any)") }, { "strike", prop("number", "Only this strike") } }, { "symbol" }) });
    tools.push_back({ "run_portfolio_risk",
                      "Recomputes portfolio risk: parametric delta-gamma, historical simulation and Monte Carlo VaR / CVaR (expected shortfall), component VaR by underlying, the spot x vol stress grid and the time-decay ladder.",
                      schema({ { "confidence", prop("number", "Confidence level: 95, 97.5 or 99 (percent)") }, { "horizon_days", prop("integer", "Holding period in trading days (1-60)") } }) });
    tools.push_back({ "scan_trade_ideas",
                      "Runs the Trade Ideas scanner over the option chains in memory and returns the market scan (ATM IV, term slope, skew, expected move, put/call ratios) and the ranked ideas with legs, credit/debit, max profit/loss, probability of profit and return on risk.",
                      schema({ { "screen", prop("string", "Premium selling | Directional debit | Volatility | Income on shares | All strategies (default: current)") },
                               { "bias", prop("string", "Any | Bullish | Bearish | Neutral | Volatile") },
                               { "universe", prop("string", "all (every stored chain) | watchlist | current (the ticker on screen)") },
                               { "min_days", prop("integer", "Earliest expiry in days") }, { "max_days", prop("integer", "Latest expiry in days") },
                               { "min_probability", prop("number", "Minimum probability of profit, percent") },
                               { "min_return_on_risk", prop("number", "Minimum max profit / max loss, percent") } }) });
    tools.push_back({ "get_sector_heatmap",
                      "Returns sector and stock performance from the Sector Heatmap (large caps grouped by sector, cap-weighted sector moves, top movers and laggards, CSV of every stock) for a period: Daily, 1W, 30D, 90D or YTD. Optionally switches the view to stocks or sectors.",
                      schema({ { "period", prop("string", "Performance period (default: current)", QJsonArray{ "Daily", "1W", "30D", "90D", "YTD" }) },
                               { "view", prop("string", "Treemap view", QJsonArray{ "stocks", "sectors" }) },
                               { "sector", prop("string", "Expand this sector to fill the map (e.g. Energy); 'all' resets to every sector") } }) });
    tools.push_back({ "get_volatility",
                      "Loads daily history for a symbol on the Volatility tab (if needed) and returns realized vol (several estimators), EWMA, GARCH fit and forecast, the vol cone and implied ATM vol.",
                      schema({ { "symbol", prop("string", "Ticker (default: current)") } }) });
    tools.push_back({ "set_market_volatility", "Sets the market volatility (sigma) used by the Pricer, Strategy and Scenario tabs.",
                      schema({ { "volatility_percent", prop("number", "Annualised volatility in percent, e.g. 32.5") } }, { "volatility_percent" }) });
    tools.push_back({ "load_strategy_preset", "Loads a strategy preset on the Strategy tab, built on the loaded chain's listed strikes and prices, and returns its metrics.",
                      schema({ { "preset", prop("string", "Preset name, e.g. Iron Condor, Long Straddle, Bull Call Spread", QJsonArray::fromStringList(m_strategy->presetNames())) } }, { "preset" }) });
    tools.push_back({ "get_strategy", "Returns the current strategy legs and metrics (CSV).", schema({}) });
    tools.push_back({ "get_pricer", "Returns the Pricer tab's inputs, price, Greeks and cross-checks (CSV).", schema({}) });
    tools.push_back({ "set_pricer_contract", "Sets the Pricer's contract (strike, expiry date, optional volatility) and shows the Pricer tab.",
                      schema({ { "strike", prop("number", "Strike") }, { "expiry_date", prop("string", "Expiry date yyyy-MM-dd") }, { "volatility_percent", prop("number", "Volatility in percent (default: market sigma)") } },
                             { "strike", "expiry_date" }) });
    tools.push_back({ "get_heatmap", "Returns the Heatmap tab's most-active contracts, put-call parity by expiry and the activity grid (CSV).", schema({}) });
    tools.push_back({ "list_watchlists", "Returns the saved watchlists with their tickers and which one is active.", schema({}) });
    tools.push_back({ "load_watchlist", "Loads a saved watchlist: the Quotes table switches to it, quotes refresh and its option chains are preloaded into memory.",
                      schema({ { "name", prop("string", "Watchlist name", QJsonArray::fromStringList(m_quotes->watchlistNames())) } }, { "name" }) });
    tools.push_back({ "create_watchlist_from_clipboard", "Creates (and loads) a new saved watchlist from the ticker symbols currently on the clipboard (e.g. \"AAPL,NVDA,IBM\").",
                      schema({ { "name", prop("string", "Name for the watchlist") } }, { "name" }) });
    tools.push_back({ "create_watchlist", "Creates (and loads) a new saved watchlist from a list of tickers.",
                      schema({ { "name", prop("string", "Name for the watchlist") }, { "tickers", QJsonObject{ { "type", "array" }, { "items", QJsonObject{ { "type", "string" } } }, { "description", "Ticker symbols" } } } },
                             { "name", "tickers" }) });
    return tools;
}

void MainWindow::executeAssistantTool(const QString& name, const QJsonObject& input, AssistantClient::ToolDone done)
{
    auto fail = [&done](const QString& message) { done(message, true); };
    auto symbolArg = [&input](const QString& fallback) {
        const QString s = input.value("symbol").toString().trimmed().toUpper();
        return s.isEmpty() ? fallback : s;
    };

    if (name == "get_screen_context") {
        assistantContext([done](const QString& text, const QImage&) { done(text, false); });
    } else if (name == "switch_tab") {
        QWidget* tab = tabByName(input.value("tab").toString());
        if (!tab) return fail(QStringLiteral("Unknown tab '%1'.").arg(input.value("tab").toString()));
        m_tabs->setCurrentWidget(tab);
        done(QStringLiteral("Showing the %1 tab.").arg(tabNameOf(tab)), false);
    } else if (name == "show_ticker") {
        const QString symbol = symbolArg(QString());
        if (symbol.isEmpty()) return fail("A symbol is required.");
        const bool stored = m_store.contains(symbol);
        showTicker(symbol, input.value("switch_to_chain").toBool(false));
        m_quotes->showTicker(symbol);
        if (stored && m_state.underlyingTicker == symbol) {
            done(QStringLiteral("%1 applied from the in-memory store: %2 contracts. ").arg(symbol).arg(m_state.chainQuotes.size()) + chainSummary(8), false);
        } else {
            done(QStringLiteral("%1 is being downloaded from Massive.com; the tabs will update when it arrives (a few seconds). Call get_option_chain afterwards for details.").arg(symbol), false);
        }
    } else if (name == "get_bars") {
        const QString symbol = symbolArg(m_quotes->chartTicker());
        if (symbol.isEmpty()) return fail("No symbol is charted; pass a symbol.");
        const int limit = std::clamp(input.value("limit").toInt(120), 10, 400);
        m_quotes->loadChartThen(symbol, input.value("timeframe").toString(), [this, done, limit](bool ok) {
            if (!ok) { done("The bars could not be loaded.", true); return; }
            done(m_quotes->contextSummary(limit), false);
        });
    } else if (name == "draw_trend_line") {
        if (m_quotes->chartTicker().isEmpty()) return fail("No chart is loaded; call get_bars or show_ticker first.");
        bool ok1 = false, ok2 = false;
        const qint64 t1 = epochSeconds(input.value("from_date"), ok1);
        const qint64 t2 = epochSeconds(input.value("to_date"), ok2);
        if (!ok1 || !ok2) return fail("Dates must be ISO yyyy-MM-dd (optionally with HH:mm).");
        QJsonObject spec{ { "type", "trend" }, { "t1", static_cast<double>(t1) }, { "p1", input.value("from_price").toDouble() }, { "t2", static_cast<double>(t2) },
                          { "p2", input.value("to_price").toDouble() }, { "extend", input.value("extend").toBool(true) } };
        m_tabs->setCurrentWidget(m_quotes);
        m_quotes->addDrawing(spec);
        done(QStringLiteral("Trend line drawn on %1 from %2 @ %3 to %4 @ %5.").arg(m_quotes->chartTicker(), input.value("from_date").toString(), ui::number(input.value("from_price").toDouble(), 2),
                                                                                  input.value("to_date").toString(), ui::number(input.value("to_price").toDouble(), 2)), false);
    } else if (name == "draw_zone") {
        if (m_quotes->chartTicker().isEmpty()) return fail("No chart is loaded; call get_bars or show_ticker first.");
        const double low = input.value("low").toDouble(), high = input.value("high").toDouble();
        if (!(low > 0.0) || !(high > 0.0)) return fail("low and high must be positive prices.");
        const QString kind = input.value("kind").toString() == "resistance" ? "resistance" : "support";
        QJsonObject spec{ { "type", "zone" }, { "kind", kind }, { "lo", std::min(low, high) }, { "hi", std::max(low, high) } };
        m_tabs->setCurrentWidget(m_quotes);
        m_quotes->addDrawing(spec);
        done(QStringLiteral("%1 zone drawn on %2 from %3 to %4.").arg(kind == "support" ? "Support" : "Resistance", m_quotes->chartTicker(), ui::number(std::min(low, high), 2), ui::number(std::max(low, high), 2)), false);
    } else if (name == "clear_drawings") {
        m_quotes->clearDrawings();
        done("All drawings removed.", false);
    } else if (name == "list_drawings") {
        done(m_quotes->drawingsJson(), false);
    } else if (name == "set_timeframe") {
        const QString tf = input.value("timeframe").toString();
        if (!m_quotes->setTimeframe(tf)) return fail(QStringLiteral("Unknown timeframe '%1'. Use one of: %2").arg(tf, m_quotes->timeframeLabels().join(", ")));
        m_tabs->setCurrentWidget(m_quotes);
        done(QStringLiteral("Timeframe set to %1; bars are reloading.").arg(tf), false);
    } else if (name == "set_chart_type") {
        if (!m_quotes->setChartType(input.value("type").toString())) return fail("Unknown chart type.");
        done(QStringLiteral("Chart style set to %1.").arg(input.value("type").toString()), false);
    } else if (name == "list_indicators") {
        const QString category = input.value("category").toString().trimmed();
        QString text = QuotesTab::indicatorCatalogText();
        if (!category.isEmpty()) {
            QStringList kept;
            bool inSection = false;
            for (const QString& line : text.split('\n')) {
                if (!line.startsWith("  ")) inSection = line.contains(category, Qt::CaseInsensitive);
                if (inSection) kept << line;
            }
            text = kept.isEmpty() ? QStringLiteral("No category matches '%1'.\n").arg(category) + text : kept.join('\n');
        }
        done(text, false);
    } else if (name == "add_indicator") {
        QString error;
        if (!m_quotes->addIndicator(input, &error)) return fail(error);
        m_tabs->setCurrentWidget(m_quotes);
        done(QStringLiteral("Indicators now: %1.").arg(m_quotes->indicatorsSummary()), false);
    } else if (name == "remove_indicator") {
        const int removed = m_quotes->removeIndicators(input.value("func").toString(input.value("type").toString()), input.value("period").toInt(0));
        done(QStringLiteral("%1 indicator(s) removed. Indicators now: %2.").arg(removed).arg(m_quotes->indicatorsSummary()), false);
    } else if (name == "set_indicators") {
        QString error;
        if (!m_quotes->setIndicators(input.value("indicators").toArray(), &error)) return fail(error);
        if (input.contains("volume")) m_quotes->setVolumeShown(input.value("volume").toBool(true));
        m_tabs->setCurrentWidget(m_quotes);
        done(QStringLiteral("Indicators now: %1.").arg(m_quotes->indicatorsSummary()), false);
    } else if (name == "get_option_chain") {
        const QString symbol = symbolArg(m_state.underlyingTicker);
        if (symbol.isEmpty()) return fail("No chain is loaded; call show_ticker with a symbol.");
        if (symbol != m_state.underlyingTicker) {
            showTicker(symbol, false);
            if (m_state.underlyingTicker != symbol) return fail(QStringLiteral("%1 is downloading; call get_option_chain again in a moment.").arg(symbol));
        }
        done(chainSummary(40) + "\nVisible slice (CSV):\n" + clip(m_chain->resultsCsv(), 9000), false);
    } else if (name == "create_alert") {
        QString error;
        int id = 0;
        if (!m_alerts->addRule(input, &error, &id)) return fail(error);
        done(QStringLiteral("Alert #%1 armed. %2").arg(id).arg(m_alerts->summaryText()), false);
    } else if (name == "list_alerts") {
        done(m_alerts->summaryText() + "\n\n" + m_alerts->rulesCsv(), false);
    } else if (name == "delete_alert") {
        const int removed = m_alerts->removeRules(input.value("target").toString());
        done(QStringLiteral("%1 alert(s) deleted. %2").arg(removed).arg(m_alerts->summaryText()), false);
    } else if (name == "get_portfolio") {
        done(m_portfolio->summaryText() + "\n\nPositions (CSV):\n" + clip(m_portfolio->resultsCsv(), 8000) + "\nRisk (CSV):\n" + clip(m_portfolio->riskCsv(), 4000), false);
    } else if (name == "list_portfolios") {
        done(QStringLiteral("Strategies: %1. Active view: %2.").arg(m_portfolio->portfolioNames().join(", "), m_portfolio->activePortfolio()), false);
    } else if (name == "load_portfolio") {
        const QString target = input.value("name").toString();
        if (!m_portfolio->loadPortfolio(target)) return fail(QStringLiteral("No strategy named '%1'. Available: %2").arg(target, m_portfolio->portfolioNames().join(", ")));
        m_tabs->setCurrentWidget(m_portfolio);
        QTimer::singleShot(1500, this, [this, done] { done(m_portfolio->summaryText(), false); });
    } else if (name == "create_portfolio") {
        const QString target = input.value("name").toString();
        if (!m_portfolio->createPortfolio(target, input.value("copy_current").toBool(false), true)) return fail(QStringLiteral("Could not create '%1' (empty, reserved or already exists).").arg(target));
        m_tabs->setCurrentWidget(m_portfolio);
        done(QStringLiteral("Strategy '%1' created and shown. Strategies: %2.").arg(target, m_portfolio->portfolioNames().join(", ")), false);
    } else if (name == "delete_portfolio") {
        const QString target = input.value("name").toString();
        if (!m_portfolio->deletePortfolio(target)) return fail(QStringLiteral("Could not delete '%1' (unknown, or it is the last strategy).").arg(target));
        done(QStringLiteral("Strategy '%1' deleted. Strategies: %2.").arg(target, m_portfolio->portfolioNames().join(", ")), false);
    } else if (name == "add_position") {
        QString error;
        if (!m_portfolio->addHolding(input, &error)) return fail(error);
        m_tabs->setCurrentWidget(m_portfolio);
        done(QStringLiteral("Position added; marks are refreshing. %1").arg(m_portfolio->summaryText().section('\n', 0, 0)), false);
    } else if (name == "remove_position") {
        const int removed = m_portfolio->removeHoldings(input.value("symbol").toString(), input.value("type").toString(), input.value("strike").toDouble());
        done(QStringLiteral("%1 position(s) removed. %2").arg(removed).arg(m_portfolio->summaryText().section('\n', 0, 0)), false);
    } else if (name == "run_portfolio_risk") {
        if (m_portfolio->holdingCount() == 0) return fail("The portfolio has no positions.");
        m_portfolio->setRiskSettings(QString(), input.value("confidence").toDouble(0.0), input.value("horizon_days").toInt(0));
        m_tabs->setCurrentWidget(m_portfolio);
        m_portfolio->runRisk();
        done(m_portfolio->summaryText() + "\n\nRisk (CSV):\n" + clip(m_portfolio->riskCsv(), 4000), false);
    } else if (name == "scan_trade_ideas") {
        if (input.contains("screen") && !m_scanner->setScreen(input.value("screen").toString())) return fail(QStringLiteral("Unknown screen. Use one of: %1").arg(m_scanner->screenNames().join(", ")));
        if (input.contains("bias") && !m_scanner->setBias(input.value("bias").toString())) return fail("Unknown bias. Use Any, Bullish, Bearish, Neutral or Volatile.");
        if (input.contains("universe") && !m_scanner->setUniverse(input.value("universe").toString())) return fail("Unknown universe. Use all, watchlist or current.");
        if (input.contains("min_days") || input.contains("max_days")) m_scanner->setDays(input.value("min_days").toInt(0), input.value("max_days").toInt(0));
        if (input.contains("min_probability")) m_scanner->setMinProbability(input.value("min_probability").toDouble());
        if (input.contains("min_return_on_risk")) m_scanner->setMinReturnOnRisk(input.value("min_return_on_risk").toDouble());
        m_tabs->setCurrentWidget(m_scanner);
        m_scanner->runScan();
        done(m_scanner->summaryText() + "\n\nMarket scan (CSV):\n" + clip(m_scanner->metricsCsv(), 4000) + "\n\nIdeas (CSV):\n" + clip(m_scanner->resultsCsv(), 7000), false);
    } else if (name == "get_sector_heatmap") {
        const QString period = input.value("period").toString();
        if (!period.isEmpty() && !m_sectorHeatmap->setPeriod(period)) return fail(QStringLiteral("Unknown period '%1'. Use Daily, 1W, 30D, 90D or YTD.").arg(period));
        if (input.contains("view")) m_sectorHeatmap->setView(input.value("view").toString());
        if (input.contains("sector")) {
            const QString sector = input.value("sector").toString().trimmed();
            if (sector.isEmpty() || sector.compare("all", Qt::CaseInsensitive) == 0) m_sectorHeatmap->clearFocus();
            else if (!m_sectorHeatmap->focusSector(sector)) return fail(QStringLiteral("No sector named '%1'.").arg(sector));
        }
        m_tabs->setCurrentWidget(m_sectorHeatmap);
        // Prices may still be downloading after a period change; give the request a moment.
        QTimer::singleShot(period.isEmpty() ? 0 : 2500, this, [this, done] {
            done(m_sectorHeatmap->summaryText() + "\n\nCSV (sector, ticker, name, market cap bn, last, performance %):\n" + clip(m_sectorHeatmap->resultsCsv(), 9000), false);
        });
    } else if (name == "get_volatility") {
        const QString symbol = symbolArg(m_volatility->ticker().isEmpty() ? m_state.underlyingTicker : m_volatility->ticker());
        if (symbol.isEmpty()) return fail("Pass a symbol.");
        m_volatility->fetchHistoryThen(symbol, [this, done](bool ok, const QString& message) {
            if (!ok) { done(message, true); return; }
            done(m_volatility->summaryText() + "\n\n" + clip(m_volatility->resultsCsv(), 6000), false);
        });
    } else if (name == "set_market_volatility") {
        const double pct = input.value("volatility_percent").toDouble();
        if (!(pct > 0.0 && pct < 500.0)) return fail("volatility_percent must be between 0 and 500.");
        m_state.market.volatility = pct / 100.0;
        m_state.notify();
        done(QStringLiteral("Market sigma set to %1%.").arg(ui::number(pct, 2)), false);
    } else if (name == "load_strategy_preset") {
        const QString preset = input.value("preset").toString();
        if (!m_strategy->selectPreset(preset)) return fail(QStringLiteral("Unknown preset '%1'. Available: %2").arg(preset, m_strategy->presetNames().join(", ")));
        m_tabs->setCurrentWidget(m_strategy);
        done(clip(m_strategy->resultsCsv(), 8000), false);
    } else if (name == "get_strategy") {
        done(clip(m_strategy->resultsCsv(), 9000), false);
    } else if (name == "get_pricer") {
        done(clip(m_pricer->resultsCsv(), 9000), false);
    } else if (name == "set_pricer_contract") {
        const QDate expiry = QDate::fromString(input.value("expiry_date").toString(), Qt::ISODate);
        if (!expiry.isValid()) return fail("expiry_date must be yyyy-MM-dd.");
        const double strike = input.value("strike").toDouble();
        if (!(strike > 0.0)) return fail("strike must be positive.");
        const double vol = input.contains("volatility_percent") ? input.value("volatility_percent").toDouble() / 100.0 : m_state.market.volatility;
        m_pricer->setContractWithDate(strike, expiry, vol);
        m_tabs->setCurrentWidget(m_pricer);
        done(clip(m_pricer->resultsCsv(), 8000), false);
    } else if (name == "get_heatmap") {
        done(clip(m_heatmap->resultsCsv(), 9000), false);
    } else if (name == "list_watchlists") {
        QJsonObject out;
        out["active"] = m_quotes->activeWatchlistName();
        QJsonObject lists;
        for (const QString& n : m_quotes->watchlistNames()) lists[n] = QJsonArray::fromStringList(n == m_quotes->activeWatchlistName() ? m_quotes->watchlist() : QStringList());
        lists[m_quotes->activeWatchlistName()] = QJsonArray::fromStringList(m_quotes->watchlist());
        out["watchlists"] = lists;
        done(out, false);
    } else if (name == "load_watchlist") {
        const QString list = input.value("name").toString();
        if (!m_quotes->loadWatchlistNamed(list)) return fail(QStringLiteral("No watchlist named '%1'. Available: %2").arg(list, m_quotes->watchlistNames().join(", ")));
        m_tabs->setCurrentWidget(m_quotes);
        done(QStringLiteral("Watchlist '%1' loaded with %2 tickers: %3. Quotes are refreshing and its option chains are being preloaded.").arg(list).arg(m_quotes->watchlist().size()).arg(m_quotes->watchlist().join(", ")), false);
    } else if (name == "create_watchlist_from_clipboard") {
        const QString list = input.value("name").toString().trimmed();
        const QStringList tickers = m_quotes->clipboardTickers();
        if (list.isEmpty()) return fail("A name is required.");
        if (tickers.isEmpty()) return fail("The clipboard holds no ticker symbols.");
        if (m_quotes->watchlistNames().contains(list)) return fail(QStringLiteral("A watchlist named '%1' already exists.").arg(list));
        m_quotes->createWatchlist(list, tickers, true);
        m_tabs->setCurrentWidget(m_quotes);
        done(QStringLiteral("Watchlist '%1' created from the clipboard with %2 tickers (%3); option chains are being preloaded.").arg(list).arg(tickers.size()).arg(tickers.join(", ")), false);
    } else if (name == "create_watchlist") {
        QStringList tickers;
        for (const QJsonValue v : input.value("tickers").toArray()) tickers << v.toString();
        const QString list = input.value("name").toString().trimmed();
        if (list.isEmpty() || tickers.isEmpty()) return fail("A name and at least one ticker are required.");
        if (m_quotes->watchlistNames().contains(list)) return fail(QStringLiteral("A watchlist named '%1' already exists.").arg(list));
        m_quotes->createWatchlist(list, tickers, true);
        m_tabs->setCurrentWidget(m_quotes);
        done(QStringLiteral("Watchlist '%1' created and loaded with %2 tickers; option chains are being preloaded.").arg(list).arg(m_quotes->watchlist().size()), false);
    } else {
        fail(QStringLiteral("Unknown tool '%1'.").arg(name));
    }
}

// MARK: - Local voice/text commands (no AI key needed)

bool MainWindow::handleLocalCommand(const QString& rawText, QString& feedback)
{
    QString text = rawText.trimmed();
    text.remove(QRegularExpression("[.!?]+$"));
    const QString lower = text.toLower();
    auto symbolFrom = [](const QString& s) { return s.trimmed().toUpper().remove(QRegularExpression("[^A-Z.]")); };

    // Trade ideas: "scan for ideas", "find premium selling ideas on the watchlist", "scan for bullish trades".
    QRegularExpression scanRe("^(?:please\\s+)?(?:scan|screen|look|search|find)(?:\\s+(?:for|me))*\\s+(.*?)(?:\\s+(?:ideas?|trades?|setups?|opportunit(?:y|ies)))?(?:\\s+(?:on|in|across|over)\\s+(?:the\\s+)?(watchlist|all\\s+chains|everything|this\\s+ticker|current\\s+ticker|[A-Za-z.]{1,6}))?$",
                              QRegularExpression::CaseInsensitiveOption);
    if (const auto m = scanRe.match(text); m.hasMatch() && (lower.contains("idea") || lower.contains("trade") || lower.contains("setup") || lower.contains("opportunit") || lower.startsWith("scan"))) {
        const QString qualifiers = m.captured(1).toLower();
        const QString where = m.captured(2).toLower();
        for (const char* bias : { "bullish", "bearish", "neutral", "volatile" }) if (qualifiers.contains(bias)) m_scanner->setBias(bias);
        if (qualifiers.contains("premium") || qualifiers.contains("credit") || qualifiers.contains("income from") || qualifiers.contains("selling")) m_scanner->setScreen("Premium selling");
        else if (qualifiers.contains("debit") || qualifiers.contains("directional")) m_scanner->setScreen("Directional debit");
        else if (qualifiers.contains("volatil") || qualifiers.contains("straddle") || qualifiers.contains("calendar")) m_scanner->setScreen("Volatility");
        else if (qualifiers.contains("covered") || qualifiers.contains("income") || qualifiers.contains("collar")) m_scanner->setScreen("Income on shares");
        else if (qualifiers.contains("all") || qualifiers.contains("every")) m_scanner->setScreen("All strategies");
        if (where.contains("watchlist")) m_scanner->setUniverse("watchlist");
        else if (where.contains("all") || where.contains("everything")) m_scanner->setUniverse("all");
        else if (!where.isEmpty()) {
            if (!where.contains("ticker")) showTicker(where.toUpper(), false);
            m_scanner->setUniverse("current");
        }
        m_tabs->setCurrentWidget(m_scanner);
        m_scanner->runScan();
        feedback = m_scanner->summaryText().section('\n', 0, 3);
        return true;
    }
    // Strategy books: "load the income portfolio", "show all strategies", "switch to the core hedged strategy".
    QRegularExpression bookRe("^(?:please\\s+)?(?:load|open|show(?:\\s+me)?|switch\\s+to)\\s+(?:the\\s+)?(?:(all\\s+strategies|global\\s+portfolio|whole\\s+book|all\\s+portfolios)|(.+?)\\s+(?:portfolio|strategy\\s+book|book|strategy))$",
                              QRegularExpression::CaseInsensitiveOption);
    if (const auto m = bookRe.match(text); m.hasMatch()) {
        const QString target = m.captured(1).isEmpty() ? m.captured(2).trimmed() : QStringLiteral("all");
        if (m_portfolio->loadPortfolio(target)) {
            m_tabs->setCurrentWidget(m_portfolio);
            feedback = QStringLiteral("Showing %1 on the Portfolio tab; marks are refreshing.").arg(m_portfolio->activePortfolio());
            return true;
        }
    }
    // Alerts: "alert me if NVDA goes above 240", "notify me when AAPL drops 3%", "alert me when the RSI on TSLA is above 70".
    if (m_alerts->addRuleFromText(text, &feedback)) {
        m_tabs->setCurrentWidget(m_alerts);
        return true;
    }
    // Indicators by name through the TA-Lib catalogue: "add a 200 day SMA", "add rsi", "show the
    // MACD (8, 17, 9)", "add bollinger bands", "plot the engulfing pattern", "remove the 50 day sma",
    // "remove rsi", "remove all indicators". Unknown names are left to the model.
    QRegularExpression addIndRe("^(?:please\\s+)?(?:add|show|plot|put|overlay|turn\\s+on|draw)\\s+(?:a\\s+|an\\s+|the\\s+)?(?:(\\d{1,3})\\s*[- ]?\\s*(?:day|bar|period|week|minute)?\\s+)?(.+?)(?:\\s+(?:pattern|patterns))?(?:\\s*\\(\\s*([\\d.]+(?:\\s*[,/ ]\\s*[\\d.]+)*)\\s*\\))?(?:\\s+(?:on|to)\\s+the\\s+chart)?$",
                               QRegularExpression::CaseInsensitiveOption);
    if (const auto m = addIndRe.match(text); m.hasMatch()) {
        const QString func = QuotesTab::resolveIndicatorName(m.captured(2));
        if (!func.isEmpty()) {
            QJsonObject spec{ { "func", func } };
            QJsonObject params;
            if (!m.captured(1).isEmpty()) params["period"] = m.captured(1).toInt();
            if (!m.captured(3).isEmpty()) {
                // "(8, 17, 9)": fill the function's integer parameters in order (fast, slow, signal for MACD).
                const QStringList numbers = m.captured(3).split(QRegularExpression("[,/ ]+"), Qt::SkipEmptyParts);
                if (const ta::FunctionInfo* info = ta::catalog().find(func.toStdString())) {
                    int n = 0;
                    for (const ta::ParamInfo& p : info->params) {
                        if (p.advanced || n >= numbers.size()) continue;
                        params[QString::fromStdString(p.name)] = numbers.at(n++).toDouble();
                    }
                }
            }
            spec["params"] = params;
            QString error;
            if (!m_quotes->addIndicator(spec, &error)) { feedback = error; return true; }
            m_tabs->setCurrentWidget(m_quotes);
            feedback = QStringLiteral("Added %1. Indicators now: %2.").arg(QuotesTab::indicatorLabel(spec), m_quotes->indicatorsSummary());
            return true;
        }
    }
    QRegularExpression removeIndRe("^(?:please\\s+)?(?:remove|hide|delete|turn\\s+off|clear)\\s+(?:the\\s+)?(?:(\\d{1,3})\\s*[- ]?\\s*(?:day|bar|period|week|minute)?\\s+)?(.+?)(?:\\s+(?:from|on)\\s+the\\s+chart)?$",
                                  QRegularExpression::CaseInsensitiveOption);
    if (const auto m = removeIndRe.match(text); m.hasMatch()) {
        const QString what = m.captured(2).trimmed().toLower();
        const int period = m.captured(1).toInt();
        QString func;
        if (what == "all indicators" || what == "indicators" || what == "every indicator") func = "all";
        else if (what == "moving averages" || what == "moving average") func = "SMA";
        else func = QuotesTab::resolveIndicatorName(what);
        if (!func.isEmpty()) {
            int removed = m_quotes->removeIndicators(func, period);
            if (what.startsWith("moving average")) removed += m_quotes->removeIndicators("EMA", period);
            feedback = removed ? QStringLiteral("Removed %1 indicator(s). Indicators now: %2.").arg(removed).arg(m_quotes->indicatorsSummary())
                               : QStringLiteral("Nothing matched. Indicators now: %1.").arg(m_quotes->indicatorsSummary());
            return true;
        }
    }
    // "pull up / show / open / load the option chain(s) for AAPL"
    QRegularExpression chainRe("^(?:please\\s+)?(?:pull\\s+up|show(?:\\s+me)?|open|load|bring\\s+up|display|get)\\s+(?:the\\s+)?(?:options?\\s*chains?|chains?)\\s+(?:for|of|on)\\s+([A-Za-z.]{1,6})$",
                               QRegularExpression::CaseInsensitiveOption);
    if (const auto m = chainRe.match(lower); m.hasMatch()) {
        const QString symbol = symbolFrom(m.captured(1));
        showTicker(symbol, true);
        feedback = QStringLiteral("Loading the %1 option chain%2.").arg(symbol, m_store.contains(symbol) ? QStringLiteral(" from memory") : QStringLiteral(" from Massive.com"));
        return true;
    }
    // "chart AAPL" / "show me the AAPL chart" / "pull up AAPL"
    QRegularExpression chartRe("^(?:please\\s+)?(?:chart|graph|plot)\\s+([A-Za-z.]{1,6})$|^(?:please\\s+)?(?:show(?:\\s+me)?|pull\\s+up|open|display)\\s+(?:the\\s+)?([A-Za-z.]{1,6})\\s+(?:chart|graph|quote|price)s?$|^(?:pull\\s+up|show(?:\\s+me)?|open)\\s+([A-Za-z.]{1,6})$",
                               QRegularExpression::CaseInsensitiveOption);
    if (const auto m = chartRe.match(lower); m.hasMatch()) {
        const QString symbol = symbolFrom(m.captured(1).isEmpty() ? (m.captured(2).isEmpty() ? m.captured(3) : m.captured(2)) : m.captured(1));
        if (!symbol.isEmpty() && symbol != "THE" && symbol != "ME") {
            m_tabs->setCurrentWidget(m_quotes);
            m_quotes->showTicker(symbol);
            showTicker(symbol, false);
            feedback = QStringLiteral("Charting %1 and loading it everywhere.").arg(symbol);
            return true;
        }
    }
    // "switch to / go to / open the volatility tab"
    QRegularExpression tabRe("^(?:please\\s+)?(?:switch|go|take\\s+me|jump|move|navigate)\\s+(?:to\\s+)?(?:the\\s+)?([A-Za-z ]+?)(?:\\s+tab)?$|^(?:open|show(?:\\s+me)?)\\s+(?:the\\s+)?([A-Za-z ]+?)\\s+tab$",
                             QRegularExpression::CaseInsensitiveOption);
    if (const auto m = tabRe.match(lower); m.hasMatch()) {
        if (QWidget* tab = tabByName(m.captured(1).isEmpty() ? m.captured(2) : m.captured(1))) {
            m_tabs->setCurrentWidget(tab);
            feedback = QStringLiteral("Showing the %1 tab.").arg(tabNameOf(tab));
            return true;
        }
    }
    // "volatility for NVDA"
    QRegularExpression volRe("^(?:please\\s+)?(?:show(?:\\s+me)?|pull\\s+up|open|get|run)\\s+(?:the\\s+)?(?:realized\\s+|historical\\s+)?vol(?:atility)?\\s+(?:for|of|on)\\s+([A-Za-z.]{1,6})$",
                             QRegularExpression::CaseInsensitiveOption);
    if (const auto m = volRe.match(lower); m.hasMatch()) {
        const QString symbol = symbolFrom(m.captured(1));
        m_tabs->setCurrentWidget(m_volatility);
        m_volatility->setTicker(symbol);
        m_volatility->fetchAll();
        feedback = QStringLiteral("Loading %1 volatility history (and its chain).").arg(symbol);
        return true;
    }
    // "load / switch to / open (the) <name> watchlist" or "load watchlist <name>"
    QRegularExpression listRe("^(?:please\\s+)?(?:load|switch\\s+to|open|show(?:\\s+me)?|use)\\s+(?:the\\s+)?(?:watchlist\\s+(.+?)|(.+?)\\s+watchlist)$", QRegularExpression::CaseInsensitiveOption);
    if (const auto m = listRe.match(text); m.hasMatch()) {
        const QString wanted = (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1)).trimmed();
        for (const QString& n : m_quotes->watchlistNames()) {
            if (n.compare(wanted, Qt::CaseInsensitive) == 0 || n.contains(wanted, Qt::CaseInsensitive)) {
                m_tabs->setCurrentWidget(m_quotes);
                m_quotes->loadWatchlistNamed(n);
                feedback = QStringLiteral("Loaded the “%1” watchlist (%2 tickers); refreshing quotes and preloading its option chains.").arg(n).arg(m_quotes->watchlist().size());
                return true;
            }
        }
        feedback = QStringLiteral("No watchlist called “%1”. Available: %2.").arg(wanted, m_quotes->watchlistNames().join(", "));
        return true;
    }
    // "create / make / new watchlist from (the) clipboard (called X)"
    QRegularExpression clipRe("^(?:please\\s+)?(?:create|make|add|new)\\s+(?:a\\s+)?(?:new\\s+)?watchlist\\s+from\\s+(?:the\\s+)?clipboard(?:\\s+(?:called|named)\\s+(.+))?$", QRegularExpression::CaseInsensitiveOption);
    if (const auto m = clipRe.match(text); m.hasMatch()) {
        const QStringList tickers = m_quotes->clipboardTickers();
        if (tickers.isEmpty()) { feedback = "The clipboard holds no ticker symbols; copy text like “AAPL,NVDA,IBM” first."; return true; }
        QString list = m.captured(1).trimmed();
        if (list.isEmpty()) list = QStringLiteral("Pasted %1").arg(QDate::currentDate().toString("MMM d HH:mm"));
        m_quotes->createWatchlist(list, tickers, true);
        m_tabs->setCurrentWidget(m_quotes);
        feedback = QStringLiteral("Created and loaded “%1” with %2 tickers from the clipboard: %3. Preloading its option chains.").arg(list).arg(tickers.size()).arg(tickers.join(", "));
        return true;
    }
    if (lower == "clear drawings" || lower == "clear the drawings" || lower == "remove all drawings") {
        m_quotes->clearDrawings();
        feedback = "Drawings cleared.";
        return true;
    }
    if (lower == "hide charts" || lower == "show charts") {
        m_tabs->setCurrentWidget(m_chain);
        feedback = "Use the Hide Charts button on the Option Chain tab.";
        return false;
    }
    return false;
}
