//
//  HelpContent.cpp
//  OptionPricing
//

#include "HelpContent.h"

namespace help {

namespace {

const char* kQuotes = R"html(
<h2>Quotes: watchlist and chart</h2>
<p>The Quotes tab is the market desk: a watchlist of tickers with live prices on the left and an
interactive price chart with technical indicators, drawings and the earnings overlay on the right.
Selecting a ticker here makes it the app-wide ticker, so the Option Chain, Strategy, Volatility and
other tabs follow.</p>

<h3>How to use</h3>
<ol>
<li><b>Build a watchlist.</b> Type one or several symbols in the box and press <i>Add</i>, or use the
<b>Lists</b> menu to create a list from the clipboard, import a text/CSV file, save the current list under
a name, rename or delete it. The <b>Watchlist</b> selector switches between saved lists; option chains
for the list are preloaded into memory in the background.</li>
<li><b>Select a row</b> to load its chart; double-click (or <i>Open Option Chain</i>) to jump to the chain.
Quotes refresh automatically (interval and auto-refresh switch below the table).</li>
<li><b>Pick a timeframe</b> (1m … 1H intraday, 1D one year of daily bars, 1W five years of weekly bars) and a
chart style (candlesticks, bars, Heikin-Ashi, line).</li>
<li><b>Add indicators</b> from the <b>Indicators</b> menu, grouped by TA-Lib category. Overlap studies (moving
averages, Bollinger Bands, SAR) draw over price; oscillators (MACD, RSI, Stochastics, ATR …) open their own
panes below the volume; candlestick patterns mark bars with arrows. Each indicator has an Edit / Remove
submenu; the set is remembered for the next launch. Drag the handle above the panes to resize them.</li>
<li><b>Draw</b> trend lines, support and resistance zones with the Draw toolbar; <i>Edit</i> moves them,
right-click deletes one, and drawings are saved per symbol.</li>
<li><b>Earnings cone</b> (checkbox and the <b>Earnings</b> menu): the option-implied move through the next
report date. See “Understanding the analysis”.</li>
<li><b>Save Image…</b> renders the chart with its panes and drawings to a PNG; <i>Reset</i> restores the default zoom.</li>
</ol>

<h3>Understanding the analysis</h3>
<ul>
<li><b>Legend</b>: hovering shows open, high, low, close, volume and every indicator's value at that bar; the
change is measured against the previous bar while hovering and against the previous session's close otherwise.</li>
<li><b>Price line</b>: the live quote (parity-implied from fresh option prices when available, otherwise the
delayed feed) or the last close.</li>
<li><b>Earnings cone</b>: the implied-vol term structure of the option chain is converted to total variance per
expiry. The variance the expiry after the report carries over the forward vol of the following window is the
<i>event variance</i>; its square root is the one-standard-deviation earnings move. The <span style="color:#22c55e">green</span>
upper band (“beat”) is scaled by 25-delta call vol and the <span style="color:#ef4444">red</span> lower band (“miss”) by
25-delta put vol, so a put-skewed name shows a deeper miss than beat. Solid bands are ±1 sd, dashed ±2 sd;
the marker sits on the report day and the axis labels give the price targets. The report date comes from the
Benzinga calendar, can be pinned by hand, or is inferred from the term structure when the calendar is silent or
disagrees with the options.</li>
<li><b>Indicator panes</b> show reference levels where they are meaningful (RSI 30/70, zero lines); values are
computed by TA-Lib on the loaded bars, so they change with the timeframe.</li>
</ul>

<h3>Tips</h3>
<ul>
<li>Ask the assistant: “add a 50-day SMA”, “show RSI”, “load the energy watchlist”, “what is the implied earnings move”.</li>
<li>Intraday bars are shown in your local time; daily and weekly bars by date.</li>
</ul>
)html";

const char* kPortfolio = R"html(
<h2>Portfolio: positions, P&amp;L and risk</h2>
<p>The book. Stock and option positions across underlyings, organised into named <b>strategies</b>, marked
from live data, with aggregate Greeks, a what-if shock row and portfolio risk: VaR, expected shortfall,
stress tests and a time-decay ladder.</p>

<h3>How to use</h3>
<ol>
<li><b>Choose a strategy book</b> in the <i>Strategy</i> selector, or <i>All strategies</i> for the global portfolio.
The <b>Books</b> menu creates a new strategy, saves the current view as a new one, renames or deletes.</li>
<li><b>Add positions</b> with <i>Add…</i> (symbol, strategy, trade date, type, currency, quantity, strike, expiry, entry
price, implied vol), <i>Import Strategy legs</i> from the Strategy tab, or <i>Import CSV…</i>
(symbol, type, quantity, strike, expiry, entry, iv %, currency, strategy, trade date). Quantities are signed:
negative is short. Options use a 100 multiplier.</li>
<li><b>Refresh marks</b> re-prices shares from the bulk snapshot and options from the stored chains (chain mid, or
the model when a contract is not listed). The <i>Mark source</i> column says which.</li>
<li><b>Reporting currency</b>: pick USD, EUR, GBP … in the selector; vendor FX rates convert positions booked in other
currencies, and <i>FX rates…</i> lets you override them. Every amount shows the currency sign with the sign in front.</li>
<li><b>What if</b>: shock spot (%), vol (points) and days and read the P&amp;L and the new Greeks.</li>
<li><b>Run risk</b> with a confidence level, horizon and Monte Carlo path count. The results fill the VaR table, the
by-strategy and by-underlying breakdowns, the stress grid, the decay ladder and the distribution chart.</li>
</ol>

<h3>Understanding the analysis</h3>
<ul>
<li><b>Delta per 1%</b>: P&amp;L for a 1% move in every underlying (delta × spot / 100). <b>Gamma per 1%</b>: how that delta
changes for a 1% move. <b>Vega</b>: P&amp;L for one volatility point. <b>Theta</b>: P&amp;L per calendar day.</li>
<li><b>Parametric VaR</b> (delta-gamma-normal): the book is approximated by its delta and gamma to each underlying;
returns are joint-normal with the covariance estimated from a year of daily closes. Fast, assumes normality.</li>
<li><b>Historical VaR</b>: every position is fully revalued under each historical window of returns (overlapping
windows for multi-day horizons). No distribution assumed; limited to what the past year contained.</li>
<li><b>Monte Carlo VaR</b>: correlated lognormal returns are simulated (Cholesky factor of the covariance), the book is
fully revalued on every path. The distribution chart shows the simulated P&amp;L with the VaR and CVaR lines; hover it
for the bin, its share of paths and the probability of a worse outcome.</li>
<li><b>VaR</b> is the loss not exceeded with the chosen confidence over the horizon; <b>CVaR</b> (expected shortfall)
is the average loss in the tail beyond VaR. <b>Component VaR</b> allocates the parametric VaR to underlyings so the
pieces add up to the total.</li>
<li><b>By strategy</b> (global view): each book's standalone VaR against the whole-book VaR; the difference is the
<i>diversification benefit</i>.</li>
<li><b>Stress grid</b>: full revaluation under combined spot shocks (rows) and relative vol shocks (columns) at
unchanged time. <b>Decay ladder</b>: P&amp;L from time passing alone.</li>
</ul>

<h3>Tips</h3>
<ul>
<li>The assistant can add and remove positions, switch or create strategy books and run risk.</li>
<li>Positions, books, currency and risk settings are saved between sessions.</li>
</ul>
)html";

