//
//  AssistantClient.cpp
//  OptionPricing
//

#include "AssistantClient.h"

#include <QtCore/QBuffer>

namespace {
constexpr const char* kAnthropicEndpoint = "https://api.anthropic.com/v1/messages";
constexpr const char* kAnthropicVersion = "2023-06-01";
constexpr const char* kOpenAiEndpoint = "https://api.openai.com/v1/chat/completions";
constexpr const char* kOpenAiModelsEndpoint = "https://api.openai.com/v1/models";
constexpr const char* kDefaultOllamaHost = "http://localhost:11434";
constexpr int kMaxToolRounds = 8;
constexpr int kMaxTokens = 2048;
constexpr int kMaxImageWidth = 1600;
constexpr int kMaxHistoryTurns = 40;
constexpr const char* kContextMarker = "[Screen context]";

QString stringContent(const QJsonValue& content)
{
    // OpenAI-style content may be a string or an array of {type:"text"} parts.
    if (content.isString()) return content.toString();
    QString text;
    for (const QJsonValue part : content.toArray()) {
        const QJsonObject p = part.toObject();
        if (p["type"].toString() == "text") text += p["text"].toString();
    }
    return text;
}
} // namespace

// MARK: - Provider, model, credentials

AssistantClient::AssistantClient()
{
    const QSettings settings;
    const QString providerName = settings.value("ai.provider", "Anthropic").toString();
    m_provider = providerName == "OpenAI" ? Provider::OpenAI : (providerName.startsWith("Ollama") ? Provider::Ollama : Provider::Anthropic);
    m_ollamaHost = settings.value("ai.ollamaHost", kDefaultOllamaHost).toString();
    m_model = settings.value(modelSettingsKey(m_provider), defaultModels(m_provider).value(0)).toString();
}

QString AssistantClient::providerName(Provider p)
{
    switch (p) {
    case Provider::Anthropic: return QStringLiteral("Anthropic");
    case Provider::OpenAI: return QStringLiteral("OpenAI");
    case Provider::Ollama: return QStringLiteral("Ollama (local)");
    }
    return {};
}

QStringList AssistantClient::providerNames()
{
    return { providerName(Provider::Anthropic), providerName(Provider::OpenAI), providerName(Provider::Ollama) };
}

QString AssistantClient::keySettingsKey(Provider p)
{
    return p == Provider::OpenAI ? QStringLiteral("ai.openaiKey") : QStringLiteral("ai.apiKey");
}

QString AssistantClient::modelSettingsKey(Provider p)
{
    switch (p) {
    case Provider::Anthropic: return QStringLiteral("ai.model");
    case Provider::OpenAI: return QStringLiteral("ai.model.openai");
    case Provider::Ollama: return QStringLiteral("ai.model.ollama");
    }
    return {};
}

void AssistantClient::setProvider(Provider p)
{
    if (p == m_provider) return;
    m_provider = p;
    QSettings settings;
    if (m_persistSelection) settings.setValue("ai.provider", providerName(p));
    m_model = settings.value(modelSettingsKey(p), defaultModels(p).value(0)).toString();
}

void AssistantClient::setModel(const QString& model)
{
    m_model = model.trimmed();
    if (!m_model.isEmpty() && m_persistSelection) QSettings().setValue(modelSettingsKey(m_provider), m_model);
}

QStringList AssistantClient::defaultModels(Provider p)
{
    switch (p) {
    case Provider::Anthropic: return { "claude-sonnet-5", "claude-opus-5-5", "claude-fable-5-1", "claude-haiku-4-5-20251001" };
    case Provider::OpenAI: return { "gpt-5", "gpt-5-mini", "gpt-4.1", "gpt-4.1-mini", "gpt-4o", "o4-mini" };
    case Provider::Ollama: return { "llama3.2-vision", "llama3.1", "qwen2.5", "mistral" };
    }
    return {};
}

bool AssistantClient::hasApiKey() const
{
    return !needsApiKey() || !apiKeySource().isEmpty();
}

QString AssistantClient::apiKeySource() const
{
    QString source;
    resolvedKey(&source);
    return source;
}

