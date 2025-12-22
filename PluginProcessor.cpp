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

    // Oversampling quality (solo bloque no lineal)
    // Eco = 2x, HQ = 4x, Insane = 8x
    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        "quality", "Quality",
        juce::StringArray { "Eco", "HQ", "Insane" },
        1));

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
        // ✅ 1.5: ruedita a la mitad pero conservar grosor actual
        auto outer = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height)
                       .reduced (6.0f);

        // radio base para mantener grosor como antes (basado en área grande)
        const float baseRadius = juce::jmin (outer.getWidth(), outer.getHeight()) * 0.5f;

        // ruedita a la mitad (centrada)
        auto bounds = outer.withSizeKeepingCentre (outer.getWidth() * 0.5f,
                                                   outer.getHeight() * 0.5f);

        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto  centre = bounds.getCentre();

        const float angle = rotaryStartAngle
                          + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        const auto emptyCol = slider.findColour (juce::Slider::rotarySliderOutlineColourId);
        const auto fillCol  = slider.findColour (juce::Slider::rotarySliderFillColourId);

        // grosor conservado (usa baseRadius, no radius chico)
        const float thickness = juce::jmax (2.0f, baseRadius * 0.12f);

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

        // ✅ 1.1: "contain" + ONLY REDUCE (no agrandar) + centrado
        auto b = getLocalBounds().toFloat();

        const float ar = (float) frameW / (float) frameH;

        // tamaño nativo del frame (en pixels)
        const float nativeW = (float) frameW;
        const float nativeH = (float) frameH;

        // 1) contain dentro del componente
        float dw = b.getWidth();
        float dh = dw / ar;

        if (dh > b.getHeight())
        {
            dh = b.getHeight();
            dw = dh * ar;
        }

        // 2) onlyReduceInSize: si el componente es más grande, NO escales hacia arriba
        if (dw > nativeW || dh > nativeH)
        {
            dw = nativeW;
            dh = nativeH;
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

        // ✅ 1.4: tamaños diferentes del slot de PNG por knob
        driveKnob.setLabelSlotHeights (12, 14); // más chico aún
        mixKnob  .setLabelSlotHeights (12, 14); // más chico aún
        toneKnob .setLabelSlotHeights (20, 14); // un poco más grande
       #endif

        // ComboBox presets (mantiene el control)
        preampBox.setJustificationType (juce::Justification::centredLeft);

        // Oversampling quality
        qualityBox.setJustificationType (juce::Justification::centredLeft);
        qualityBox.addItem ("Eco (2x)",    1);
        qualityBox.addItem ("HQ (4x)",     2);
        qualityBox.addItem ("Insane (8x)", 3);

        int itemId = 1;
        for (const auto& it : PresetRegistry::items)
            preampBox.addItem (it.displayName, itemId++);

        if (preampBox.getNumItems() == 0)
            preampBox.addItem ("(none)", 1);

        addAndMakeVisible (preampBox);
        addAndMakeVisible (qualityBox);

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
        qualityAtt = std::make_unique<ComboBoxAttachment> (processor.apvts, "quality", qualityBox);

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

        // ✅ 1.2: Header con menos altura
        auto headerArea = area.removeFromTop (72).reduced (6, 6);
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

        // ✅ 1.3: model.png mucho más chico
        auto left = bottom.removeFromLeft (44);
       #if PLUGIN_HAS_ASSETS
        if (modelImg.isValid())
            modelImage.setBounds (left.reduced (2, 18));
       #endif

        auto boxes = bottom.reduced (0, 12);
        auto right = boxes.removeFromRight (juce::jmax (160, boxes.getWidth() / 3));
        qualityBox.setBounds (right);
        preampBox.setBounds (boxes.reduced (0, 0));
    }