const char* kPricer = R"html(
<h2>Pricer: a single contract</h2>
<p>Price one option with full Greeks, probability metrics and implied volatility, and cross-check the closed
form against independent numerical engines. The Pricer is also the editor for the shared market inputs
(spot, rate, dividend yield, volatility, dividends) that every other tab uses.</p>

<h3>How to use</h3>
<ol>
<li><b>Market inputs</b>: spot (or futures price under Black-76), risk-free rate, dividend yield, volatility, discrete
dividends. <i>Treasury Curve</i> on the Option Chain tab fills the rate curve so each maturity uses its own rate.</li>
<li><b>Contract</b>: underlying model, exercise style, type, strike, expiry as years or as a date with a day-count
convention, and the theta basis.</li>
<li><b>Read the results</b>: call and put prices, first- and second-order Greeks, probability metrics.</li>
<li><b>Implied volatility</b>: enter a market price and type; the solver returns the vol that reproduces it and reports
when the price is outside the no-arbitrage bounds.</li>
<li><b>Cross-check</b>: re-prices with a binomial tree (two step counts), finite differences and Monte Carlo so you
can see the engines agree and how fast they converge.</li>
<li>Send a chain contract here by double-clicking it on the Option Chain tab.</li>
</ol>

<h3>The option models</h3>
<ul>
<li><b>Black-Scholes-Merton</b> (spot asset): European prices under lognormal spot with constant volatility, a
continuously compounded risk-free rate and a continuous dividend yield. Discrete cash dividends use the
<i>escrowed-dividend</i> model: their present value is removed from spot before pricing.</li>
<li><b>Black-76</b> (futures): the same framework for a futures price; the cost of carry is zero, so the forward is the
futures price itself and discounting is the only role of the rate.</li>
<li><b>American exercise</b>: early exercise is valued with a <b>Cox-Ross-Rubinstein binomial tree</b> (recombining,
risk-neutral up/down moves; Greeks from the tree nodes), and reported next to the European value so the
<i>early-exercise premium</i> is visible. The cross-check adds <b>Bjerksund-Stensland (1993)</b>, a fast closed-form
approximation with a flat exercise boundary, and a <b>Crank-Nicolson finite-difference</b> solution of the pricing PDE
on a log-spot grid, with projected SOR enforcing the early-exercise constraint at every time step.</li>
<li><b>Monte Carlo</b>: terminal spots simulated under the risk-neutral measure with antithetic variates; the standard
error is reported with the estimate. Useful as an independent check and as a reminder of sampling noise.</li>
<li><b>Implied volatility</b>: Newton steps guarded by a shrinking bisection bracket, converging from any start because
the price is strictly increasing in volatility. Prices at or below intrinsic (discounted) value or above the
no-arbitrage ceiling are reported as such instead of a number.</li>
</ul>

