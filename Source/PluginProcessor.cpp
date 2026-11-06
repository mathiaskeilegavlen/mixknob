#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr int kMaxDryDelay = 1 << 18;   // ~5.9 s at 44.1 kHz
}

juce::AudioProcessorValueTreeState::ParameterLayout MixWrapperProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "in", 1 }, "Input",
                    NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                    AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix",
                    NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                    AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "out", 1 }, "Output",
                    NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                    AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "latcomp", 1 }, "Latency Comp", true));
    return layout;
}

MixWrapperProcessor::MixWrapperProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout()),
      scanner (formatManager)
{
    juce::addDefaultFormatsToManager (formatManager);
    inParam      = apvts.getRawParameterValue ("in");
    mixParam     = apvts.getRawParameterValue ("mix");
    outParam     = apvts.getRawParameterValue ("out");
    latencyParam = apvts.getRawParameterValue ("latcomp");
}

MixWrapperProcessor::~MixWrapperProcessor()
{
    cancelPendingUpdate();
    if (hosted != nullptr)
        hosted->releaseResources();
}

bool MixWrapperProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == layouts.getMainInputChannelSet();
}

double MixWrapperProcessor::getTailLengthSeconds() const
{
    return hosted != nullptr ? hosted->getTailLengthSeconds() : 0.0;
}

//==============================================================================
void MixWrapperProcessor::configureHosted (juce::AudioPluginInstance& p)
{
    const int n = juce::jmax (1, getMainBusNumInputChannels());
    const auto set = juce::AudioChannelSet::canonicalChannelSet (n);

    // No sidechains / aux buses
    for (int i = 1; i < p.getBusCount (true); ++i)
        if (auto* b = p.getBus (true, i)) b->enable (false);
    for (int i = 1; i < p.getBusCount (false); ++i)
        if (auto* b = p.getBus (false, i)) b->enable (false);

    auto layout = p.getBusesLayout();
    if (p.getBusCount (true) > 0)  layout.getChannelSet (true, 0)  = set;
    if (p.getBusCount (false) > 0) layout.getChannelSet (false, 0) = set;
    p.setBusesLayout (layout);   // best effort; mono-only plugins keep their own layout

    const double sr = currentSR > 0.0 ? currentSR : 44100.0;
    const int bs = juce::jmax (currentBlock, 512);

    p.releaseResources();
    p.setRateAndBufferSizeDetails (sr, bs);
    p.prepareToPlay (sr, bs);
}

void MixWrapperProcessor::resizeWetBuffer()   // call with hostLock held
{
    int ch = juce::jmax (1, getTotalNumInputChannels());
    if (hosted != nullptr)
        ch = juce::jmax (ch, hosted->getTotalNumInputChannels(), hosted->getTotalNumOutputChannels());

    wetBuffer.setSize (ch, juce::jmax (currentBlock, 512));
    wetBuffer.clear();
}

void MixWrapperProcessor::prepareToPlay (double sr, int block)
{
    currentSR = sr;
    currentBlock = block;

    const int ch = juce::jmax (1, getTotalNumInputChannels());

    {
        const juce::ScopedLock sl (hostLock);
        dryBuffer.setSize (ch, juce::jmax (block, 512));
        dryBuffer.clear();
        dryDelayBuf.setSize (ch, kMaxDryDelay + 1);
        dryDelayBuf.clear();
        delayPos = 0;

        if (hosted != nullptr)
            configureHosted (*hosted);

        resizeWetBuffer();
        hostedLatency = hosted != nullptr ? hosted->getLatencySamples() : 0;
    }

    inSm.reset (sr, 0.02);  inSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inParam->load()));
    outSm.reset (sr, 0.02); outSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outParam->load()));
    mixSm.reset (sr, 0.02); mixSm.setCurrentAndTargetValue (mixParam->load() * 0.01f);

    setLatencySamples (hostedLatency.load());
}

