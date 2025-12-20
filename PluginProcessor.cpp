#include "PluginProcessor.h"
#include <cstring> // std::memset

//==============================================================================
// Parameters
static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        "drive", "Drive",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.25f));

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        "tone", "Tone",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.5f));

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        "mix", "Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        1.0f));

    juce::StringArray preampChoices;
    for (const auto& it : PresetRegistry::items)
        preampChoices.add (it.displayName);

    if (preampChoices.isEmpty())
        preampChoices.add ("(none)");

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        "preamp", "Preamp:",
        preampChoices,
        0));

    return { params.begin(), params.end() };
}

namespace
{
class MinimalEditor final : public juce::AudioProcessorEditor
{
public:
    explicit MinimalEditor (YourPluginAudioProcessor& proc)
        : juce::AudioProcessorEditor (&proc)
        , processor (proc)
        , driveKnob ("Drive")
        , toneKnob  ("Tone")
        , mixKnob   ("Mix")
    {
        lnf.trackColour = juce::Colour::fromRGB (45, 45, 45);
        lnf.valueColour = juce::Colour::fromRGB (90, 255, 130);

        driveKnob.slider.setLookAndFeel (&lnf);
        toneKnob .slider.setLookAndFeel (&lnf);
        mixKnob  .slider.setLookAndFeel (&lnf);

        addAndMakeVisible (driveKnob);
        addAndMakeVisible (toneKnob);
        addAndMakeVisible (mixKnob);

        preampLabel.setText ("Preamp:", juce::dontSendNotification);
        preampLabel.setJustificationType (juce::Justification::centredLeft);
        preampLabel.setInterceptsMouseClicks (false, false);

        preampBox.setJustificationType (juce::Justification::centredLeft);

        int itemId = 1;
        for (const auto& it : PresetRegistry::items)
            preampBox.addItem (it.displayName, itemId++);

        if (preampBox.getNumItems() == 0)
            preampBox.addItem ("(none)", 1);

        addAndMakeVisible (preampLabel);
        addAndMakeVisible (preampBox);

        using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

        driveAtt  = std::make_unique<SliderAttachment>   (processor.apvts, "drive",  driveKnob.slider);
        toneAtt   = std::make_unique<SliderAttachment>   (processor.apvts, "tone",   toneKnob.slider);
        mixAtt    = std::make_unique<SliderAttachment>   (processor.apvts, "mix",    mixKnob.slider);
        preampAtt = std::make_unique<ComboBoxAttachment> (processor.apvts, "preamp", preampBox);

        setSize (420, 210);
    }

    ~MinimalEditor() override
    {
        driveKnob.slider.setLookAndFeel (nullptr);
        toneKnob .slider.setLookAndFeel (nullptr);
        mixKnob  .slider.setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);

        auto topRow = r.removeFromTop (160);
        const int w = topRow.getWidth() / 3;

        driveKnob.setBounds (topRow.removeFromLeft (w).reduced (8));
        toneKnob .setBounds (topRow.removeFromLeft (w).reduced (8));
        mixKnob  .setBounds (topRow.removeFromLeft (w).reduced (8));

        r.removeFromTop (6);

        auto bottom = r.removeFromTop (32);
        preampLabel.setBounds (bottom.removeFromLeft (70));
        preampBox  .setBounds (bottom.reduced (0, 2));
    }

private:
    YourPluginAudioProcessor& processor;

    plugin::ui::SimpleKnobLookAndFeel lnf;

    plugin::ui::LabeledKnob driveKnob;
    plugin::ui::LabeledKnob toneKnob;
    plugin::ui::LabeledKnob mixKnob;

    juce::Label preampLabel;
    juce::ComboBox preampBox;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> driveAtt, toneAtt, mixAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> preampAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MinimalEditor)
};
} // namespace

//==============================================================================
// Constructor
YourPluginAudioProcessor::YourPluginAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
                            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                            .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    , apvts (*this, nullptr, "PARAMS", makeLayout())
{
    pDrive  = apvts.getRawParameterValue ("drive");
    pTone   = apvts.getRawParameterValue ("tone");
    pMix    = apvts.getRawParameterValue ("mix");
    pPreamp = apvts.getRawParameterValue ("preamp");
}

//==============================================================================
// Required JUCE overrides
const juce::String YourPluginAudioProcessor::getName() const { return JucePlugin_Name; }

bool YourPluginAudioProcessor::acceptsMidi() const { return false; }
bool YourPluginAudioProcessor::producesMidi() const { return false; }
bool YourPluginAudioProcessor::isMidiEffect() const { return false; }

double YourPluginAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int YourPluginAudioProcessor::getNumPrograms() { return 1; }
int YourPluginAudioProcessor::getCurrentProgram() { return 0; }
void YourPluginAudioProcessor::setCurrentProgram (int) {}
const juce::String YourPluginAudioProcessor::getProgramName (int) { return {}; }
void YourPluginAudioProcessor::changeProgramName (int, const juce::String&) {}