<h3>Understanding the Greeks</h3>
<ul>
<li><b>Delta</b>: price change per unit of spot (also the risk-neutral hedge ratio). <b>Gamma</b>: change in delta per
unit of spot. <b>Vega</b>: price change per one volatility point. <b>Theta</b>: price change per day (calendar or trading
day, your choice). <b>Rho</b>: price change per 1% of rate.</li>
<li><b>Vanna</b>: change in delta per vol point (how a hedge drifts when vol moves). <b>Volga</b>: change in vega per vol
point (convexity to volatility). <b>Charm</b>: delta decay per day. <b>Speed</b>: change in gamma per unit of spot.
<b>Color</b>: gamma decay per day. <b>Zomma</b>: change in gamma per vol point.</li>
<li><b>P(expires ITM)</b> is the risk-neutral probability N(d₂) (calls) or N(−d₂) (puts) of finishing in the money;
<b>P(touch)</b> the probability the spot path reaches the strike before expiry. Risk-neutral probabilities are
market-implied, not forecasts.</li>
</ul>

<h3>Conventions</h3>
<ul>
<li>Day counts: ACT/365, ACT/360 or BUS/252 convert an expiry date into years. Theta can be quoted per calendar day
(÷365) or per trading day (÷252).</li>
<li>Rates are continuously compounded; the Treasury curve is converted on import.</li>
</ul>
)html";

const char* kStrategy = R"html(
<h2>Strategy: multi-leg positions</h2>
<p>Build a position from stock and option legs, see its P&amp;L profile at expiry, today and at any date in
between, its net Greeks, breakevens, maximum gain and loss and probability of profit. With an option chain
loaded, legs snap to listed strikes and expiries and are marked at chain mids with the chain's implied vols.</p>

