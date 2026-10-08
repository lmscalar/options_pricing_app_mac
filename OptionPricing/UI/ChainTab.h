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
    /// Re-fetches only the underlying price and re-applies the spot policy.
    void refreshUnderlying(bool automatic = false);
    void loadTreasuryCurve();
    void loadDividends();
    /// Asks for an API key. Returns true if a key is now available.
    bool promptForApiKey();
    MarketDataClient& client() { return m_client; }

    QString resultsCsv() const;

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
    void setLiveBusy(bool busy, const QString& status);
    void finishLive(bool ok, const QString& message);
    void refreshKeyStatus();

    MarketState& m_state;
    MarketDataClient m_client;
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
    double m_lastSliceKey = -1.0;      ///< maturity of the slice last shown, to keep scroll position on refresh
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
    QChart* m_termChart = nullptr;
    QValueAxis* m_termX = nullptr;
    QValueAxis* m_termY = nullptr;
};
