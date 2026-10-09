//
//  AlertsTab.h
//  OptionPricing
//
//  Price, percent-change, implied-volatility and technical-indicator alerts on any ticker.
//  Rules are evaluated on every watchlist quote refresh (and on the tab's own poll for
//  symbols outside the watchlist); indicator rules compute the TA-Lib value from daily
//  bars. A trigger shows an in-app banner, a macOS notification, a sound and optionally a
//  spoken sentence, is logged, and the rule is disarmed until reset (or re-armed when it
//  repeats). Rules persist in the preferences; the assistant can create, list and delete them.
//

#pragma once

#include "QtHeaders.h"
#include "ChainStore.h"
#include "MarketDataClient.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"

#include <QtWidgets/QSystemTrayIcon>

#include <functional>
#include <map>
#include <vector>

class AlertsTab : public QWidget
{
public:
    enum class Condition { PriceAbove, PriceBelow, ChangeAbove, ChangeBelow, IvAbove, IvBelow, IndicatorAbove, IndicatorBelow };

    struct Rule {
        int id = 0;
        QString symbol;
        Condition condition = Condition::PriceAbove;
        double threshold = 0.0;
        QString indicator;        ///< TA-Lib function for indicator conditions, e.g. "RSI"
        int period = 14;          ///< indicator period
        bool repeat = false;      ///< re-arm automatically once the condition clears
        bool armed = true;
        QDateTime created;
        QDateTime triggeredAt;
        double lastValue = std::numeric_limits<double>::quiet_NaN();
        QString note;
    };

    struct Trigger {
        int ruleId = 0;
        QString symbol;
        QString message;
        double value = 0.0;
        QDateTime at;
    };

    explicit AlertsTab(MarketState& state, QWidget* parent = nullptr);

    void setStore(ChainStore* store) { m_store = store; }
    void applyTheme(const Theme& theme);
    /// Evaluates price, percent and (via the store) IV rules against a quote snapshot.
    void evaluateQuotes(const std::vector<MarketDataClient::Quote>& quotes);
    /// Polls quotes for every armed symbol and evaluates all rules (indicator rules fetch bars).
    void checkNow();

    // ---- Assistant hooks ----
    /// Creates a rule from JSON: {symbol, condition: "price_above"|"price_below"|"change_above"|"change_below"|"iv_above"|"iv_below"|"indicator_above"|"indicator_below", threshold, indicator, period, repeat, note}.
    bool addRule(const QJsonObject& spec, QString* error = nullptr, int* idOut = nullptr);
    /// Parses a sentence such as "alert me if NVDA goes above 240" into a rule; false if it is not an alert request.
    bool addRuleFromText(const QString& text, QString* feedback);
    int removeRules(const QString& symbolOrId);
    QString summaryText() const;
    QString rulesCsv() const;
    int ruleCount() const { return static_cast<int>(m_rules.size()); }
    static QString conditionName(Condition c);
    static bool conditionFromName(const QString& text, Condition& out);

    /// Shown when a rule fires: the host displays the banner and may cascade the ticker.
    std::function<void(const Trigger& trigger)> onTriggered;
    std::function<void(const QString& ticker)> onTickerSelected;

private:
    void buildUi();
    void wire();
    void loadRules();
    void saveRules() const;
    void fillTable();
    void fillLog();
    void promptRule(int editId);
    void removeSelected();
    void resetSelected();
    void evaluateRule(Rule& rule, double value, const QString& valueText);
    void fire(Rule& rule, double value, const QString& valueText);
    void evaluateIndicatorRules();
    double impliedVolFor(const QString& symbol) const;
    QString describe(const Rule& rule) const;
    static QString conditionLabel(Condition c);
    int nextId() const;

    MarketState& m_state;
    MarketDataClient m_client;
    ChainStore* m_store = nullptr;
    Theme m_theme;
    std::vector<Rule> m_rules;
    std::vector<Trigger> m_log;
    std::map<QString, MarketDataClient::Quote> m_lastQuotes;
    struct Bars { QDateTime fetchedAt; std::vector<MarketDataClient::Bar> bars; };
    std::map<QString, Bars> m_bars;
    QStringList m_barQueue;
    int m_barInFlight = 0;
    QTimer* m_poll = nullptr;

    QTableWidget* m_table = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_edit = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_reset = nullptr;
    QPushButton* m_check = nullptr;
    QCheckBox* m_sound = nullptr;
    QCheckBox* m_speak = nullptr;
    QCheckBox* m_notify = nullptr;
    QLabel* m_status = nullptr;
    QPlainTextEdit* m_logView = nullptr;
    QSystemTrayIcon* m_tray = nullptr;
};