<h3>How to use</h3>
<ol>
<li><b>Load a preset</b> (long call, covered call, spreads, straddles, strangles, iron condor, butterfly, calendars,
risk reversal …) at a chosen expiry, or add legs one by one with <i>Add Leg</i>. Edit any cell in the table:
kind, quantity (negative = short), strike, expiry, vol, entry price.</li>
<li><b>Reprice Entries</b> sets entry prices to the current model price or chain mid, so the position starts at zero P&amp;L.
<i>Apply Vol Surface to Legs</i> gives each leg the fitted implied vol of its strike and expiry.</li>
<li><b>Read the cards</b>: net premium (debit or credit), max profit, max loss, breakevens, probability of profit and
expected P&amp;L at the first expiry; the Greeks block below.</li>
<li><b>Charts</b>: the P&amp;L chart shows the profile at expiry, today, and at the slider's future date; the Greek chart
plots any Greek across spot.</li>
<li><b>Hand-offs</b>: the Scenarios tab grids this position; the Portfolio tab's <i>Import Strategy legs</i> books it;
the Trade Ideas tab sends candidates here.</li>
</ol>

<h3>Understanding the analysis</h3>
<ul>
<li><b>Net premium</b>: positive is a debit paid, negative a credit received (per position, with the multiplier).</li>
<li><b>Max profit / loss</b> are evaluated at the first expiry across a wide spot grid, the strikes and near-zero spot;
“unbounded” appears when the payoff keeps growing (long calls, long stock) or falling (short calls) with spot.</li>
<li><b>Breakevens</b>: spot levels where the expiry P&amp;L crosses zero, refined by bisection.</li>
<li><b>Probability of profit</b> and <b>expected P&amp;L</b>: under the risk-neutral lognormal distribution at the market
volatility to the first expiry. They are market-implied figures: a fairly priced position has expected P&amp;L near zero.</li>
<li><b>Today / future-date curves</b> use the model with the legs' vols and the remaining time, so they show how time
decay and gamma reshape the profile before expiry.</li>
</ul>
)html";

const char* kScenarios = R"html(
<h2>Scenarios: grids of outcomes</h2>
<p>A colour-coded table of one metric for the Strategy position (or a single Pricer contract) across spot on
the rows and either a volatility shift or elapsed days on the columns.</p>

<h3>How to use</h3>
<ol>
<li>Pick the <b>source</b>: the Strategy tab's position, or the Pricer's contract as a long call or long put.</li>
<li>Pick the <b>metric</b> (P&amp;L, value, delta, gamma, vega, theta …) and the <b>column axis</b>: volatility shift or days elapsed.</li>
<li>Set the spot range and step, the vol shifts or day steps, and read the grid. Green cells are gains, red losses;
the colour saturates at the largest absolute value in the grid.</li>
</ol>

<h3>Understanding the analysis</h3>
<ul>
<li>Every cell is a full model revaluation of the position at that spot, with every leg's vol shifted by the column's
points (vol axis) or with the column's days removed from every maturity (time axis), minus the entry cost for P&amp;L.</li>
<li>Reading down a column shows directional exposure; across a row, vega or theta exposure. A P&amp;L grid that is
positive along the current spot row and negative at the edges is a short-volatility profile, and the reverse a long one.</li>
<li>Legs that expire within the elapsed days are valued at intrinsic value.</li>
</ul>
)html";

const char* kChain = R"html(
<h2>Option Chain: the listed market</h2>
<p>Download a ticker's full option chain from Massive.com (or import a CSV, or generate a synthetic chain), see
implied vols by strike and expiry, static-arbitrage flags, an SVI smile fit per expiry, smile and
term-structure charts, and hand any contract to the Pricer.</p>

<h3>How to use</h3>
<ol>
<li><b>Fetch Live Chain</b> for the ticker (an API key is needed once: <i>Set Key…</i>). Chains for watchlist tickers are
preloaded into memory, so switching tickers elsewhere is instant; stale chains refresh in the background.</li>
<li><b>Treasury Curve</b> loads the latest yields into the rate curve; <b>Dividends</b> projects the regular cash dividends
into the Pricer's schedule. Both improve implied vols and parity checks.</li>
<li><b>Pick an expiry</b> in the selector to see its strikes: bid, ask, mid, volume, open interest, implied vol, the SVI fit
and any arbitrage flags. <i>Use ATM Vol as σ</i> copies that expiry's at-the-money fitted vol into the market inputs.</li>
<li><b>Charts</b>: the smile (implied vol by strike with the fit) and the term structure (ATM vol by expiry); hover for values.
<i>Hide Charts</i> gives the table the whole tab.</li>
<li><b>Double-click a contract</b> to price it on the Pricer with the chain's inputs.</li>
</ol>

