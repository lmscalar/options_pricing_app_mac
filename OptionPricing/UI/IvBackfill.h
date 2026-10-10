//
//  IvBackfill.h
//  OptionPricing
//
//  Rebuilds about a year of daily implied-vol history for a ticker from historical option
//  bars. Month by month: the call and put nearest the money at the month's first close,
//  expiring on the third Friday of the following month (so 20 to 50 days out), are priced
//  from their daily closes against the stock's closes; the pair's average implied vol is
//  the day's sample. About three requests per month, run one after another, written into
//  the chain store's iv_history table without overwriting live snapshot samples.
//

#pragma once

#include "QtHeaders.h"
#include "ChainStore.h"
#include "MarketDataClient.h"

#include <functional>
#include <vector>

class IvBackfill
{
public:
    IvBackfill(MarketDataClient& client, ChainStore& store);

    struct Close {
        QDate date;
        double close = 0.0;
    };

    /// `closes` ascending daily closes of the underlying (a year or more). Progress fires per month.
    void run(const QString& ticker, std::vector<Close> closes, std::function<double(double maturity)> rateFor,
             std::function<void(int done, int total, const QString& message)> progress,
             std::function<void(bool ok, int samples, const QString& message)> done);
    bool busy() const { return m_busy; }
    void cancel() { m_cancel = true; }

    static QDate thirdFriday(int year, int month);

private:
    struct Step {
        QDate first, last;        ///< trading days covered by this month's contracts
        QDate expiry;
        double anchorClose = 0.0; ///< the close the strike is chosen against
        bool retried = false;     ///< already moved to the Thursday before a holiday Friday
    };
    void nextStep();
    void finish(bool ok, const QString& message);

    MarketDataClient& m_client;
    ChainStore& m_store;
    bool m_busy = false;
    bool m_cancel = false;
    QString m_ticker;
    std::vector<Close> m_closes;
    std::function<double(double)> m_rateFor;
    std::vector<Step> m_steps;
    size_t m_index = 0;
    int m_samples = 0;
    int m_skipped = 0;
    std::function<void(int, int, const QString&)> m_progress;
    std::function<void(bool, int, const QString&)> m_done;
};
