//
//  AssistantPanel.cpp
//  OptionPricing
//

#include "AssistantPanel.h"
#include "Widgets.h"

namespace {
constexpr int kSilenceMs = 1800;   ///< pause that ends a dictation and sends it
} // namespace

AssistantPanel::AssistantPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
    wire();
    refreshKeyStatus();
    appendNote("Type a request below and press Enter (Shift+Enter for a new line), or click Listen to dictate. "
               "Ask about what is on screen, or tell me what to do: “pull up option chains for AAPL”, “mark support and resistance on this chart”, "
               "“switch to the volatility tab”.");
}

// MARK: - Construction

void AssistantPanel::buildUi()
{
    m_provider = new QComboBox(this);
    m_provider->addItems(AssistantClient::providerNames());
    m_provider->setCurrentIndex(static_cast<int>(m_client.provider()));
    m_provider->setToolTip("Where the assistant runs: Anthropic or OpenAI in the cloud, or Ollama models on this Mac");
    m_model = new QComboBox(this);
    m_model->setEditable(true);
    m_model->setInsertPolicy(QComboBox::NoInsert);
    m_model->setToolTip("Model used by the assistant (type a name or pick one; Refresh lists the provider's models)");
    m_model->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_refreshModels = new QToolButton(this);
    m_refreshModels->setText("↻");
    m_refreshModels->setToolTip("List the provider's available models (Ollama: installed models; OpenAI: your account's chat models)");
    m_refreshModels->setCursor(Qt::PointingHandCursor);
    populateModels(AssistantClient::defaultModels(m_client.provider()));
    m_setKey = ui::makeButton(this, "AI Key…", "secondary", "Enter the provider's API key, or the Ollama server address");
    m_keyStatus = new QLabel(this);
    m_keyStatus->setObjectName("muted");
    m_keyStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_speak = new QCheckBox("Speak replies", this);
    m_speak->setChecked(QSettings().value("ai.speak", false).toBool());
    m_speak->setToolTip("Read the assistant's replies aloud");
    m_clear = ui::makeButton(this, "Clear", "secondary", "Forget the conversation");

    // Header: the model on its own row so its name is never truncated; key status, key
    // button and Clear underneath.
    auto* top = new QHBoxLayout;
    top->setSpacing(6);
    top->addWidget(m_provider);
    top->addWidget(m_model, 1);
    top->addWidget(m_refreshModels);
    auto* second = new QHBoxLayout;
    second->setSpacing(6);
    second->addWidget(m_keyStatus, 1);
    second->addWidget(m_setKey);
    second->addWidget(m_clear);

    m_transcript = new QTextBrowser(this);
    m_transcript->setOpenExternalLinks(true);
    m_transcript->setObjectName("assistantTranscript");
    m_transcript->setMinimumHeight(160);

    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText("Ask about this screen, or say what to do… (Enter sends, Shift+Enter for a new line)");
    m_input->setFixedHeight(64);
    m_input->installEventFilter(this);

    m_mic = new QToolButton(this);
    m_mic->setText("🎙 Listen");
    m_mic->setCheckable(true);
    m_mic->setCursor(Qt::PointingHandCursor);
    m_mic->setToolTip("Dictate a request. Dictation ends after a short pause and is sent automatically. (⌘⇧V)");
    m_analyze = ui::makeButton(this, "Analyze screen", "secondary", "Ask the assistant to analyse what is currently displayed (⌘⇧L)");
    m_send = ui::makeButton(this, "Send", "primary", "Send the request");
    // Two action rows so the buttons keep their labels in a narrow dock.
    m_mic->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_analyze->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* actionsTop = new QHBoxLayout;
    actionsTop->setSpacing(6);
    actionsTop->addWidget(m_mic, 1);
    actionsTop->addWidget(m_analyze, 1);
    auto* actionsBottom = new QHBoxLayout;
    actionsBottom->setSpacing(6);
    actionsBottom->addWidget(m_speak);
    actionsBottom->addStretch(1);
    actionsBottom->addWidget(m_send);
    auto* actions = new QVBoxLayout;
    actions->setSpacing(6);
    actions->addLayout(actionsTop);
    actions->addLayout(actionsBottom);

    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_thinking = new ui::SpinningDiamond(this, 18);
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(6);
    statusRow->addWidget(m_thinking);
    statusRow->addWidget(m_status, 1);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 8, 10, 8);   // slim left edge: the dock sits right against the chart
    root->setSpacing(6);
    root->addLayout(top);
    root->addLayout(second);
    root->addWidget(m_transcript, 1);
    root->addWidget(m_input);
    root->addLayout(actions);
    root->addLayout(statusRow);
    setMinimumWidth(340);

    m_silenceTimer = new QTimer(this);
    m_silenceTimer->setSingleShot(true);
    m_silenceTimer->setInterval(kSilenceMs);
}