<h3>Understanding the analysis</h3>
<ul>
<li><b>Implied vol</b> is solved from each contract's mid under the market model with the maturity's rate. Where the
plan carries no quotes, mids are last trades and can be stale: contracts outside no-arbitrage bounds are flagged.</li>
<li><b>Static-arbitrage checks</b>: call and put prices must fall (rise) monotonically with strike and be convex in strike;
violations mark the rows.</li>
<li><b>SVI fit</b>: each expiry's smile is fitted with the raw stochastic-volatility-inspired parameterisation of total
variance in log-moneyness (five parameters), giving a smooth curve for interpolation and a fit error.</li>
<li><b>Implied spot</b>: put-call parity on a near expiry gives a forward, discounted back to spot; because option
prints are fresher than the delayed stock feed, this is the default spot while the market is open.</li>
<li><b>Parity by expiry</b> (Heatmap tab) compares the forward implied by call-put differences with the model forward;
the gap converts into an implied dividend yield.</li>
</ul>
)html";

const char* kIdeas = R"html(
<h2>Trade Ideas: how to use the scanner</h2>
<p>The scanner looks through the option chains already held in memory and builds candidate trades on the
listed strikes. Nothing is downloaded when you scan; it works on the chains the watchlist preload and the
Option Chain tab have fetched.</p>

<h3>Step by step</h3>
<ol>
<li><b>Pick a screen.</b> Each screen chooses a family of strategies and sets sensible default filters:
  <ul>
  <li><b>Premium selling</b>: bull put / bear call spreads, iron condors and iron butterflies, short strikes at about 30 delta. Credit trades that profit when the stock stays in a range.</li>
  <li><b>Directional debit</b>: bull call and bear put spreads plus outright calls and puts at the money. Defined-risk bets on direction.</li>
  <li><b>Volatility</b>: long straddles and strangles for a big move either way; calendars when near-dated vol is cheap against far-dated vol.</li>
  <li><b>Income on shares</b>: covered calls, collars and protective puts against 100 shares.</li>
  <li><b>All strategies</b>: every preset, best three per ticker.</li>
  </ul></li>
<li><b>Choose a bias</b> (Any, Bullish, Bearish, Neutral, Volatile) to keep only strategies with that view.</li>
<li><b>Choose the universe</b>: every stored chain, the Quotes watchlist, or just the ticker on screen.</li>
<li><b>Set the expiry window</b> in days. Strikes are placed on the listed expiry closest to the middle of the window, plus the next one inside it.</li>
<li><b>Tighten the filters</b> if you want fewer, stronger ideas: minimum probability of profit, minimum return on risk, maximum bid-ask spread per leg, minimum open interest per leg, and <i>Defined risk only</i> to skip naked short options and share-based structures.</li>
<li><b>Press Scan.</b> The market scan fills with one row per ticker; the ideas table lists the ranked trades.</li>
<li><b>Click a ticker</b> in the market scan to see only its ideas; <b>All tickers</b> clears the focus. Double-click a ticker to load it on every tab.</li>
<li><b>Act on an idea</b>: double-click it (or press <i>Open in Strategy</i>) to load its legs into the Strategy tab for the payoff chart, Greeks and what-if analysis; <i>Add to Portfolio</i> books the legs at the chain mids; <i>Export CSV</i> saves the table.</li>
</ol>