private:
    YourPluginAudioProcessor& processor;

    KnobLNF knobLNF;

    AnimatedHeader header;

    plugin::ui::LabeledKnob driveKnob;
    plugin::ui::LabeledKnob toneKnob;
    plugin::ui::LabeledKnob mixKnob;

    juce::ComboBox preampBox;
    juce::ComboBox qualityBox;

   #if PLUGIN_HAS_ASSETS
    juce::ImageComponent modelImage;
    juce::Image modelImg;
   #endif

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> driveAtt, toneAtt, mixAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> preampAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> qualityAtt;

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
    pQuality = apvts.getRawParameterValue ("quality");
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
// Tone (Tilt EQ) + interacción Drive↔Tone (pre/post)
void YourPluginAudioProcessor::updateToneCoeffs (float tone01, float drive01)
{
    const float t = juce::jlimit (0.0f, 1.0f, tone01);
    const float d = juce::jlimit (0.0f, 1.0f, drive01);

    const float tiltDb = plugin::mapToneTiltDb (t);
    const float mag01  = juce::jlimit (0.0f, 1.0f, std::abs (tiltDb) / 12.0f);

    // A poco drive: Tone actúa más PRE (EQ)
    // A mucho drive: Tone actúa más POST (carácter / tame harshness)
    const float postBlend = std::pow (d, 1.50f);
    const float preBlend  = 1.0f - postBlend;

    const float preTiltDb  = tiltDb * (0.90f * preBlend + 0.10f);
    const float postTiltDb = tiltDb * (0.85f * postBlend);

    // Q dinámico (en centro casi plano, hacia extremos más "decisión")
    const float q = 0.707f + 0.38f * mag01;

    // -------------------------
    // PRE tilt (pivot musical ~900Hz)
    float fcLowPre  = 420.0f;
    float fcHighPre = 2100.0f;

    if (preTiltDb < 0.0f) // dark
    {
        fcHighPre *= (1.0f - 0.30f * mag01);
        fcLowPre  *= (1.0f + 0.18f * mag01);
    }
    else if (preTiltDb > 0.0f) // bright
    {
        fcHighPre *= (1.0f + 0.40f * mag01);
        fcLowPre  *= (1.0f - 0.12f * mag01);
    }

    fcLowPre  = juce::jlimit (140.0f,  900.0f, fcLowPre);
    fcHighPre = juce::jlimit (900.0f,  6500.0f, fcHighPre);

    auto lowPre  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (
        sr, fcLowPre, q, juce::Decibels::decibelsToGain (-preTiltDb));
    auto highPre = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
        sr, fcHighPre, q, juce::Decibels::decibelsToGain ( preTiltDb));

    *lowShelfL.coefficients  = *lowPre;
    *lowShelfR.coefficients  = *lowPre;
    *highShelfL.coefficients = *highPre;
    *highShelfR.coefficients = *highPre;

    // -------------------------
    // POST tilt: más enfocado en "fritura"/brillo de la distorsión
    float fcLowPost  = 280.0f;
    float fcHighPost = 3800.0f;

    if (postTiltDb < 0.0f) // dark
        fcHighPost *= (1.0f - 0.35f * mag01);
    else if (postTiltDb > 0.0f) // bright
        fcHighPost *= (1.0f + 0.55f * mag01);

    fcLowPost  = juce::jlimit (120.0f,  900.0f, fcLowPost);
    fcHighPost = juce::jlimit (1400.0f, 9000.0f, fcHighPost);

    const float qPost = 0.707f + 0.25f * mag01;
    auto lowPost  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (
        sr, fcLowPost, qPost, juce::Decibels::decibelsToGain (-postTiltDb));
    auto highPost = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
        sr, fcHighPost, qPost, juce::Decibels::decibelsToGain ( postTiltDb));

    *lowShelfPostL.coefficients  = *lowPost;
    *lowShelfPostR.coefficients  = *lowPost;
    *highShelfPostL.coefficients = *highPost;
    *highShelfPostR.coefficients = *highPost;

    // Drive ↔ Tone: softening highs (anti-fizz) a más drive
    const float softPos = std::pow (d, 1.20f);
    const float hfHz = 18000.0f - 9000.0f * softPos; // 18k -> 9k
    driveSat.setHighSoftHz (hfHz);
}