void AssistantPanel::wire()
{
    connect(m_send, &QPushButton::clicked, this, [this] { submit(m_input->toPlainText()); });
    connect(m_analyze, &QPushButton::clicked, this, [this] { analyzeScreen(); });
    connect(m_mic, &QToolButton::clicked, this, [this](bool) { toggleListening(); });
    connect(m_clear, &QPushButton::clicked, this, [this] {
        m_client.reset();
        m_entries.clear();
        m_transcript->clear();
        appendNote("Conversation cleared.");
    });
    connect(m_setKey, &QPushButton::clicked, this, [this] { promptForApiKey(); });
    connect(m_model, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        const QString model = text.trimmed();
        if (model.isEmpty() || m_updatingModels) return;
        m_client.setModel(model);
    });
    connect(m_provider, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_client.setProvider(static_cast<AssistantClient::Provider>(index));
        populateModels(AssistantClient::defaultModels(m_client.provider()));
        refreshKeyStatus();
        if (m_client.provider() == AssistantClient::Provider::Ollama) refreshModelList();
        appendNote(QStringLiteral("Provider: %1, model %2.").arg(AssistantClient::providerName(m_client.provider()), m_client.model()));
    });
    connect(m_refreshModels, &QToolButton::clicked, this, [this] { refreshModelList(); });
    if (m_client.provider() == AssistantClient::Provider::Ollama) QTimer::singleShot(300, this, [this] { refreshModelList(); });
    connect(m_speak, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue("ai.speak", on);
        if (!on) SpeechDictation::stopSpeaking();
    });
    connect(m_silenceTimer, &QTimer::timeout, this, [this] {
        if (m_listening && !m_partialTranscript.trimmed().isEmpty()) stopListening(true);
    });
}

void AssistantPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!m_relayoutTimer) {
        m_relayoutTimer = new QTimer(this);
        m_relayoutTimer->setSingleShot(true);
        m_relayoutTimer->setInterval(150);
        connect(m_relayoutTimer, &QTimer::timeout, this, [this] { rebuildTranscript(); });
    }
    m_relayoutTimer->start();
}

