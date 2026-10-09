//
//  AssistantPanel.h
//  OptionPricing
//
//  The AI assistant dock shown beside every tab: a transcript, a text box, a microphone
//  button for dictation, "Analyze this screen", and spoken replies. The panel owns the
//  API client and the speech recogniser; MainWindow supplies the screen context, the
//  tools the model may call, and a local parser for simple voice commands.
//

#pragma once

#include "QtHeaders.h"
#include "AssistantClient.h"
#include "SpeechDictation.h"
#include "Theme.h"
#include "Widgets.h"

#include <QtCore/QRegularExpression>
#include <QtGui/QKeyEvent>
#include <QtGui/QTextDocument>
#include <QtWidgets/QTextBrowser>

#include <functional>
#include <vector>

class AssistantPanel : public QWidget
{
public:
    explicit AssistantPanel(QWidget* parent = nullptr);

    AssistantClient& client() { return m_client; }

    /// Supplies the current screen context (text) and an optional image for the model.
    std::function<void(std::function<void(const QString& context, const QImage& image)> done)> contextProvider;
    /// Handles simple navigation commands locally ("pull up option chains for AAPL").
    /// Returns true when the text was handled; `feedback` is shown in the transcript.
    std::function<bool(const QString& text, QString& feedback)> localCommandHandler;

    /// Sends text to the assistant (after trying the local command handler).
    void submit(const QString& text);
    /// Asks the model to analyse the current screen.
    void analyzeScreen();
    /// Starts or stops dictation.
    void toggleListening();
    bool promptForApiKey();
    void focusInput();
    void appendNote(const QString& text);
    void applyTheme(const Theme& theme);
    bool busy() const { return m_client.busy(); }
    /// Fired when a request starts or finishes (MainWindow mirrors it in the header).
    std::function<void(bool busy)> onBusyChanged;
    /// Test hook: fired with every assistant reply (or error message).
    std::function<void(const QString& reply, bool ok)> onReply;
    /// Test hooks: type into the input box and send with the Enter key or the Send button;
    /// read back the last transcript entry.
    void debugTypeAndSend(const QString& text, bool useButton);
    /// Test hook: shows or hides the thinking indicators as if a request were in flight.
    void debugSetBusy(bool busy) { setBusy(busy); }
    /// Test hook: renders a sample analyst note (headings, tables, lists, callout).
    void debugRenderSample();
    void debugScrollTranscriptToTop() { m_transcript->verticalScrollBar()->setValue(0); }
    QString debugLastTranscriptLine() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildUi();
    void wire();
    void appendMessage(const QString& role, const QString& text);
    void appendTool(const QString& text, bool isError);
    void rebuildTranscript();
    /// Markdown subset to rich text: headings, pipe tables (numeric cells right-aligned,
    /// signed changes coloured), nested bullet/numbered lists, rules, bold/italic/code,
    /// and a highlighted "Bottom line" callout.
    QString renderMarkdown(const QString& text) const;
    QString renderTable(const QStringList& rows) const;
    QString inlineMarkdown(const QString& raw) const;
    void setStatus(const QString& text, bool error = false);
    void startListening();
    void stopListening(bool sendTranscript);
    void refreshKeyStatus();
    void populateModels(const QStringList& models);
    void refreshModelList();
    static QString escape(const QString& text);

    struct Entry {
        enum class Kind { User, Assistant, Tool, Note } kind = Kind::Note;
        QString text;
        QString meta;      ///< model and time for assistant notes, time for user turns
        bool error = false;
    };
    std::vector<Entry> m_entries;

    AssistantClient m_client;
    SpeechDictation m_speech;
    Theme m_theme;
    QString m_transcriptHtml;
    QString m_partialTranscript;
    QTimer* m_silenceTimer = nullptr;
    QTimer* m_relayoutTimer = nullptr;
    bool m_listening = false;
    bool m_sendAfterStop = false;
    bool m_lastInputWasVoice = false;
    bool m_updatingModels = false;

    QComboBox* m_provider = nullptr;
    QComboBox* m_model = nullptr;
    QToolButton* m_refreshModels = nullptr;
    QPushButton* m_setKey = nullptr;
    QLabel* m_keyStatus = nullptr;
    QCheckBox* m_speak = nullptr;
    QPushButton* m_clear = nullptr;
    QTextBrowser* m_transcript = nullptr;
    QPlainTextEdit* m_input = nullptr;
    QToolButton* m_mic = nullptr;
    QPushButton* m_analyze = nullptr;
    QPushButton* m_send = nullptr;
    QLabel* m_status = nullptr;
    ui::SpinningDiamond* m_thinking = nullptr;
    void setBusy(bool busy);
};