//==============================================================================
// Oversampling (Eco/HQ/Insane) - SOLO bloque no lineal
void YourPluginAudioProcessor::rebuildOversampling (int newExponent)
{
    const auto channels = (size_t) juce::jmax (1, juce::jmin (2, getTotalNumInputChannels()));

    osExponent = juce::jlimit (1, 3, newExponent);

    // Elegimos filtros según modo (más CPU/menos aliasing a mayor calidad).
    // En JUCE "clásico" (muchos proyectos), Oversampling solo expone:
    //  - filterHalfBandPolyphaseIIR
    //  - filterHalfBandFIREquiripple
    // (PolyphaseFIR/"FIR" no están en todas las versiones).
    //
    // Mapeo robusto (compatible):
    //   Eco    (2x) -> IIR (ligero)
    //   HQ     (4x) -> Equiripple (calidad)
    //   Insane (8x) -> Equiripple + maxQuality (más taps/stopband)
    juce::dsp::Oversampling<float>::FilterType type = juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR;
    bool maxQuality = false;
    if (osExponent >= 2)
        type = juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple;
    if (osExponent >= 3)
        maxQuality = true;

    oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
        channels,
        osExponent,
        type,
        maxQuality);

    oversampling->reset();
    oversampling->initProcessing ((size_t) juce::jmax (1, maxBlockSizePrepared));

    osExponentPrepared = osExponent;

    const int latency = (int) oversampling->getLatencyInSamples();
    setLatencySamples (latency);

    // Dry delay (alineación para MIX)
    dryDelaySamples    = latency;
    dryDelayWritePos   = 0;
    dryDelayBufferSize = juce::jmax (1, maxBlockSizePrepared + dryDelaySamples + 1);

    dryDelayBuffer.setSize ((int) channels, dryDelayBufferSize, false, false, true);
    dryDelayBuffer.clear();

    // wet buffer (base SR)
    wetBuffer.setSize ((int) channels, juce::jmax (1, maxBlockSizePrepared), false, false, true);

    // SR oversampled
    const float osFactor = (float) (1u << osExponent);
    osSr = (float) (sr * osFactor);

    // Módulos oversampled
    stereoInteract.prepare (osSr);
    driveSat.prepare (osSr);

    // Si hay preset activo, re-prepare al nuevo SR interno
    if (activePreset != nullptr)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            void* st = (void*) &presetState[(size_t) ch];
            if (presetStateConstructed[(size_t) ch])
            {
                if (activePreset->prepare) activePreset->prepare (st, osSr);
                if (activePreset->reset)   activePreset->reset (st);
            }
        }
    }
}

