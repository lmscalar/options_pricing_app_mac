//
//  AlertsTab.cpp
//  OptionPricing
//

#include "AlertsTab.h"

#include <QtGui/QStandardItemModel>
#include "Formatting.h"
#include "SpeechDictation.h"
#include "../Pricing/TechnicalAnalysis.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr const char* kRulesKey = "alerts/rules";
constexpr const char* kSoundKey = "alerts/sound";
constexpr const char* kSpeakKey = "alerts/speak";
constexpr const char* kNotifyKey = "alerts/notify";
constexpr int kPollSeconds = 60;

enum Column { ColSymbol = 0, ColCondition, ColThreshold, ColLast, ColStatus, ColRepeat, ColCreated, ColNote, ColumnCount };

QString formatValue(AlertsTab::Condition c, double v)
{
    switch (c) {
    case AlertsTab::Condition::ChangeAbove:
    case AlertsTab::Condition::ChangeBelow: return QStringLiteral("%1%2%").arg(v >= 0 ? "+" : "").arg(QString::number(v, 'f', 2));
    case AlertsTab::Condition::IvAbove:
    case AlertsTab::Condition::IvBelow: return QStringLiteral("%1%").arg(QString::number(v, 'f', 1));
    case AlertsTab::Condition::IndicatorAbove:
    case AlertsTab::Condition::IndicatorBelow: return QString::number(v, 'f', 2);
    case AlertsTab::Condition::PriceAbove:
    case AlertsTab::Condition::PriceBelow: break;
    }
    return QString::number(v, 'f', 2);
}

bool isAbove(AlertsTab::Condition c)
{
    return c == AlertsTab::Condition::PriceAbove || c == AlertsTab::Condition::ChangeAbove || c == AlertsTab::Condition::IvAbove || c == AlertsTab::Condition::IndicatorAbove;
}

} // namespace

QString AlertsTab::conditionName(Condition c)
{
    switch (c) {
    case Condition::PriceAbove: return "price_above";
    case Condition::PriceBelow: return "price_below";
    case Condition::ChangeAbove: return "change_above";
    case Condition::ChangeBelow: return "change_below";
    case Condition::IvAbove: return "iv_above";
    case Condition::IvBelow: return "iv_below";
    case Condition::IndicatorAbove: return "indicator_above";
    case Condition::IndicatorBelow: return "indicator_below";
    }
    return "price_above";
}

QString AlertsTab::conditionLabel(Condition c)
{
    switch (c) {
    case Condition::PriceAbove: return "Price above";
    case Condition::PriceBelow: return "Price below";
    case Condition::ChangeAbove: return "Day change above";
    case Condition::ChangeBelow: return "Day change below";
    case Condition::IvAbove: return "ATM implied vol above";
    case Condition::IvBelow: return "ATM implied vol below";
    case Condition::IndicatorAbove: return "Indicator above";
    case Condition::IndicatorBelow: return "Indicator below";
    }
    return QString();
}

bool AlertsTab::conditionFromName(const QString& text, Condition& out)
{
    static const QHash<QString, Condition> names{
        { "price_above", Condition::PriceAbove }, { "above", Condition::PriceAbove }, { "price_below", Condition::PriceBelow }, { "below", Condition::PriceBelow },
        { "change_above", Condition::ChangeAbove }, { "pct_above", Condition::ChangeAbove }, { "change_below", Condition::ChangeBelow }, { "pct_below", Condition::ChangeBelow },
        { "iv_above", Condition::IvAbove }, { "iv_below", Condition::IvBelow }, { "indicator_above", Condition::IndicatorAbove }, { "indicator_below", Condition::IndicatorBelow },
    };
    const QString key = text.trimmed().toLower().replace(' ', '_');
    if (!names.contains(key)) return false;
    out = names.value(key);
    return true;
}

AlertsTab::AlertsTab(MarketState& state, QWidget* parent) : QWidget(parent), m_state(state)
{
    buildUi();
    wire();
    loadRules();
    fillTable();
    fillLog();
}

// MARK: - UI

