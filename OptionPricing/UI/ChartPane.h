//
//  ChartPane.h
//  OptionPricing
//
//  A secondary price chart for the Quotes tab's multi-chart layouts: its own symbol (or
//  linked to the main chart's symbol) and timeframe, the same chart page as the main
//  chart, fed by QuotesTab (bars, indicators, options, theme). Two to four charts side by
//  side give a multi-timeframe or multi-symbol view in the style of professional
//  terminals.
//

#pragma once

#include "QtHeaders.h"
#include "ChartPage.h"
#include "MarketDataClient.h"

#include <functional>

class ChartPane : public QFrame
{
public:
    explicit ChartPane(QWidget* parent = nullptr);

    // Configuration (edited in the header row; persisted by the host).
    QString symbol() const;
    void setSymbol(const QString& symbol);
    bool linked() const;              ///< follows the main chart's symbol
    void setLinked(bool on);
    QString timeframe() const;
    void setTimeframe(const QString& label);
    void setTimeframes(const QStringList& labels);
    QJsonObject toJson() const;
    void fromJson(const QJsonObject& object);

    // Chart page
    void runJs(const QString& script);           ///< queued until the page has loaded
    void legendText(std::function<void(const QString& text)> done);
    void setSeries(const MarketDataClient::BarSeries& series) { m_series = series; }
    const MarketDataClient::BarSeries& series() const { return m_series; }
    /// Each load gets a sequence number so a slow response for an old symbol is dropped.
    int beginLoad() { return ++m_loadSequence; }
    int loadSequence() const { return m_loadSequence; }
    void setStatus(const QString& text);

    std::function<void()> onConfigChanged;                   ///< symbol, link or timeframe edited by the user
    std::function<void(const QString& symbol)> onPromote;    ///< "Main" pressed: show this symbol on the main chart

private:
    QCheckBox* m_link = nullptr;
    QLineEdit* m_symbol = nullptr;
    QComboBox* m_timeframe = nullptr;
    QLabel* m_status = nullptr;
    QToolButton* m_promote = nullptr;
    QWebEngineView* m_view = nullptr;
    ChartWebPage* m_page = nullptr;
    MarketDataClient::BarSeries m_series;
    QStringList m_pending;
    bool m_ready = false;
    bool m_updating = false;
    int m_loadSequence = 0;
};