//==============================================================================
// Prepare
void YourPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

    // ✅ Anti-zipper PRO (smoothing dependiente de magnitud + slew limiter)
    driveSm.prepare (sr, 2.0f, 24.0f, 10.0f);
    toneSm .prepare (sr, 1.8f, 26.0f, 12.0f);
    mixSm  .prepare (sr, 1.2f, 18.0f, 20.0f);

    driveSm.reset (pDrive ? *pDrive : 0.25f);
    toneSm .reset (pTone  ? *pTone  : 0.5f);
    mixSm  .reset (pMix   ? *pMix   : 1.0f);

    // Inicializa coef pointers (evita null)
    lowShelfL.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 220.0f, 0.707f, 1.0f);
    lowShelfR.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 220.0f, 0.707f, 1.0f);
    highShelfL.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3200.0f, 0.707f, 1.0f);
    highShelfR.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3200.0f, 0.707f, 1.0f);

    lowShelfPostL.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 220.0f, 0.707f, 1.0f);
    lowShelfPostR.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 220.0f, 0.707f, 1.0f);
    highShelfPostL.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 4200.0f, 0.707f, 1.0f);
    highShelfPostR.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 4200.0f, 0.707f, 1.0f);

    // ✅ AutoGain por bloque (entrada alineada vs salida real) para volumen constante
    autoGain.prepare (sr);

    // Guardar block size inicial (si el host luego usa uno mayor, creceremos buffers/OS)
    maxBlockSizePrepared = juce::jmax (1, samplesPerBlock);

    // Oversampling (solo bloque no lineal) según Quality
    const int choice = (pQuality != nullptr ? (int) std::round (*pQuality) : 1);
    const int exponent = juce::jlimit (1, 3, 1 + choice);
    rebuildOversampling (exponent);

    // ahora que driveSat ya conoce osSr, actualiza Tone↔Drive correctamente
    updateToneCoeffs (pTone ? *pTone : 0.5f, pDrive ? *pDrive : 0.25f);

    // ---------------------------------------------------------------------
    // Preset state lifecycle
    //
    // prepareToPlay() puede llamarse múltiples veces. Si ya teníamos un preset
    // activo, destruimos los estados construidos para evitar UB.
    if (activePreset != nullptr && activePreset->destruct != nullptr)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            if (presetStateConstructed[(size_t) ch])
                activePreset->destruct ((void*) &presetState[(size_t) ch]);
        }
    }

    presetStateConstructed = {{ false, false }};

    // Limpia bytes (no es estrictamente necesario, pero deja todo en un estado limpio)
    for (auto& st : presetState)
        std::memset (&st, 0, sizeof (st));

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
            if (activePreset->construct != nullptr)
            {
                activePreset->construct (st);
                presetStateConstructed[(size_t) ch] = true;
            }
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

    // FL Studio y otros hosts pueden variar el block size. Si crece,
    // re-inicializamos oversampling y buffers para evitar desalineaciones.
    if (numSamples > maxBlockSizePrepared)
    {
        maxBlockSizePrepared = numSamples;
        if (oversampling != nullptr)
            oversampling->initProcessing ((size_t) maxBlockSizePrepared);

        // crecer wetBuffer (pre OS)
        const int wetCh = juce::jmax (1, juce::jmin (2, numCh));
        wetBuffer.setSize (wetCh, maxBlockSizePrepared, false, false, true);

        // crecer delay para DRY alineado
        dryDelayBufferSize = maxBlockSizePrepared + dryDelaySamples + 1;
        dryDelayBuffer.setSize (wetCh, dryDelayBufferSize, false, false, true);
        dryDelayBuffer.clear();
        dryDelayWritePos = 0;
    }

    for (int ch = 2; ch < numCh; ++ch)
        buffer.clear (ch, 0, numSamples);

    auto* ch0 = buffer.getWritePointer (0);
    auto* ch1 = (numCh > 1) ? buffer.getWritePointer (1) : nullptr;

    // Targets (blindado por si el host llama sin parámetros aún)
    const float driveTarget = (pDrive != nullptr ? *pDrive : 0.25f);
    const float toneTarget  = (pTone  != nullptr ? *pTone  : 0.5f);
    const float mixTarget   = (pMix   != nullptr ? *pMix   : 1.0f);

    // Quality: si cambia, reconstruimos oversampling SOLO para el bloque no lineal
    const int choice = (pQuality != nullptr ? (int) std::round (*pQuality) : 1);
    const int exponent = juce::jlimit (1, 3, 1 + choice);
    if (exponent != osExponentPrepared)
        rebuildOversampling (exponent);

    // Selección de preset
    int preampIndex = 0;
    if (pPreamp != nullptr && PresetRegistry::items.size() > 0)
        preampIndex = juce::jlimit (0, (int) PresetRegistry::items.size() - 1,
                                    (int) std::lround (*pPreamp));

    if ((activePreset == nullptr || preampIndex != activePresetIndex) && PresetRegistry::items.size() > 0)
    {
        // Destruir estado anterior antes de cambiar de preset
        if (activePreset != nullptr && activePreset->destruct != nullptr)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                if (presetStateConstructed[(size_t) ch])
                    activePreset->destruct ((void*) &presetState[(size_t) ch]);
            }
        }

        presetStateConstructed = {{ false, false }};

        activePresetIndex = preampIndex;
        activePreset = &PresetRegistry::items[(size_t) preampIndex];
        // osSr ya lo define rebuildOversampling(). Si cambias de preset, solo re-prepare.

        for (int ch = 0; ch < 2; ++ch)
        {
            void* st = (void*) &presetState[(size_t) ch];
            if (activePreset->construct != nullptr)
            {
                activePreset->construct (st);
                presetStateConstructed[(size_t) ch] = true;
            }
            if (activePreset->prepare) activePreset->prepare (st, osSr);
            if (activePreset->reset)   activePreset->reset (st);
        }
    }

    // Actualiza Tone (tilt) en base al valor suavizado actual (evita zipper por bloques)
    updateToneCoeffs (toneSm.getCurrent(), driveSm.getCurrent());

    // Asegura wetBuffer sin realocar cada bloque
    const int wetCh = juce::jmax (1, juce::jmin (2, numCh));
    if (wetBuffer.getNumChannels() != wetCh || wetBuffer.getNumSamples() < numSamples)
        wetBuffer.setSize (wetCh, numSamples, false, false, true);

    auto* wetL = wetBuffer.getWritePointer (0);
    auto* wetR = (wetCh > 1) ? wetBuffer.getWritePointer (1) : nullptr;

    // -------------------------------------------------------------------------
    // 1) WET base SR: input drive real (dB) + Tone PRE (tilt)
    float drive01ForBlock = driveSm.getCurrent();
    for (int i = 0; i < numSamples; ++i)
    {
        const float drive01 = driveSm.process (driveTarget);
        (void) toneSm.process (toneTarget); // smoothing para próximos bloques
        drive01ForBlock = drive01;

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
    // 2) Oversampling SOLO donde duele (bloque no-lineal)
    if (oversampling != nullptr)
    {
        juce::dsp::AudioBlock<float> baseBlock (wetBuffer);

        // procesamos SOLO numSamples (aunque wetBuffer sea más grande)
        baseBlock = baseBlock.getSubBlock (0, (size_t) numSamples);

        auto osBlock = oversampling->processSamplesUp (baseBlock);

        const size_t osSamples = osBlock.getNumSamples();
        const size_t osCh = osBlock.getNumChannels();

        const auto satParams = DriveSaturator::makeParams (drive01ForBlock);

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

                // Drive multi-etapa (3D) + bias + dc blocker
                xL = driveSat.processSample (xL, 0, satParams);
                xR = driveSat.processSample (xR, 1, satParams);

                if (activePreset != nullptr && activePreset->process != nullptr)
                {
                    xL = activePreset->process (stL, xL);
                    xR = activePreset->process (stR, xR);
                }

                L[n] = xL;
                R[n] = xR;
            }
        }
        else
        {
            for (size_t ch = 0; ch < osCh; ++ch)
            {
                float* data = osBlock.getChannelPointer (ch);
                void* st = (void*) &presetState[juce::jmin ((size_t) 1, ch)];

                for (size_t n = 0; n < osSamples; ++n)
                {
                    float x = driveSat.processSample (data[n], (int) ch, satParams);
                    if (activePreset != nullptr && activePreset->process != nullptr)
                        x = activePreset->process (st, x);
                    data[n] = x;
                }
            }
        }

        oversampling->processSamplesDown (baseBlock);
    }

    // -------------------------------------------------------------------------
    // 2.5) Tone POST (carácter / tame harshness con drive)
    for (int i = 0; i < numSamples; ++i)
    {
        float yL = wetL[i];
        yL = lowShelfPostL.processSample (yL);
        yL = highShelfPostL.processSample (yL);
        wetL[i] = yL;

        if (wetR != nullptr)
        {
            float yR = wetR[i];
            yR = lowShelfPostR.processSample (yR);
            yR = highShelfPostR.processSample (yR);
            wetR[i] = yR;
        }
    }

    // -------------------------------------------------------------------------
    // 3) Mix + AUTO-LEVEL (volumen constante) - ULTRA (por muestra)
