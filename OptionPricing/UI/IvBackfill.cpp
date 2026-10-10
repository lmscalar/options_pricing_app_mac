//
//  IvBackfill.cpp
//  OptionPricing
//

#include "IvBackfill.h"
#include "../Pricing/IvHistory.h"

#include <QtCore/QTimeZone>

#include <algorithm>
#include <cmath>
#include <map>

IvBackfill::IvBackfill(MarketDataClient& client, ChainStore& store) : m_client(client), m_store(store) {}

QDate IvBackfill::thirdFriday(int year, int month)
{
    const QDate first(year, month, 1);
    const int toFriday = (5 - first.dayOfWeek() + 7) % 7;
    return first.addDays(toFriday + 14);
}

void IvBackfill::run(const QString& ticker, std::vector<Close> closes, std::function<double(double)> rateFor,
                     std::function<void(int, int, const QString&)> progress, std::function<void(bool, int, const QString&)> done)
{
    if (m_busy) { if (done) done(false, 0, "A backfill is already running."); return; }
    m_ticker = ticker.trimmed().toUpper();
    m_closes = std::move(closes);
    std::sort(m_closes.begin(), m_closes.end(), [](const Close& a, const Close& b) { return a.date < b.date; });
    m_rateFor = rateFor ? std::move(rateFor) : [](double) { return 0.04; };
    m_progress = std::move(progress);
    m_done = std::move(done);
    m_steps.clear();
    m_index = 0;
    m_samples = 0;
    m_skipped = 0;
    m_cancel = false;
    if (m_ticker.isEmpty() || m_closes.size() < 20) { finish(false, "Load at least a month of daily bars first."); return; }

    // One step per calendar month, the last 13 months at most, up to the last close.
    const QDate today = QDate::currentDate();
    const QDate earliest = today.addMonths(-13);
    std::map<std::pair<int, int>, Step> months;
    for (const Close& c : m_closes) {
        if (c.date < earliest || c.date > today || c.close <= 0.0) continue;
        Step& s = months[{ c.date.year(), c.date.month() }];
        if (!s.first.isValid()) { s.first = c.date; s.anchorClose = c.close; }
        s.last = c.date;
    }
    for (auto& [ym, s] : months) {
        s.expiry = thirdFriday(ym.first + (ym.second == 12 ? 1 : 0), ym.second == 12 ? 1 : ym.second + 1);
        if (s.first.daysTo(s.expiry) < 20) s.expiry = thirdFriday(s.expiry.addMonths(1).year(), s.expiry.addMonths(1).month());
        if (s.expiry > today) continue;   // contracts still live: the daily snapshots cover this stretch
        m_steps.push_back(s);
    }
    if (m_steps.empty()) { finish(false, "Nothing to backfill: the history is covered by live snapshots."); return; }
    m_busy = true;
    nextStep();
}

