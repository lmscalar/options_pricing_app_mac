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
#include "PricerTab.h"
#include "QuotesTab.h"
#include "ScenarioTab.h"
#include "StrategyTab.h"
#include "VolatilityTab.h"

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
    "support/resistance zones on the chart, change the timeframe or chart style, read the option chain, volatility and strategy "
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

    m_assistant->contextProvider = [this](std::function<void(const QString&, const QImage&)> done) { assistantContext(std::move(done)); };
    m_assistant->localCommandHandler = [this](const QString& text, QString& feedback) { return handleLocalCommand(text, feedback); };
    m_assistant->client().setSystemPrompt(QString::fromUtf8(kSystemPrompt));
    m_assistant->client().setTools(assistantTools());
    m_assistant->client().setToolExecutor([this](const QString& name, const QJsonObject& input, AssistantClient::ToolDone done) {
        executeAssistantTool(name, input, std::move(done));
    });
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
    const QJsonArray tabs{ "Quotes", "Pricer", "Strategy", "Scenarios", "Option Chain", "Heatmap", "Volatility" };
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
    tools.push_back({ "add_indicator",
                      "Adds a technical indicator to the Quotes chart (kept as the default for future sessions): an SMA or EMA with a period (at most three of each), or the MACD with fast/slow/signal periods (default 12/26/9; re-adding replaces its parameters).",
                      schema({ { "type", prop("string", "Indicator", QJsonArray{ "sma", "ema", "macd" }) }, { "period", prop("integer", "Moving-average period in bars (2-500)") },
                               { "fast", prop("integer", "MACD fast EMA period") }, { "slow", prop("integer", "MACD slow EMA period") }, { "signal", prop("integer", "MACD signal EMA period") } },
                             { "type" }) });
    tools.push_back({ "remove_indicator", "Removes indicators from the Quotes chart: by type (optionally a specific moving-average period), or every indicator with type 'all'.",
                      schema({ { "type", prop("string", "Indicator", QJsonArray{ "sma", "ema", "macd", "all" }) }, { "period", prop("integer", "Only the moving average with this period") } }, { "type" }) });
    tools.push_back({ "set_indicators", "Replaces the whole indicator set on the Quotes chart and optionally toggles the volume histogram.",
                      schema({ { "indicators", QJsonObject{ { "type", "array" }, { "description", "List of {type:'sma'|'ema', period} or {type:'macd', fast, slow, signal}" },
                                                             { "items", QJsonObject{ { "type", "object" } } } } },
                               { "volume", prop("boolean", "Show the volume histogram") } },
                             { "indicators" }) });
    tools.push_back({ "get_option_chain",
                      "Returns the loaded option chain's per-expiry summary (ATM implied vol, forward, contract counts) and the visible strike slice. Use show_ticker first for another symbol.",
                      schema({ { "symbol", prop("string", "Ticker (default: the loaded chain)") } }) });
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
    } else if (name == "add_indicator") {
        QString error;
        if (!m_quotes->addIndicator(input, &error)) return fail(error);
        m_tabs->setCurrentWidget(m_quotes);
        done(QStringLiteral("Indicators now: %1.").arg(m_quotes->indicatorsSummary()), false);
    } else if (name == "remove_indicator") {
        const int removed = m_quotes->removeIndicators(input.value("type").toString(), input.value("period").toInt(0));
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

    // "add a 200 day SMA", "add the 21 EMA", "show the MACD", "remove the 50 day sma", "remove the macd", "remove all indicators"
    QRegularExpression addIndRe("^(?:please\\s+)?(?:add|show|plot|put|overlay|turn\\s+on)\\s+(?:a\\s+|an\\s+|the\\s+)?(?:(\\d{1,3})\\s*[- ]?\\s*(?:day|bar|period|week|minute)?\\s*(sma|ema|simple\\s+moving\\s+average|exponential\\s+moving\\s+average|moving\\s+average)|(macd)(?:\\s*\\(?\\s*(\\d+)\\s*[,/ ]\\s*(\\d+)\\s*[,/ ]\\s*(\\d+)\\s*\\)?)?)(?:\\s+(?:on|to)\\s+the\\s+chart)?$",
                               QRegularExpression::CaseInsensitiveOption);
    if (const auto m = addIndRe.match(text); m.hasMatch()) {
        QJsonObject spec;
        if (!m.captured(3).isEmpty()) {
            spec["type"] = "macd";
            if (!m.captured(4).isEmpty()) { spec["fast"] = m.captured(4).toInt(); spec["slow"] = m.captured(5).toInt(); spec["signal"] = m.captured(6).toInt(); }
        } else {
            const QString kind = m.captured(2).toLower();
            spec["type"] = kind.startsWith("ema") || kind.startsWith("exponential") ? "ema" : "sma";
            spec["period"] = m.captured(1).toInt();
        }
        QString error;
        if (!m_quotes->addIndicator(spec, &error)) { feedback = error; return true; }
        m_tabs->setCurrentWidget(m_quotes);
        feedback = QStringLiteral("Added %1. Indicators now: %2.").arg(QuotesTab::indicatorLabel(spec), m_quotes->indicatorsSummary());
        return true;
    }
    QRegularExpression removeIndRe("^(?:please\\s+)?(?:remove|hide|delete|turn\\s+off|clear)\\s+(?:the\\s+)?(?:(\\d{1,3})\\s*[- ]?\\s*(?:day|bar|period|week|minute)?\\s*)?(sma|ema|macd|all\\s+indicators|indicators|moving\\s+averages?)(?:\\s+(?:from|on)\\s+the\\s+chart)?$",
                                  QRegularExpression::CaseInsensitiveOption);
    if (const auto m = removeIndRe.match(text); m.hasMatch()) {
        const QString what = m.captured(2).toLower();
        const int period = m.captured(1).toInt();
        int removed = 0;
        if (what.startsWith("all") || what == "indicators") removed = m_quotes->removeIndicators("all");
        else if (what.startsWith("moving")) removed = m_quotes->removeIndicators("sma", period) + m_quotes->removeIndicators("ema", period);
        else removed = m_quotes->removeIndicators(what, period);
        feedback = removed ? QStringLiteral("Removed %1 indicator(s). Indicators now: %2.").arg(removed).arg(m_quotes->indicatorsSummary())
                           : QStringLiteral("Nothing matched. Indicators now: %1.").arg(m_quotes->indicatorsSummary());
        return true;
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