//==============================================================================
void MixWrapperProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int n  = buffer.getNumSamples();
    const int ch = juce::jmin (2, buffer.getNumChannels());

    inSm.setTargetValue  (juce::Decibels::decibelsToGain (inParam->load()));
    outSm.setTargetValue (juce::Decibels::decibelsToGain (outParam->load()));
    mixSm.setTargetValue (mixParam->load() * 0.01f);
    const bool compensate = latencyParam->load() > 0.5f;

    // ---- input gain
    for (int i = 0; i < n; ++i)
    {
        const float g = inSm.getNextValue();
        for (int c = 0; c < ch; ++c)
            buffer.getWritePointer (c)[i] *= g;
    }

    // ---- is the hosted plugin available and are our buffers big enough?
    const juce::ScopedTryLock lock (hostLock);
    auto* p = hosted.get();

    bool ok = lock.isLocked() && p != nullptr
              && dryBuffer.getNumChannels() >= ch && dryBuffer.getNumSamples() >= n
              && dryDelayBuf.getNumChannels() >= ch;

    int hostedOut = 1, wetCh = ch;
    if (ok)
    {
        hostedOut = juce::jmax (1, p->getTotalNumOutputChannels());
        wetCh = juce::jmax (ch, p->getTotalNumInputChannels(), hostedOut);
        ok = wetBuffer.getNumChannels() >= wetCh && wetBuffer.getNumSamples() >= n;
    }

    if (! ok)
    {
        // No plugin (or momentarily unavailable): plain pass-through with output gain
        mixSm.skip (n);
        for (int i = 0; i < n; ++i)
        {
            const float g = outSm.getNextValue();
            for (int c = 0; c < ch; ++c)
                buffer.getWritePointer (c)[i] *= g;
        }
        return;
    }

    // ---- latency tracking
    const int latency = p->getLatencySamples();
    if (latency != hostedLatency.load())
    {
        hostedLatency = latency;
        triggerAsyncUpdate();   // tells Logic about the new latency
    }

    // ---- copy dry
    for (int c = 0; c < ch; ++c)
        dryBuffer.copyFrom (c, 0, buffer, c, 0, n);

    // ---- run the hosted plugin on the wet copy
    for (int c = 0; c < wetCh; ++c)
    {
        if (c < ch) wetBuffer.copyFrom (c, 0, buffer, c, 0, n);
        else        wetBuffer.clear (c, 0, n);
    }

    p->setPlayHead (getPlayHead());
    {
        juce::AudioBuffer<float> wetView (wetBuffer.getArrayOfWritePointers(), wetCh, n);
        p->processBlock (wetView, midi);
    }

    // ---- delay the dry signal by the plugin's latency so both paths line up
    {
        const int delay = compensate ? juce::jlimit (0, kMaxDryDelay, latency) : 0;
        const int size = dryDelayBuf.getNumSamples();

        for (int c = 0; c < ch; ++c)
        {
            auto* d = dryBuffer.getWritePointer (c);
            auto* r = dryDelayBuf.getWritePointer (c);
            int pos = delayPos;

            for (int i = 0; i < n; ++i)
            {
                r[pos] = d[i];
                int rp = pos - delay;
                if (rp < 0) rp += size;
                d[i] = r[rp];
                if (++pos == size) pos = 0;
            }
        }
        delayPos = (delayPos + n) % size;
    }

    // ---- mix + output gain
    const float* dry[2] = { dryBuffer.getReadPointer (0), ch > 1 ? dryBuffer.getReadPointer (1) : nullptr };
    float* out[2]       = { buffer.getWritePointer (0),   ch > 1 ? buffer.getWritePointer (1)   : nullptr };
    const float* wet[2] = { wetBuffer.getReadPointer (0),
                            ch > 1 ? wetBuffer.getReadPointer (juce::jmin (1, hostedOut - 1)) : nullptr };

    for (int i = 0; i < n; ++i)
    {
        const float m = mixSm.getNextValue();
        const float g = outSm.getNextValue();

        for (int c = 0; c < ch; ++c)
            out[c][i] = (dry[c][i] * (1.0f - m) + wet[c][i] * m) * g;
    }
}

//==============================================================================
void MixWrapperProcessor::loadPlugin (const juce::PluginDescription& desc, const juce::MemoryBlock& state)
{
    juce::WeakReference<MixWrapperProcessor> weak (this);

    formatManager.createPluginInstanceAsync (desc,
        currentSR > 0.0 ? currentSR : 44100.0, juce::jmax (currentBlock, 512),
        [weak, desc, state] (std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& error)
        {
            if (weak == nullptr) return;

            if (inst == nullptr)
            {
                weak->lastError = error;
                weak->sendChangeMessage();
                return;
            }
            weak->installPlugin (std::move (inst), desc, state);
        });
}

void MixWrapperProcessor::installPlugin (std::unique_ptr<juce::AudioPluginInstance> inst,
                                         const juce::PluginDescription& desc,
                                         const juce::MemoryBlock& state)
{
    // Restore the plugin's own state first: it may change its latency
    if (state.getSize() > 0)
        inst->setStateInformation (state.getData(), (int) state.getSize());

    configureHosted (*inst);

    // Tell the editor to close any hosted-plugin window BEFORE the old plugin is destroyed
    sendSynchronousChangeMessage();

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::ScopedLock sl (hostLock);
        old = std::move (hosted);
        hosted = std::move (inst);
        hostedDesc = desc;
        resizeWetBuffer();
        hostedLatency = hosted->getLatencySamples();
    }

    if (old != nullptr)
        old->releaseResources();
    old.reset();

    lastError = {};
    triggerAsyncUpdate();
    sendChangeMessage();
}

void MixWrapperProcessor::unloadPlugin()
{
    sendSynchronousChangeMessage();

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        const juce::ScopedLock sl (hostLock);
        old = std::move (hosted);
        hostedLatency = 0;
        resizeWetBuffer();
    }

    if (old != nullptr)
        old->releaseResources();
    old.reset();

    triggerAsyncUpdate();
    sendChangeMessage();
}

//==============================================================================
void MixWrapperProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto xml = std::make_unique<juce::XmlElement> ("MixWrapperState");

    if (auto params = apvts.copyState().createXml())
        xml->addChildElement (params.release());

    if (hosted != nullptr)
    {
        auto* h = xml->createNewChildElement ("Hosted");
        if (auto d = hostedDesc.createXml())
            h->addChildElement (d.release());

        juce::MemoryBlock mb;
        hosted->getStateInformation (mb);
        h->setAttribute ("state", mb.toBase64Encoding());
    }

    copyXmlToBinary (*xml, dest);
}

void MixWrapperProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName ("MixWrapperState"))
        return;

    if (auto* params = xml->getChildByName ("PARAMS"))
        apvts.replaceState (juce::ValueTree::fromXml (*params));

    if (auto* h = xml->getChildByName ("Hosted"))
    {
        juce::PluginDescription desc;
        if (auto* d = h->getFirstChildElement(); d != nullptr && desc.loadFromXml (*d))
        {
            juce::MemoryBlock mb;
            mb.fromBase64Encoding (h->getStringAttribute ("state"));
            loadPlugin (desc, mb);
        }
    }
    else if (hosted != nullptr)
    {
        unloadPlugin();
    }
}

juce::AudioProcessorEditor* MixWrapperProcessor::createEditor()
{
    return new MixWrapperEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MixWrapperProcessor();
}