<h3>Market scan columns</h3>
<ul>
<li><b>ATM IV</b>: implied vol of the at-the-money straddle at the chosen expiry. <b>IV rank</b>: where that vol sits in the ticker's stored one-year history (see the Volatility tab). <b>Far IV</b> and <b>Term</b>: the same at the next tenor and the difference; a negative term slope means near-dated vol is bid, often an event.</li>
<li><b>Skew 25d</b>: 25-delta put vol minus 25-delta call vol. Positive means puts are expensive relative to calls.</li>
<li><b>1 sd move</b>: the straddle price as a share of spot, the market's one-standard-deviation move to that expiry.</li>
<li><b>P/C vol</b> and <b>P/C OI</b>: put/call ratios of volume and open interest across the chain.</li>
<li><b>Spread</b>: median bid-ask spread near the money, when the chain carries quotes.</li>
</ul>

<h3>Ideas columns</h3>
<ul>
<li><b>Legs</b>: signed contracts, type and strike (+1 P 740 = buy one 740 put). <b>Net</b>: credit received (+) or debit paid (−) for the whole position.</li>
<li><b>Max profit / Max loss</b> at the first expiry; <i>open</i> when a side is unlimited.</li>
<li><b>PoP</b>: probability of finishing profitable at expiry under a lognormal distribution at the ATM implied vol (risk-neutral, so it is a market-implied figure, not a forecast).</li>
<li><b>Return/risk</b>: max profit divided by max loss. Open-ended sides are measured two expected moves away.</li>
<li><b>Exp. P&amp;L</b>: expected profit under the same distribution; close to zero for fairly priced trades.</li>
<li><b>Breakevens</b>, <b>Delta</b> and <b>Theta/d</b> of the whole position; <b>IV</b> is the chain's ATM vol.</li>
<li><b>Score</b>: probability of profit × return on risk (capped at 300%), discounted for wide markets. Ideas are sorted by score; sort any column by clicking its header.</li>
</ul>

<h3>Good to know</h3>
<ul>
<li>Legs are marked at chain mids. Outside market hours, or when the data plan carries no quotes, mids are last trades and can be stale. Contracts whose price breaks no-arbitrage bounds, legs priced far from the ATM vol, and structures that cannot lose are dropped automatically, but always check the legs on the Option Chain tab before trading.</li>
<li>Strikes are delta-targeted: shorts near 30 delta, wings near 15 delta, snapped to the listed ladder and kept out of the money.</li>
<li>Settings persist between sessions. The assistant can run scans too: try “scan for premium selling ideas on the watchlist” or “find bullish trades on NVDA”.</li>
</ul>
)html";

const char* kHeatmap = R"html(
<h2>Heatmap: where the chain is active</h2>
<p>A strike × expiry grid of the loaded option chain coloured by volume or open interest, the most active
contracts, and put-call parity by expiry.</p>

<h3>How to use</h3>
<ol>
<li>Load a chain (Option Chain tab, or <i>Fetch Chain</i> here).</li>
<li>Choose the <b>metric</b>: volume, open interest, their sum, or turnover (volume / open interest); choose calls, puts
or both; narrow the strike band around spot if the grid is too tall.</li>
<li>Read the grid: darker cells carry more activity; hover a cell for the exact figures. The tables below rank the most
active contracts and summarise parity per expiry. <i>Hide Tables</i> gives the grid the whole tab.</li>
</ol>

<h3>Understanding the analysis</h3>
<ul>
<li><b>Volume</b> is today's traded contracts; <b>open interest</b> the contracts outstanding. High volume on low open
interest (high turnover) points at new positioning; high open interest at established positions and likely
“pinning” strikes near expiry.</li>
<li><b>Most active</b> lists each contract with its implied vol, the mid of its counterpart (same strike, other type)
and the <b>parity gap</b>: C − P − (F − K)e<sup>−rT</sup>. A persistent gap across strikes means the forward (hence
dividend assumption or rate) is off; a gap on one strike means a stale print.</li>
<li><b>Parity by expiry</b>: the median implied forward from call-put pairs versus the model forward, the implied
dividend yield that would reconcile them, the mean absolute gap and the worst strike.</li>
<li><b>Put/call ratios</b> of volume and open interest summarise positioning: well above 1 is defensive.</li>
</ul>
)html";

const char* kSectorHeatmap = R"html(
<h2>Sector Heatmap: the market map</h2>
<p>A treemap of large-cap stocks grouped by sector, each tile sized by market capitalisation and coloured by
performance over the chosen period, in the style of professional terminals.</p>

