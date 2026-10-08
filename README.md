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

## Building

The project is an Xcode command-line tool target that links Homebrew's Qt 6
(`/opt/homebrew/opt/qt`). Install Qt with `brew install qt`, open
`OptionPricing.xcodeproj` and build the `OptionPricing` scheme. Run the tests with the
`PricingTests` scheme or:

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

## Tabs

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
  always uses every quote. Drag the grip under the table or use Hide Charts to see more
  strikes.

- **Heatmap**: activity view of the loaded chain. A strike x expiry grid coloured by
  volume, open interest, both, or turnover; summary cards (call/put volume and open
  interest, put/call ratios, busiest expiry and strike, mean parity gap); a ranked table
  of the most active contracts with implied vol and per-contract parity gap; and a
  put-call parity table per expiry with the implied forward, the dividend yield that
  reconciles it, and the worst-offending strike. Double-click a cell or row to price it.

The **File** menu saves and opens workspace files (`.optws`, JSON), imports chains,
batch-prices a CSV and exports or copies the current tab as CSV. The **Market** menu
fetches live data and edits the zero-rate curve.

## Live market data (Massive.com)

The Option Chain tab and the Market menu pull data from the Massive.com REST API
(formerly Polygon.io) through `UI/MarketDataClient`:

| Action | Endpoint | Effect |
|--------|----------|--------|
| Fetch Live Chain | stock snapshot, option chain snapshot (paged, filtered by expiration date) | Sets the market spot and replaces the chain with every unexpired expiry, or only the N nearest when a count is set. Each quote carries its expiration date and days to expiry (DTE); the expiry selector, smile title, fit summary and CSV export show them, and double-clicking a strike puts that calendar date into the Pricer. |
| Treasury Curve | `/fed/v1/treasury-yields` (latest row) | Loads the zero-rate curve and enables it. Bond-equivalent yields are converted to continuous compounding. |
| Dividends | `/v3/reference/dividends` | Projects the latest regular cash dividend forward at its cadence for three years into the Pricer's cash-dividend schedule and zeroes the continuous yield. |

The API key is read from `MASSIVE_API_KEY` or `POLYGON_API_KEY`. When the app is launched
from Xcode or Finder those variables are usually absent, so **Market > Set Massive API
Key** accepts a key for the session, optionally remembered (unencrypted) in the
application preferences. The key is only ever sent to `api.massive.com`.

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

downloads the chain, curve and dividends for a ticker, prints a summary and exits.

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