void AlertsTab::buildUi()
{
    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ "Symbol", "Condition", "Threshold", "Last value", "Status", "Repeat", "Created", "Note" });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setObjectName("heatmapHeader");
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(24);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->setMinimumHeight(110);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_table->setColumnWidth(ColSymbol, 70);
    m_table->setColumnWidth(ColCondition, 190);
    m_table->setColumnWidth(ColThreshold, 90);
    m_table->setColumnWidth(ColLast, 90);
    m_table->setColumnWidth(ColStatus, 170);
    m_table->setColumnWidth(ColRepeat, 60);
    m_table->setColumnWidth(ColCreated, 130);

    m_add = ui::makeButton(this, "Add…", "secondary", "Create an alert");
    m_edit = ui::makeButton(this, "Edit…", "secondary", "Edit the selected alert");
    m_remove = ui::makeButton(this, "Remove", "secondary", "Delete the selected alerts");
    m_reset = ui::makeButton(this, "Re-arm", "secondary", "Arm the selected alerts again after they fired");
    m_check = ui::makeButton(this, "Check now", "secondary", "Fetch quotes for every alert symbol and evaluate all rules");
    m_sound = new QCheckBox("Sound", this);
    m_speak = new QCheckBox("Speak", this);
    m_notify = new QCheckBox("System notification", this);
    m_sound->setChecked(QSettings().value(kSoundKey, true).toBool());
    m_speak->setChecked(QSettings().value(kSpeakKey, false).toBool());
    m_notify->setChecked(QSettings().value(kNotifyKey, true).toBool());
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    for (QPushButton* b : { m_add, m_edit, m_remove, m_reset }) buttons->addWidget(b);
    buttons->addSpacing(10);
    buttons->addWidget(m_sound);
    buttons->addWidget(m_speak);
    buttons->addWidget(m_notify);
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_check);

    auto* rulesPane = new QFrame(this);
    rulesPane->setObjectName("pane");
    auto* rulesLayout = new QVBoxLayout(rulesPane);
    rulesLayout->setContentsMargins(12, 10, 12, 10);
    rulesLayout->setSpacing(8);
    auto* hint = new QLabel("Alerts are checked on every watchlist refresh, every minute for other symbols, and when you press Check now. "
                            "Say or type “alert me if NVDA goes above 240”, “alert me when AAPL drops 3%”, “alert me when the RSI on TSLA is above 70”.", this);
    hint->setObjectName("muted");
    hint->setWordWrap(true);
    rulesLayout->addWidget(hint);
    rulesLayout->addWidget(m_table, 1);
    rulesLayout->addLayout(buttons);

    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(500);
    m_logView->setMinimumHeight(80);
    auto* logPane = new QFrame(this);
    logPane->setObjectName("pane");
    auto* logLayout = new QVBoxLayout(logPane);
    logLayout->setContentsMargins(12, 10, 12, 10);
    logLayout->setSpacing(6);
    auto* logTitle = new QLabel("Triggered alerts", this);
    logTitle->setObjectName("sectionTitle");
    logLayout->addWidget(logTitle);
    logLayout->addWidget(m_logView, 1);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(rulesPane);
    splitter->addWidget(logPane);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 520, 180 });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 10);
    root->addWidget(splitter);

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollSeconds * 1000);
}

void AlertsTab::wire()
{
    connect(m_add, &QPushButton::clicked, this, [this] { promptRule(-1); });
    connect(m_edit, &QPushButton::clicked, this, [this] {
        const auto rows = m_table->selectionModel()->selectedRows();
        if (!rows.isEmpty()) promptRule(m_table->item(rows.first().row(), ColSymbol)->data(Qt::UserRole + 1).toInt());
    });
    connect(m_table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) { if (item) promptRule(m_table->item(item->row(), ColSymbol)->data(Qt::UserRole + 1).toInt()); });
    connect(m_remove, &QPushButton::clicked, this, [this] { removeSelected(); });
    connect(m_reset, &QPushButton::clicked, this, [this] { resetSelected(); });
    connect(m_check, &QPushButton::clicked, this, [this] { checkNow(); });
    connect(m_sound, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(kSoundKey, on); });
    connect(m_speak, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(kSpeakKey, on); });
    connect(m_notify, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(kNotifyKey, on); });
    connect(m_poll, &QTimer::timeout, this, [this] { if (std::any_of(m_rules.begin(), m_rules.end(), [](const Rule& r) { return r.armed; })) checkNow(); });
    m_poll->start();
}

void AlertsTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    fillTable();
}

// MARK: - Rules

int AlertsTab::nextId() const
{
    int id = 0;
    for (const Rule& r : m_rules) id = std::max(id, r.id);
    return id + 1;
}

void AlertsTab::loadRules()
{
    m_rules.clear();
    for (const QJsonValue v : QJsonDocument::fromJson(QSettings().value(kRulesKey).toByteArray()).array()) {
        const QJsonObject o = v.toObject();
        Rule r;
        r.id = o.value("id").toInt();
        r.symbol = o.value("symbol").toString().toUpper();
        conditionFromName(o.value("condition").toString(), r.condition);
        r.threshold = o.value("threshold").toDouble();
        r.indicator = o.value("indicator").toString();
        r.period = o.value("period").toInt(14);
        r.repeat = o.value("repeat").toBool(false);
        r.armed = o.value("armed").toBool(true);
        r.created = QDateTime::fromString(o.value("created").toString(), Qt::ISODate);
        r.triggeredAt = QDateTime::fromString(o.value("triggeredAt").toString(), Qt::ISODate);
        r.note = o.value("note").toString();
        if (!r.symbol.isEmpty()) m_rules.push_back(r);
    }
}

void AlertsTab::saveRules() const
{
    QJsonArray list;
    for (const Rule& r : m_rules) {
        list.append(QJsonObject{ { "id", r.id }, { "symbol", r.symbol }, { "condition", conditionName(r.condition) }, { "threshold", r.threshold }, { "indicator", r.indicator },
                                 { "period", r.period }, { "repeat", r.repeat }, { "armed", r.armed }, { "created", r.created.toString(Qt::ISODate) },
                                 { "triggeredAt", r.triggeredAt.isValid() ? r.triggeredAt.toString(Qt::ISODate) : QString() }, { "note", r.note } });
    }
    QSettings().setValue(kRulesKey, QJsonDocument(list).toJson(QJsonDocument::Compact));
}