bool AssistantPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !(key->modifiers() & Qt::ShiftModifier)) {
            submit(m_input->toPlainText());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// MARK: - Conversation

void AssistantPanel::submit(const QString& rawText)
{
    const QString text = rawText.trimmed();
    if (text.isEmpty()) return;
    if (m_client.busy()) {
        // Keep the typed text so it is not lost; the user can send it when the reply arrives.
        setStatus("Still working on the previous request — press Enter again when it finishes.", true);
        return;
    }
    m_input->clear();
    appendMessage("user", text);

    QString feedback;
    if (localCommandHandler && localCommandHandler(text, feedback)) {
        appendMessage("assistant", feedback);
        if (m_speak->isChecked() && m_lastInputWasVoice) SpeechDictation::speak(feedback);
        m_lastInputWasVoice = false;
        if (onReply) onReply(feedback, true);
        return;
    }
    if (!m_client.hasApiKey()) {
        appendNote(QStringLiteral("No %1 API key is set. Simple commands such as “pull up option chains for AAPL” still work; for analysis, set a key with AI Key…, export %2, or switch the provider to Ollama (local).")
                       .arg(AssistantClient::providerName(m_client.provider()), m_client.provider() == AssistantClient::Provider::OpenAI ? "OPENAI_API_KEY" : "ANTHROPIC_API_KEY"));
        m_lastInputWasVoice = false;
        return;
    }
    m_lastInputWasVoice = false;
    auto sendWithContext = [this, text](const QString& context, const QImage& image) {
        AssistantClient::Callbacks callbacks;
        callbacks.onStatus = [this](const QString& status) { setStatus(status); };
        callbacks.onToolCall = [this](const QString& name, const QJsonObject& input) {
            // One muted line per action, readable rather than raw JSON.
            QStringList parts;
            for (auto it = input.begin(); it != input.end(); ++it) {
                const QJsonValue v = it.value();
                parts << (v.isDouble() ? QString::number(v.toDouble(), 'g', 8) : (v.isBool() ? (v.toBool() ? "yes" : "no") : v.toVariant().toString()));
            }
            QString pretty = name;
            pretty.replace('_', ' ');
            appendTool(parts.isEmpty() ? pretty : QStringLiteral("%1 · %2").arg(pretty, parts.join(" · ")), false);
        };
        callbacks.onToolResult = [this](const QString& name, const QString& preview, bool isError) {
            if (isError) appendTool(QStringLiteral("%1 failed: %2").arg(name, preview), true);
        };
        callbacks.onReply = [this](const QString& reply) {
            setBusy(false);
            appendMessage("assistant", reply);
            setStatus(QStringLiteral("%1 · %2 in / %3 out tokens").arg(m_client.model()).arg(m_client.lastInputTokens()).arg(m_client.lastOutputTokens()));
            if (m_speak->isChecked()) SpeechDictation::speak(reply);
            if (onReply) onReply(reply, true);
        };
        callbacks.onError = [this](const QString& message) {
            setBusy(false);
            appendNote(message);
            setStatus(message, true);
            if (onReply) onReply(message, false);
        };
        m_client.send(text, context, image, callbacks);
    };
    setBusy(true);
    setStatus("Gathering screen context…");
    if (contextProvider) contextProvider(sendWithContext);
    else sendWithContext(QString(), QImage());
}

void AssistantPanel::debugTypeAndSend(const QString& text, bool useButton)
{
    // Drives the real widgets: text in the box, then either the Send button or the Enter key.
    m_input->setPlainText(text);
    if (useButton) {
        m_send->click();
    } else {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(m_input, &press);
    }
}

QString AssistantPanel::debugLastTranscriptLine() const
{
    // Last rendered transcript entry, as plain text.
    QTextDocument doc;
    doc.setHtml(m_transcriptHtml);
    const QStringList lines = doc.toPlainText().split('\n', Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString() : lines.last().trimmed();
}

void AssistantPanel::analyzeScreen()
{
    submit("Analyze what is currently on screen. Summarize the key figures, point out anything notable, and suggest what to look at or do next.");
}

void AssistantPanel::appendMessage(const QString& role, const QString& text)
{
    Entry entry;
    entry.kind = role == "user" ? Entry::Kind::User : Entry::Kind::Assistant;
    entry.text = text;
    entry.meta = entry.kind == Entry::Kind::Assistant ? QStringLiteral("%1 · %2").arg(m_client.model(), QTime::currentTime().toString("HH:mm")) : QTime::currentTime().toString("HH:mm");
    m_entries.push_back(entry);
    rebuildTranscript();
}

void AssistantPanel::appendTool(const QString& text, bool isError)
{
    Entry entry;
    entry.kind = Entry::Kind::Tool;
    entry.text = text;
    entry.error = isError;
    m_entries.push_back(entry);
    rebuildTranscript();
}

void AssistantPanel::appendNote(const QString& text)
{
    Entry entry;
    entry.kind = Entry::Kind::Note;
    entry.text = text;
    m_entries.push_back(entry);
    rebuildTranscript();
}

void AssistantPanel::setStatus(const QString& text, bool error)
{
    ui::setStatus(m_status, text, error ? ui::StatusKind::Error : ui::StatusKind::Info);
    m_status->setToolTip(text);
}

void AssistantPanel::setBusy(bool busy)
{
    if (busy) m_thinking->start();
    else m_thinking->stop();
    m_send->setEnabled(!busy);
    m_analyze->setEnabled(!busy);
    if (onBusyChanged) onBusyChanged(busy);
}

QString AssistantPanel::escape(const QString& text)
{
    return text.toHtmlEscaped();
}

// MARK: - Transcript rendering

namespace {

QString pick(const QString& value, const char* fallback) { return value.isEmpty() ? QString::fromLatin1(fallback) : value; }

bool isTableRow(const QString& line)
{
    const QString t = line.trimmed();
    return t.startsWith('|') && t.count('|') >= 2;
}

bool isTableSeparator(const QString& line)
{
    static const QRegularExpression re("^\\s*\\|?\\s*:?-{2,}:?\\s*(\\|\\s*:?-{2,}:?\\s*)*\\|?\\s*$");
    return re.match(line).hasMatch();
}

QStringList splitCells(QString line)
{
    line = line.trimmed();
    if (line.startsWith('|')) line.remove(0, 1);
    if (line.endsWith('|')) line.chop(1);
    QStringList cells;
    for (const QString& c : line.split('|')) cells << c.trimmed();
    return cells;
}

/// Numbers, prices, percentages and signed changes: "$463.39", "+4.77 %", "−0.0176", "2.75 M".
bool looksNumeric(const QString& cell)
{
    static const QRegularExpression re("^[\\$€£]?\\s?[+\\-−]?\\s?\\d[\\d,]*(\\.\\d+)?\\s?(%|[kKmMbB]|bp|pts?|x)?$");
    return re.match(cell.trimmed()).hasMatch();
}

int signOf(const QString& cell)
{
    const QString t = cell.trimmed();
    if (t.startsWith('+')) return 1;
    if (t.startsWith('-') || t.startsWith(QChar(0x2212))) return -1;
    return 0;
}

} // namespace

QString AssistantPanel::inlineMarkdown(const QString& raw) const
{
    QString t = escape(raw);
    // Allow <br> inside table cells (the models use it for multi-line cells).
    t.replace("&lt;br&gt;", "<br>");
    t.replace("&lt;br/&gt;", "<br>");
    t.replace("&lt;br /&gt;", "<br>");
    t.replace(QRegularExpression("\\*\\*(.+?)\\*\\*"), "<b>\\1</b>");
    t.replace(QRegularExpression("(^|[\\s(>])\\*([^*\\s][^*]*?)\\*(?=[\\s).,;:!?<]|$)"), "\\1<i>\\2</i>");
    t.replace(QRegularExpression("`([^`]+)`"),
              QStringLiteral("<code style='font-family:Menlo,\"SF Mono\",monospace;font-size:11px;color:%1'>\\1</code>").arg(pick(m_theme.accent3, "#22d3ee")));
    return t;
}

QString AssistantPanel::renderTable(const QStringList& rows) const
{
    // rows[0] = header, rows[1] = separator, rows[2..] = body
    const QStringList header = splitCells(rows.value(0));
    const QStringList separators = splitCells(rows.value(1));
    // Wide tables in a narrow panel would break words inside cells; show each row as a card instead.
    const int available = m_transcript ? m_transcript->viewport()->width() : 400;
    if (header.size() >= 4 && available < 520) {
        const QString strong = pick(m_theme.textStrong, "#f3f6fb");
        const QString muted = pick(m_theme.textMuted, "#8294ad");
        const QString surfaceAlt = pick(m_theme.surfaceAlt, "#192338");
        const QString border = pick(m_theme.border, "#273449");
        QString html;
        for (int r = 2; r < rows.size(); ++r) {
            const QStringList cells = splitCells(rows[r]);
            QString body = QStringLiteral("<div style='color:%1;font-weight:700'>%2</div>").arg(strong, inlineMarkdown(cells.value(0)));
            for (int i = 1; i < header.size(); ++i) {
                const QString cell = cells.value(i);
                if (cell.isEmpty()) continue;
                const bool numeric = looksNumeric(cell);
                body += QStringLiteral("<div><span style='color:%1;font-size:11px'>%2</span>&nbsp; <span%3>%4</span></div>")
                            .arg(muted, inlineMarkdown(header[i].toUpper()), numeric ? QStringLiteral(" style='font-family:Menlo,\"SF Mono\",monospace'") : QString(), inlineMarkdown(cell));
            }
            html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='6' style='margin:4px 0'><tr><td bgcolor='%1' style='border-bottom:1px solid %2'>%3</td></tr></table>")
                        .arg(surfaceAlt, border, body);
        }
        return html;
    }
    QStringList align;
    for (int i = 0; i < header.size(); ++i) {
        const QString sep = separators.value(i);
        align << (sep.startsWith(':') && sep.endsWith(':') ? "center" : (sep.endsWith(':') ? "right" : "left"));
    }
    const QString headBg = pick(m_theme.surfaceAlt, "#192338");
    const QString border = pick(m_theme.border, "#273449");
    const QString accent = pick(m_theme.accent, "#3b82f6");
    const QString strong = pick(m_theme.textStrong, "#f3f6fb");
    const QString up = pick(m_theme.up, "#22c55e");
    const QString down = pick(m_theme.down, "#ef4444");
    const QString zebra = pick(m_theme.surface, "#121a2b");

    QString html = QStringLiteral("<table width='100%' cellspacing='0' cellpadding='5' style='margin:6px 0 8px 0'>");
    html += "<tr>";
    for (int i = 0; i < header.size(); ++i) {
        html += QStringLiteral("<th align='%1' bgcolor='%2' style='color:%3;font-size:11px;font-weight:700;border-bottom:2px solid %4'>%5</th>")
                    .arg(align.value(i), headBg, strong, accent, inlineMarkdown(header[i].toUpper()));
    }
    html += "</tr>";
    for (int r = 2; r < rows.size(); ++r) {
        const QStringList cells = splitCells(rows[r]);
        html += QStringLiteral("<tr%1>").arg(r % 2 == 0 ? QStringLiteral(" bgcolor='%1'").arg(zebra) : QString());
        for (int i = 0; i < header.size(); ++i) {
            const QString cell = cells.value(i);
            const bool numeric = looksNumeric(cell);
            const int sign = numeric ? signOf(cell) : 0;
            QString style = QStringLiteral("border-bottom:1px solid %1;").arg(border);
            if (numeric) style += "font-family:Menlo,'SF Mono',monospace;font-size:12px;";
            if (sign > 0) style += QStringLiteral("color:%1;font-weight:600;").arg(up);
            if (sign < 0) style += QStringLiteral("color:%1;font-weight:600;").arg(down);
            if (i == 0 && !numeric) style += QStringLiteral("color:%1;font-weight:600;").arg(strong);
            html += QStringLiteral("<td align='%1' style='%2'>%3</td>").arg(numeric ? QStringLiteral("right") : align.value(i), style, inlineMarkdown(cell));
        }
        html += "</tr>";
    }
    html += "</table>";
    return html;
}

QString AssistantPanel::renderMarkdown(const QString& text) const
{
    const QString accent2 = pick(m_theme.accent2, "#f59e0b");
    const QString strong = pick(m_theme.textStrong, "#f3f6fb");
    const QString muted = pick(m_theme.textMuted, "#8294ad");
    const QString surfaceAlt = pick(m_theme.surfaceAlt, "#192338");
    const QString border = pick(m_theme.border, "#273449");

    QString html;
    const QStringList lines = text.split('\n');
    int listDepth = 0;                 // open <ul>/<ol> nesting
    QStringList listKinds;             // "ul" or "ol" per depth
    auto closeLists = [&](int toDepth) {
        while (listDepth > toDepth) { html += "</" + listKinds.takeLast() + ">"; --listDepth; }
    };
    static const QRegularExpression bulletRe("^(\\s*)[-*•]\\s+(.*)$");
    static const QRegularExpression numberedRe("^(\\s*)(\\d+)[.)]\\s+(.*)$");
    static const QRegularExpression headingRe("^(#{1,4})\\s+(.*?)\\s*#*\\s*$");
    static const QRegularExpression ruleRe("^\\s*(-{3,}|\\*{3,}|_{3,})\\s*$");
    static const QRegularExpression calloutRe("^\\**\\s*(bottom line|summary|takeaway|verdict|conclusion)\\b", QRegularExpression::CaseInsensitiveOption);

    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines[i];
        // Tables: a header row followed by a separator row.
        if (isTableRow(line) && i + 1 < lines.size() && isTableSeparator(lines[i + 1])) {
            closeLists(0);
            QStringList rows{ line, lines[i + 1] };
            int j = i + 2;
            while (j < lines.size() && isTableRow(lines[j]) && !lines[j].trimmed().isEmpty()) rows << lines[j++];
            html += renderTable(rows);
            i = j - 1;
            continue;
        }
        const QRegularExpressionMatch bullet = bulletRe.match(line);
        const QRegularExpressionMatch numbered = numberedRe.match(line);
        if (bullet.hasMatch() || numbered.hasMatch()) {
            const int depth = 1 + static_cast<int>((bullet.hasMatch() ? bullet.captured(1) : numbered.captured(1)).size()) / 2;
            const QString kind = bullet.hasMatch() ? "ul" : "ol";
            if (depth > listDepth) {
                while (listDepth < depth) {
                    html += kind == "ul" ? "<ul style='margin:2px 0 2px 16px;padding:0'>" : "<ol style='margin:2px 0 2px 18px;padding:0'>";
                    listKinds << kind;
                    ++listDepth;
                }
            } else {
                closeLists(depth);
            }
            html += "<li style='margin:1px 0'>" + inlineMarkdown(bullet.hasMatch() ? bullet.captured(2) : numbered.captured(3)) + "</li>";
            continue;
        }
        closeLists(0);
        if (line.trimmed().isEmpty()) { html += "<div style='height:5px'></div>"; continue; }
        if (ruleRe.match(line).hasMatch()) { html += QStringLiteral("<hr style='border:0;border-top:1px solid %1;margin:6px 0'>").arg(border); continue; }
        const QRegularExpressionMatch heading = headingRe.match(line);
        if (heading.hasMatch()) {
            const int level = static_cast<int>(heading.captured(1).size());
            const QString body = inlineMarkdown(heading.captured(2));
            if (level == 1) html += QStringLiteral("<div style='font-size:15px;font-weight:700;color:%1;margin:8px 0 2px 0'>%2</div>").arg(accent2, body);
            else if (level == 2) html += QStringLiteral("<div style='font-size:13px;font-weight:700;color:%1;margin:8px 0 2px 0'>%2</div>").arg(strong, body);
            else html += QStringLiteral("<div style='font-size:11px;font-weight:700;color:%1;margin:7px 0 1px 0'>%2</div>").arg(muted, body.toUpper());
            continue;
        }
        if (calloutRe.match(line.trimmed()).hasMatch()) {
            html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='7' style='margin:8px 0'><tr><td width='4' bgcolor='%1'></td>"
                                   "<td bgcolor='%2' style='color:%3'>%4</td></tr></table>")
                        .arg(accent2, surfaceAlt, strong, inlineMarkdown(line.trimmed()));
            continue;
        }
        // A bold "Label:" lead-in reads as a key figure; keep paragraphs tight.
        html += "<div style='margin:2px 0'>" + inlineMarkdown(line) + "</div>";
    }
    closeLists(0);
    return html;
}