void IvBackfill::nextStep()
{
    if (m_cancel) { finish(m_samples > 0, "Backfill cancelled."); return; }
    if (m_index >= m_steps.size()) {
        finish(m_samples > 0, m_samples > 0 ? QStringLiteral("Backfilled %1 daily implied-vol samples over %2 months%3.").arg(m_samples).arg(m_steps.size())
                                                     .arg(m_skipped ? QStringLiteral(" (%1 month(s) without usable contracts)").arg(m_skipped) : QString())
                                            : QStringLiteral("No implied-vol samples could be rebuilt from option history."));
        return;
    }
    const Step step = m_steps[m_index];
    if (m_progress) m_progress(static_cast<int>(m_index), static_cast<int>(m_steps.size()), QStringLiteral("%1: contracts expiring %2…").arg(step.first.toString("MMM yyyy"), step.expiry.toString(Qt::ISODate)));
    auto skip = [this](const QString& why) {
        ++m_skipped;
        qInfo("[iv-backfill] %s: %s", qPrintable(m_ticker), qPrintable(why));
        ++m_index;
        nextStep();
    };
    m_client.fetchOptionContracts(m_ticker, step.expiry, step.anchorClose * 0.96, step.anchorClose * 1.04,
        [this, step, skip](const std::vector<MarketDataClient::OptionContract>& contracts) {
            // The strike nearest the anchor close that lists both a call and a put.
            std::map<double, std::pair<QString, QString>> byStrike;
            for (const MarketDataClient::OptionContract& c : contracts) (c.type == pricing::OptionType::Call ? byStrike[c.strike].first : byStrike[c.strike].second) = c.ticker;
            double bestStrike = 0.0;
            for (const auto& [strike, pair] : byStrike) {
                if (pair.first.isEmpty() || pair.second.isEmpty()) continue;
                if (bestStrike == 0.0 || std::fabs(strike - step.anchorClose) < std::fabs(bestStrike - step.anchorClose)) bestStrike = strike;
            }
            if (bestStrike == 0.0) {
                // A third Friday that is an exchange holiday (Good Friday, Juneteenth, 3 July): contracts expire the Thursday before.
                if (!m_steps[m_index].retried) {
                    m_steps[m_index].retried = true;
                    m_steps[m_index].expiry = step.expiry.addDays(-1);
                    nextStep();
                    return;
                }
                skip(QStringLiteral("no call/put pair near %1 for %2").arg(step.anchorClose).arg(step.expiry.toString(Qt::ISODate)));
                return;
            }
            const QString callTicker = byStrike[bestStrike].first, putTicker = byStrike[bestStrike].second;
            auto bars = std::make_shared<std::map<QDate, std::pair<double, double>>>();
            auto afterPut = [this, step, bestStrike, bars](const MarketDataClient::BarSeries& put) {
                for (const MarketDataClient::Bar& b : put.bars) (*bars)[QDateTime::fromMSecsSinceEpoch(b.timeMs, QTimeZone::utc()).date()].second = b.close;
                std::vector<ChainStore::IvPoint> points;
                for (const Close& c : m_closes) {
                    if (c.date < step.first || c.date > step.last) continue;
                    const auto it = bars->find(c.date);
                    if (it == bars->end()) continue;
                    const double maturity = c.date.daysTo(step.expiry) / 365.0;
                    const double iv = pricing::ivhist::impliedPairFromCloses(c.close, bestStrike, maturity, m_rateFor(maturity), it->second.first, it->second.second);
                    if (iv <= 0.0 || iv > 5.0) continue;
                    ChainStore::IvPoint p;
                    p.date = c.date;
                    p.iv30 = iv;
                    p.ivNear = iv;
                    p.spot = c.close;
                    p.source = QStringLiteral("backfill K%1 %2").arg(bestStrike).arg(step.expiry.toString(Qt::ISODate));
                    points.push_back(p);
                }
                if (!points.empty()) m_store.putIvSamples(m_ticker, points, false);
                m_samples += static_cast<int>(points.size());
                ++m_index;
                nextStep();
            };
            m_client.fetchAggregates(callTicker, 1, "day", step.first, step.last,
                [this, putTicker, step, bars, afterPut](const MarketDataClient::BarSeries& call) {
                    for (const MarketDataClient::Bar& b : call.bars) (*bars)[QDateTime::fromMSecsSinceEpoch(b.timeMs, QTimeZone::utc()).date()].first = b.close;
                    m_client.fetchAggregates(putTicker, 1, "day", step.first, step.last, afterPut, [afterPut](const QString&) { afterPut(MarketDataClient::BarSeries{}); });
                },
                [this, putTicker, step, bars, afterPut](const QString&) {
                    m_client.fetchAggregates(putTicker, 1, "day", step.first, step.last, afterPut, [afterPut](const QString&) { afterPut(MarketDataClient::BarSeries{}); });
                });
        },
        [skip](const QString& message) { skip(message); });
}

void IvBackfill::finish(bool ok, const QString& message)
{
    m_busy = false;
    if (m_done) m_done(ok, m_samples, message);
}