<h3>How to use</h3>
<ol>
<li>Pick a <b>period</b>: Daily (change since the previous close), 1W, 30D, 90D or YTD. The colour scale saturates at a
move that is large for that period, so a bright tile means a big move for the horizon.</li>
<li>Pick a <b>view</b>: <i>Stocks</i> shows every tile inside its sector; <i>Sectors</i> collapses each sector into one
cap-weighted tile.</li>
<li>Pick a <b>universe</b>: the built-in large-cap list or your watchlist (sectors come from the tickers' industry data).</li>
<li><b>Click a sector</b> in the Sectors view to expand it and see its stocks; <i>Reset</i> returns to the full map.
Click a stock to make it the app-wide ticker; double-click to open its option chain. <i>Refresh</i> reloads prices.</li>
</ol>

<h3>Understanding the analysis</h3>
<ul>
<li><b>Tile size</b> is market cap, so the map shows where the money is: a sector's colour is the cap-weighted
performance of its members, not a simple average.</li>
<li>Performance over the longer periods is measured against the close on the first session of the period (grouped
daily aggregates); YTD uses the last close of the previous year.</li>
<li>The breadth line at the top counts advancers against decliners and names the best and worst sectors and stocks.</li>
</ul>
)html";

const char* kVolatility = R"html(
<h2>Volatility: realized, forecast and implied</h2>
<p>Daily bars drive five realized-volatility estimators, a volatility cone, an EWMA and a GARCH or GJR-GARCH fit
with a forecast term structure, all compared with the option chain's implied vols, plus the implied-vol
history that gives IV rank and percentile.</p>

<h3>How to use</h3>
<ol>
<li>Enter a ticker and <b>Fetch History</b> (1 to 10 years of daily bars), or <i>Sample Data</i> for a simulated path. A ticker
entered here also pulls its option chain so the other tabs follow.</li>
<li>Choose the <b>estimator</b>, the short and long rolling <b>windows</b>, the <b>model</b> (GARCH(1,1) or GJR-GARCH(1,1)) and the
forecast <b>horizon</b>.</li>
<li>Read the cards: realized vol for both windows with its cone percentile, EWMA, GARCH next-day and horizon forecast,
long-run vol, persistence, the implied ATM vol for the horizon, and <b>IV rank</b> / <b>IV percentile</b>.</li>
<li><b>Backfill IV History</b> rebuilds about a year of daily implied vol from historical option bars so the rank has a
full year behind it; afterwards every stored chain adds the day's sample automatically.</li>
<li><i>Use Realized as σ</i> or <i>Use Forecast as σ</i> pushes a volatility into the market inputs for the Pricer and Strategy tabs.</li>
<li>Hover any chart for a crosshair readout of every series at that point.</li>
</ol>

<h3>Understanding the analysis</h3>
<ul>
<li><b>Estimators</b>: close-to-close uses closes only; Parkinson uses the high-low range; Garman-Klass adds open and close;
Rogers-Satchell allows a drift; Yang-Zhang combines overnight and intraday variance and is the most efficient
for stocks with gaps. All are annualised with 252 trading days.</li>
<li><b>Volatility cone</b>: for each window length, the distribution of realized vol over the history (10th, 25th,
median, 75th, 90th percentiles) against the current value; implied vols are plotted at their days to expiry, so
you can see whether options are rich or cheap against what the stock has done over comparable windows.</li>
<li><b>EWMA</b> (RiskMetrics, λ = 0.94) weights recent squared returns more; <b>GARCH(1,1)</b> fits ω, α, β by
quasi-maximum likelihood: α is the reaction to yesterday's shock, β the persistence, α + β near one means slow
mean reversion; the <b>long-run vol</b> is the level the forecast decays to and the <b>half-life</b> how fast.
<b>GJR-GARCH</b> adds γ for the extra reaction to negative shocks (leverage effect).</li>
<li><b>Forecast term structure</b>: the average variance from today to each horizon, so it can be compared with implied
vol at the same tenor. Implied above forecast means the market is paying up for protection.</li>
<li><b>IV30</b>: the at-the-money implied vol interpolated to a constant 30 days in total variance. <b>IV rank</b> places
today's IV30 between the one-year low and high (0% at the low, 100% at the high); <b>IV percentile</b> is the share of
days in the year with a lower IV30. High rank and percentile favour selling premium; low ones favour buying it.</li>
</ul>
)html";