bool AlertsTab::addRule(const QJsonObject& spec, QString* error, int* idOut)
{
    auto fail = [error](const QString& why) { if (error) *error = why; return false; };
    Rule r;
    r.symbol = spec.value("symbol").toString().trimmed().toUpper();
    if (r.symbol.isEmpty() || r.symbol.size() > 8) return fail("An alert needs a ticker symbol.");
    if (!conditionFromName(spec.value("condition").toString(), r.condition)) {
        return fail("Unknown condition. Use price_above, price_below, change_above, change_below, iv_above, iv_below, indicator_above or indicator_below.");
    }
    if (!spec.contains("threshold") || !spec.value("threshold").isDouble()) return fail("An alert needs a numeric threshold.");
    r.threshold = spec.value("threshold").toDouble();
    if (r.condition == Condition::IndicatorAbove || r.condition == Condition::IndicatorBelow) {
        const QString func = spec.value("indicator").toString().trimmed().toUpper();
        const ta::FunctionInfo* info = ta::catalog().find(func.toStdString());
        if (!info) return fail(QStringLiteral("Unknown indicator '%1' (use a TA-Lib name such as RSI, SMA, EMA, ADX, CCI).").arg(func));
        if (info->candlestick || info->outputs.empty()) return fail("Indicator alerts need a numeric indicator, not a candlestick pattern.");
        r.indicator = QString::fromStdString(info->name);
        r.period = std::max(1, spec.value("period").toInt(14));
    }
    r.repeat = spec.value("repeat").toBool(false);
    r.note = spec.value("note").toString();
    r.created = QDateTime::currentDateTime();
    r.id = nextId();
    m_rules.push_back(r);
    saveRules();
    fillTable();
    if (idOut) *idOut = r.id;
    ui::setStatus(m_status, QStringLiteral("Alert #%1 armed: %2").arg(r.id).arg(describe(r)), ui::StatusKind::Info);
    // Evaluate right away against what we already know, and fetch what we do not.
    QTimer::singleShot(0, this, [this] { checkNow(); });
    return true;
}

