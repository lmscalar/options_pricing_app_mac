//
//  RateCurveDialog.cpp
//  OptionPricing
//

#include "RateCurveDialog.h"
#include "Widgets.h"

RateCurveDialog::RateCurveDialog(const pricing::RateCurve& curve, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Rate Curve");
    setModal(true);
    resize(420, 380);

    auto* intro = new QLabel("Continuously compounded zero rates by tenor. Rates are interpolated linearly "
                             "between tenors and held flat beyond the ends.", this);
    intro->setObjectName("muted");
    intro->setWordWrap(true);

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({ "Tenor (yrs)", "Zero rate (%)" });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto* add = ui::makeButton(this, "Add Tenor", "secondary");
    auto* remove = ui::makeButton(this, "Remove", "secondary");
    auto* sample = ui::makeButton(this, "Sample Curve", "secondary", "Fill in an upward-sloping example curve");
    auto* buttonsRow = new QHBoxLayout;
    buttonsRow->addWidget(add);
    buttonsRow->addWidget(remove);
    buttonsRow->addStretch(1);
    buttonsRow->addWidget(sample);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    layout->addWidget(intro);
    layout->addWidget(m_table, 1);
    layout->addLayout(buttonsRow);
    layout->addWidget(box);

    connect(add, &QPushButton::clicked, this, [this] {
        double lastTenor = 0.0, lastRate = 5.0;
        if (m_table->rowCount() > 0) {
            if (auto* t = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(m_table->rowCount() - 1, 0))) lastTenor = t->value();
            if (auto* r = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(m_table->rowCount() - 1, 1))) lastRate = r->value();
        }
        addRow(lastTenor + 1.0, lastRate);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row >= 0) m_table->removeRow(row);
        else if (m_table->rowCount() > 0) m_table->removeRow(m_table->rowCount() - 1);
    });
    connect(sample, &QPushButton::clicked, this, [this] { loadSample(); });

    if (curve.empty()) {
        loadSample();
    } else {
        for (const pricing::RatePoint& p : curve.points()) {
            addRow(p.tenor, p.rate * 100.0);
        }
    }
}

void RateCurveDialog::addRow(double tenor, double ratePercent)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    auto* tenorBox = ui::makeSpinBox(m_table, 0.0, 50.0, 0.25, 3, tenor, " yrs");
    auto* rateBox = ui::makeSpinBox(m_table, -10.0, 100.0, 0.05, 3, ratePercent, " %");
    tenorBox->setFrame(false);
    rateBox->setFrame(false);
    m_table->setCellWidget(row, 0, tenorBox);
    m_table->setCellWidget(row, 1, rateBox);
}

void RateCurveDialog::loadSample()
{
    m_table->setRowCount(0);
    const double tenors[] = { 1.0 / 12.0, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0 };
    const double rates[] = { 4.30, 4.25, 4.15, 4.00, 3.85, 3.90, 4.10 };
    for (size_t i = 0; i < 7; ++i) {
        addRow(tenors[i], rates[i]);
    }
}

pricing::RateCurve RateCurveDialog::curve() const
{
    std::vector<pricing::RatePoint> points;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* tenorBox = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, 0));
        auto* rateBox = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, 1));
        if (tenorBox && rateBox) {
            points.push_back({ tenorBox->value(), rateBox->value() / 100.0 });
        }
    }
    return pricing::RateCurve(points);
}
