#include "PluginEditor.h"

namespace
{
    void setupKnob (juce::Component& parent, juce::Slider& s, juce::Label& l, const juce::String& text)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 20);
        parent.addAndMakeVisible (s);

        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        parent.addAndMakeVisible (l);
    }
}

MixWrapperEditor::MixWrapperEditor (MixWrapperProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setupKnob (*this, inSlider,  inLabel,  "Input (dB)");
    setupKnob (*this, mixSlider, mixLabel, "MIX (%)");
    setupKnob (*this, outSlider, outLabel, "Output (dB)");
    mixLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));

    inAtt  = std::make_unique<SliderAtt> (proc.apvts, "in",  inSlider);
    mixAtt = std::make_unique<SliderAtt> (proc.apvts, "mix", mixSlider);
    outAtt = std::make_unique<SliderAtt> (proc.apvts, "out", outSlider);

    addAndMakeVisible (latencyButton);
    latencyAtt = std::make_unique<ButtonAtt> (proc.apvts, "latcomp", latencyButton);

    latencyInfo.setJustificationType (juce::Justification::centredRight);
    latencyInfo.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (latencyInfo);

    pluginButton.onClick = [this] { showMenu(); };
    editButton.onClick   = [this] { toggleWindow(); };
    addAndMakeVisible (pluginButton);
    addAndMakeVisible (editButton);

    proc.addChangeListener (this);
    proc.scanner.addChangeListener (this);

    if (proc.scanner.list.getNumTypes() == 0)
        proc.scanner.start();

    setSize (380, 300);
    refresh();
    startTimerHz (4);
}

MixWrapperEditor::~MixWrapperEditor()
{
    stopTimer();
    window.reset();   // must go before anything else: it references the hosted plugin
    proc.scanner.removeChangeListener (this);
    proc.removeChangeListener (this);
}

void MixWrapperEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e1f24));
}

void MixWrapperEditor::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto top = area.removeFromTop (32);
    editButton.setBounds (top.removeFromRight (80));
    top.removeFromRight (8);
    pluginButton.setBounds (top);

    area.removeFromTop (12);

    auto bottom = area.removeFromBottom (28);
    latencyButton.setBounds (bottom.removeFromLeft (220));
    latencyInfo.setBounds (bottom);
    area.removeFromBottom (8);

    const int col = area.getWidth() / 4;
    auto left  = area.removeFromLeft (col);
    auto right = area.removeFromRight (col);

    auto place = [] (juce::Rectangle<int> r, juce::Label& l, juce::Slider& s)
    {
        l.setBounds (r.removeFromTop (22));
        s.setBounds (r);
    };

    place (left,  inLabel,  inSlider);
    place (area,  mixLabel, mixSlider);
    place (right, outLabel, outSlider);
}

void MixWrapperEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    // The hosted plugin is being swapped or removed: its window must not outlive it
    if (source == &proc)
        window.reset();

    refresh();
}

void MixWrapperEditor::refresh()
{
    const bool loaded = proc.hasHostedPlugin();

    if (proc.scanner.isScanning())
        pluginButton.setButtonText ("Scanning plugins... " + juce::String ((int) (proc.scanner.getProgress() * 100.0f)) + "%");
    else if (proc.lastError.isNotEmpty())
        pluginButton.setButtonText ("Failed to load: " + proc.lastError);
    else
        pluginButton.setButtonText (loaded ? proc.getHostedName() : "Click to load a plugin...");

    editButton.setEnabled (loaded);
    refreshLatencyLabel();
}

void MixWrapperEditor::refreshLatencyLabel()
{
    latencyInfo.setText (proc.hasHostedPlugin() ? "Plugin latency: " + juce::String (proc.getHostedLatency()) + " smp"
                                                : juce::String(),
                         juce::dontSendNotification);
}

void MixWrapperEditor::showMenu()
{
    if (proc.scanner.isScanning())
        return;

    juce::Array<juce::PluginDescription> types;
    for (auto& d : proc.scanner.list.getTypes())
        if (! d.isInstrument && d.name != "Mix Wrapper")
            types.add (d);

    juce::PopupMenu menu;

    if (proc.hasHostedPlugin())
    {
        menu.addItem (2, "Unload current plugin");
        menu.addSeparator();
    }

    juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);
    menu.addSeparator();
    menu.addItem (1, "Rescan plugins...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (pluginButton),
        [safe = juce::Component::SafePointer<MixWrapperEditor> (this), types] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            if (result == 1) { safe->proc.scanner.start(); return; }
            if (result == 2) { safe->proc.unloadPlugin();  return; }

            const int index = juce::KnownPluginList::getIndexChosenByMenu (types, result);
            if (index >= 0)
                safe->proc.loadPlugin (types[index]);
        });
}

void MixWrapperEditor::toggleWindow()
{
    if (window != nullptr)
    {
        window.reset();
        return;
    }

    if (auto* p = proc.getHosted())
    {
        juce::AudioProcessorEditor* ed = p->hasEditor() ? p->createEditorIfNeeded()
                                                        : new juce::GenericAudioProcessorEditor (*p);
        if (ed == nullptr)
            return;

        window = std::make_unique<PluginWindow> (ed, p->getName(),
            [safe = juce::Component::SafePointer<MixWrapperEditor> (this)]
            {
                juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->window.reset(); });
            });
    }
}