void AssistantPanel::rebuildTranscript()
{
    const QString accent2 = pick(m_theme.accent2, "#f59e0b");
    const QString accent3 = pick(m_theme.accent3, "#22d3ee");
    const QString muted = pick(m_theme.textMuted, "#8294ad");
    const QString down = pick(m_theme.down, "#ef4444");
    const QString surfaceAlt = pick(m_theme.surfaceAlt, "#192338");
    const QString text = pick(m_theme.text, "#c7d2e3");

    QString html = QStringLiteral("<div style='color:%1'>").arg(text);
    for (const Entry& e : m_entries) {
        switch (e.kind) {
        case Entry::Kind::User:
            html += QStringLiteral("<div style='margin:8px 0 4px 0'><span style='color:%1;font-weight:700;font-size:11px'>YOU</span>"
                                   "<span style='color:%2;font-size:11px'> · %3</span><div style='margin:2px 0 0 0'>%4</div></div>")
                        .arg(accent2, muted, e.meta, renderMarkdown(e.text));
            break;
        case Entry::Kind::Assistant:
            // Report card: a left accent bar beside the rendered note.
            html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0' style='margin:6px 0 10px 0'><tr><td width='3' bgcolor='%1'></td>"
                                   "<td style='padding:6px 8px 8px 10px'><span style='color:%1;font-weight:700;font-size:11px'>ASSISTANT</span>"
                                   "<span style='color:%2;font-size:11px'> · %3</span>%4</td></tr></table>")
                        .arg(accent3, muted, e.meta, renderMarkdown(e.text));
            break;
        case Entry::Kind::Tool:
            html += QStringLiteral("<div style='margin:1px 0 1px 10px;color:%1;font-size:11px'>⚙ %2</div>").arg(e.error ? down : muted, escape(e.text));
            break;
        case Entry::Kind::Note:
            html += QStringLiteral("<div style='margin:6px 0;color:%1;font-style:italic'>%2</div>").arg(muted, escape(e.text));
            break;
        }
    }
    html += "</div>";
    Q_UNUSED(surfaceAlt);
    m_transcriptHtml = html;
    m_transcript->setHtml(html);
    m_transcript->moveCursor(QTextCursor::End);
}