bool YourPluginAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* YourPluginAudioProcessor::createEditor()
{
    return new MinimalEditor (*this);
}

//==============================================================================
// Layout
bool YourPluginAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (in != out)
        return false;

    return (in == juce::AudioChannelSet::mono()
         || in == juce::AudioChannelSet::stereo());
}

//==============================================================================
// State save/load
void YourPluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void YourPluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
// Tilt
void YourPluginAudioProcessor::updateTiltCoeffs (float tone01)
{
    const float t = juce::jlimit (0.0f, 1.0f, tone01);
    const float tiltDb = juce::jmap (t, 0.0f, 1.0f, -6.0f, 6.0f);

    const float fcLow  = 180.0f;
    const float fcHigh = 3800.0f;

    auto low  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, fcLow,  0.707f,
                                                                    juce::Decibels::decibelsToGain (-tiltDb));
    auto high = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, fcHigh, 0.707f,
                                                                    juce::Decibels::decibelsToGain ( tiltDb));

    *lowShelfL.coefficients  = *low;
    *lowShelfR.coefficients  = *low;
    *highShelfL.coefficients = *high;
    *highShelfR.coefficients = *high;
}

//==============================================================================
// Prepare
void YourPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

    // Smoothing 20ms
    driveSm.reset (sr, 0.02);
    toneSm .reset (sr, 0.02);
    mixSm  .reset (sr, 0.02);

    driveSm.setCurrentAndTargetValue (*pDrive);
    toneSm .setCurrentAndTargetValue (*pTone);
    mixSm  .setCurrentAndTargetValue (*pMix);

    // Inicializa coef pointers (evita null)
    lowShelfL.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 180.0f, 0.707f, 1.0f);
    lowShelfR.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 180.0f, 0.707f, 1.0f);
    highShelfL.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3800.0f, 0.707f, 1.0f);
    highShelfR.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3800.0f, 0.707f, 1.0f);

    updateTiltCoeffs (*pTone);

    // Level matcher (base)
    levelMatch.prepare (sr);
    levelMatch.setGateDb (-60.0f);
    levelMatch.setClampDb (-12.0f, 12.0f);
    levelMatch.setMeasurementWindowMs (140.0f);
    levelMatch.setGainSmoothingMs (10.0f, 140.0f);

    // Oversampling (solo para WET)
    const auto channels = (size_t) juce::jmax (1, juce::jmin (2, getTotalNumInputChannels()));
    oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
        channels,
        kOversamplingExponent,
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
        true /* max quality */);

    oversampling->reset();
    oversampling->initProcessing ((size_t) samplesPerBlock);
    setLatencySamples ((int) oversampling->getLatencyInSamples());

    // buffers
    wetBuffer.setSize ((int) juce::jmax ((size_t)1, juce::jmin ((size_t)2, channels)),
                       samplesPerBlock, false, false, true);

    // sample rate interno del preset (oversampled) - NO hardcode
    const float osFactor = (float) (1u << kOversamplingExponent);
    osSr = (float) (sr * osFactor);

    // ✅ A) Preparar módulo de interacción estéreo PRO al SR oversampled
    stereoInteract.prepare (osSr);

    // Limpia storage de estados
    for (auto& st : presetState)
        std::memset (&st, 0, sizeof(st));

    // ---- PRESET inicial (blindado) ----
    activePresetIndex = -1;
    activePreset = nullptr;

    int preampIndex = 0;
    if (pPreamp != nullptr && PresetRegistry::items.size() > 0)
        preampIndex = juce::jlimit (0, (int) PresetRegistry::items.size() - 1,
                                    (int) std::lround (*pPreamp));

    if (PresetRegistry::items.size() > 0)
    {
        activePresetIndex = preampIndex;
        activePreset = &PresetRegistry::items[(size_t) preampIndex];

        for (int ch = 0; ch < 2; ++ch)
        {
            void* st = (void*) &presetState[(size_t) ch];
            if (activePreset->prepare) activePreset->prepare (st, osSr);
            if (activePreset->reset)   activePreset->reset (st);
        }
    }
}

