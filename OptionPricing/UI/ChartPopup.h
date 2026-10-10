//
//  ChartPopup.h
//  OptionPricing
//
//  Pop-out price chart: a second copy of the Quotes chart page in its own resizable
//  window, kept identical to the Quotes chart by QuotesTab::mirrorChartTo (every script
//  the Quotes tab sends to its page is sent here too). Opened when a stock is picked on
//  the Sector Heatmap, so a drill-down shows the stock's performance without leaving the
//  map. Its timeframe buttons and Live switch drive the Quotes chart, which drives the
//  mirror, so both views always agree.
//

#pragma once

#include "QtHeaders.h"
#include "ChartPage.h"

#include <functional>
#include <vector>

class ChartPopup : public QWidget
{
public:
    /// An independent top-level window (no parent), so it floats freely and is resized
    /// like any other window rather than being tied to the main window's screen state.
    ChartPopup();

    /// Sets the window and header titles; the chart content arrives through runJs.
    void showSymbol(const QString& symbol, const QString& name);
    /// Runs a chartApi script on the page, queued until the page has loaded.
    void runJs(const QString& script);
    bool pageReady() const { return m_ready; }
    /// The chart legend (symbol, OHLC, indicators) as plain text, for logs and the live smoke.
    void legendText(std::function<void(const QString& text)> done);

    /// Timeframe buttons (the Quotes tab's labels, e.g. 1m … 1D, 1W) and which one is current.
    void setTimeframes(const QStringList& labels);
    void setTimeframe(const QString& label);
    QString timeframe() const;
    /// Live tracking state shown on the switch: on/off and the refresh interval in seconds.
    void setLive(bool on, int intervalSeconds);
    bool liveChecked() const;
    /// Chart type key as the Quotes tab uses it: candles, bars, heikin or line.
    void setChartType(const QString& key);
    QString chartType() const;
    /// Earnings (implied move) cone switch.
    void setEventCone(bool on);
    bool eventConeChecked() const;
    /// True when the window came back at a size and position saved from an earlier session.
    bool restoredGeometry() const { return m_restoredGeometry; }

    std::function<void(const QString& label)> onTimeframe;   ///< a timeframe button was pressed
    std::function<void(bool on)> onLiveToggled;              ///< the Live switch was toggled
    std::function<void(const QString& key)> onChartType;     ///< a chart type was chosen
    std::function<void(bool on)> onEventCone;                ///< the earnings cone switch was toggled
    std::function<void()> onOpenQuotes;                      ///< "Open in Quotes" pressed
    std::function<void()> onOpenChain;                       ///< "Option Chain" pressed

protected:
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void saveGeometry();

    QLabel* m_title = nullptr;
    QLabel* m_name = nullptr;
    QButtonGroup* m_timeframes = nullptr;
    QHBoxLayout* m_timeframeRow = nullptr;
    QCheckBox* m_live = nullptr;
    QComboBox* m_chartType = nullptr;
    QCheckBox* m_cone = nullptr;
    QWebEngineView* m_view = nullptr;
    ChartWebPage* m_page = nullptr;
    bool m_ready = false;
    bool m_restoredGeometry = false;
    bool m_updating = false;
    QStringList m_pending;
};