QString AssistantClient::resolvedKey(QString* source) const
{
    // Precedence: a key entered in this session, then one remembered in the preferences,
    // then the environment. An explicit entry must win over a stale shell variable.
    if (!needsApiKey()) {
        if (source) *source = QStringLiteral("not needed");
        return {};
    }
    if (m_sessionKeyProvider == m_provider && !m_sessionKey.isEmpty()) {
        if (source) *source = QStringLiteral("this session");
        return m_sessionKey;
    }
    const QString stored = QSettings().value(keySettingsKey(m_provider)).toString().trimmed();
    if (!stored.isEmpty()) {
        if (source) *source = QStringLiteral("application preferences");
        return stored;
    }
    const char* envName = m_provider == Provider::OpenAI ? "OPENAI_API_KEY" : "ANTHROPIC_API_KEY";
    const QString env = QProcessEnvironment::systemEnvironment().value(QString::fromLatin1(envName)).trimmed();
    if (!env.isEmpty()) {
        if (source) *source = QStringLiteral("environment variable %1").arg(QString::fromLatin1(envName));
        return env;
    }
    if (source) source->clear();
    return {};
}

void AssistantClient::setApiKey(const QString& key, bool remember)
{
    m_sessionKey = key.trimmed();
    m_sessionKeyProvider = m_provider;
    if (remember && !m_sessionKey.isEmpty()) QSettings().setValue(keySettingsKey(m_provider), m_sessionKey);
}

void AssistantClient::setOllamaHost(const QString& host)
{
    m_ollamaHost = host.trimmed().isEmpty() ? QString::fromLatin1(kDefaultOllamaHost) : host.trimmed();
    while (m_ollamaHost.endsWith('/')) m_ollamaHost.chop(1);
    QSettings().setValue("ai.ollamaHost", m_ollamaHost);
}

QUrl AssistantClient::chatEndpoint() const
{
    switch (m_provider) {
    case Provider::Anthropic: return QUrl(QString::fromLatin1(kAnthropicEndpoint));
    case Provider::OpenAI: return QUrl(QString::fromLatin1(kOpenAiEndpoint));
    case Provider::Ollama: return QUrl(m_ollamaHost + "/v1/chat/completions");
    }
    return {};
}

bool AssistantClient::supportsImages() const
{
    const QString m = m_model.toLower();
    switch (m_provider) {
    case Provider::Anthropic: return true;
    case Provider::OpenAI: return !m.startsWith("o1-mini") && !m.startsWith("o3-mini") && !m.startsWith("gpt-3.5");
    case Provider::Ollama:
        for (const char* hint : { "vision", "llava", "vl", "minicpm-v", "gemma3", "moondream", "pixtral", "bakllava" }) {
            if (m.contains(QLatin1String(hint))) return true;
        }
        return false;
    }
    return false;
}

void AssistantClient::fetchModels(std::function<void(const QStringList&, const QString&)> done)
{
    if (m_provider == Provider::Anthropic) {
        done(defaultModels(Provider::Anthropic), QString());
        return;
    }
    QNetworkRequest request(m_provider == Provider::Ollama ? QUrl(m_ollamaHost + "/api/tags") : QUrl(QString::fromLatin1(kOpenAiModelsEndpoint)));
    request.setTransferTimeout(8000);
    if (m_provider == Provider::OpenAI) {
        const QString key = resolvedKey();
        if (key.isEmpty()) { done(defaultModels(Provider::OpenAI), QStringLiteral("No OpenAI key; showing the built-in list.")); return; }
        request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    }
    const Provider provider = m_provider;
    QNetworkReply* reply = m_manager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done, provider] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(defaultModels(provider), provider == Provider::Ollama
                                              ? QStringLiteral("Ollama is not reachable (%1). Start it with `ollama serve`.").arg(reply->errorString())
                                              : QStringLiteral("Could not list OpenAI models: %1").arg(reply->errorString()));
            return;
        }
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        QStringList models;
        if (provider == Provider::Ollama) {
            for (const QJsonValue v : body["models"].toArray()) models << v.toObject()["name"].toString();
        } else {
            for (const QJsonValue v : body["data"].toArray()) {
                const QString id = v.toObject()["id"].toString();
                // Chat-capable families only; skip embeddings, audio, image and moderation models.
                if ((id.startsWith("gpt-") || id.startsWith("o1") || id.startsWith("o3") || id.startsWith("o4") || id.startsWith("chatgpt-"))
                    && !id.contains("audio") && !id.contains("realtime") && !id.contains("transcribe") && !id.contains("tts") && !id.contains("image") && !id.contains("search") && !id.contains("embedding")) {
                    models << id;
                }
            }
            models.sort();
        }
        if (models.isEmpty()) models = defaultModels(provider);
        done(models, QString());
    });
}

// MARK: - Sending

