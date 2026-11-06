#pragma once
#include "PluginProcessor.h"

// Floating window that holds the hosted plugin's own GUI
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessorEditor* editor, const juce::String& title, std::function<void()> onClose)
        : DocumentWindow (title, juce::Colours::darkgrey, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { if (closeCallback) closeCallback(); }

private:
    std::function<void()> closeCallback;
};

//==============================================================================
class MixWrapperEditor : public juce::AudioProcessorEditor,
                         private juce::ChangeListener,
                         private juce::Timer
{
public:
    explicit MixWrapperEditor (MixWrapperProcessor&);
    ~MixWrapperEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override { refreshLatencyLabel(); }

    void refresh();
    void refreshLatencyLabel();
    void showMenu();
    void toggleWindow();

    MixWrapperProcessor& proc;

    juce::TextButton pluginButton, editButton { "Open UI" };
    juce::Slider inSlider, mixSlider, outSlider;
    juce::Label inLabel, mixLabel, outLabel, latencyInfo;
    juce::ToggleButton latencyButton { "Latency compensation" };

    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAtt> inAtt, mixAtt, outAtt;
    std::unique_ptr<ButtonAtt> latencyAtt;

    std::unique_ptr<PluginWindow> window;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixWrapperEditor)
};
