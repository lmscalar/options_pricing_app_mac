//
//  SpeechDictation.mm
//  OptionPricing
//

#include "SpeechDictation.h"

#include <QtCore/QRegularExpression>

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Speech/Speech.h>

struct SpeechDictation::Impl {
    SFSpeechRecognizer* recognizer = nil;
    AVAudioEngine* engine = nil;
    SFSpeechAudioBufferRecognitionRequest* request = nil;
    SFSpeechRecognitionTask* task = nil;
    bool listening = false;
    std::function<void(const QString&, bool)> onResult;
    std::function<void(const QString&)> onError;
};

namespace {
AVSpeechSynthesizer* synthesizer()
{
    static AVSpeechSynthesizer* synth = [[AVSpeechSynthesizer alloc] init];
    return synth;
}

QString fromNS(NSString* s) { return s ? QString::fromNSString(s) : QString(); }
} // namespace

SpeechDictation::SpeechDictation()
    : m_impl(std::make_unique<Impl>())
{
    m_impl->recognizer = [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale currentLocale]];
    if (!m_impl->recognizer) m_impl->recognizer = [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale localeWithLocaleIdentifier:@"en_US"]];
    m_impl->engine = [[AVAudioEngine alloc] init];
}

SpeechDictation::~SpeechDictation()
{
    stop();
}

bool SpeechDictation::isAvailable() const
{
    return m_impl->recognizer != nil && m_impl->recognizer.available;
}

QString SpeechDictation::unavailableReason() const
{
    if (!m_impl->recognizer) return QStringLiteral("Speech recognition is not supported for the current locale.");
    if (!m_impl->recognizer.available) return QStringLiteral("Speech recognition is temporarily unavailable (check Siri & Dictation in System Settings).");
    return {};
}

void SpeechDictation::requestAuthorization(std::function<void(bool, const QString&)> done)
{
    [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus status) {
        dispatch_async(dispatch_get_main_queue(), ^{
            switch (status) {
            case SFSpeechRecognizerAuthorizationStatusAuthorized: {
                // Microphone permission is requested separately by the audio session on first capture.
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        done(granted, granted ? QString() : QStringLiteral("Microphone access was denied. Allow it in System Settings > Privacy & Security > Microphone."));
                    });
                }];
                break;
            }
            case SFSpeechRecognizerAuthorizationStatusDenied:
                done(false, QStringLiteral("Speech recognition was denied. Allow it in System Settings > Privacy & Security > Speech Recognition."));
                break;
            case SFSpeechRecognizerAuthorizationStatusRestricted:
                done(false, QStringLiteral("Speech recognition is restricted on this Mac."));
                break;
            default:
                done(false, QStringLiteral("Speech recognition permission has not been granted yet."));
                break;
            }
        });
    }];
}

bool SpeechDictation::start(std::function<void(const QString&, bool)> onResult, std::function<void(const QString&)> onError)
{
    if (m_impl->listening) return true;
    if (!isAvailable()) {
        if (onError) onError(unavailableReason());
        return false;
    }
    m_impl->onResult = std::move(onResult);
    m_impl->onError = std::move(onError);

    SFSpeechAudioBufferRecognitionRequest* request = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
    request.shouldReportPartialResults = YES;
    if (@available(macOS 10.15, *)) {
        if (m_impl->recognizer.supportsOnDeviceRecognition) request.requiresOnDeviceRecognition = NO;   // prefer accuracy; falls back to on-device automatically offline
    }
    if (@available(macOS 13.0, *)) {
        request.addsPunctuation = YES;
    }
    m_impl->request = request;

    AVAudioInputNode* input = m_impl->engine.inputNode;
    AVAudioFormat* format = [input outputFormatForBus:0];
    if (format.sampleRate == 0 || format.channelCount == 0) {
        if (m_impl->onError) m_impl->onError(QStringLiteral("No microphone input is available."));
        m_impl->request = nil;
        return false;
    }
    [input removeTapOnBus:0];
    [input installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer* buffer, AVAudioTime*) {
        [request appendAudioPCMBuffer:buffer];
    }];
    [m_impl->engine prepare];
    NSError* error = nil;
    if (![m_impl->engine startAndReturnError:&error]) {
        [input removeTapOnBus:0];
        m_impl->request = nil;
        if (m_impl->onError) m_impl->onError(QStringLiteral("Could not start audio capture: %1").arg(fromNS(error.localizedDescription)));
        return false;
    }

    Impl* impl = m_impl.get();
    m_impl->task = [m_impl->recognizer recognitionTaskWithRequest:request resultHandler:^(SFSpeechRecognitionResult* result, NSError* taskError) {
        NSString* text = result ? result.bestTranscription.formattedString : nil;
        const bool final = result ? result.isFinal : false;
        NSString* message = taskError ? taskError.localizedDescription : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (impl->request != request) return;   // a newer session replaced this one
            if (text && impl->onResult) impl->onResult(fromNS(text), final);
            if (message && impl->listening && impl->onError) {
                // Cancellation after stop() is expected; anything else is reported.
                if (![message containsString:@"canceled"] && ![message containsString:@"Cancel"]) impl->onError(fromNS(message));
            }
            if (final || message) {
                impl->listening = false;
            }
        });
    }];
    m_impl->listening = true;
    return true;
}

void SpeechDictation::stop()
{
    if (!m_impl->request && !m_impl->listening) return;
    if (m_impl->engine.isRunning) {
        [m_impl->engine stop];
        [m_impl->engine.inputNode removeTapOnBus:0];
    }
    [m_impl->request endAudio];     // lets the recogniser deliver its final result
    m_impl->listening = false;
}

bool SpeechDictation::listening() const
{
    return m_impl->listening;
}

void SpeechDictation::speak(const QString& text)
{
    if (text.trimmed().isEmpty()) return;
    AVSpeechSynthesizer* synth = synthesizer();
    if (synth.isSpeaking) [synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    // Strip light markdown so bullets and emphasis markers are not read out.
    QString plain = text;
    plain.remove(QRegularExpression("[*`#_]+"));
    AVSpeechUtterance* utterance = [AVSpeechUtterance speechUtteranceWithString:plain.toNSString()];
    utterance.voice = [AVSpeechSynthesisVoice voiceWithLanguage:[[NSLocale currentLocale] localeIdentifier]] ?: [AVSpeechSynthesisVoice voiceWithLanguage:@"en-US"];
    [synth speakUtterance:utterance];
}

void SpeechDictation::stopSpeaking()
{
    AVSpeechSynthesizer* synth = synthesizer();
    if (synth.isSpeaking) [synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
}

bool SpeechDictation::isSpeaking()
{
    return synthesizer().isSpeaking;
}