void AssistantClient::trimHistory()
{
    // Older turns lose their screen context and image: they describe screens that have since changed.
    for (Turn& t : m_history) {
        if (t.kind == Turn::Kind::User) { t.context.clear(); t.imagePng.clear(); }
    }
    while (m_history.size() > static_cast<size_t>(kMaxHistoryTurns)) m_history.erase(m_history.begin());
    // Start on a user turn whose tool traffic is complete.
    while (!m_history.empty() && (m_history.front().kind != Turn::Kind::User || m_history.front().text.isEmpty())) m_history.erase(m_history.begin());
}

void AssistantClient::send(const QString& userText, const QString& context, const QImage& image, Callbacks callbacks)
{
    if (m_busy) {
        if (callbacks.onError) callbacks.onError("The assistant is still working on the previous request.");
        return;
    }
    if (!hasApiKey()) {
        if (callbacks.onError) callbacks.onError(QStringLiteral("No %1 API key. Set %2 or use the AI Key button.")
                                                     .arg(providerName(m_provider), m_provider == Provider::OpenAI ? "OPENAI_API_KEY" : "ANTHROPIC_API_KEY"));
        return;
    }
    m_callbacks = std::move(callbacks);
    trimHistory();

    Turn turn;
    turn.kind = Turn::Kind::User;
    turn.text = userText.trimmed().isEmpty() ? QStringLiteral("Analyze what is on screen.") : userText.trimmed();
    turn.context = context;
    if (!image.isNull() && supportsImages()) {
        QImage scaled = image.width() > kMaxImageWidth ? image.scaledToWidth(kMaxImageWidth, Qt::SmoothTransformation) : image;
        QBuffer buffer(&turn.imagePng);
        buffer.open(QIODevice::WriteOnly);
        scaled.save(&buffer, "PNG");
    } else if (!image.isNull() && !turn.context.isEmpty()) {
        turn.context += "\n(The chart image was not attached: this model does not accept images. Use the bars in the context or get_bars.)";
    }
    m_history.push_back(turn);

    m_busy = true;
    m_rounds = 0;
    request();
}

QJsonObject AssistantClient::buildAnthropicBody() const
{
    QJsonArray messages;
    for (const Turn& t : m_history) {
        QJsonArray content;
        switch (t.kind) {
        case Turn::Kind::User:
            if (!t.context.isEmpty()) content.append(QJsonObject{ { "type", "text" }, { "text", QString(kContextMarker) + "\n" + t.context } });
            if (!t.imagePng.isEmpty()) {
                content.append(QJsonObject{ { "type", "image" }, { "source", QJsonObject{ { "type", "base64" }, { "media_type", "image/png" }, { "data", QString::fromLatin1(t.imagePng.toBase64()) } } } });
            }
            content.append(QJsonObject{ { "type", "text" }, { "text", t.text } });
            messages.append(QJsonObject{ { "role", "user" }, { "content", content } });
            break;
        case Turn::Kind::Assistant:
            if (!t.text.isEmpty()) content.append(QJsonObject{ { "type", "text" }, { "text", t.text } });
            for (const ToolCall& c : t.calls) content.append(QJsonObject{ { "type", "tool_use" }, { "id", c.id }, { "name", c.name }, { "input", c.input } });
            if (content.isEmpty()) content.append(QJsonObject{ { "type", "text" }, { "text", "(no content)" } });
            messages.append(QJsonObject{ { "role", "assistant" }, { "content", content } });
            break;
        case Turn::Kind::ToolResults:
            for (const ToolResult& r : t.results) {
                QJsonObject block{ { "type", "tool_result" }, { "tool_use_id", r.id }, { "content", r.content } };
                if (r.isError) block["is_error"] = true;
                content.append(block);
            }
            messages.append(QJsonObject{ { "role", "user" }, { "content", content } });
            break;
        }
    }
    QJsonArray tools;
    for (const Tool& t : m_tools) tools.append(QJsonObject{ { "name", t.name }, { "description", t.description }, { "input_schema", t.inputSchema } });
    QJsonObject body{ { "model", m_model }, { "max_tokens", kMaxTokens }, { "messages", messages } };
    if (!m_system.isEmpty()) body["system"] = m_system;
    if (!tools.isEmpty()) body["tools"] = tools;
    return body;
}

