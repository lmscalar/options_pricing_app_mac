//
//  AssistantClient.h
//  OptionPricing
//
//  Chat client with tool use for three providers: Anthropic (Messages API), OpenAI (Chat
//  Completions) and a local Ollama server (OpenAI-compatible endpoint). The conversation is
//  kept in a provider-neutral form and serialised per provider, so switching providers or
//  models keeps the history. Each user turn carries the current screen context (text and,
//  on the Quotes tab, the chart image); the model may call the application's tools, whose
//  results are fed back until it produces a final reply. Pure QtNetwork, no SDKs.
//

#pragma once

#include "QtHeaders.h"

#include <functional>
#include <vector>

class AssistantClient
{
public:
    enum class Provider { Anthropic, OpenAI, Ollama };

    struct Tool {
        QString name;
        QString description;
        QJsonObject inputSchema;     ///< JSON Schema for the tool's input object
    };

    /// Executes a tool call and reports its result (text or JSON) asynchronously.
    using ToolDone = std::function<void(const QJsonValue& result, bool isError)>;
    using ToolExecutor = std::function<void(const QString& name, const QJsonObject& input, ToolDone done)>;

    struct Callbacks {
        std::function<void(const QString& name, const QJsonObject& input)> onToolCall;
        std::function<void(const QString& name, const QString& resultPreview, bool isError)> onToolResult;
        std::function<void(const QString& text)> onReply;
        std::function<void(const QString& message)> onError;
        std::function<void(const QString& status)> onStatus;
    };

    AssistantClient();

    // ---- Provider, model, credentials ----
    static QString providerName(Provider p);
    static QStringList providerNames();
    Provider provider() const { return m_provider; }
    void setProvider(Provider p);
    QString model() const { return m_model; }
    void setModel(const QString& model);
    /// When false, provider/model changes are not written to the preferences (tests).
    void setPersistSelection(bool persist) { m_persistSelection = persist; }
    /// Built-in model suggestions for a provider (Ollama's come from the server; see fetchModels).
    static QStringList defaultModels(Provider p);
    /// Asks the provider for its installed/available models (Ollama: /api/tags; OpenAI: /v1/models).
    void fetchModels(std::function<void(const QStringList& models, const QString& error)> done);

    /// Key for the current provider (Ollama needs none). Environment variables
    /// ANTHROPIC_API_KEY / OPENAI_API_KEY win over stored keys.
    bool hasApiKey() const;
    bool needsApiKey() const { return m_provider != Provider::Ollama; }
    QString apiKeySource() const;
    void setApiKey(const QString& key, bool remember);
    QString ollamaHost() const { return m_ollamaHost; }
    void setOllamaHost(const QString& host);

    void setSystemPrompt(const QString& prompt) { m_system = prompt; }
    void setTools(const std::vector<Tool>& tools) { m_tools = tools; }
    void setToolExecutor(ToolExecutor executor) { m_executor = std::move(executor); }

    /// Sends a user turn. `context` is attached as a separate text block (older turns'
    /// context blocks are dropped to bound the prompt); `image` (may be null) is attached as
    /// a PNG scaled to at most 1600 px wide, when the model is believed to accept images.
    void send(const QString& userText, const QString& context, const QImage& image, Callbacks callbacks);
    bool busy() const { return m_busy; }
    void reset() { m_history.clear(); }
    int turnCount() const { return static_cast<int>(m_history.size()); }

    int lastInputTokens() const { return m_lastInputTokens; }
    int lastOutputTokens() const { return m_lastOutputTokens; }
    /// True when the current provider/model is expected to accept image input.
    bool supportsImages() const;

private:
    struct ToolCall { QString id; QString name; QJsonObject input; };
    struct ToolResult { QString id; QString name; QString content; bool isError = false; };
    struct Turn {
        enum class Kind { User, Assistant, ToolResults } kind = Kind::User;
        QString text;                 ///< user text or assistant text
        QString context;              ///< user turn: screen context (dropped from older turns)
        QByteArray imagePng;          ///< user turn: chart image
        std::vector<ToolCall> calls;  ///< assistant turn
        std::vector<ToolResult> results;
    };

    void request();
    QJsonObject buildAnthropicBody() const;
    QJsonObject buildOpenAiBody() const;
    void handleAnthropic(const QJsonObject& body);
    void handleOpenAi(const QJsonObject& body);
    void afterAssistantTurn(const QString& text, const std::vector<ToolCall>& calls);
    void runTools(const std::vector<ToolCall>& calls, size_t index, std::vector<ToolResult> results);
    void finish(const QString& text);
    void fail(const QString& message);
    static QString previewOf(const QJsonValue& value);
    static QString keySettingsKey(Provider p);
    static QString modelSettingsKey(Provider p);
    QUrl chatEndpoint() const;
    void trimHistory();
    /// The key in effect for the current provider and where it came from (see apiKeySource).
    QString resolvedKey(QString* source = nullptr) const;

    QNetworkAccessManager m_manager;
    Provider m_provider = Provider::Anthropic;
    QString m_model;
    QString m_ollamaHost;
    QString m_sessionKey;             ///< key set for this session only (per provider, see m_sessionKeyProvider)
    Provider m_sessionKeyProvider = Provider::Anthropic;
    QString m_system;
    std::vector<Tool> m_tools;
    ToolExecutor m_executor;
    std::vector<Turn> m_history;
    Callbacks m_callbacks;
    bool m_busy = false;
    int m_rounds = 0;
    int m_lastInputTokens = 0;
    int m_lastOutputTokens = 0;
    int m_nextCallId = 1;
    bool m_persistSelection = true;
};