bool AlertsTab::addRuleFromText(const QString& rawText, QString* feedback)
{
    QString text = rawText.trimmed();
    text.remove(QRegularExpression("[.!?]+$"));
    // "alert me / notify me / let me know / tell me / ping me  if|when ..."
    static const QRegularExpression lead("^(?:please\\s+)?(?:set\\s+(?:an\\s+)?alert\\s+(?:for\\s+)?|alert\\s+me\\s+|notify\\s+me\\s+|let\\s+me\\s+know\\s+|tell\\s+me\\s+|ping\\s+me\\s+|warn\\s+me\\s+)(?:if|when|once|as\\s+soon\\s+as)?\\s*(.+)$",
                                         QRegularExpression::CaseInsensitiveOption);
    const auto m = lead.match(text);
    if (!m.hasMatch()) return false;
    QString body = m.captured(1).trimmed();
    QJsonObject spec;
    // Indicator: "the RSI on TSLA is above 70", "TSLA rsi(14) goes above 70", "the 14 day RSI of AAPL drops below 30"
    static const QRegularExpression indRe("^(?:the\\s+)?(?:(\\d{1,3})[- ]?(?:day|bar|period)?\\s+)?([A-Za-z]{2,12})(?:\\s*\\((\\d{1,3})\\))?\\s+(?:on|of|for)\\s+([A-Za-z.]{1,6})\\s+(?:is|goes|moves|rises|climbs|drops|falls|crosses|gets)?\\s*(above|over|below|under|past)\\s+(-?\\d+(?:\\.\\d+)?)$",
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression indRe2("^([A-Za-z.]{1,6})(?:'s)?\\s+(?:(\\d{1,3})[- ]?(?:day|bar|period)?\\s+)?([A-Za-z]{2,12})(?:\\s*\\((\\d{1,3})\\))?\\s+(?:is|goes|moves|rises|climbs|drops|falls|crosses|gets)?\\s*(above|over|below|under|past)\\s+(-?\\d+(?:\\.\\d+)?)$",
                                           QRegularExpression::CaseInsensitiveOption);
    auto indicatorSpec = [&](const QString& func, const QString& period, const QString& symbol, const QString& dir, const QString& value) -> bool {
        const ta::FunctionInfo* info = ta::catalog().find(func.toUpper().toStdString());
        if (!info) return false;
        spec["symbol"] = symbol.toUpper();
        spec["indicator"] = QString::fromStdString(info->name);
        spec["period"] = period.isEmpty() ? 14 : period.toInt();
        spec["condition"] = (dir.compare("below", Qt::CaseInsensitive) == 0 || dir.compare("under", Qt::CaseInsensitive) == 0) ? "indicator_below" : "indicator_above";
        spec["threshold"] = value.toDouble();
        return true;
    };
    bool parsed = false;
    if (const auto im = indRe.match(body); im.hasMatch()) parsed = indicatorSpec(im.captured(2), im.captured(1).isEmpty() ? im.captured(3) : im.captured(1), im.captured(4), im.captured(5), im.captured(6));
    if (!parsed) { if (const auto im = indRe2.match(body); im.hasMatch() && im.captured(3).compare("iv", Qt::CaseInsensitive) != 0) parsed = indicatorSpec(im.captured(3), im.captured(2).isEmpty() ? im.captured(4) : im.captured(2), im.captured(1), im.captured(5), im.captured(6)); }
    // Implied vol: "NVDA implied vol goes above 60", "IV on AAPL drops below 20%"
    static const QRegularExpression ivRe("^(?:the\\s+)?(?:(?:implied\\s+vol(?:atility)?|iv)\\s+(?:on|of|for)\\s+([A-Za-z.]{1,6})|([A-Za-z.]{1,6})(?:'s)?\\s+(?:implied\\s+vol(?:atility)?|iv))\\s+(?:is|goes|moves|rises|climbs|drops|falls|gets)?\\s*(above|over|below|under)\\s+(\\d+(?:\\.\\d+)?)\\s*%?$",
                                         QRegularExpression::CaseInsensitiveOption);
    if (!parsed) {
        if (const auto iv = ivRe.match(body); iv.hasMatch()) {
            spec["symbol"] = (iv.captured(1).isEmpty() ? iv.captured(2) : iv.captured(1)).toUpper();
            spec["condition"] = (iv.captured(3).compare("below", Qt::CaseInsensitive) == 0 || iv.captured(3).compare("under", Qt::CaseInsensitive) == 0) ? "iv_below" : "iv_above";
            spec["threshold"] = iv.captured(4).toDouble();
            parsed = true;
        }
    }
    // Percent change: "AAPL is up 3%", "AAPL drops 3%", "AAPL falls more than 2 percent", "AAPL moves down 5%"
    static const QRegularExpression pctRe("^([A-Za-z.]{1,6})\\s+(?:is\\s+|goes\\s+|moves\\s+|rises\\s+|climbs\\s+|drops\\s+|falls\\s+|gains\\s+|loses\\s+|rallies\\s+|sells\\s+off\\s+)?(up|down|higher|lower|more\\s+than|by|over)?\\s*(-?\\d+(?:\\.\\d+)?)\\s*(?:%|percent|pct)(?:\\s+(?:today|on\\s+the\\s+day|or\\s+more))?$",
                                          QRegularExpression::CaseInsensitiveOption);
    if (!parsed) {
        if (const auto pm = pctRe.match(body); pm.hasMatch()) {
            const QString verbPart = body.toLower();
            const bool down = pm.captured(2).compare("down", Qt::CaseInsensitive) == 0 || pm.captured(2).compare("lower", Qt::CaseInsensitive) == 0
                              || verbPart.contains("drop") || verbPart.contains("fall") || verbPart.contains("lose") || verbPart.contains("sells off") || pm.captured(3).startsWith('-');
            spec["symbol"] = pm.captured(1).toUpper();
            spec["condition"] = down ? "change_below" : "change_above";
            spec["threshold"] = down ? -std::fabs(pm.captured(3).toDouble()) : std::fabs(pm.captured(3).toDouble());
            parsed = true;
        }
    }
    // Price: "NVDA goes above 240", "NVDA breaks 240", "AAPL drops below 320", "TSLA hits 400", "SPY trades under 700"
    static const QRegularExpression priceRe("^([A-Za-z.]{1,6})\\s+(?:(?:is|goes|moves|rises|climbs|trades|gets|closes|breaks|breaks\\s+out|hits|reaches|touches|drops|falls|dips|crosses)\\s*)+(above|over|below|under|past|through|to|at)?\\s*\\$?(\\d+(?:\\.\\d+)?)$",
                                            QRegularExpression::CaseInsensitiveOption);
    if (!parsed) {
        if (const auto pr = priceRe.match(body); pr.hasMatch()) {
            const QString lower = body.toLower();
            const QString dir = pr.captured(2).toLower();
            bool below = dir == "below" || dir == "under" || lower.contains("drop") || lower.contains("fall") || lower.contains("dip");
            if (dir.isEmpty() || dir == "to" || dir == "at" || dir == "past" || dir == "through") {
                // "hits 240" / "breaks 240": direction from the current quote when known, else above.
                const auto q = m_lastQuotes.find(pr.captured(1).toUpper());
                if (q != m_lastQuotes.end() && q->second.last > 0.0) below = pr.captured(3).toDouble() < q->second.last;
            }
            spec["symbol"] = pr.captured(1).toUpper();
            spec["condition"] = below ? "price_below" : "price_above";
            spec["threshold"] = pr.captured(3).toDouble();
            parsed = true;
        }
    }
    if (!parsed) {
        if (feedback) *feedback = "I understood this as an alert request but could not parse the condition. Try “alert me if NVDA goes above 240”, “alert me when AAPL drops 3%”, “alert me when the RSI on TSLA is above 70”.";
        return true;
    }
    QString error;
    int id = 0;
    if (!addRule(spec, &error, &id)) { if (feedback) *feedback = error; return true; }
    if (feedback) *feedback = QStringLiteral("Alert #%1 set: %2. You will get a banner%3 when it triggers.").arg(id).arg(describe(m_rules.back()), m_notify->isChecked() ? " and a notification" : "");
    return true;
}

int AlertsTab::removeRules(const QString& symbolOrId)
{
    const QString key = symbolOrId.trimmed().toUpper();
    bool ok = false;
    const int id = key.toInt(&ok);
    int removed = 0;
    std::vector<Rule> keep;
    for (const Rule& r : m_rules) {
        const bool match = key == "ALL" || (ok && r.id == id) || (!ok && r.symbol == key);
        if (match) ++removed; else keep.push_back(r);
    }
    if (removed) { m_rules = keep; saveRules(); fillTable(); }
    return removed;
}

QString AlertsTab::describe(const Rule& r) const
{
    QString what;
    switch (r.condition) {
    case Condition::PriceAbove: what = QStringLiteral("%1 price above %2").arg(r.symbol).arg(QString::number(r.threshold, 'f', 2)); break;
    case Condition::PriceBelow: what = QStringLiteral("%1 price below %2").arg(r.symbol).arg(QString::number(r.threshold, 'f', 2)); break;
    case Condition::ChangeAbove: what = QStringLiteral("%1 day change above %2%").arg(r.symbol).arg(QString::number(r.threshold, 'f', 2)); break;
    case Condition::ChangeBelow: what = QStringLiteral("%1 day change below %2%").arg(r.symbol).arg(QString::number(r.threshold, 'f', 2)); break;
    case Condition::IvAbove: what = QStringLiteral("%1 ATM implied vol above %2%").arg(r.symbol).arg(QString::number(r.threshold, 'f', 1)); break;
    case Condition::IvBelow: what = QStringLiteral("%1 ATM implied vol below %2%").arg(r.symbol).arg(QString::number(r.threshold, 'f', 1)); break;
    case Condition::IndicatorAbove: what = QStringLiteral("%1 %2(%3) above %4").arg(r.symbol, r.indicator).arg(r.period).arg(QString::number(r.threshold, 'f', 2)); break;
    case Condition::IndicatorBelow: what = QStringLiteral("%1 %2(%3) below %4").arg(r.symbol, r.indicator).arg(r.period).arg(QString::number(r.threshold, 'f', 2)); break;
    }
    return what;
}

void AlertsTab::promptRule(int editId)
{
    const auto existing = std::find_if(m_rules.begin(), m_rules.end(), [editId](const Rule& r) { return r.id == editId; });
    const bool editing = existing != m_rules.end();
    Rule current = editing ? *existing : Rule{};
    QDialog dialog(this);
    dialog.setWindowTitle(editing ? "Edit alert" : "Add alert");
    auto* form = new QFormLayout(&dialog);
    form->setContentsMargins(14, 12, 14, 10);
    form->setSpacing(8);
    auto* symbol = new QLineEdit(current.symbol, &dialog);
    symbol->setPlaceholderText("e.g. NVDA");
    symbol->setMaxLength(8);
    auto* condition = new QComboBox(&dialog);
    for (int c = 0; c <= static_cast<int>(Condition::IndicatorBelow); ++c) condition->addItem(conditionLabel(static_cast<Condition>(c)), c);
    condition->setCurrentIndex(static_cast<int>(current.condition));
    auto* threshold = ui::makeSpinBox(&dialog, -1e7, 1e7, 1.0, 2, current.threshold);
    auto* indicator = new QComboBox(&dialog);
    indicator->setMaxVisibleItems(24);
    indicator->setToolTip("TA-Lib indicator for the Indicator above / below conditions; choosing one switches the condition to an indicator rule");
    auto* indicatorModel = qobject_cast<QStandardItemModel*>(indicator->model());
    for (const std::string& group : ta::catalog().groups) {
        if (group == "Pattern Recognition") continue;
        // A category header the user cannot pick, then the functions in that category.
        indicator->addItem(QStringLiteral("— %1 —").arg(QString::fromStdString(group)), QString());
        if (indicatorModel) if (QStandardItem* header = indicatorModel->item(indicator->count() - 1)) { header->setEnabled(false); QFont f = header->font(); f.setBold(true); header->setFont(f); }
        for (const ta::FunctionInfo* info : ta::catalog().inGroup(group)) indicator->addItem(QString::fromStdString(info->displayName()), QString::fromStdString(info->name));
    }
    indicator->setCurrentIndex(std::max(0, indicator->findData(current.indicator.isEmpty() ? "RSI" : current.indicator)));
    auto* period = ui::makeIntSpinBox(&dialog, 1, 500, current.period);
    auto* indicatorHint = new QLabel("Indicator and period apply to the “Indicator above / below” conditions; picking an indicator selects that condition.", &dialog);
    indicatorHint->setObjectName("muted");
    indicatorHint->setWordWrap(true);
    auto* repeat = new QCheckBox("Re-arm automatically when the condition clears", &dialog);
    repeat->setChecked(current.repeat);
    auto* note = new QLineEdit(current.note, &dialog);
    note->setPlaceholderText("optional note");
    form->addRow("Symbol", symbol);
    form->addRow("Condition", condition);
    form->addRow("Threshold", threshold);
    form->addRow("Indicator", indicator);
    form->addRow("Period", period);
    form->addRow(indicatorHint);
    form->addRow(repeat);
    form->addRow("Note", note);
    // The indicator controls stay live for every condition (a disabled combo looked broken);
    // choosing an indicator moves the condition onto the matching indicator rule instead.
    auto syncEnabled = [&] {
        const Condition c = static_cast<Condition>(condition->currentData().toInt());
        const bool ind = c == Condition::IndicatorAbove || c == Condition::IndicatorBelow;
        indicatorHint->setVisible(!ind);
        threshold->setSuffix(c == Condition::ChangeAbove || c == Condition::ChangeBelow || c == Condition::IvAbove || c == Condition::IvBelow ? " %" : QString());
    };
    syncEnabled();
    connect(condition, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int) { syncEnabled(); });
    connect(indicator, qOverload<int>(&QComboBox::activated), &dialog, [&](int) {
        const Condition c = static_cast<Condition>(condition->currentData().toInt());
        if (c == Condition::IndicatorAbove || c == Condition::IndicatorBelow) return;
        condition->setCurrentIndex(condition->findData(static_cast<int>(isAbove(c) ? Condition::IndicatorAbove : Condition::IndicatorBelow)));
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(editing ? "Apply" : "Add");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QJsonObject spec{ { "symbol", symbol->text() }, { "condition", conditionName(static_cast<Condition>(condition->currentData().toInt())) }, { "threshold", threshold->value() },
                      { "indicator", indicator->currentData().toString() }, { "period", period->value() }, { "repeat", repeat->isChecked() }, { "note", note->text() } };
    if (editing) removeRules(QString::number(editId));
    QString error;
    if (!addRule(spec, &error)) ui::setStatus(m_status, error, ui::StatusKind::Error);
}

void AlertsTab::removeSelected()
{
    for (const QModelIndex& index : m_table->selectionModel()->selectedRows()) removeRules(QString::number(m_table->item(index.row(), ColSymbol)->data(Qt::UserRole + 1).toInt()));
}

void AlertsTab::resetSelected()
{
    for (const QModelIndex& index : m_table->selectionModel()->selectedRows()) {
        const int id = m_table->item(index.row(), ColSymbol)->data(Qt::UserRole + 1).toInt();
        for (Rule& r : m_rules) if (r.id == id) { r.armed = true; r.triggeredAt = QDateTime(); }
    }
    saveRules();
    fillTable();
}

// MARK: - Evaluation

double AlertsTab::impliedVolFor(const QString& symbol) const
{
    // The loaded chain's fitted surface first, then a stored chain's nearest-expiry ATM vendor vols.
    if (symbol == m_state.underlyingTicker && !m_state.surface.empty()) {
        for (const pricing::ExpirySlice& slice : m_state.surface.slices()) if (slice.fitted && slice.atmVol() > 0.0) return slice.atmVol() * 100.0;
    }
    if (!m_store) return 0.0;
    const auto chain = m_store->get(symbol);
    if (!chain || chain->download.quotes.empty()) return 0.0;
    const double spot = chain->snapshot.price;
    double bestMaturity = 1e9;
    for (const pricing::ChainQuote& q : chain->download.quotes) if (q.maturity > 7.0 / 365.0) bestMaturity = std::min(bestMaturity, q.maturity);
    std::vector<std::pair<double, double>> candidates;   // |strike-spot|, iv
    for (const pricing::ChainQuote& q : chain->download.quotes) {
        if (std::fabs(q.maturity - bestMaturity) > 1e-9 || q.vendorImpliedVol <= 0.0) continue;
        candidates.emplace_back(std::fabs(q.strike - spot), q.vendorImpliedVol);
    }
    if (candidates.empty()) return 0.0;
    std::sort(candidates.begin(), candidates.end());
    double sum = 0.0;
    int n = 0;
    for (size_t i = 0; i < std::min<size_t>(4, candidates.size()); ++i) { sum += candidates[i].second; ++n; }
    return n ? sum / n * 100.0 : 0.0;
}

void AlertsTab::evaluateRule(Rule& rule, double value, const QString& valueText)
{
    rule.lastValue = value;
    const bool met = isAbove(rule.condition) ? value > rule.threshold : value < rule.threshold;
    if (rule.armed) {
        if (met) fire(rule, value, valueText);
    } else if (rule.repeat && !met) {
        rule.armed = true;   // condition cleared: ready to fire again
    }
}

void AlertsTab::fire(Rule& rule, double value, const QString& valueText)
{
    rule.armed = false;
    rule.triggeredAt = QDateTime::currentDateTime();
    Trigger t;
    t.ruleId = rule.id;
    t.symbol = rule.symbol;
    t.value = value;
    t.at = rule.triggeredAt;
    t.message = QStringLiteral("%1 — now %2").arg(describe(rule), valueText);
    m_log.insert(m_log.begin(), t);
    if (m_log.size() > 200) m_log.resize(200);
    saveRules();
    fillTable();
    fillLog();
    if (m_sound->isChecked()) QApplication::beep();
    if (m_speak->isChecked()) SpeechDictation::speak(QStringLiteral("Alert: %1, now %2").arg(describe(rule), valueText));
    if (m_notify->isChecked() && QSystemTrayIcon::isSystemTrayAvailable()) {
        if (!m_tray) {
            QPixmap pm(32, 32);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(QColor(m_theme.accent.isEmpty() ? "#3b82f6" : m_theme.accent));
            p.setPen(Qt::NoPen);
            const QPointF pts[4] = { { 16, 2 }, { 30, 16 }, { 16, 30 }, { 2, 16 } };
            p.drawPolygon(pts, 4);
            m_tray = new QSystemTrayIcon(QIcon(pm), this);
            m_tray->setToolTip("Option Pricer alerts");
            connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] { if (onTickerSelected && !m_log.empty()) onTickerSelected(m_log.front().symbol); });
            m_tray->show();
        }
        m_tray->showMessage(QStringLiteral("Option Pricer · %1").arg(rule.symbol), t.message, QSystemTrayIcon::Information, 10000);
    }
    if (onTriggered) onTriggered(t);
}