QJsonObject AssistantClient::buildOpenAiBody() const
{
    QJsonArray messages;
    if (!m_system.isEmpty()) messages.append(QJsonObject{ { "role", "system" }, { "content", m_system } });
    for (const Turn& t : m_history) {
        switch (t.kind) {
        case Turn::Kind::User: {
            QString text = t.text;
            if (!t.context.isEmpty()) text = QString(kContextMarker) + "\n" + t.context + "\n\n" + t.text;
            if (t.imagePng.isEmpty()) {
                messages.append(QJsonObject{ { "role", "user" }, { "content", text } });
            } else {
                QJsonArray parts;
                parts.append(QJsonObject{ { "type", "text" }, { "text", text } });
                parts.append(QJsonObject{ { "type", "image_url" }, { "image_url", QJsonObject{ { "url", "data:image/png;base64," + QString::fromLatin1(t.imagePng.toBase64()) } } } });
                messages.append(QJsonObject{ { "role", "user" }, { "content", parts } });
            }
            break;
        }
        case Turn::Kind::Assistant: {
            QJsonObject message{ { "role", "assistant" } };
            message["content"] = t.text.isEmpty() ? QJsonValue() : QJsonValue(t.text);
            if (!t.calls.empty()) {
                QJsonArray calls;
                for (const ToolCall& c : t.calls) {
                    calls.append(QJsonObject{ { "id", c.id }, { "type", "function" },
                                              { "function", QJsonObject{ { "name", c.name }, { "arguments", QString::fromUtf8(QJsonDocument(c.input).toJson(QJsonDocument::Compact)) } } } });
                }
                message["tool_calls"] = calls;
            }
            messages.append(message);
            break;
        }
        case Turn::Kind::ToolResults:
            for (const ToolResult& r : t.results) {
                messages.append(QJsonObject{ { "role", "tool" }, { "tool_call_id", r.id }, { "content", r.isError ? "ERROR: " + r.content : r.content } });
            }
            break;
        }
    }
    QJsonArray tools;
    for (const Tool& t : m_tools) tools.append(QJsonObject{ { "type", "function" }, { "function", QJsonObject{ { "name", t.name }, { "description", t.description }, { "parameters", t.inputSchema } } } });
    QJsonObject body{ { "model", m_model }, { "messages", messages } };
    if (m_provider == Provider::OpenAI) body["max_completion_tokens"] = kMaxTokens;   // newer OpenAI models reject max_tokens
    else body["max_tokens"] = kMaxTokens;
    if (!tools.isEmpty()) body["tools"] = tools;
    return body;
}

void AssistantClient::request()
{
    QNetworkRequest request(chatEndpoint());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(m_provider == Provider::Ollama ? 600000 : 120000);   // local models can be slow
    QJsonObject body;
    if (m_provider == Provider::Anthropic) {
        request.setRawHeader("x-api-key", resolvedKey().toUtf8());
        request.setRawHeader("anthropic-version", kAnthropicVersion);
        body = buildAnthropicBody();
    } else {
        if (m_provider == Provider::OpenAI) {
            request.setRawHeader("Authorization", "Bearer " + resolvedKey().toUtf8());
        }
        body = buildOpenAiBody();
    }
    if (m_callbacks.onStatus) {
        m_callbacks.onStatus(m_rounds == 0 ? QStringLiteral("Thinking (%1)…").arg(m_model)
                                           : QStringLiteral("Thinking (after %1 tool call%2)…").arg(m_rounds).arg(m_rounds == 1 ? "" : "s"));
    }
    const Provider provider = m_provider;
    QNetworkReply* reply = m_manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, reply, [this, reply, provider] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            QString message = body["error"].isObject() ? body["error"].toObject()["message"].toString() : body["error"].toString();
            if (message.isEmpty()) message = reply->errorString();
            if (provider == Provider::Ollama && status == 0) message += " — is Ollama running? (`ollama serve`)";
            fail(QStringLiteral("%1 API error%2: %3").arg(providerName(provider), status ? QStringLiteral(" (HTTP %1)").arg(status) : QString(), message));
            return;
        }
        if (provider == Provider::Anthropic) handleAnthropic(body);
        else handleOpenAi(body);
    });
}

void AssistantClient::handleAnthropic(const QJsonObject& body)
{
    const QJsonObject usage = body["usage"].toObject();
    m_lastInputTokens = usage["input_tokens"].toInt();
    m_lastOutputTokens = usage["output_tokens"].toInt();
    QString text;
    std::vector<ToolCall> calls;
    for (const QJsonValue v : body["content"].toArray()) {
        const QJsonObject block = v.toObject();
        if (block["type"].toString() == "text") text += block["text"].toString();
        else if (block["type"].toString() == "tool_use") calls.push_back({ block["id"].toString(), block["name"].toString(), block["input"].toObject() });
    }
    if (body["stop_reason"].toString() != "tool_use") calls.clear();
    afterAssistantTurn(text, calls);
}

