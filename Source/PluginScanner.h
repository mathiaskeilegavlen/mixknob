#pragma once
#include <JuceHeader.h>

// Scans installed AU / VST3 plugins one file at a time on the message thread
// (timer driven, so the UI stays responsive) and caches the result on disk.
class PluginScanner : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    explicit PluginScanner (juce::AudioPluginFormatManager& fm) : formats (fm)
    {
        cacheFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                        .getChildFile ("MixWrapper").getChildFile ("plugins.xml");

        if (auto xml = juce::parseXML (cacheFile))
            list.recreateFromXml (*xml);
    }

    ~PluginScanner() override { stopTimer(); }

    juce::KnownPluginList list;   // declared before 'scanner' so it outlives it

    bool isScanning() const { return isTimerRunning(); }

    float getProgress() const
    {
        const int n = formats.getNumFormats();
        if (n == 0) return 0.0f;
        const float p = scanner != nullptr ? (float) scanner->getProgress() : 0.0f;
        return juce::jlimit (0.0f, 1.0f, ((float) formatIndex + p) / (float) n);
    }

    void start()
    {
        if (isScanning()) return;
        formatIndex = -1;
        if (nextFormat())
            startTimer (2);
    }

private:
    bool nextFormat()
    {
        scanner.reset();

        if (++formatIndex < formats.getNumFormats())
        {
            auto* f = formats.getFormat (formatIndex);
            scanner = std::make_unique<juce::PluginDirectoryScanner> (
                list, *f, f->getDefaultLocationsToSearch(), true,
                juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("MixWrapperScan.txt"),
                true);
            return true;
        }
        return false;
    }

    void finish()
    {
        stopTimer();
        scanner.reset();
        cacheFile.getParentDirectory().createDirectory();
        if (auto xml = list.createXml())
            xml->writeTo (cacheFile);
        sendChangeMessage();
    }

    void timerCallback() override
    {
        if (scanner == nullptr) { finish(); return; }

        juce::String name;
        if (! scanner->scanNextFile (true, name))
            if (! nextFormat())
                finish();

        sendChangeMessage();
    }

    juce::AudioPluginFormatManager& formats;
    juce::File cacheFile;
    int formatIndex = -1;
    std::unique_ptr<juce::PluginDirectoryScanner> scanner;
};