void AssistantPanel::debugRenderSample()
{
    appendMessage("user", "Analyze what is on screen.");
    appendMessage("assistant",
                  "# MPC · $463.39 · +4.77% · at the 52-week high\n"
                  "Marathon Petroleum closed near the session high on volume about 30% above its 10-day average.\n\n"
                  "## Key levels\n"
                  "| Level | Price | Type | Why it matters |\n"
                  "|-------|------:|------|----------------|\n"
                  "| 52-week high | $467.85 | Resistance | Two tests this week; a close above opens $480 |\n"
                  "| Breakout shelf | $440.00 – $450.00 | Support | Early-October consolidation base |\n"
                  "| SMA 20 | $426.40 | Support | Rising; held every pullback since July |\n"
                  "| EMA 50 | $384.10 | Support | Trend anchor; a break would end the advance |\n\n"
                  "## Technicals\n"
                  "- Strong uptrend since the July 5 low near $266: **+74% in three months**.\n"
                  "- Price is riding above both moving averages; the last dip (Sep 20) was bought within two sessions.\n"
                  "- Day's range $447.59 – $467.85; change **+4.77%**, previous close $442.26.\n\n"
                  "## Options and volatility\n"
                  "| Measure | Value |\n|---------|------:|\n| Market sigma (app) | 20.00% |\n| 20-day realized vol | 38.40% |\n| ATM implied, 21 days | 41.20% |\n\n"
                  "## Risks\n"
                  "- Extended move: a failure to hold $450 could retrace toward the SMA 20.\n"
                  "- Implied vol is rich versus realized, so long premium is expensive here.\n\n"
                  "## Next steps\n"
                  "1. I can mark the $440–$450 support and $468–$480 resistance zones on the chart.\n"
                  "2. Open the option chain to compare near-term skew.\n"
                  "3. Load a bull call spread 450/470 on the November expiry and show its metrics.\n\n"
                  "**Bottom line:** MPC is at a new 52-week high on heavy volume; respect $450 as the line in the sand and let the chain's implied vol guide whether to buy or sell premium.");
}