const char* kAlerts = R"html(
<h2>Alerts: price, change, IV and indicator rules</h2>
<p>Rules on any ticker, evaluated on every watchlist refresh (and on this tab's own poll for other symbols).
A trigger shows the orange banner at the top right, a macOS notification, a sound and optionally a spoken
sentence, and the rule disarms until you re-arm it (or re-arms itself when set to repeat).</p>

<h3>How to use</h3>
<ol>
<li><b>Add…</b> a rule: symbol, condition, threshold, and for indicator rules the TA-Lib indicator and period (picking an
indicator switches the condition to <i>Indicator above / below</i>). <i>Re-arm automatically</i> makes the rule fire
again after the condition clears.</li>
<li><b>Conditions</b>: price above / below a level; day change above / below a percent; at-the-money implied vol above /
below a percent; an indicator above / below a value.</li>
<li><b>Check now</b> fetches quotes for every alert symbol and evaluates all rules; <i>Re-arm</i> arms selected rules again;
<i>Edit…</i> and <i>Remove</i> manage them. Sound, Speak and System notification switches apply to every trigger.</li>
<li>Create rules by speaking or typing to the assistant: “alert me when NVDA drops 3%”, “alert me when the RSI on TSLA
is above 70”, “notify me if SPY implied vol goes above 25%”.</li>
</ol>

<h3>Understanding the analysis</h3>
<ul>
<li>Price and change rules use the bulk snapshot (about 15 minutes delayed on the standard plan). IV rules use the fitted
ATM vol of the loaded chain for the current ticker, otherwise the nearest-expiry ATM vendor vol from the stored chain.</li>
<li>Indicator rules compute the indicator with TA-Lib on one year of daily bars, so they refer to daily values.</li>
<li>The <i>Last value</i> column shows the latest reading against each threshold, and the status column when a rule fired.</li>
</ul>
)html";

const char* kAssistant = R"html(
<h2>Assistant</h2>
<p>The assistant panel (toggle it with the button in the header) sees the current tab's figures and can act on
the app: load tickers and watchlists, add indicators and drawings, create alerts, add positions and run risk,
run the trade scanner, pin earnings dates and answer questions about what is on screen. Dictation and spoken
replies are available. Short commands such as “show RSI” or “load the energy watchlist” run instantly without
a model call.</p>
)html";

struct Page { const char* name; const char* html; };
const Page kPages[] = {
    { "Quotes", kQuotes }, { "Portfolio", kPortfolio }, { "Pricer", kPricer }, { "Strategy", kStrategy }, { "Scenarios", kScenarios },
    { "Option Chain", kChain }, { "Trade Ideas", kIdeas }, { "Heatmap", kHeatmap }, { "Sector Heatmap", kSectorHeatmap },
    { "Volatility", kVolatility }, { "Alerts", kAlerts }, { "Assistant", kAssistant },
};

} // namespace

QStringList topics()
{
    QStringList out;
    for (const Page& p : kPages) out << QString::fromLatin1(p.name);
    return out;
}

QString htmlFor(const QString& tabName)
{
    const QString wanted = tabName.trimmed().toLower();
    if (wanted.isEmpty()) return QString();
    for (const Page& p : kPages) if (QString::fromLatin1(p.name).toLower() == wanted) return QString::fromUtf8(p.html);
    // "Heatmap" must not match "Sector Heatmap": exact names first, then the longest containing name.
    const Page* best = nullptr;
    for (const Page& p : kPages) {
        const QString name = QString::fromLatin1(p.name).toLower();
        if (name.contains(wanted) || wanted.contains(name)) if (!best || name.size() > QString::fromLatin1(best->name).size()) best = &p;
    }
    return best ? QString::fromUtf8(best->html) : QString();
}

} // namespace help