void AlertsTab::evaluateQuotes(const std::vector<MarketDataClient::Quote>& quotes)
{
    for (const MarketDataClient::Quote& q : quotes) m_lastQuotes[q.ticker] = q;
    bool changed = false;
    for (Rule& r : m_rules) {
        const auto q = m_lastQuotes.find(r.symbol);
        switch (r.condition) {
        case Condition::PriceAbove:
        case Condition::PriceBelow:
            if (q != m_lastQuotes.end() && q->second.last > 0.0) { evaluateRule(r, q->second.last, QString::number(q->second.last, 'f', 2)); changed = true; }
            break;
        case Condition::ChangeAbove:
        case Condition::ChangeBelow:
            if (q != m_lastQuotes.end() && q->second.previousClose > 0.0) { evaluateRule(r, q->second.changePercent, formatValue(r.condition, q->second.changePercent)); changed = true; }
            break;
        case Condition::IvAbove:
        case Condition::IvBelow: {
            const double iv = impliedVolFor(r.symbol);
            if (iv > 0.0) { evaluateRule(r, iv, formatValue(r.condition, iv)); changed = true; }
            break;
        }
        case Condition::IndicatorAbove:
        case Condition::IndicatorBelow:
            break;   // needs bars: see evaluateIndicatorRules()
        }
    }
    if (changed) fillTable();
}

