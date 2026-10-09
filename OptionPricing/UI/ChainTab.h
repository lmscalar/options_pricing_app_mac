//
//  ChainTab.h
//  OptionPricing
//
//  Option chain view: live download from Massive.com, CSV import (or a synthetic
//  sample), implied volatility per strike, static-arbitrage flags, SVI smile fit per
//  expiry, smile and term-structure charts, and hand-off of a selected contract to the
//  Pricer tab.
//

#pragma once

#include "QtHeaders.h"
#include "ChainStore.h"
#include "MarketDataClient.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/VolSurface.h"

#include <functional>
#include <vector>

class ChainTab : public QWidget
{
public:
    explicit ChainTab(MarketState& state, QWidget* parent = nullptr);

    /// Loads a chain CSV, replacing the current quotes. Shows a message box on failure.
    void importCsvFile(const QString& path);
    void applyTheme(const Theme& theme);

    /// Recomputes implied vols, checks and fits from the quotes held in the market state.
    void rebuild();

    /// Replaces the chain with a synthetic one that has skew and smile.
    void generateSample();

    // Live data (Massive.com). Each operation reports through onLiveOperationFinished.
    void setTicker(const QString& ticker);
    QString ticker() const;
    /// Downloads the underlying price and chain. `automatic` marks a timer-driven refresh,
    /// which keeps the controls enabled and the status line brief.
    void fetchLiveChain(bool automatic = false);
    /// In-memory chain database shared by the app; downloads are stored there and
    /// applyStoredChain() serves from it without a network round trip.
    void setStore(ChainStore* store) { m_store = store; }
    /// Applies the stored chain for `symbol` (sets the ticker, market spot, chain and
    /// notifies every tab). Returns false when the store has no chain for it.
    bool applyStoredChain(const QString& symbol);
    /// Re-fetches only the underlying price and re-applies the spot policy.
    void refreshUnderlying(bool automatic = false);
    void loadTreasuryCurve();
    void loadDividends();
    /// Asks for an API key. Returns true if a key is now available.
    bool promptForApiKey();
    MarketDataClient& client() { return m_client; }

    QString resultsCsv() const;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

public:
    std::function<void(double strike, double maturity, double volatility, const QString& expiryDate)> onSendToPricer;
    std::function<void(bool ok, const QString& message)> onLiveOperationFinished;

private:
    void buildUi();
    void wire();
    void clearChain();
    void showSlice(int index);
    void updateCharts(const pricing::ExpirySlice* slice);
    void setMarketVolToAtm();
    /// True when the strike passes the current strike-range filter (reference = forward).
    bool strikeInRange(double strike, double reference) const;
    void updateStrikeControls();
    /// Chooses the market spot: parity-implied from fresh option prices when enabled and
    /// available, otherwise the vendor's (delayed) stock price.
    void applySpotPolicy();
    void updateSpotLabels();
    void updateTimers();
    /// Fetches company name and icon for the current ticker (served from the disk cache when possible).
    void ensureBranding();
    static QString logoCachePath(const QString& ticker);
    /// Sizes columns to their content and shares spare width, unless the user has resized them.
    void fitColumns();
    void setLiveBusy(bool busy, const QString& status);
    /// Puts a downloaded (or stored) chain and underlying snapshot into the market state.
    void applyDownload(const QString& symbol, const MarketDataClient::UnderlyingSnapshot& snap, const MarketDataClient::ChainDownload& download,
                       const QDateTime& fetchedAt);
    QString downloadSummary(const QString& symbol, const MarketDataClient::ChainDownload& download, const QDate& today) const;
    void finishLive(bool ok, const QString& message);
    void refreshKeyStatus();

    MarketState& m_state;
    MarketDataClient m_client;
    ChainStore* m_store = nullptr;
    Theme m_theme;
    std::vector<pricing::ExpirySlice> m_slices;
    bool m_updating = false;
    bool m_busy = false;

    QPushButton* m_import = nullptr;
    QPushButton* m_sample = nullptr;
    QPushButton* m_clear = nullptr;
    QDoubleSpinBox* m_defaultMaturity = nullptr;
    QComboBox* m_expiry = nullptr;
    QLabel* m_marketInfo = nullptr;
    QLabel* m_summary = nullptr;
    QTableWidget* m_table = nullptr;
    QPlainTextEdit* m_issues = nullptr;
    QLabel* m_sviInfo = nullptr;
    QPushButton* m_useAtm = nullptr;

    // Strike filter and layout
    QComboBox* m_strikeMode = nullptr;
    QDoubleSpinBox* m_strikePercent = nullptr;
    QDoubleSpinBox* m_strikeMin = nullptr;
    QDoubleSpinBox* m_strikeMax = nullptr;
    QLabel* m_strikeToLabel = nullptr;
    QLabel* m_tableTitle = nullptr;
    QLabel* m_spotLabel = nullptr;
    QLabel* m_spotDetail = nullptr;
    QLabel* m_logoLabel = nullptr;
    QString m_brandingRequested;       ///< ticker whose branding fetch is in flight or done
    double m_lastSliceKey = -1.0;      ///< maturity of the slice last shown, to keep scroll position on refresh
    bool m_fittingColumns = false;     ///< true while fitColumns() adjusts widths programmatically
    bool m_userResizedColumns = false; ///< set once the user drags a column divider
    bool m_atmCentered = false;        ///< whether the current slice has been centred on the ATM row while visible

    // Auto-refresh
    QCheckBox* m_autoRefresh = nullptr;
    QSpinBox* m_chainInterval = nullptr;
    QSpinBox* m_spotInterval = nullptr;
    QCheckBox* m_useImpliedSpot = nullptr;
    QTimer* m_chainTimer = nullptr;
    QTimer* m_spotTimer = nullptr;
    QString m_vendorSource;
    bool m_automatic = false;
    QPushButton* m_toggleCharts = nullptr;
    QSplitter* m_splitter = nullptr;
    QWidget* m_bottomPane = nullptr;

    QLineEdit* m_ticker = nullptr;
    QSpinBox* m_expiryCount = nullptr;
    QPushButton* m_fetch = nullptr;
    QPushButton* m_treasury = nullptr;
    QPushButton* m_dividends = nullptr;
    QPushButton* m_setKey = nullptr;
    QLabel* m_liveStatus = nullptr;
    QLabel* m_keyStatus = nullptr;

    QChart* m_smileChart = nullptr;
    QValueAxis* m_smileX = nullptr;
    QValueAxis* m_smileY = nullptr;
    QLabel* m_smileHover = nullptr;
    QChart* m_termChart = nullptr;
    QValueAxis* m_termX = nullptr;
    QValueAxis* m_termY = nullptr;
    QLabel* m_termHover = nullptr;

    /// Shows a chart hover readout both as a tooltip at the cursor and in the label under the chart.
    void showChartHover(QLabel* readout, const QString& text, bool state);
};
