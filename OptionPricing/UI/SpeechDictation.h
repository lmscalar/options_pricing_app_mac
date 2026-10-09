//
//  SpeechDictation.h
//  OptionPricing
//
//  Voice input and output through Apple's Speech and AppKit frameworks: live dictation
//  from the microphone (SFSpeechRecognizer, on-device when available) and spoken replies
//  (NSSpeechSynthesizer). Callbacks are delivered on the main thread.
//

#pragma once

#include "QtHeaders.h"

#include <functional>
#include <memory>

class SpeechDictation
{
public:
    SpeechDictation();
    ~SpeechDictation();
    SpeechDictation(const SpeechDictation&) = delete;
    SpeechDictation& operator=(const SpeechDictation&) = delete;

    /// True when a recogniser exists for the current locale.
    bool isAvailable() const;
    QString unavailableReason() const;

    /// Asks for speech-recognition and microphone permission (the system prompts once).
    void requestAuthorization(std::function<void(bool granted, const QString& message)> done);

    /// Starts listening. `onResult` receives the running transcript; `isFinal` is true for
    /// the last result after stop(). Returns false (with an error) if capture cannot start.
    bool start(std::function<void(const QString& text, bool isFinal)> onResult, std::function<void(const QString& error)> onError);
    void stop();
    bool listening() const;

    /// Speaks text aloud (interrupting any current speech).
    static void speak(const QString& text);
    static void stopSpeaking();
    static bool isSpeaking();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
