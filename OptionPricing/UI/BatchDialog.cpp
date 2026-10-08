//
//  BatchDialog.cpp
//  OptionPricing
//

#include "BatchDialog.h"
#include "Formatting.h"
#include "Widgets.h"
#include "../Pricing/American.h"

using namespace pricing;

BatchDialog::BatchDialog(const BatchParseResult& parsed, const QString& sourceName, QWidget* parent)
    : QDialog(parent)
    , m_sourceName(sourceName)
{
    setWindowTitle("Batch Pricing");
    setModal(true);
    resize(1100, 560);

    m_summary = new QLabel(this);
    m_summary->setObjectName("muted");
    m_summary->setWordWrap(true);

    m_table = new QTableWidget(0, 16, this);
    m_table->setHorizontalHeaderLabels({ "Label", "Type", "Exercise", "Model", "Spot", "Strike", "Rate", "Yield", "Vol", "T (yrs)",
                                         "Price", "Delta", "Gamma", "Vega", "Theta", "Rho" });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);

    auto* save = ui::makeButton(this, "Save Results as CSV…", "primary");
    auto* close = ui::makeButton(this, "Close", "secondary");
    connect(save, &QPushButton::clicked, this, [this] { this->save(); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(close);
    buttons->addWidget(save);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    layout->addWidget(m_summary);
    layout->addWidget(m_table, 1);
    layout->addLayout(buttons);

    price(parsed.contracts);

    QString summary = QStringLiteral("Priced %1 contract%2 from %3.").arg(parsed.contracts.size()).arg(parsed.contracts.size() == 1 ? "" : "s").arg(sourceName);
    if (!parsed.warnings.empty()) {
        summary += QStringLiteral(" %1 row%2 skipped: ").arg(parsed.warnings.size()).arg(parsed.warnings.size() == 1 ? "" : "s");
        QStringList details;
        for (size_t i = 0; i < parsed.warnings.size() && i < 5; ++i) details << QString::fromStdString(parsed.warnings[i]);
        summary += details.join(" ");
        if (parsed.warnings.size() > 5) summary += " …";
    }
    m_summary->setText(summary);
}

void BatchDialog::price(const std::vector<BatchContract>& contracts)
{
    QTextStream csv(&m_csv);
    csv << "label,type,exercise,model,spot,strike,rate,dividend_yield,volatility,maturity_years,price,delta,gamma,vega,theta,rho\n";
    m_table->setRowCount(static_cast<int>(contracts.size()));
    int row = 0;
    for (const BatchContract& c : contracts) {
        double value = 0.0;
        Greeks g;
        if (c.american) {
            const AmericanResult a = americanValuation(c.inputs, c.type);
            value = a.price;
            g = a.greeks;
        } else {
            const Result r = pricing::price(c.inputs);
            value = c.type == OptionType::Call ? r.callPrice : r.putPrice;
            g = c.type == OptionType::Call ? r.call : r.put;
        }
        const QString type = c.type == OptionType::Call ? "call" : "put";
        const QString exercise = c.american ? "American" : "European";
        const QString model = c.inputs.model == Model::Black76 ? "Black-76" : "BSM";
        const QStringList cells = {
            QString::fromStdString(c.label), type, exercise, model,
            ui::number(c.inputs.spot, 2), ui::number(c.inputs.strike, 2), ui::percent(c.inputs.riskFreeRate, 2),
            ui::percent(c.inputs.dividendYield, 2), ui::percent(c.inputs.volatility, 2), ui::number(c.inputs.maturity, 4),
            ui::number(value), ui::number(g.delta), ui::number(g.gamma), ui::number(g.vega), ui::number(g.theta), ui::number(g.rho),
        };
        for (int col = 0; col < cells.size(); ++col) {
            m_table->setItem(row, col, ui::makeCell(cells[col], col < 4 ? (Qt::AlignLeft | Qt::AlignVCenter) : (Qt::AlignRight | Qt::AlignVCenter)));
        }
        csv << QString::fromStdString(pricing::csv::quote(c.label)) << "," << type << "," << exercise << "," << model << ","
            << c.inputs.spot << "," << c.inputs.strike << "," << c.inputs.riskFreeRate << "," << c.inputs.dividendYield << ","
            << c.inputs.volatility << "," << c.inputs.maturity << "," << value << "," << g.delta << "," << g.gamma << ","
            << g.vega << "," << g.theta << "," << g.rho << "\n";
        ++row;
    }
}

void BatchDialog::save()
{
    const QString suggested = QFileInfo(m_sourceName).completeBaseName() + "_priced.csv";
    const QString path = QFileDialog::getSaveFileName(this, "Save batch results", suggested, "CSV files (*.csv)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Save failed", QStringLiteral("Could not write %1.").arg(path));
        return;
    }
    file.write(m_csv.toUtf8());
    m_summary->setText(QStringLiteral("Saved results to %1.").arg(path));
}