void AlertsTab::evaluateIndicatorRules()
{
    const QDateTime stale = QDateTime::currentDateTime().addSecs(-15 * 60);
    for (const Rule& r : m_rules) {
        if (r.condition != Condition::IndicatorAbove && r.condition != Condition::IndicatorBelow) continue;
        const auto bars = m_bars.find(r.symbol);
        if ((bars == m_bars.end() || bars->second.fetchedAt < stale) && !m_barQueue.contains(r.symbol)) m_barQueue << r.symbol;
    }
    auto evaluateWithBars = [this] {
        bool changed = false;
        for (Rule& r : m_rules) {
            if (r.condition != Condition::IndicatorAbove && r.condition != Condition::IndicatorBelow) continue;
            const auto it = m_bars.find(r.symbol);
            if (it == m_bars.end() || it->second.bars.size() < static_cast<size_t>(r.period + 2)) continue;
            ta::Bars tb;
            for (const MarketDataClient::Bar& b : it->second.bars) { tb.open.push_back(b.open); tb.high.push_back(b.high); tb.low.push_back(b.low); tb.close.push_back(b.close); tb.volume.push_back(b.volume); }
            // Replace the last close with the live quote so the indicator reflects the current session.
            const auto q = m_lastQuotes.find(r.symbol);
            if (q != m_lastQuotes.end() && q->second.last > 0.0 && !tb.close.empty()) tb.close.back() = q->second.last;
            const ta::FunctionInfo* info = ta::catalog().find(r.indicator.toStdString());
            if (!info) continue;
            ta::Params params;
            for (const ta::ParamInfo& p : info->params) if (p.name == "optInTimePeriod") params[p.name] = r.period;
            const ta::Result result = ta::compute(r.indicator.toStdString(), tb, params);
            if (!result.ok || result.outputs.empty()) continue;
            const std::vector<double>& values = result.outputs.front().values;
            double last = std::numeric_limits<double>::quiet_NaN();
            for (auto v = values.rbegin(); v != values.rend(); ++v) if (!std::isnan(*v)) { last = *v; break; }
            if (std::isnan(last)) continue;
            evaluateRule(r, last, QString::number(last, 'f', 2));
            changed = true;
        }
        if (changed) fillTable();
    };
    auto pump = std::make_shared<std::function<void()>>();
    *pump = [this, pump, evaluateWithBars] {
        while (m_barInFlight < 2 && !m_barQueue.isEmpty()) {
            const QString symbol = m_barQueue.takeFirst();
            ++m_barInFlight;
            const QDate to = QDate::currentDate();
            m_client.fetchAggregates(symbol, 1, "day", to.addDays(-400), to, [this, symbol, pump](const MarketDataClient::BarSeries& series) {
                m_bars[symbol] = Bars{ QDateTime::currentDateTime(), series.bars };
                --m_barInFlight;
                (*pump)();
            }, [this, pump](const QString&) { --m_barInFlight; (*pump)(); });
        }
        if (m_barInFlight == 0 && m_barQueue.isEmpty()) evaluateWithBars();
    };
    (*pump)();
}

