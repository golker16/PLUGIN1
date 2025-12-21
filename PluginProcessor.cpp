#include "PluginProcessor.h"
#include <cstring> // std::memset

//------------------------------------------------------------------------------
// Si CMake no define PLUGIN_HAS_ASSETS por alguna razón, no rompas el build:
#ifndef PLUGIN_HAS_ASSETS
 #define PLUGIN_HAS_ASSETS 0
#endif

#if PLUGIN_HAS_ASSETS
 #include "BinaryData.h"
#endif

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
//------------------------------------------------------------------------------
// Per-knob LookAndFeel: usa colours del Slider para (vacío/lleno)
struct KnobLNF : public juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional,
                           float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height)
                        .reduced (6.0f);

        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre  = bounds.getCentre();

        const float angle = rotaryStartAngle
                          + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        const auto emptyCol = slider.findColour (juce::Slider::rotarySliderOutlineColourId);
        const auto fillCol  = slider.findColour (juce::Slider::rotarySliderFillColourId);

        // ✅ más minimal (sin manecilla) y un poco más fino
        const float thickness = juce::jmax (2.0f, radius * 0.12f);

        juce::Path bgArc;
        bgArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);

        g.setColour (emptyCol);
        g.strokePath (bgArc, juce::PathStrokeType (thickness,
                                                   juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                                rotaryStartAngle, angle, true);

        g.setColour (fillCol);
        g.strokePath (valueArc, juce::PathStrokeType (thickness,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

        // ✅ manecilla eliminada -> solo vacío + relleno
    }
};

//------------------------------------------------------------------------------
// Animated header from sprite sheet (assets/header_sheet.png)
// - 45 frames (horizontal strip)
// - 3 fps (very light for plugins)
class AnimatedHeader final : public juce::Component,
                             private juce::Timer
{
public:
    void setSpriteSheet (juce::Image sheetImage, int framesInRow, int fps)
    {
        sheet = sheetImage;
        numFrames = juce::jmax (1, framesInRow);
        frameIndex = 0;

        if (! sheet.isValid() || sheet.getWidth() <= 0 || sheet.getHeight() <= 0)
        {
            stopTimer();
            return;
        }

        frameW = juce::jmax (1, sheet.getWidth() / numFrames);
        frameH = juce::jmax (1, sheet.getHeight());

        startTimerHz (juce::jmax (1, fps));
        repaint();
    }

    void stop() { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        if (! sheet.isValid() || numFrames <= 0 || frameW <= 0 || frameH <= 0)
            return;

        const int sx = frameIndex * frameW;
        const int sy = 0;

        // ✅ Fit centrado manteniendo aspect ratio (sin estirar)
        auto b = getLocalBounds().toFloat();

        const float ar = (float) frameW / (float) frameH;

        float dw = b.getWidth();
        float dh = dw / ar;

        if (dh > b.getHeight())
        {
            dh = b.getHeight();
            dw = dh * ar;
        }

        const float dx = b.getX() + (b.getWidth()  - dw) * 0.5f;
        const float dy = b.getY() + (b.getHeight() - dh) * 0.5f;

        g.drawImage (sheet,
                     dx, dy, dw, dh,
                     (float) sx, (float) sy, (float) frameW, (float) frameH);
    }

private:
    void timerCallback() override
    {
        if (! sheet.isValid())
            return;

        frameIndex = (frameIndex + 1) % numFrames;
        repaint();
    }

    juce::Image sheet;
    int numFrames  = 1;
    int frameIndex = 0;
    int frameW = 0;
    int frameH = 0;
};

//------------------------------------------------------------------------------
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
        // LookAndFeel per knob (colores por slider)
        driveKnob.slider.setLookAndFeel (&knobLNF);
        toneKnob .slider.setLookAndFeel (&knobLNF);
        mixKnob  .slider.setLookAndFeel (&knobLNF);

        // ✅ Colores (AARRGGBB)
        // DRIVE: vacío 006700, lleno 65ff65
        driveKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FF006700"));
        driveKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FF65FF65"));

        // TONE: vacío f9ff34, lleno cc66ff
        toneKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FFF9FF34"));
        toneKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FFCC66FF"));

        // MIX: vacío 555555, lleno ffffff
        mixKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FF555555"));
        mixKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FFFFFFFF"));

        // Labels más pequeños (por si compilas sin PNGs)
        driveKnob.label.setFont (juce::Font (11.0f));
        toneKnob .label.setFont (juce::Font (11.0f));
        mixKnob  .label.setFont (juce::Font (11.0f));

        addAndMakeVisible (driveKnob);
        addAndMakeVisible (toneKnob);
        addAndMakeVisible (mixKnob);

        addAndMakeVisible (header);

       #if PLUGIN_HAS_ASSETS
        // header_sheet.png -> sprite sheet animado (45 frames, 3 fps)
        {
            int dataSize = 0;
            if (auto* data = BinaryData::getNamedResource ("header_sheet_png", dataSize))
            {
                auto sheet = juce::ImageCache::getFromMemory (data, dataSize);
                header.setSpriteSheet (sheet, 45, 3);
            }
        }
       #endif

        // ✅ PNG encima de knobs (desde /assets -> BinaryData) SOLO si existe
       #if PLUGIN_HAS_ASSETS
        driveKnob.setLabelImage (juce::ImageCache::getFromMemory (BinaryData::drive_png, BinaryData::drive_pngSize));
        toneKnob .setLabelImage (juce::ImageCache::getFromMemory (BinaryData::tone_png,  BinaryData::tone_pngSize));
        mixKnob  .setLabelImage (juce::ImageCache::getFromMemory (BinaryData::mix_png,   BinaryData::mix_pngSize));
       #endif

        // ComboBox presets (mantiene el control)
        preampBox.setJustificationType (juce::Justification::centredLeft);

        int itemId = 1;
        for (const auto& it : PresetRegistry::items)
            preampBox.addItem (it.displayName, itemId++);

        if (preampBox.getNumItems() == 0)
            preampBox.addItem ("(none)", 1);

        addAndMakeVisible (preampBox);

       #if PLUGIN_HAS_ASSETS
        modelImg = juce::ImageCache::getFromMemory (BinaryData::model_png, BinaryData::model_pngSize);
        modelImage.setImage (modelImg);

        // ✅ mantener AR + centrar + solo reducir (no agrandar / no deformar)
        modelImage.setImagePlacement (juce::RectanglePlacement::centred
                                    | juce::RectanglePlacement::onlyReduceInSize);

        addAndMakeVisible (modelImage);
       #endif

        using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

        driveAtt  = std::make_unique<SliderAttachment>   (processor.apvts, "drive",  driveKnob.slider);
        toneAtt   = std::make_unique<SliderAttachment>   (processor.apvts, "tone",   toneKnob.slider);
        mixAtt    = std::make_unique<SliderAttachment>   (processor.apvts, "mix",    mixKnob.slider);
        preampAtt = std::make_unique<ComboBoxAttachment> (processor.apvts, "preamp", preampBox);

        // ✅ UI más grande
        setSize (820, 460);
    }

    ~MinimalEditor() override
    {
        header.stop();
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
        auto area = getLocalBounds().reduced (44);

        // --- Header animado arriba (banner centrado) ---
        auto headerArea = area.removeFromTop (96).reduced (6, 6);
        header.setBounds (headerArea);

        // Aire entre header y knobs
        area.removeFromTop (16);

        // Barra inferior (selector Preamp)
        auto bottom = area.removeFromBottom (74);

        // --- Layout knobs (pequeños, pegados y a la izquierda) ---
        const int knobW = 90;     // componente LabeledKnob
        const int knobH = 104;
        const int gap   = 8;      // arrimados

        const int startX = area.getX();   // pegado a la izquierda
        const int y      = area.getY();   // arriba del bloque de knobs

        driveKnob.setBounds (startX,                         y, knobW, knobH);
        toneKnob .setBounds (startX + knobW + gap,           y, knobW, knobH);
        mixKnob  .setBounds (startX + (knobW + gap) * 2,     y, knobW, knobH);

        // --- Bottom row: [model.png] [ComboBox] ---
        bottom.reduce (0, 10);

        auto left = bottom.removeFromLeft (90); // ✅ más pequeño
       #if PLUGIN_HAS_ASSETS
        if (modelImg.isValid())
            modelImage.setBounds (left.reduced (6, 10));
       #endif

        preampBox.setBounds (bottom.reduced (0, 12));
    }

