#pragma once
#include <JuceHeader.h>
#include "PluginScanner.h"

class MixWrapperProcessor : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster,
                            private juce::AsyncUpdater
{
public:
    MixWrapperProcessor();
    ~MixWrapperProcessor() override;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Mix Wrapper"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Hosting (message thread only)
    void loadPlugin (const juce::PluginDescription&, const juce::MemoryBlock& state = {});
    void unloadPlugin();
    juce::AudioPluginInstance* getHosted() const { return hosted.get(); }
    bool hasHostedPlugin() const                 { return hosted != nullptr; }
    juce::String getHostedName() const           { return hosted != nullptr ? hostedDesc.name : juce::String(); }
    int getHostedLatency() const                 { return hostedLatency.load(); }

    juce::AudioProcessorValueTreeState apvts;
    juce::AudioPluginFormatManager formatManager;
    PluginScanner scanner;
    juce::String lastError;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void installPlugin (std::unique_ptr<juce::AudioPluginInstance>,
                        const juce::PluginDescription&, const juce::MemoryBlock& state);
    void configureHosted (juce::AudioPluginInstance&);
    void resizeWetBuffer();
    void handleAsyncUpdate() override { setLatencySamples (hostedLatency.load()); }

    // Audio-thread access to 'hosted' is guarded by this lock (try-lock in processBlock).
    juce::CriticalSection hostLock;
    std::unique_ptr<juce::AudioPluginInstance> hosted;
    juce::PluginDescription hostedDesc;

    juce::AudioBuffer<float> dryBuffer, wetBuffer, dryDelayBuf;
    int delayPos = 0;

    juce::SmoothedValue<float> inSm, outSm, mixSm;
    std::atomic<float>* inParam = nullptr;
    std::atomic<float>* outParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* latencyParam = nullptr;

    std::atomic<int> hostedLatency { 0 };
    double currentSR = 0.0;
    int currentBlock = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (MixWrapperProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixWrapperProcessor)
};