void AlertsTab::checkNow()
{
    QStringList symbols;
    for (const Rule& r : m_rules) if (!symbols.contains(r.symbol)) symbols << r.symbol;
    if (symbols.isEmpty()) { ui::setStatus(m_status, "No alerts.", ui::StatusKind::Info); return; }
    ui::setStatus(m_status, QStringLiteral("Checking %1 symbol(s)…").arg(symbols.size()), ui::StatusKind::Info);
    m_client.fetchQuotes(symbols, [this](const std::vector<MarketDataClient::Quote>& quotes) {
        evaluateQuotes(quotes);
        evaluateIndicatorRules();
        int armed = 0;
        for (const Rule& r : m_rules) armed += r.armed ? 1 : 0;
        ui::setStatus(m_status, QStringLiteral("Checked at %1 · %2 alert(s), %3 armed").arg(QTime::currentTime().toString("HH:mm:ss")).arg(m_rules.size()).arg(armed), ui::StatusKind::Info);
    }, [this](const QString& message) { ui::setStatus(m_status, message, ui::StatusKind::Error); evaluateIndicatorRules(); });
}

// MARK: - Filling and reporting

void AlertsTab::fillTable()
{
    m_table->setSortingEnabled(false);
    m_table->setRowCount(static_cast<int>(m_rules.size()));
    for (size_t i = 0; i < m_rules.size(); ++i) {
        const Rule& r = m_rules[i];
        const int row = static_cast<int>(i);
        auto* symbol = ui::makeCell(r.symbol, Qt::AlignLeft | Qt::AlignVCenter);
        symbol->setData(Qt::UserRole + 1, r.id);
        QFont bold = symbol->font();
        bold.setBold(true);
        symbol->setFont(bold);
        symbol->setForeground(QBrush(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2)));
        m_table->setItem(row, ColSymbol, symbol);
        QString cond = conditionLabel(r.condition);
        if (r.condition == Condition::IndicatorAbove || r.condition == Condition::IndicatorBelow) cond = QStringLiteral("%1(%2) %3").arg(r.indicator).arg(r.period).arg(isAbove(r.condition) ? "above" : "below");
        m_table->setItem(row, ColCondition, ui::makeCell(cond, Qt::AlignLeft | Qt::AlignVCenter));
        m_table->setItem(row, ColThreshold, ui::makeCell(formatValue(r.condition, r.threshold)));
        m_table->setItem(row, ColLast, ui::makeCell(std::isnan(r.lastValue) ? QStringLiteral("–") : formatValue(r.condition, r.lastValue)));
        auto* status = ui::makeCell(r.armed ? QStringLiteral("Armed") : QStringLiteral("Triggered %1").arg(r.triggeredAt.toString("MM-dd HH:mm")), Qt::AlignLeft | Qt::AlignVCenter);
        status->setForeground(QBrush(QColor(r.armed ? (m_theme.up.isEmpty() ? "#22c55e" : m_theme.up) : (m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2))));
        m_table->setItem(row, ColStatus, status);
        m_table->setItem(row, ColRepeat, ui::makeCell(r.repeat ? "yes" : "no", Qt::AlignCenter));
        m_table->setItem(row, ColCreated, ui::makeCell(r.created.toString("yyyy-MM-dd HH:mm"), Qt::AlignLeft | Qt::AlignVCenter));
        m_table->setItem(row, ColNote, ui::makeCell(r.note, Qt::AlignLeft | Qt::AlignVCenter));
    }
    m_table->setSortingEnabled(true);
    m_edit->setEnabled(!m_rules.empty());
    m_remove->setEnabled(!m_rules.empty());
    m_reset->setEnabled(!m_rules.empty());
}