void AssistantClient::handleOpenAi(const QJsonObject& body)
{
    const QJsonObject usage = body["usage"].toObject();
    m_lastInputTokens = usage["prompt_tokens"].toInt();
    m_lastOutputTokens = usage["completion_tokens"].toInt();
    const QJsonArray choices = body["choices"].toArray();
    if (choices.isEmpty()) {
        fail(QStringLiteral("%1 returned no choices.").arg(providerName(m_provider)));
        return;
    }
    const QJsonObject message = choices.first().toObject()["message"].toObject();
    const QString text = stringContent(message["content"]);
    std::vector<ToolCall> calls;
    for (const QJsonValue v : message["tool_calls"].toArray()) {
        const QJsonObject call = v.toObject();
        const QJsonObject fn = call["function"].toObject();
        QJsonObject input;
        const QJsonValue args = fn["arguments"];
        if (args.isString()) input = QJsonDocument::fromJson(args.toString().toUtf8()).object();
        else if (args.isObject()) input = args.toObject();   // Ollama may send an object
        QString id = call["id"].toString();
        if (id.isEmpty()) id = QStringLiteral("call_%1").arg(m_nextCallId++);
        calls.push_back({ id, fn["name"].toString(), input });
    }
    afterAssistantTurn(text, calls);
}

void AssistantClient::afterAssistantTurn(const QString& text, const std::vector<ToolCall>& calls)
{
    Turn turn;
    turn.kind = Turn::Kind::Assistant;
    turn.text = text;
    turn.calls = calls;
    m_history.push_back(turn);
    if (!calls.empty()) {
        if (++m_rounds > kMaxToolRounds) {
            fail("The assistant made too many tool calls in one turn; stopping.");
            return;
        }
        runTools(calls, 0, {});
        return;
    }
    finish(text);
}

void AssistantClient::runTools(const std::vector<ToolCall>& calls, size_t index, std::vector<ToolResult> results)
{
    if (index >= calls.size()) {
        Turn turn;
        turn.kind = Turn::Kind::ToolResults;
        turn.results = std::move(results);
        m_history.push_back(turn);
        request();
        return;
    }
    const ToolCall call = calls[index];
    if (m_callbacks.onToolCall) m_callbacks.onToolCall(call.name, call.input);
    if (m_callbacks.onStatus) m_callbacks.onStatus(QStringLiteral("Running %1…").arg(call.name));

    auto done = [this, calls, index, results, call](const QJsonValue& result, bool isError) mutable {
        QString text = result.isString() ? result.toString()
                                         : QString::fromUtf8(QJsonDocument(result.isObject() ? QJsonDocument(result.toObject()) : QJsonDocument(result.toArray())).toJson(QJsonDocument::Compact));
        if (text.size() > 16000) text = text.left(16000) + "\n…(truncated)";
        results.push_back({ call.id, call.name, text, isError });
        if (m_callbacks.onToolResult) m_callbacks.onToolResult(call.name, previewOf(result), isError);
        runTools(calls, index + 1, results);
    };
    if (!m_executor) {
        done(QStringLiteral("Tool %1 is not available.").arg(call.name), true);
        return;
    }
    m_executor(call.name, call.input, done);
}

QString AssistantClient::previewOf(const QJsonValue& value)
{
    QString text = value.isString() ? value.toString() : QString::fromUtf8(QJsonDocument(value.isObject() ? QJsonDocument(value.toObject()) : QJsonDocument(value.toArray())).toJson(QJsonDocument::Compact));
    text.replace('\n', ' ');
    return text.size() > 160 ? text.left(160) + "…" : text;
}

void AssistantClient::finish(const QString& text)
{
    m_busy = false;
    if (m_callbacks.onStatus) m_callbacks.onStatus(QString());
    if (m_callbacks.onReply) m_callbacks.onReply(text.trimmed().isEmpty() ? QStringLiteral("(no reply)") : text.trimmed());
}

void AssistantClient::fail(const QString& message)
{
    m_busy = false;
    // Drop the incomplete tail (pending tool results, dangling assistant/tool turns and the
    // user turn that started them) so the history is well-formed for the next attempt.
    while (!m_history.empty() && m_history.back().kind != Turn::Kind::User) m_history.pop_back();
    if (!m_history.empty()) m_history.pop_back();
    if (m_callbacks.onStatus) m_callbacks.onStatus(QString());
    if (m_callbacks.onError) m_callbacks.onError(message);
}