// MARK: - Keys and theme

void AssistantPanel::populateModels(const QStringList& models)
{
    m_updatingModels = true;
    const QString current = m_client.model();
    m_model->clear();
    m_model->addItems(models);
    if (!current.isEmpty() && !models.contains(current)) m_model->addItem(current);
    m_model->setCurrentText(current.isEmpty() ? models.value(0) : current);
    m_updatingModels = false;
    if (m_client.model().isEmpty() && !models.isEmpty()) m_client.setModel(models.first());
}

void AssistantPanel::refreshModelList()
{
    setStatus(QStringLiteral("Listing %1 models…").arg(AssistantClient::providerName(m_client.provider())));
    m_client.fetchModels([this](const QStringList& models, const QString& error) {
        populateModels(models);
        if (!error.isEmpty()) { appendNote(error); setStatus(error, true); }
        else setStatus(QStringLiteral("%1 model%2 available").arg(models.size()).arg(models.size() == 1 ? "" : "s"));
    });
}

bool AssistantPanel::promptForApiKey()
{
    const AssistantClient::Provider provider = m_client.provider();
    const bool ollama = provider == AssistantClient::Provider::Ollama;
    QDialog dialog(this);
    dialog.setWindowTitle(ollama ? QStringLiteral("Ollama Server") : QStringLiteral("%1 API Key").arg(AssistantClient::providerName(provider)));
    auto* intro = new QLabel(ollama ? QStringLiteral("Address of the Ollama server running your local models (default http://localhost:11434). No key is needed.")
                                    : QStringLiteral("Paste a%1 API key for the assistant. It is sent only to %2. Setting the %3 environment variable avoids this dialog.")
                                          .arg(provider == AssistantClient::Provider::OpenAI ? "n OpenAI" : "n Anthropic",
                                               provider == AssistantClient::Provider::OpenAI ? "api.openai.com" : "api.anthropic.com",
                                               provider == AssistantClient::Provider::OpenAI ? "OPENAI_API_KEY" : "ANTHROPIC_API_KEY"), &dialog);
    intro->setWordWrap(true);
    intro->setObjectName("muted");
    auto* edit = new QLineEdit(&dialog);
    edit->setEchoMode(ollama ? QLineEdit::Normal : QLineEdit::Password);
    edit->setPlaceholderText(ollama ? QStringLiteral("http://localhost:11434") : (provider == AssistantClient::Provider::OpenAI ? QStringLiteral("sk-…") : QStringLiteral("sk-ant-…")));
    if (ollama) edit->setText(m_client.ollamaHost());
    edit->setMinimumWidth(360);
    auto* remember = new QCheckBox("Remember on this Mac (stored in the application preferences, unencrypted)", &dialog);
    remember->setVisible(!ollama);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    layout->addWidget(intro);
    layout->addWidget(edit);
    layout->addWidget(remember);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted) {
        if (ollama) {
            m_client.setOllamaHost(edit->text());
            refreshModelList();
        } else if (!edit->text().trimmed().isEmpty()) {
            m_client.setApiKey(edit->text(), remember->isChecked());
        }
    }
    refreshKeyStatus();
    return m_client.hasApiKey();
}