private:
    YourPluginAudioProcessor& processor;

    KnobLNF knobLNF;

    AnimatedHeader header;

    plugin::ui::LabeledKnob driveKnob;
    plugin::ui::LabeledKnob toneKnob;
    plugin::ui::LabeledKnob mixKnob;

    juce::ComboBox preampBox;

   #if PLUGIN_HAS_ASSETS
    juce::ImageComponent modelImage;
    juce::Image modelImg;
   #endif

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

    // ✅ AutoGain EXACTO por bloque (dry vs mixed)
    autoGain.prepare (sr);

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

                stereoInteract.processSample (xL, xR);

                L[n] = activePreset->process (stL, xL);
                R[n] = activePreset->process (stR, xR);
            }
        }
        else
        {
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
    // 3) Mix (primera pasada) + cálculo de energía por bloque
    double dryPow   = 0.0;
    double mixedPow = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix01 = mixSm.getNextValue();

        const float dryL = ch0[i];
        const float dryR = (ch1 != nullptr) ? ch1[i] : dryL;

        const float wetOutL = wetL[i];
        const float wetOutR = (wetR != nullptr) ? wetR[i] : wetOutL;

        const float mixedL = plugin::equalPowerMix (dryL, wetOutL, mix01);
        const float mixedR = plugin::equalPowerMix (dryR, wetOutR, mix01);

        ch0[i] = mixedL;
        if (ch1 != nullptr) ch1[i] = mixedR;

        const double dP = 0.5 * (double(dryL)   * double(dryL)   + double(dryR)   * double(dryR));
        const double mP = 0.5 * (double(mixedL) * double(mixedL) + double(mixedR) * double(mixedR));

        dryPow   += dP;
        mixedPow += mP;
    }

    dryPow   /= (double) juce::jmax (1, numSamples);
    mixedPow /= (double) juce::jmax (1, numSamples);

    const float g = autoGain.updateFromBlockPowers (dryPow, mixedPow);

    // Segunda pasada: aplicar gain + softclip
    for (int i = 0; i < numSamples; ++i)
    {
        ch0[i] = plugin::softClipSafety (ch0[i] * g);
        if (ch1 != nullptr) ch1[i] = plugin::softClipSafety (ch1[i] * g);
    }
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YourPluginAudioProcessor();
}