//==============================================================================
// Process
void YourPluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    for (int ch = 2; ch < numCh; ++ch)
        buffer.clear (ch, 0, numSamples);

    auto* ch0 = buffer.getWritePointer (0);
    auto* ch1 = (numCh > 1) ? buffer.getWritePointer (1) : nullptr;

    driveSm.setTargetValue (*pDrive);
    toneSm .setTargetValue (*pTone);
    mixSm  .setTargetValue (*pMix);

    // Selección de preset
    int preampIndex = 0;
    if (pPreamp != nullptr && PresetRegistry::items.size() > 0)
        preampIndex = juce::jlimit (0, (int) PresetRegistry::items.size() - 1,
                                    (int) std::lround (*pPreamp));

    if ((activePreset == nullptr || preampIndex != activePresetIndex) && PresetRegistry::items.size() > 0)
    {
        activePresetIndex = preampIndex;
        activePreset = &PresetRegistry::items[(size_t) preampIndex];

        const float osFactor = (float) (1u << kOversamplingExponent);
        osSr = (float) (sr * osFactor);

        // ✅ Mantén stereoInteract alineado si cambia SR/OS (o si rearmas oversampling)
        stereoInteract.prepare (osSr);

        for (int ch = 0; ch < 2; ++ch)
        {
            void* st = (void*) &presetState[(size_t) ch];
            if (activePreset->prepare) activePreset->prepare (st, osSr);
            if (activePreset->reset)   activePreset->reset (st);
        }
    }

    // Tone smoothing real (por bloque) antes de usar tilt
    toneSm.setTargetValue (*pTone);
    toneSm.skip (numSamples);
    updateTiltCoeffs (toneSm.getCurrentValue());

    // Asegura wetBuffer sin realocar cada bloque
    const int wetCh = juce::jmax (1, juce::jmin (2, numCh));
    if (wetBuffer.getNumChannels() != wetCh || wetBuffer.getNumSamples() < numSamples)
        wetBuffer.setSize (wetCh, numSamples, false, false, true);

    auto* wetL = wetBuffer.getWritePointer (0);
    auto* wetR = (wetCh > 1) ? wetBuffer.getWritePointer (1) : nullptr;

    // -------------------------------------------------------------------------
    // 1) WET base SR: pregain + tilt
    for (int i = 0; i < numSamples; ++i)
    {
        const float drive01 = driveSm.getNextValue();
        const float pregain = juce::Decibels::decibelsToGain (plugin::mapDriveDb (drive01));

        float xL = ch0[i] * pregain;
        xL = lowShelfL.processSample (xL);
        xL = highShelfL.processSample (xL);
        wetL[i] = xL;

        if (wetR != nullptr && ch1 != nullptr)
        {
            float xR = ch1[i] * pregain;
            xR = lowShelfR.processSample (xR);
            xR = highShelfR.processSample (xR);
            wetR[i] = xR;
        }
    }

    // -------------------------------------------------------------------------
    // 2) Oversampling -> preset PRO (stateful) -> downsample
    if (oversampling != nullptr && activePreset != nullptr && activePreset->process != nullptr)
    {
        juce::dsp::AudioBlock<float> baseBlock (wetBuffer);

        // procesamos SOLO numSamples (aunque wetBuffer sea más grande)
        baseBlock = baseBlock.getSubBlock (0, (size_t) numSamples);

        auto osBlock = oversampling->processSamplesUp (baseBlock);

        const size_t osSamples = osBlock.getNumSamples();
        const size_t osCh = osBlock.getNumChannels();

        // ✅ B) Loop por muestra con interacción estéreo PRO (si osCh == 2)
        if (osCh == 2)
        {
            float* L = osBlock.getChannelPointer (0);
            float* R = osBlock.getChannelPointer (1);

            void* stL = (void*) &presetState[0];
            void* stR = (void*) &presetState[1];

            for (size_t n = 0; n < osSamples; ++n)
            {
                float xL = L[n];
                float xR = R[n];

                // interacción estéreo PRO: freq + nivel + fase (micro allpass)
                stereoInteract.processSample (xL, xR);

                // procesa preset por canal con estados separados
                L[n] = activePreset->process (stL, xL);
                R[n] = activePreset->process (stR, xR);
            }
        }
        else
        {
            // path mono (o canales != 2) como estaba
            for (size_t ch = 0; ch < osCh; ++ch)
            {
                float* data = osBlock.getChannelPointer (ch);
                void* st = (void*) &presetState[juce::jmin ((size_t) 1, ch)];

                for (size_t n = 0; n < osSamples; ++n)
                    data[n] = activePreset->process (st, data[n]);
            }
        }

        oversampling->processSamplesDown (baseBlock);
    }

    // -------------------------------------------------------------------------
    // 3) Mix + level match + safety clip
    for (int i = 0; i < numSamples; ++i)
    {
        const float mix01 = mixSm.getNextValue();

        const float dryL = ch0[i];
        const float dryR = (ch1 != nullptr) ? ch1[i] : dryL;

        const float wetOutL = wetL[i];
        const float wetOutR = (wetR != nullptr) ? wetR[i] : wetOutL;

        const float mixedL = plugin::equalPowerMix (dryL, wetOutL, mix01);
        const float mixedR = plugin::equalPowerMix (dryR, wetOutR, mix01);

        // ✅ C) matcher estéreo correcto (ya lo tienes)
        const float g = levelMatch.processStereo (dryL, dryR, mixedL, mixedR);

        ch0[i] = plugin::softClipSafety (mixedL * g);
        if (ch1 != nullptr) ch1[i] = plugin::softClipSafety (mixedR * g);
    }
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YourPluginAudioProcessor();
}
