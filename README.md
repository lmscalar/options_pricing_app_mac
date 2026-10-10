# Option Pricer

A desktop options analytics workstation written in C++20 with a Qt 6 Widgets front end.
It prices European and American options, reports first- and second-order Greeks, solves
implied volatility, analyses multi-leg strategies, builds scenario heatmaps, fits
volatility smiles from option chains and prices batches of contracts from CSV.

## Layout

| Path | Contents |
|------|----------|
| `OptionPricing/Pricing/` | Header-only pricing library with no Qt dependency. |
| `OptionPricing/UI/` | Qt Widgets user interface, one file pair per tab. |
| `OptionPricing/main.cpp` | Application entry point. |
| `PricingTests/` | XCTest bundle (Objective-C++) exercising the pricing library. |
| `ThirdParty/` | Vendored TradingView Lightweight Charts bundle and licence. |
| `Tools/` | `embed_chart_bundle.py`, which regenerates the embedded chart bundle. |
| `Samples/` | Example CSV files for the chain importer and batch pricer. |

### Pricing library

| Header | What it provides |
|--------|------------------|
| `Normal.h` | Normal CDF, PDF and inverse CDF. |
| `BlackScholes.h` | Black-Scholes-Merton and Black-76 prices, delta, gamma, vega, theta, rho, vanna, volga, charm, speed, color, zomma, discrete cash dividends (escrowed model), implied volatility solver, probability metrics. |
| `American.h` | Cox-Ross-Rubinstein binomial tree with tree Greeks, Bjerksund-Stensland (1993) approximation, early-exercise premium. |
| `FiniteDifference.h` | Crank-Nicolson solver on a log-spot grid with PSOR for American exercise. |
| `MonteCarlo.h` | Antithetic terminal-value Monte Carlo with standard error. |
| `DayCount.h` | Civil-date arithmetic, ACT/365, ACT/360 and BUS/252 year fractions. |
| `RateCurve.h` | Zero-rate term structure with linear interpolation. |
| `Strategy.h` | Multi-leg positions, P&L, net Greeks, breakevens, max gain/loss, probability of profit, 19 strategy presets. |
| `Scenario.h` | Spot x volatility and spot x time grids for any position metric. |
| `VolSurface.h` | Chain implied vols, static-arbitrage checks, raw SVI smile fit, surface interpolation in total variance, synthetic chain generator. |
| `Csv.h` | CSV reader/writer and parsers for option chains and batch pricing requests. |
| `Activity.h` | Strike x expiry activity grid, most-active contract ranking, put-call parity by expiry, activity summary. |
| `ChainStrategy.h` | Chain index, marking legs to listed quotes, chain-driven strategy presets. |
| `Portfolio.h` | Book of stock and option holdings, exposures, full revaluation. |
| `Risk.h` | Return histories, covariance, parametric / historical / Monte Carlo VaR and expected shortfall, component VaR, stress grid, decay ladder. |
| `Scanner.h` | Chain metrics (ATM IV, term slope, skew, expected move, put/call ratios), delta-targeted strategy construction, ranked trade ideas. |
| `Events.h` | Earnings analytics: ATM term structure in total variance, event variance isolation, skew-adjusted implied move, price cone. |
| `IvHistory.h` | Implied-vol history: 30-day constant-maturity ATM vol samples, IV rank and percentile, implied vol from call/put closes. |
| `Optimizer.h` | Strategy comparison and optimisation: view-based metrics (expected P&L, PoP, P&L at target), enumeration of listed strikes and expiries per family, preset comparison. |
| `Treemap.h` | Squarified treemap layout for the sector heatmap. |
| `TechnicalAnalysis.h` | TA-Lib catalogue wrapper and generic indicator evaluation. |
| `Volatility.h` | Realized vol estimators (close-to-close, Parkinson, Garman-Klass, Rogers-Satchell, Yang-Zhang), rolling series, volatility cone, EWMA, GARCH(1,1) / GJR-GARCH(1,1) by quasi-maximum likelihood with variance forecasts and forecast term structure. |

## Building

The project is an Xcode command-line tool target that links Homebrew's Qt 6
(`/opt/homebrew/opt/qt`): QtWidgets, QtCharts, QtNetwork and QtWebEngineWidgets (for the
Quotes chart). Install Qt with `brew install qt`, open `OptionPricing.xcodeproj` and
build the `OptionPricing` scheme. The target sets `HEADER_SEARCH_PATHS` to
`/opt/homebrew/opt/qt/include` because the WebEngine headers pull in the header-only
QtQmlIntegration module. Run the tests with the `PricingTests` scheme or:

```sh
xcodebuild test -scheme PricingTests -destination 'platform=macOS'
```

The pricing library compiles standalone without Qt:

```sh
clang++ -std=gnu++20 -I. your_tool.cpp
```

### Developer flag

```sh
OptionPricing --screenshot /tmp/shots
```

loads sample data, renders every tab to PNG, round-trips a workspace file and exits.

## In-app guide

The **How to use** button in the header (also Help ▸ How to Use This Tab, or F1) opens a guide
page for the tab on screen: step-by-step instructions, what every figure means, and for the
Pricer an explanation of the option models (Black-Scholes-Merton, Black-76, the CRR binomial
tree, Bjerksund-Stensland, Crank-Nicolson with PSOR, Monte Carlo, the implied-vol solver) and
of every Greek. Help ▸ User Guide lists every page.

## Tabs

- **Quotes**: a watchlist and a chart. The watchlist (Ticker, Last, Price Chg., Pct
  Change) is user-editable: type a symbol and press Add, or use the Remove button on a
  row; it is remembered between sessions and auto-refreshes from Massive's bulk snapshot
  (default every 60 s). Columns are resizable by dragging the header dividers, and
  clicking a header sorts numerically by Last, Price Chg. or Pct Change (a third click
  restores watchlist order); widths and sort are remembered. Selecting a row loads its
  chart; the ticker from the Option Chain tab is added automatically. Watchlists are named
  and saved: the **Watchlist** selector loads one (the table switches, quotes refresh and
  the list's option chains are preloaded into memory, refreshing any older than ten
  minutes), and the **Lists** menu creates an empty list, saves the current one under a new
  name, renames, deletes, creates a list from the clipboard (“AAPL,NVDA,IBM,ORCL”, lines
  or spaces, or cells copied from a spreadsheet as a row or a column; with a header such as
  Ticker or Symbol only that column is used; a leading `$`, quotes and Excel's `="AAPL"`
  are ignored), adds clipboard tickers to the current
  list, imports tickers from a text/CSV file or exports them. The Add box also accepts
  several tickers at once. Ticker
  edits save into the active list automatically; the assistant can list, load and create
  watchlists, and "load the energy watchlist" works as a spoken or typed command. The chart is TradingView's open-source Lightweight Charts
  (see below) inside a `QWebEngineView`, with timeframe buttons 1m, 2m, 3m, 5m, 15m, 1H,
  1D (one year of daily bars) and 1W (five years of weekly bars), the selected one
  highlighted in the accent colour; Candlestick, Bar,
  Heikin-Ashi and Line styles; an **Indicators** drop-down backed by the
  [TA-Lib](https://ta-lib.org) C library (`brew install ta-lib`): every TA-Lib function
  is listed by category (Overlap Studies, Momentum Indicators, Volume Indicators,
  Volatility Indicators, Price Transform, Cycle Indicators, Pattern Recognition,
  Statistic Functions) with a searchable **Browse indicators…** dialog, and each one
  opens a parameter dialog generated from TA-Lib's own description (periods, deviations,
  moving-average type, a colour per output). Overlap studies (SMA, EMA, Bollinger
  Bands, SAR …) draw over the price; oscillators (MACD, RSI, Stochastics, ATR, OBV …)
  get framed panes below the volume with their own value axis, zero line and reference
  levels (RSI 30/70 and the like); candlestick patterns (CDLENGULFING, CDLDOJI …) mark
  bars with green arrows for bullish and red for bearish signals. Panes are second charts
  whose time scale and crosshair are linked to the main one, so panning, zooming and
  hovering stay in step and Save Image… includes them; drag the handle above the panes
  to resize them, or double-click the handle / click the ⤢ button to expand and restore
  (`quotes/paneHeight`). Up to 12 indicators, three copies of one function and four
  panes; each indicator's submenu offers Edit and Remove, and the set is remembered as
  the default for the next launch (`quotes/indicators`). The catalogue is read from
  TA-Lib at runtime through `Pricing/TechnicalAnalysis.h`, which also evaluates any
  function generically (`ta::compute`). A crosshair legend shows OHLC, volume and every indicator's value at the hovered bar; Save
  Image… (rendered by the chart library, so it works headless too) and Open Option Chain.
  Intraday bars are shown in local time. A **Draw** toolbar adds trend lines (drag, or
  click-move-click; the line extends to the right as a dashed ray) and shaded
  **Support** / **Resistance** zones (drag vertically between two prices; green and red
  fills with the price range labelled). **Edit** selects a drawing to drag its handles,
  edges or body; Delete removes it, Undo removes the last one, Clear removes all, and Esc
  returns to the cursor. Right-clicking anywhere on the chart opens a menu: on a drawing it
  offers to delete that drawing, and always offers Delete all drawings and Reset chart view.
  The **Reset** button under the chart reloads the bars, restores autoscale and the default
  zoom and returns to the cursor tool (drawings are kept). Drag the price axis to stretch
  or compress the scale (the indicator pane's axis too); an **Auto** button then appears on
  the axis to return to automatic scaling, double-clicking an axis resets it, and loading
  another symbol or timeframe always starts from autoscale. Drawings are anchored to bar time and price, so they survive
  scrolling, zooming and timeframe changes, are saved per symbol, and are included in
  Save Image…. The drawing layer is the app's own canvas over the chart; Lightweight
  Charts itself has no drawing tools.
- **Pricer**: market and contract inputs (spot or futures, European or American, negative
  rates allowed, expiry as years or as a date with a day-count convention, cash dividends,
  rate curve), prices with intrinsic/time value or early-exercise premium, eleven Greeks,
  probability metrics, implied volatility, and an engine cross-check (two tree sizes,
  Bjerksund-Stensland, finite differences, Monte Carlo).
- **Strategy**: legs table with presets, net premium, max profit/loss, breakevens,
  probability of profit, net Greeks, a P&L chart (at expiry, today, and N days forward)
  and a Greek-versus-spot chart.
- **Scenarios**: heatmap of P&L, value or a Greek across spot and either volatility
  shifts or days elapsed, for the strategy or the pricer's contract.
- **Option Chain**: live download from Massive.com, CSV import or synthetic sample,
  implied vols per strike, arbitrage flags, SVI fit per expiry, smile and term-structure
  charts, hand-off to the pricer and to strategy legs. A strike filter (all, a band
  around the forward, or a custom min/max) limits the table and smile chart; the SVI fit
  always uses every quote. The expiry title and underlying price sit above the table and
  stay fixed on one compact line (slice, logo, price and change, provenance, Hide Charts;
  full details in tooltips); the column header is part of the table and stays fixed while
  rows scroll.
  Columns are resizable (double-click a divider to fit). Drag the grip under the table or
  use Hide Charts to see more strikes; the chart pane scrolls when squeezed. Hovering a
  point on the smile or term-structure chart shows its strike, implied vol, quote
  details and fitted vol both as a tooltip and in the readout under the chart.

- **Heatmap**: activity view of the loaded chain. A strike x expiry grid coloured by
  volume, open interest, both, or turnover; summary cards (call/put volume and open
  interest, put/call ratios, busiest expiry and strike, mean parity gap); a ranked table
  of the most active contracts with implied vol and per-contract parity gap; and a
  put-call parity table per expiry with the implied forward, the dividend yield that
  reconciles it, and the worst-offending strike. Double-click a cell or row to price it.

- **3D volatility surface** (Option Chain tab): the fitted surface as a rotatable, zoomable 3D view
  (strikes 70% to 130% of spot × every fitted expiry, coloured by vol, spot marked, hover readout),
  rendered in software so it follows the theme and appears in screenshots. **Greeks over time**
  (Strategy tab): the Greek chart switches between across-spot and a calendar view that plots any
  Greek day by day to the first expiry at spot and at ±5% / ±10% moves.
- **Optimizer**: strategy comparison and optimisation on the current ticker's chain. State a view
  (target price or % move at the first expiry, and the uncertainty around it, defaulting to the ATM
  implied vol), pick an objective (expected P&L under the view, P&L at the target, probability of
  profit under the view, return on risk, or probability-weighted return on risk) and a family
  (vertical spreads, condors and butterflies, straddles and strangles, single options, calendars,
  share-based structures, risk reversals, or all). **Optimize** enumerates every strike combination
  within ±20% of spot on every expiry in the window and lists the best thirty; **Compare presets**
  builds one delta-targeted candidate per preset on one expiry. The table shows credit or debit, max
  profit and loss, market and view probabilities of profit, expected P&L under the view, P&L at the
  target, return on risk, Greeks and score; selected rows overlay on a payoff chart with spot and
  target marked and a crosshair readout. Candidates open in the Strategy tab or book into the
  Portfolio. The assistant runs it too ("optimize for a move to 350", "compare presets").
- **IV rank and percentile** (Volatility tab): every stored option chain writes the day's 30-day
  constant-maturity ATM implied vol into the chain store's `iv_history` table, so a history
  accumulates for every watchlist ticker as the app runs. **Backfill IV History** rebuilds about a
  year at once from historical option bars (one at-the-money call/put pair per month, priced
  from daily closes against the stock's closes, about three requests per month). The IV rank
  card shows where today's IV30 sits between the one-year low and high (with their dates); the IV
  percentile card shows the share of days below it, with the median and mean; the realized-vol
  history chart gains an "Implied σ (30d)" line. The Trade Ideas market scan has an IV rank
  column, and the assistant answers "what is the IV rank" and runs the backfill on request.
- **Earnings overlay** (Quotes chart, the **Earnings cone** checkbox and the **Earnings ▾** menu): the option-implied earnings move as a
  cone drawn from the last bar through the report date. The at-the-money implied-vol term
  structure is converted to total variance per expiry; the variance the expiry after the
  event carries over the post-event forward vol (the diffusive baseline) is the event
  variance, whose square root is the one-standard-deviation earnings move. The cone is
  asymmetric: the beat side scales with 25-delta call vol, the miss side with 25-delta put
  vol, so a put-skewed name shows a deeper miss than beat. ±1 sd solid, ±2 sd dashed, the
  event marked on the band, beat and miss targets as axis labels, and a caption with the
  numbers. The report date comes from the Benzinga calendar when the plan carries it, is
  otherwise projected from the filing cadence, can be pinned by hand, or is inferred from
  the term structure when nothing else is known. The assistant answers "what is the
  implied earnings move" and pins dates ("earnings for NVDA on 2026-10-30").
- **Trade Ideas**: a scanner over the option chains already in memory (every stored
  chain, the watchlist or the current ticker). The market-scan table shows each chain's
  ATM implied vol at the chosen tenor, the term slope to the next tenor, 25-delta put skew,
  the straddle-implied one-sigma move, put/call volume and open-interest ratios and the
  median bid-ask spread near the money. Screens (Premium selling, Directional debit,
  Volatility, Income on shares, All strategies) pick the strategy family; strikes are
  delta-targeted on the listed ladder (30-delta shorts, 15-delta wings), legs are marked at
  chain mids with the chain's implied vols, and every candidate is analysed for credit or
  debit, max profit and loss, breakevens, probability of profit at expiry and return on
  risk, then ranked by probability-weighted return on risk discounted for wide markets.
  Filters: expiry window, minimum probability, minimum return on risk, maximum spread,
  minimum open interest, bias, defined risk only. Double-click an idea to open it in the
  Strategy tab, or book it straight into the Portfolio. The assistant runs scans too
  ("scan for premium selling ideas on the watchlist").
- **Portfolio**: the book, organised as named **strategies**. Each strategy is its own
  book of positions (New, Save as, Rename, Delete under **Books ▾**; every position shows
  its strategy; the Add dialog, CSV import and the assistant can target a strategy by
  name), and **All strategies** is the global portfolio that brings every book together
  with a per-strategy breakdown in the risk panel: value, P&L, Greeks and each strategy's
  standalone VaR against the whole book, i.e. the diversification benefit. P&L and risk
  run on whichever view is selected. Stock and option positions across underlyings (Add/Edit
  dialog with the trade date, Import Strategy legs, CSV import/export, saved in the preferences), marked from
  the bulk snapshot (shares) and the in-memory option chains (contract mids and implied
  vols, falling back to the model), with per-position and aggregate delta, gamma $,
  vega, theta and P&L; a what-if row re-values the book for a spot shock, a vol shock and
  days passing. Every trade carries its own currency (a **Ccy** column; the Add dialog,
  CSV and the assistant accept any ISO code) and the book reports in a chosen
  **reporting currency** (USD, EUR, GBP, JPY, CHF, CAD, AUD, …): values, P&L, Greeks and
  risk are converted at vendor forex rates (overridable under **FX rates…**) and shown as
  signed currency amounts such as `+$1,234` or `−€567`, while entry and mark prices stay
  in the trade currency. **Risk** (`Pricing/Risk.h`, unit tested): parametric delta-gamma VaR and
  expected shortfall with component VaR by underlying, historical simulation with full
  revaluation over a year of aligned daily returns, Monte Carlo with correlated
  (Cholesky) log-normal returns, at 95/97.5/99% over 1–60 days; a spot × vol stress grid,
  a time-decay ladder and the simulated P&L distribution with VaR and CVaR marked. The
  assistant reads and edits the book through `get_portfolio`, `add_position`,
  `remove_position` and `run_portfolio_risk`.
- **Alerts**: rules on any ticker — price above/below, day change above/below (percent),
  ATM implied vol above/below (from the loaded surface or a stored chain), and any
  numeric TA-Lib indicator above/below a level (computed from daily bars with the live
  quote as the last close). Rules are checked on every watchlist refresh, once a minute
  for symbols outside the watchlist, and on **Check now**; a trigger shows an in-app
  banner (click it to load the ticker everywhere), a macOS notification, a sound and,
  optionally, a spoken sentence, and is logged; rules disarm until **Re-arm** or re-arm
  themselves when marked repeat. Rules persist in the preferences. Spoken or typed:
  “alert me if NVDA goes above 240”, “notify me when AAPL drops 3%”, “alert me when the
  RSI on TSLA is above 70”, “alert me when SPY implied vol goes above 25%”; the assistant
  also has `create_alert`, `list_alerts` and `delete_alert`.
- **Chart layouts, comparisons and templates** (Quotes tab). A **Layout** selector shows
  one to four charts: the main chart plus secondary panes (`UI/ChartPane`), each with its
  own symbol or **Link**ed to the main symbol (a second timeframe of the same stock) and
  its own timeframe; they share indicators, chart style and theme, reload intraday bars on
  the refresh interval, and **Main ↗** promotes a pane's symbol to the whole app
  (`quotes/chartLayout`, `quotes/chartPanes`). **Compare** overlays up to five symbols
  (quick picks SPY, QQQ, IWM and the app-wide ticker) as lines fetched on the chart's
  timeframe; the price axis switches to percentage mode so every series is the change from
  the first visible bar, and the legend shows each symbol's move (`quotes/compare`).
  **Templates** in the Indicators menu apply a named set in one step: built-in Trend
  following, Momentum, Mean reversion, Volume & strength and Clean chart, plus sets saved
  from the current chart (`quotes/indicatorTemplates`). The assistant understands "compare
  with SPY", "show 4 charts" and "apply the momentum template". Charts that share the
  space use a compact legend (symbol, timeframe, close and change), and **View ▸ Text
  Size** (Compact 85% … Large 110%, ⌘- / ⌘=) scales every interface and chart font
  (`ui/textScale`) for laptop screens or large monitors.
- **Sector Heatmap**: a market treemap in the style of professional terminals. Some 180
  large caps are grouped into sectors (Internet, Software, Semis, Hardware, Consumer,
  Healthcare, Finance, Energy, Industrials, Utilities, Materials, Real Estate); every tile
  is sized by market capitalisation and coloured on a red–green ramp by performance over
  the selected period (Daily, 1W, 30D, 90D, YTD), with a legend giving the ramp's range.
  A **Sectors** view collapses each sector to one cap-weighted tile; clicking a sector
  tile (or a sector's title band in the Stocks view) expands that sector's stocks across
  the whole map, and **Reset** returns to the full map. The **Watchlist** universe shows
  the active watchlist grouped by industry instead. Prices come from the
  Massive.com bulk snapshot, period reference closes from grouped daily aggregates (with a
  per-ticker fallback), and market caps from ticker details, cached for two weeks in
  `sector-heatmap-caps.json`. Hover a tile for name, cap, last and change; click to load
  the ticker everywhere and open a **pop-out chart** (`UI/ChartPopup`), an exact copy of the
  Quotes chart with its timeframe, indicators, drawings and earnings cone, kept in sync by
  mirroring every script the Quotes tab sends to its chart page; double-click to open its
  option chain. The layout is a squarified
  treemap (`Pricing/Treemap.h`, unit tested) painted natively. The assistant reads it
  through `get_sector_heatmap`.
- **Volatility**: realized and forecast volatility for a ticker from daily bars (Massive
  aggregates, one to ten years, or a simulated sample path). Five realized estimators
  (close-to-close, Parkinson, Garman-Klass, Rogers-Satchell, Yang-Zhang) over a short and
  a long window; a volatility cone (10/20/30/60/90/120/252-day windows with the current
  reading, its percentile rank and the historical range); RiskMetrics EWMA (λ 0.94); and a
  GARCH(1,1) or GJR-GARCH(1,1) fit by Gaussian quasi-maximum likelihood with ω, α, β, γ,
  persistence, half-life, log-likelihood, AIC and BIC. Charts show the rolling realized,
  EWMA and GARCH conditional vol history, the cone with the chain's ATM implied vols
  overlaid, and the GARCH forecast term structure against implied vols by expiry. Cards
  compare realized, EWMA, next-day and horizon forecast, long-run and implied ATM vol with
  the IV − RV and IV − forecast spreads. **Use Realized as σ** and **Use Forecast as σ**
  push the chosen vol into the market inputs. The tabs are linked both ways: a chain
  downloaded anywhere switches the Volatility tab to that ticker, and Fetch History here
  also downloads the ticker's option chain, which in turn updates the Quotes watchlist,
  Heatmap and Strategy tabs.
  Controls and cards sit in a left sidebar; every pane (sidebar, history chart, cone
  table, cone and forecast charts) and the table columns are resizable, and the layout is
  remembered between sessions.

## Assistant (AI and voice)

An **Assistant** dock sits beside every tab; the **✦ Assistant** button in the header (or
⌘⇧A) collapses and expands it, and the tabs take the freed width. Three providers are
supported from one client (`UI/AssistantClient`, QtNetwork, no SDKs), with the
conversation kept in a provider-neutral form so switching keeps the history:

| Provider | Endpoint | Credentials | Models |
|----------|----------|-------------|--------|
| Anthropic | Messages API | `ANTHROPIC_API_KEY` or AI Key… | `claude-sonnet-5` (default), Opus 5.5, Fable 5.1, Haiku 4.5 |
| OpenAI | Chat Completions | `OPENAI_API_KEY` or AI Key… | `gpt-5`, `gpt-5-mini`, `gpt-4.1`, `gpt-4o`, … (↻ lists your account's chat models) |
| Ollama (local) | `http://localhost:11434/v1` (OpenAI-compatible) | none; Server… sets the address | ↻ lists the models installed on this Mac |

A key entered in the panel (for the session, or remembered) takes precedence over the
environment variable, so a stale shell key never shadows one you typed in.
Tool calling works with all three; chart images are attached only to models that accept
them (Anthropic, OpenAI; Ollama vision models such as `llama3.2-vision`). Every request
carries a *screen context* block describing
the active tab (figures as CSV, the chain's per-expiry ATM vols, the Quotes chart's recent
bars, indicator settings and drawings) and, on the Quotes tab, an image of the chart, so
the analysis is grounded in what is displayed. **Analyze this screen** (⌘⇧L) asks for a
summary of the current tab.

The model can act through tools: `show_ticker` (loads a symbol everywhere), `switch_tab`,
`get_bars`, `draw_trend_line`, `draw_zone` (shaded support/resistance), `clear_drawings`,
`list_drawings`, `set_timeframe`, `set_chart_type`, `list_indicators`, `add_indicator`,
`remove_indicator`, `set_indicators`, `get_option_chain`, `get_volatility`,
`set_market_volatility`, `load_strategy_preset`, `get_strategy`, `get_pricer`,
`set_pricer_contract`, `get_heatmap`. "Mark support and resistance on this chart" therefore
produces real zones and trend lines on the chart, persisted like hand-drawn ones.

Replies are rendered as analyst notes: a headline, a **Key levels** table (numeric cells
right-aligned, signed changes coloured), Technicals / Options and volatility / Risks /
Next steps sections, and a highlighted **Bottom line**. The panel's renderer handles
Markdown headings, pipe tables, nested lists, rules, bold, italic and code, and re-colours
the whole transcript when the theme changes.

**Text or voice.** Type in the box at the bottom of the panel and press Enter (Shift+Enter
inserts a line break) or click Send; ⌘⇧K opens the panel and focuses the box from any tab.
Typed text is kept if the assistant is still busy. While a request is in flight a diamond
spins next to the status line and in the header beside the ✦ Assistant button (so it is
visible with the panel collapsed), and Send is disabled until the reply arrives. The microphone button (⌘⇧V) dictates through Apple's Speech framework (the
system asks for microphone and speech-recognition permission once; the usage strings are
in `OptionPricing/Info.plist`, embedded in the binary). Dictation ends after a short pause
and is sent. Replies can be read aloud (**Speak replies**, AVSpeechSynthesizer). Simple
navigation commands are parsed locally and need no AI key: "pull up option chains for
AAPL", "chart NVDA", "switch to the volatility tab", "show volatility for TSLA", "clear
drawings". Anything else goes to the model.

The headline banner shows the ticker, company name and logo, the spot and daily change,
and a one-line description of the business (industry classification and the first
sentence of the issuer's description from Massive's reference data, cached on disk next
to the logo); hovering it shows the full description and the price provenance (parity
versus vendor, timestamps, delay, contract count).

The **File** menu saves and opens workspace files (`.optws`, JSON), imports chains,
batch-prices a CSV and exports or copies the current tab as CSV. The **Market** menu
fetches live data and edits the zero-rate curve.

## Live market data (Massive.com)

The Option Chain tab and the Market menu pull data from the Massive.com REST API
(formerly Polygon.io) through `UI/MarketDataClient`:

| Action | Endpoint | Effect |
|--------|----------|--------|
| Fetch Live Chain | stock snapshot, option chain snapshot (paged, filtered by expiration date) | Sets the market spot and replaces the chain with every unexpired expiry, or only the N nearest when a count is set. Each quote carries its expiration date and days to expiry (DTE); the expiry selector, smile title, fit summary and CSV export show them, and double-clicking a strike puts that calendar date into the Pricer. |
| Quotes watchlist | `/v2/snapshot/locale/us/markets/stocks/tickers?tickers=…` | One bulk request per refresh; last price is the latest minute bar close, with change and percent change versus the previous close. |
| Quotes chart | `/v2/aggs/ticker/{T}/range/{mult}/{timespan}/{from}/{to}` (paged) | Minute, hour, day or week bars for the selected timeframe. |
| Volatility history | same aggregates endpoint, daily bars | One to ten years of OHLC for the realized-vol estimators and the GARCH fit. |
| Treasury Curve | `/fed/v1/treasury-yields` (latest row) | Loads the zero-rate curve and enables it. Bond-equivalent yields are converted to continuous compounding. |
| Dividends | `/v3/reference/dividends` | Projects the latest regular cash dividend forward at its cadence for three years into the Pricer's cash-dividend schedule and zeroes the continuous yield. |

The API key is read from `MASSIVE_API_KEY` or `POLYGON_API_KEY`. When the app is launched
from Xcode or Finder those variables are usually absent, so **Market > Set Massive API
Key** accepts a key for the session, optionally remembered (unencrypted) in the
application preferences. The key is only ever sent to `api.massive.com`.

### In-memory chain store

Every ticker on the Quotes watchlist has its full option chain and underlying snapshot
downloaded shortly after start-up (two downloads in flight at a time) into an in-memory
SQLite database (`UI/ChainStore`, QtSql with the bundled SQLite driver; tables `chains`
and `contracts`, about 0.5 MB per large-cap chain). Highlighting a ticker on the Quotes
watchlist, opening it from the Heatmap or Volatility tab, or fetching it on the Option
Chain tab applies the stored chain instantly and cascades it to every tab through the
shared market state; chains older than ten minutes are refreshed in the background and
re-applied when they arrive. Tickers added to the watchlist are preloaded as well, and
chain downloads made on the Option Chain tab (all expiries) are stored too. The status
bar reports preload progress and the store's size.

The store is persisted between sessions: on exit (and after each preload pass) the
in-memory database is written to `~/Library/Application Support/Luis Molina/Option
Pricing/chains.sqlite` with SQLite's `VACUUM INTO`, including the watchlist's last
underlying prices. On launch it is read back so the Quotes watchlist shows the previous
prices and every stored chain is available immediately; times to expiry are re-measured
from today and expired contracts dropped. Everything older than ten minutes is then
refreshed in the background, and tickers without a saved chain are downloaded from
Massive.com. Deleting the file simply forces a full reload.

### Refresh and delay

- **Auto-refresh** on the Option Chain tab re-downloads the chain and re-checks the
  underlying on separate timers (defaults: chain every 120 s, spot every 60 s). Refreshes
  keep the table's scroll position and leave the controls enabled.
- Massive's stock prices are delayed on plans without real-time entitlement (about 15
  minutes at the time of writing; the real-time last-trade and NBBO endpoints return
  "not entitled"). Option snapshots, however, arrive within about a minute. The spot
  label therefore shows the vendor price with its own timestamp and delay in minutes.
- **Underlying from option parity** (on by default) derives a near real-time spot from
  the option market: the median of `K + (C − P)·e^{rT}` over the strike pairs within 6%
  of spot on the nearest expiry with at least two days to run, discounted back to today
  and adjusted for dividends before that expiry. The label reports the expiry, pair count
  and dispersion used. Untick the box to fall back to the vendor's delayed price. A
  hand-entered spot on the Pricer tab overrides both until the next fetch.
- True tick-level streaming would need Massive's WebSocket feeds and a real-time plan.
- **One quote everywhere.** The shared market state holds the underlying's spot (parity
  policy), vendor price, previous close and timestamps, and every display reads it: the
  headline banner, the Option Chain header, the watchlist row of the app-wide ticker (which
  shows the headline figures rather than the raw vendor snapshot, with the vendor price in
  its tooltip) and the Quotes chart legend, which adds a "Live" readout and a dashed price
  line at the shared spot (a single price line: the live quote when there is one, else the
  last close; the **Price line** checkbox or the right-click menu hides it). Changes are always computed as last minus the same previous
  close; the vendor's own change fields are not used. The watchlist refresh and the chain
  tab's spot timer both feed the state (newest timestamp wins), and when daily bars are
  loaded for the ticker the previous close is pinned to the last completed session's close,
  which is more reliable than the vendor's prevDay around the overnight roll. Daily bars are
  stamped at midnight New York time, so their session dates are read in that zone (a
  local-time reading in a western zone would label today's partial bar as yesterday). Chart legend
  changes for daily and weekly bars are measured against the prior bar, not the first bar
  of the series.

Notes on the data:

- Plans without options quotes return no bid/ask; the app then uses the last trade or
  day close as the mid and says so in the status line. Implied vols for illiquid strikes
  can be stale in that case.
- Listed equity options are American-style; the chain's implied vols use the European
  model, which is standard practice but slightly overstates put vols for dividend payers.
- The chain download is paged 250 contracts per request. A full AAPL chain is roughly
  a dozen requests; lower-tier plans are rate limited and a 429 is reported with a
  suggestion to reduce the expiry count.
- Massive also publishes an MCP server at `https://mcp.massive.com/` for AI assistants.
  It is a separate integration from this application, which uses the REST API directly.

```sh
OptionPricing --live-smoke AAPL
```

downloads the chain, curve, dividends, watchlist quotes, two years of daily bars for the
volatility tab (printing the realized, EWMA, GARCH and implied figures) and a year of
daily bars for the chart, prints a summary, renders the live tabs to `$TMPDIR/optshots-live/` (plus
`chart.png` exported by the chart library itself) and exits. `--window WxH` fixes the
window size for either flag.

## Third-party code

| Component | Location | Licence |
|-----------|----------|---------|
| TradingView Lightweight Charts 4.2.3 | `ThirdParty/lightweight-charts/` | Apache 2.0 |

The chart bundle is embedded in the binary as a string constant
(`OptionPricing/UI/LightweightChartsJs.inc`). After upgrading the library, regenerate it:

```sh
python3 Tools/embed_chart_bundle.py
```

The Apache licence requires the TradingView attribution logo the library draws in the
chart corner to stay visible. TradingView's commercial Advanced Charts product is a
separately licensed download and is not used here.

## CSV formats

### Option chain

Required columns (case-insensitive, aliases accepted): `strike`, `type` (`call`/`put` or
`C`/`P`), and either `bid` + `ask` or a single `mid`/`last`/`price`. Expiry comes from
an ISO date column (`expiration`, `expiry date`), `expiry` (years), `days`/`dte`, or the
"Default expiry" field when absent. Dates are measured from today on ACT/365. Optional:
`volume`, `open interest`. See `Samples/sample_chain.csv`.

### Batch pricing

Columns: `label` (optional), `type`, `strike`, and `expiry` (years) or `days` are
required. `spot`, `rate`, `dividend`, `vol` fall back to the Pricer tab's inputs when
missing; values above 1 are treated as percentages. Optional `exercise` (`american`) and
`model` (`futures` for Black-76). See `Samples/batch_contracts.csv`.

## Conventions

- Rates, yields and volatilities are continuously compounded decimals internally and
  percentages in the UI.
- Vega, rho, vanna and zomma are quoted per 1 percentage point. Volga is the change in
  vega (per 1%) for a 1-point move in volatility.
- Theta, charm and color are per day; the basis (365 calendar or 252 trading days) is a
  Pricer setting.
- Probabilities and expected values are risk-neutral.
- Realized and GARCH volatilities are annualised with 252 trading days. GARCH is fitted
  on demeaned daily log returns with Gaussian quasi-likelihood; the leverage term γ in
  GJR-GARCH is constrained to be non-negative and persistence to be below one. The
  forecast term vol for a horizon is the square root of the average forecast variance over
  that many trading days, which is the quantity comparable to an implied vol.