void AssistantPanel::refreshKeyStatus()
{
    if (m_client.provider() == AssistantClient::Provider::Ollama) {
        m_keyStatus->setText(QStringLiteral("Local: %1").arg(m_client.ollamaHost()));
        m_keyStatus->setToolTip("Ollama server; click AI Key… to change the address");
        m_setKey->setText("Server…");
        return;
    }
    m_setKey->setText("AI Key…");
    if (m_client.hasApiKey()) {
        const QString source = m_client.apiKeySource();
        m_keyStatus->setText(source.startsWith("environment") ? QStringLiteral("Key: environment") : QStringLiteral("Key: %1").arg(source));
        m_keyStatus->setToolTip(QStringLiteral("%1 API key source: %2").arg(AssistantClient::providerName(m_client.provider()), source));
    } else {
        m_keyStatus->setText("No AI key");
        m_keyStatus->setToolTip(QStringLiteral("Set %1 or click AI Key…").arg(m_client.provider() == AssistantClient::Provider::OpenAI ? "OPENAI_API_KEY" : "ANTHROPIC_API_KEY"));
    }
}

void AssistantPanel::applyTheme(const Theme& theme)
{
    m_theme = theme;
    m_thinking->setColor(QColor(theme.accent3.isEmpty() ? "#22d3ee" : theme.accent3));
    rebuildTranscript();   // colours come from the theme
}