//
// Objetivo: que el volumen se mantenga constante aunque muevas cualquier knob.
// Este modo es muy estable en hosts con buffer variable (FL Studio, etc.).
//
// Medimos potencia (HP) de:
//   - ENTRADA: dry ALINEADO con la latencia del oversampling (dryDelay)
//   - SALIDA:  mixed PRE autogain
// y aplicamos la ganancia compensatoria por muestra (con attack/release internos).
//
// ⚠️ Importante: la medición de salida es PRE gain para evitar auto-cancelación.

    // ✅ 3.3) Punteros del delay (una vez por bloque, no por sample)
    auto* dL = dryDelayBuffer.getWritePointer (0);
    auto* dR = (dryDelayBuffer.getNumChannels() > 1) ? dryDelayBuffer.getWritePointer (1) : nullptr;

    const int size = dryDelayBuffer.getNumSamples();

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix01 = mixSm.process (mixTarget);

        // ✅ DRY retrasado/alineado con oversampling
        const int wp = dryDelayWritePos;
        int rp = wp - dryDelaySamples;
        if (rp < 0) rp += size;

        // entrada actual (sin delay)
        const float inL = ch0[i];
        const float inR = (ch1 != nullptr) ? ch1[i] : inL;

        // escribir entrada al delay
        dL[wp] = inL;
        if (dR != nullptr) dR[wp] = inR;

        // leer dry alineado
        const float dryL = dL[rp];
        const float dryR = (dR != nullptr) ? dR[rp] : dryL;

        // avanzar puntero
        dryDelayWritePos = wp + 1;
        if (dryDelayWritePos >= size) dryDelayWritePos = 0;

        const float wetOutL = wetL[i];
        const float wetOutR = (wetR != nullptr) ? wetR[i] : wetOutL;

        // -----------------------------------------------------------------
        // BYPASS EXACTO CUANDO MIX=0
        //
        // Si el usuario pone MIX=0, quiere el audio idéntico al original (sin
        // autogain, sin safety clip, sin nada que cambie el nivel/tono).
        // Igual actualizamos el autogain con dry->dry para que se mantenga en unity.
        if (mix01 <= 1.0e-4f)
        {
            (void) autoGain.processStereo (dryL, dryR, dryL, dryR);
            ch0[i] = dryL;
            if (ch1 != nullptr)
                ch1[i] = dryR;
            continue;
        }

        const float mixedL = plugin::equalPowerMix (dryL, wetOutL, mix01);
        const float mixedR = plugin::equalPowerMix (dryR, wetOutR, mix01);

        // AutoGain ULTRA: calcula ganancia desde dry vs mixed PRE gain
        const float g = autoGain.processStereo (dryL, dryR, mixedL, mixedR);

        ch0[i] = plugin::softClipSafety (mixedL * g);
        if (ch1 != nullptr)
            ch1[i] = plugin::softClipSafety (mixedR * g);
    }
}
//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YourPluginAudioProcessor();
}