void AlertsTab::fillLog()
{
    QStringList lines;
    for (const Trigger& t : m_log) lines << QStringLiteral("%1  %2").arg(t.at.toString("yyyy-MM-dd HH:mm:ss"), t.message);
    m_logView->setPlainText(lines.isEmpty() ? QStringLiteral("No alerts have fired yet.") : lines.join('\n'));
}

QString AlertsTab::summaryText() const
{
    if (m_rules.empty()) return QStringLiteral("Alerts: none set.");
    QStringList parts;
    for (const Rule& r : m_rules) {
        parts << QStringLiteral("#%1 %2 [%3%4]").arg(r.id).arg(describe(r), r.armed ? "armed" : "triggered " + r.triggeredAt.toString("MM-dd HH:mm"),
                                                      std::isnan(r.lastValue) ? QString() : QStringLiteral(", last %1").arg(formatValue(r.condition, r.lastValue)));
    }
    QString out = QStringLiteral("Alerts (%1): %2.").arg(m_rules.size()).arg(parts.join("; "));
    if (!m_log.empty()) out += QStringLiteral(" Most recent trigger: %1 at %2.").arg(m_log.front().message, m_log.front().at.toString("HH:mm"));
    return out;
}

QString AlertsTab::rulesCsv() const
{
    QString out = "id,symbol,condition,threshold,indicator,period,repeat,status,last_value,created,note\n";
    for (const Rule& r : m_rules) {
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,\"%11\"\n").arg(r.id).arg(r.symbol, conditionName(r.condition)).arg(r.threshold).arg(r.indicator).arg(r.period)
                   .arg(r.repeat ? "yes" : "no", r.armed ? "armed" : "triggered", std::isnan(r.lastValue) ? QString() : QString::number(r.lastValue, 'f', 2), r.created.toString(Qt::ISODate), QString(r.note).replace('"', '\''));
    }
    return out;
}