void AssistantPanel::focusInput()
{
    m_input->setFocus();
}

// MARK: - Voice

void AssistantPanel::toggleListening()
{
    if (m_listening) stopListening(true);
    else startListening();
}

void AssistantPanel::startListening()
{
    if (m_listening) return;
    if (!m_speech.isAvailable()) {
        appendNote(m_speech.unavailableReason());
        m_mic->setChecked(false);
        return;
    }
    SpeechDictation::stopSpeaking();
    setStatus("Requesting microphone and speech permission…");
    m_speech.requestAuthorization([this](bool granted, const QString& message) {
        if (!granted) {
            appendNote(message);
            setStatus(message, true);
            m_mic->setChecked(false);
            return;
        }
        m_partialTranscript.clear();
        const bool started = m_speech.start(
            [this](const QString& text, bool isFinal) {
                m_partialTranscript = text;
                m_input->setPlainText(text);
                m_input->moveCursor(QTextCursor::End);
                if (isFinal) {
                    m_listening = false;
                    m_mic->setChecked(false);
                    m_silenceTimer->stop();
                    setStatus(QString());
                    if (m_sendAfterStop && !text.trimmed().isEmpty()) {
                        m_lastInputWasVoice = true;
                        submit(text);
                    }
                    m_sendAfterStop = false;
                } else {
                    m_silenceTimer->start();   // a pause ends the dictation
                    setStatus("Listening… pause to send, or click the microphone again.");
                }
            },
            [this](const QString& error) {
                appendNote(QStringLiteral("Dictation error: %1").arg(error));
                setStatus(error, true);
                m_listening = false;
                m_mic->setChecked(false);
            });
        if (!started) {
            m_mic->setChecked(false);
            return;
        }
        m_listening = true;
        m_sendAfterStop = true;
        m_mic->setChecked(true);
        setStatus("Listening…");
    });
}

void AssistantPanel::stopListening(bool sendTranscript)
{
    if (!m_listening) return;
    m_sendAfterStop = sendTranscript;
    m_silenceTimer->stop();
    setStatus("Finishing dictation…");
    m_speech.stop();
    // If the recogniser never delivers a final result (short clips sometimes don't), fall back to the last partial.
    QTimer::singleShot(1200, this, [this] {
        if (m_listening) {
            m_listening = false;
            m_mic->setChecked(false);
            setStatus(QString());
            if (m_sendAfterStop && !m_partialTranscript.trimmed().isEmpty()) {
                m_lastInputWasVoice = true;
                submit(m_partialTranscript);
            }
            m_sendAfterStop = false;
        }
    });
}
