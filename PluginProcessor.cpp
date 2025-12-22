#include "PluginProcessor.h"
#include <cstring> // std::memset
#include <atomic>



// -----------------------------------------------------------------------------
// Fallbacks to keep PluginProcessor.cpp self-contained even if the header was not
// updated yet. If the class already defines these members/constants, those will
// be used instead (member lookup wins over globals).
namespace
{
    std::atomic<int> activeOSIndex { 1 }; // default HQ
    constexpr int kNumOSModes = 3;
}
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
    // AutoGain toggle (default ON). Disable it to save CPU in mixing.
    params.push_back (std::make_unique<juce::AudioParameterBool>(
        "autogain", "AutoGain",
        true));

    // Output trim (dB). Useful when AutoGain is off.
    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        "output", "Output",
        juce::NormalisableRange<float> (-24.0f, 12.0f, 0.01f),
        0.0f));


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
        , outputKnob ("Out")
    {
        // LookAndFeel per knob (colores por slider)
        driveKnob.slider.setLookAndFeel (&knobLNF);
        toneKnob .slider.setLookAndFeel (&knobLNF);
        mixKnob  .slider.setLookAndFeel (&knobLNF);
        outputKnob.slider.setLookAndFeel (&knobLNF);

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
        outputKnob.label.setFont (juce::Font (11.0f));

        addAndMakeVisible (driveKnob);
        addAndMakeVisible (toneKnob);
        addAndMakeVisible (mixKnob);
        addAndMakeVisible (outputKnob);

        autoGainButton.setButtonText ("AutoGain");
        addAndMakeVisible (autoGainButton);

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
        using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;


        driveAtt  = std::make_unique<SliderAttachment>   (processor.apvts, "drive",  driveKnob.slider);
        toneAtt   = std::make_unique<SliderAttachment>   (processor.apvts, "tone",   toneKnob.slider);
        mixAtt    = std::make_unique<SliderAttachment>   (processor.apvts, "mix",    mixKnob.slider);
        outputAtt = std::make_unique<SliderAttachment>   (processor.apvts, "output", outputKnob.slider);
        preampAtt = std::make_unique<ComboBoxAttachment> (processor.apvts, "preamp", preampBox);
        qualityAtt = std::make_unique<ComboBoxAttachment> (processor.apvts, "quality", qualityBox);
        autoGainAtt = std::make_unique<ButtonAttachment> (processor.apvts, "autogain", autoGainButton);

        // ✅ UI más grande
        setSize (820, 460);
    }

    ~MinimalEditor() override
    {
        header.stop();
        driveKnob.slider.setLookAndFeel (nullptr);
        toneKnob .slider.setLookAndFeel (nullptr);
        mixKnob  .slider.setLookAndFeel (nullptr);
        outputKnob.slider.setLookAndFeel (nullptr);
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
        auto bottom = area.removeFromBottom (120);

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

        // Right side controls: [AutoGain] [Out] [Quality]
        auto qualityArea = boxes.removeFromRight (170);
        auto outArea     = boxes.removeFromRight (100);
        auto agArea      = boxes.removeFromRight (120);

        // ComboBoxes (compact)
        auto preampArea = boxes;
        preampBox.setBounds (preampArea.removeFromTop (28));
        qualityBox.setBounds (qualityArea.removeFromTop (28));

        // AutoGain button + Output knob
        autoGainButton.setBounds (agArea.removeFromTop (28));
        outputKnob.setBounds (outArea.withSizeKeepingCentre (90, 104));

    }

private:
    YourPluginAudioProcessor& processor;

    KnobLNF knobLNF;

    AnimatedHeader header;

    plugin::ui::LabeledKnob driveKnob;
    plugin::ui::LabeledKnob toneKnob;
    plugin::ui::LabeledKnob mixKnob;

    plugin::ui::LabeledKnob outputKnob;

    juce::ToggleButton autoGainButton;

    juce::ComboBox preampBox;
    juce::ComboBox qualityBox;

   #if PLUGIN_HAS_ASSETS
    juce::ImageComponent modelImage;
    juce::Image modelImg;
   #endif

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> driveAtt, toneAtt, mixAtt, outputAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> preampAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> qualityAtt;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoGainAtt;

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
    pAutoGain = apvts.getRawParameterValue ("autogain");
    pOutput   = apvts.getRawParameterValue ("output");
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

//==============================================================================
// Tone (Tilt EQ) + interacción Drive↔Tone (pre/post)
// RT-safe: NO crea Coefficients::Ptr nuevos por bloque.
namespace
{
    
inline void setBiquad (juce::dsp::IIR::Coefficients<float>& c,
                       double b0, double b1, double b2,
                       double a0, double a1, double a2) noexcept
{
    const double invA0 = (a0 != 0.0 ? 1.0 / a0 : 1.0);

    // We write through getRawCoefficients() to avoid relying on internal layout/API differences
    // between JUCE versions (and to avoid lvalue issues on some toolchains).
    if (auto* p = c.getRawCoefficients())
    {
        // JUCE order: b0, b1, b2, a0, a1, a2
        p[0] = (float) (b0 * invA0);
        p[1] = (float) (b1 * invA0);
        p[2] = (float) (b2 * invA0);
        p[3] = 1.0f;
        p[4] = (float) (a1 * invA0);
        p[5] = (float) (a2 * invA0);
    }
    else
    {
        // Extremely defensive fallback (should not happen).
        // Avoids heap allocations: assigns into the existing object.
        c = juce::dsp::IIR::Coefficients<float> ((float) (b0 * invA0),
                                                (float) (b1 * invA0),
                                                (float) (b2 * invA0),
                                                1.0f,
                                                (float) (a1 * invA0),
                                                (float) (a2 * invA0));
    }
}

    // RBJ Audio EQ Cookbook shelf (S = shelf slope). We map JUCE's "Q" argument to S.
    inline void setLowShelf (juce::dsp::IIR::Coefficients<float>& c,
                             double sampleRate, double freqHz, double S,
                             double gainLinear) noexcept
    {
        const double A  = std::sqrt (juce::jmax (1.0e-12, gainLinear));
        const double w0 = 2.0 * juce::MathConstants<double>::pi * (freqHz / juce::jmax (1.0, sampleRate));
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double sqrtA = std::sqrt (A);
        const double slope = juce::jmax (1.0e-4, S);
        const double alpha = (sw / 2.0) * std::sqrt (((A + 1.0 / A) * (1.0 / slope - 1.0)) + 2.0);

        const double b0 =    A * ((A + 1.0) - (A - 1.0) * cw + 2.0 * sqrtA * alpha);
        const double b1 =  2*A * ((A - 1.0) - (A + 1.0) * cw);
        const double b2 =    A * ((A + 1.0) - (A - 1.0) * cw - 2.0 * sqrtA * alpha);
        const double a0 =         (A + 1.0) + (A - 1.0) * cw + 2.0 * sqrtA * alpha;
        const double a1 =    -2 * ((A - 1.0) + (A + 1.0) * cw);
        const double a2 =         (A + 1.0) + (A - 1.0) * cw - 2.0 * sqrtA * alpha;

        setBiquad (c, b0, b1, b2, a0, a1, a2);
    }

    inline void setHighShelf (juce::dsp::IIR::Coefficients<float>& c,
                              double sampleRate, double freqHz, double S,
                              double gainLinear) noexcept
    {
        const double A  = std::sqrt (juce::jmax (1.0e-12, gainLinear));
        const double w0 = 2.0 * juce::MathConstants<double>::pi * (freqHz / juce::jmax (1.0, sampleRate));
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double sqrtA = std::sqrt (A);
        const double slope = juce::jmax (1.0e-4, S);
        const double alpha = (sw / 2.0) * std::sqrt (((A + 1.0 / A) * (1.0 / slope - 1.0)) + 2.0);

        const double b0 =    A * ((A + 1.0) + (A - 1.0) * cw + 2.0 * sqrtA * alpha);
        const double b1 = -2*A * ((A - 1.0) + (A + 1.0) * cw);
        const double b2 =    A * ((A + 1.0) + (A - 1.0) * cw - 2.0 * sqrtA * alpha);
        const double a0 =         (A + 1.0) - (A - 1.0) * cw + 2.0 * sqrtA * alpha;
        const double a1 =     2 * ((A - 1.0) - (A + 1.0) * cw);
        const double a2 =         (A + 1.0) - (A - 1.0) * cw - 2.0 * sqrtA * alpha;

        setBiquad (c, b0, b1, b2, a0, a1, a2);
    }
}

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

    // "Q" dinámico (aquí lo usamos como shelf slope S)
    const float q = 0.707f + 0.38f * mag01;

    // -------------------------
    // PRE tilt
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

    const double gLowPre  = juce::Decibels::decibelsToGain (-preTiltDb);
    const double gHighPre = juce::Decibels::decibelsToGain ( preTiltDb);

    setLowShelf  (*lowShelfL.coefficients,  sr, fcLowPre,  q, gLowPre);
    setLowShelf  (*lowShelfR.coefficients,  sr, fcLowPre,  q, gLowPre);
    setHighShelf (*highShelfL.coefficients, sr, fcHighPre, q, gHighPre);
    setHighShelf (*highShelfR.coefficients, sr, fcHighPre, q, gHighPre);

    // -------------------------
    // POST tilt
    float fcLowPost  = 280.0f;
    float fcHighPost = 3800.0f;

    if (postTiltDb < 0.0f) // dark
        fcHighPost *= (1.0f - 0.35f * mag01);
    else if (postTiltDb > 0.0f) // bright
        fcHighPost *= (1.0f + 0.55f * mag01);

    fcLowPost  = juce::jlimit (120.0f,  900.0f, fcLowPost);
    fcHighPost = juce::jlimit (1400.0f, 9000.0f, fcHighPost);

    const float qPost = 0.707f + 0.25f * mag01;

    const double gLowPost  = juce::Decibels::decibelsToGain (-postTiltDb);
    const double gHighPost = juce::Decibels::decibelsToGain ( postTiltDb);

    setLowShelf  (*lowShelfPostL.coefficients,  sr, fcLowPost,  qPost, gLowPost);
    setLowShelf  (*lowShelfPostR.coefficients,  sr, fcLowPost,  qPost, gLowPost);
    setHighShelf (*highShelfPostL.coefficients, sr, fcHighPost, qPost, gHighPost);
    setHighShelf (*highShelfPostR.coefficients, sr, fcHighPost, qPost, gHighPost);

    // Drive ↔ Tone: softening highs (anti-fizz) a más drive
    const float softPos = std::pow (d, 1.20f);
    const float hfHz = 18000.0f - 9000.0f * softPos; // 18k -> 9k
    for (auto& ds : driveSatBank)
        ds.setHighSoftHz (hfHz);
}

//==============================================================================
// Oversampling banks (Eco/HQ/Insane) - construido en prepareToPlay (NO audio thread)

void YourPluginAudioProcessor::buildOversamplingBanks()
{
    const auto channels = (size_t) juce::jmax (1, juce::jmin (2, getTotalNumInputChannels()));

    maxOSLatencySamples = 0;

    for (int i = 0; i < kNumOSModes; ++i)
    {
        const int exponent = osExponentBank[(size_t) i];

        juce::dsp::Oversampling<float>::FilterType type = juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR;
        bool maxQuality = false;
        if (exponent >= 2)
            type = juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple;
        if (exponent >= 3)
            maxQuality = true;

        oversamplingBank[(size_t) i] = std::make_unique<juce::dsp::Oversampling<float>> (
            channels,
            exponent,
            type,
            maxQuality);

        oversamplingBank[(size_t) i]->reset();
        oversamplingBank[(size_t) i]->initProcessing ((size_t) juce::jmax (1, maxBlockSizePrepared));

        const int latency = (int) oversamplingBank[(size_t) i]->getLatencyInSamples();
        osLatencyBank[(size_t) i] = latency;
        maxOSLatencySamples = juce::jmax (maxOSLatencySamples, latency);

        const float osFactor = (float) (1u << exponent);
        osSrBank[(size_t) i] = (float) (sr * osFactor);

        stereoInteractBank[(size_t) i].prepare (osSrBank[(size_t) i]);
        driveSatBank[(size_t) i].prepare (osSrBank[(size_t) i]);

        preShaperLPzBank[(size_t) i] = {{ 0.0f, 0.0f }};
        osAaLPzBank[(size_t) i]      = {{ 0.0f, 0.0f }};
    }

    // Fijamos latencia al MAX (8x) y la mantenemos estable.
    setLatencySamples (maxOSLatencySamples);

    // Buffers RT-safe (sin realloc en processBlock)
    dryDelayWritePos   = 0;
    outDelayWritePos   = 0;

    dryDelayBufferSize = juce::jmax (1, maxBlockSizePrepared + maxOSLatencySamples + 1);
    outDelayBufferSize = dryDelayBufferSize;

    dryDelayBuffer.setSize ((int) channels, dryDelayBufferSize, false, false, true);
    dryDelayBuffer.clear();

    outDelayBuffer.setSize ((int) channels, outDelayBufferSize, false, false, true);
    outDelayBuffer.clear();

    wetBuffer.setSize ((int) channels, juce::jmax (1, maxBlockSizePrepared), false, false, true);
    wetBuffer.clear();
}

//==============================================================================
// Prepare


void YourPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

    // Anti-zipper PRO
    driveSm.prepare (sr, 2.0f, 24.0f, 10.0f);
    toneSm .prepare (sr, 1.8f, 26.0f, 12.0f);
    mixSm  .prepare (sr, 1.2f, 18.0f, 20.0f);

    driveSm.reset (pDrive ? pDrive->load() : 0.25f);
    toneSm .reset (pTone  ? pTone ->load() : 0.5f);
    mixSm  .reset (pMix   ? pMix  ->load() : 1.0f);

    // Inicializa coef pointers (evita null). Esta asignacion ocurre SOLO en prepareToPlay (no RT).
    lowShelfL.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 220.0f, 0.707f, 1.0f);
    lowShelfR.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 220.0f, 0.707f, 1.0f);
    highShelfL.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3200.0f, 0.707f, 1.0f);
    highShelfR.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3200.0f, 0.707f, 1.0f);

    lowShelfPostL.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 220.0f, 0.707f, 1.0f);
    lowShelfPostR.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 220.0f, 0.707f, 1.0f);
    highShelfPostL.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 4200.0f, 0.707f, 1.0f);
    highShelfPostR.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 4200.0f, 0.707f, 1.0f);

    // AutoGain (CPU optional)
    autoGain.prepare (sr);
    autoGainWasEnabled = (pAutoGain != nullptr ? (pAutoGain->load() >= 0.5f) : true);
    if (! autoGainWasEnabled)
        autoGain.reset();

    // Pre-alloc: procesamos por chunks <= maxBlockSizePrepared para evitar realloc en audio thread.
    maxBlockSizePrepared = juce::jmax (1, samplesPerBlock);
    maxBlockSizePrepared = juce::jmax (maxBlockSizePrepared, 8192); // margen para hosts con blocks mas grandes

    buildOversamplingBanks();

    // OS activo inicial segun Quality
    const int choice = (pQuality != nullptr ? (int) std::lround (pQuality->load()) : 1);
    activeOSIndex.store (juce::jlimit (0, kNumOSModes - 1, choice));

    // Actualiza Tone↔Drive correctamente (usa driveSatBank ya preparado)
    updateToneCoeffs (pTone ? pTone->load() : 0.5f,
                      pDrive ? pDrive->load() : 0.25f);

    // ---------------------------------------------------------------------
    // Preset state lifecycle (por modo OS)
    if (activePreset != nullptr && activePreset->destruct != nullptr)
    {
        for (int os = 0; os < kNumOSModes; ++os)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                if (presetStateConstructedBank[(size_t) os][(size_t) ch])
                    activePreset->destruct ((void*) &presetStateBank[(size_t) os][(size_t) ch]);
            }
        }
    }

    for (int os = 0; os < kNumOSModes; ++os)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            presetStateConstructedBank[(size_t) os][(size_t) ch] = false;
            std::memset (&presetStateBank[(size_t) os][(size_t) ch], 0, sizeof (PresetStateStorage));
        }
    }

    activePresetIndex = -1;
    activePreset = nullptr;

    int preampIndex = 0;
    if (pPreamp != nullptr && ! PresetRegistry::items.empty())
        preampIndex = juce::jlimit (0, (int) PresetRegistry::items.size() - 1,
                                    (int) std::lround (pPreamp->load()));

    if (! PresetRegistry::items.empty())
    {
        activePresetIndex = preampIndex;
        activePreset = &PresetRegistry::items[(size_t) preampIndex];

        for (int os = 0; os < kNumOSModes; ++os)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                void* st = (void*) &presetStateBank[(size_t) os][(size_t) ch];
                if (activePreset->construct != nullptr)
                {
                    activePreset->construct (st);
                    presetStateConstructedBank[(size_t) os][(size_t) ch] = true;
                }
                if (activePreset->prepare) activePreset->prepare (st, osSrBank[(size_t) os]);
                if (activePreset->reset)   activePreset->reset (st);
            }
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

    // Targets (blindado)
    const float driveTarget = (pDrive   != nullptr ? pDrive->load()   : 0.25f);
    const float toneTarget  = (pTone    != nullptr ? pTone->load()    : 0.5f);
    const float mixTarget   = (pMix     != nullptr ? pMix->load()     : 1.0f);

    const bool  autoGainEnabled = (pAutoGain != nullptr ? (pAutoGain->load() >= 0.5f) : true);
    if (autoGainEnabled && ! autoGainWasEnabled)
        autoGain.reset();
    autoGainWasEnabled = autoGainEnabled;

    const float outDb    = (pOutput != nullptr ? pOutput->load() : 0.0f);
    const float outGain  = juce::Decibels::decibelsToGain (outDb);

    // Quality: RT-safe switch (solo cambia indice, sin alloc)
    const int desiredOS = juce::jlimit (0, kNumOSModes - 1,
                                       (pQuality != nullptr ? (int) std::lround (pQuality->load()) : 1));
    activeOSIndex.store (desiredOS);

    const int osIdx     = activeOSIndex.load();
    const int osLatency = osLatencyBank[(size_t) osIdx];
    const int outExtraDelay = juce::jmax (0, maxOSLatencySamples - osLatency);

    auto* os = oversamplingBank[(size_t) osIdx].get();
    if (os == nullptr)
        return;

    // Seleccion de preset
    int preampIndex = 0;
    if (pPreamp != nullptr && ! PresetRegistry::items.empty())
        preampIndex = juce::jlimit (0, (int) PresetRegistry::items.size() - 1,
                                    (int) std::lround (pPreamp->load()));

    if (! PresetRegistry::items.empty() && (activePreset == nullptr || preampIndex != activePresetIndex))
    {
        // Destruir estado anterior
        if (activePreset != nullptr && activePreset->destruct != nullptr)
        {
            for (int osMode = 0; osMode < kNumOSModes; ++osMode)
                for (int ch = 0; ch < 2; ++ch)
                    if (presetStateConstructedBank[(size_t) osMode][(size_t) ch])
                        activePreset->destruct ((void*) &presetStateBank[(size_t) osMode][(size_t) ch]);
        }

        for (int osMode = 0; osMode < kNumOSModes; ++osMode)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                presetStateConstructedBank[(size_t) osMode][(size_t) ch] = false;
                std::memset (&presetStateBank[(size_t) osMode][(size_t) ch], 0, sizeof (PresetStateStorage));
            }
        }

        activePresetIndex = preampIndex;
        activePreset = &PresetRegistry::items[(size_t) preampIndex];

        for (int osMode = 0; osMode < kNumOSModes; ++osMode)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                void* st = (void*) &presetStateBank[(size_t) osMode][(size_t) ch];
                if (activePreset->construct != nullptr)
                {
                    activePreset->construct (st);
                    presetStateConstructedBank[(size_t) osMode][(size_t) ch] = true;
                }
                if (activePreset->prepare) activePreset->prepare (st, osSrBank[(size_t) osMode]);
                if (activePreset->reset)   activePreset->reset (st);
            }
        }
    }

    auto* ch0 = buffer.getWritePointer (0);
    auto* ch1 = (numCh > 1) ? buffer.getWritePointer (1) : nullptr;

    // Modo OS activo
    const float osSr   = osSrBank[(size_t) osIdx];
    auto& stereoInteract = stereoInteractBank[(size_t) osIdx];
    auto& driveSat       = driveSatBank[(size_t) osIdx];
    auto& preShaperZ     = preShaperLPzBank[(size_t) osIdx];
    auto& osAaZ          = osAaLPzBank[(size_t) osIdx];

    // Procesamos en chunks para evitar realloc incluso si el host envia buffers gigantes.
    const int chunkSize = juce::jmax (1, maxBlockSizePrepared);

    for (int base = 0; base < numSamples; base += chunkSize)
    {
        const int n = juce::jmin (chunkSize, numSamples - base);

        // 0) Coefs de Tone 1 vez por chunk (igual que antes: por bloque)
        updateToneCoeffs (toneSm.getCurrent(), driveSm.getCurrent());

        // 1) Pre-tilt + smoothing (genera wetBuffer @ base SR)
        auto* wetL = wetBuffer.getWritePointer (0);
        auto* wetR = (wetBuffer.getNumChannels() > 1) ? wetBuffer.getWritePointer (1) : nullptr;

        float drive01ForBlock = driveSm.getCurrent();

        for (int i = 0; i < n; ++i)
        {
            const float drive01 = driveSm.process (driveTarget);
            const float tone01  = toneSm .process (toneTarget);
            (void) tone01; // tone01 se usa indirectamente via updateToneCoeffs() por bloque
            drive01ForBlock = drive01;

            const float inL = ch0[base + i];
            const float inR = (ch1 != nullptr) ? ch1[base + i] : inL;

            float xL = lowShelfL.processSample (0, inL);
            xL       = highShelfL.processSample (0, xL);

            float xR = lowShelfR.processSample (0, inR);
            xR       = highShelfR.processSample (0, xR);

            wetL[i] = xL;
            if (wetR != nullptr) wetR[i] = xR;
        }

        // 2) Oversampled non-linear block
        auto baseBlock = juce::dsp::AudioBlock<float> (wetBuffer).getSubBlock (0, (size_t) n);
        auto osBlock   = os->processSamplesUp (baseBlock);

        const int osCh = (int) osBlock.getNumChannels();
        const int osN  = (int) osBlock.getNumSamples();

        const auto satParams = DriveSaturator::makeParams (drive01ForBlock);

        for (int i = 0; i < osN; ++i)
        {
            float xL = osBlock.getSample (0, (size_t) i);
            float xR = (osCh > 1) ? osBlock.getSample (1, (size_t) i) : xL;

            // Pre shaper 1-pole LP (anti-alias)
            xL = plugin::onePoleLP (xL, preShaperZ[0], 0.25f);
            xR = plugin::onePoleLP (xR, preShaperZ[1], 0.25f);

            if (activePreset != nullptr)
            {
                void* stL = (void*) &presetStateBank[(size_t) osIdx][0];
                void* stR = (void*) &presetStateBank[(size_t) osIdx][1];

                if (activePreset->processSample) xL = activePreset->processSample (stL, xL);
                if (activePreset->processSample) xR = activePreset->processSample (stR, xR);
            }

            stereoInteract.processSample (xL, xR);

            xL = driveSat.processSample (0, xL, satParams);
            xR = driveSat.processSample (1, xR, satParams);

            // AA LP post non-linearity
            xL = plugin::onePoleLP (xL, osAaZ[0], 0.15f);
            xR = plugin::onePoleLP (xR, osAaZ[1], 0.15f);

            osBlock.setSample (0, (size_t) i, xL);
            if (osCh > 1)
                osBlock.setSample (1, (size_t) i, xR);
        }

        os->processSamplesDown (baseBlock);

        // 2.1) Post tilt (domar harshness con drive)
        for (int i = 0; i < n; ++i)
        {
            float yL = wetL[i];
            yL = lowShelfPostL.processSample (0, yL);
            yL = highShelfPostL.processSample (0, yL);
            wetL[i] = yL;

            if (wetR != nullptr)
            {
                float yR = wetR[i];
                yR = lowShelfPostR.processSample (0, yR);
                yR = highShelfPostR.processSample (0, yR);
                wetR[i] = yR;
            }
        }

        // 3) Mix + AutoGain + Output trim + fixed-latency output delay
        auto* dL = dryDelayBuffer.getWritePointer (0);
        auto* dR = (dryDelayBuffer.getNumChannels() > 1) ? dryDelayBuffer.getWritePointer (1) : nullptr;
        auto* oL = outDelayBuffer.getWritePointer (0);
        auto* oR = (outDelayBuffer.getNumChannels() > 1) ? outDelayBuffer.getWritePointer (1) : nullptr;

        const int dSize = dryDelayBuffer.getNumSamples();
        const int oSize = outDelayBuffer.getNumSamples();

        for (int i = 0; i < n; ++i)
        {
            const float mix01 = mixSm.process (mixTarget);

            const int wp = dryDelayWritePos;
            int rp = wp - osLatency;
            if (rp < 0) rp += dSize;

            const float inL = ch0[base + i];
            const float inR = (ch1 != nullptr) ? ch1[base + i] : inL;

            dL[wp] = inL;
            if (dR != nullptr) dR[wp] = inR;

            const float dryL = dL[rp];
            const float dryR = (dR != nullptr) ? dR[rp] : dryL;

            dryDelayWritePos = wp + 1;
            if (dryDelayWritePos >= dSize) dryDelayWritePos = 0;

            const float wetOutL = wetL[i];
            const float wetOutR = (wetR != nullptr) ? wetR[i] : wetOutL;

            float outLx = 0.0f;
            float outRx = 0.0f;

            if (mix01 <= 1.0e-4f)
            {
                if (autoGainEnabled)
                    (void) autoGain.processStereo (dryL, dryR, dryL, dryR);

                outLx = dryL;
                outRx = dryR;
            }
            else
            {
                const float mixedL = plugin::equalPowerMix (dryL, wetOutL, mix01);
                const float mixedR = plugin::equalPowerMix (dryR, wetOutR, mix01);

                const float g = autoGainEnabled ? autoGain.processStereo (dryL, dryR, mixedL, mixedR) : 1.0f;

                outLx = plugin::softClipSafety (mixedL * g * outGain);
                outRx = plugin::softClipSafety (mixedR * g * outGain);
            }

            // Output delay extra (latencia total fija al max)
            const int ow = outDelayWritePos;
            int orp = ow - outExtraDelay;
            if (orp < 0) orp += oSize;

            oL[ow] = outLx;
            if (oR != nullptr) oR[ow] = outRx;

            const float yL = oL[orp];
            const float yR = (oR != nullptr) ? oR[orp] : yL;

            outDelayWritePos = ow + 1;
            if (outDelayWritePos >= oSize) outDelayWritePos = 0;

            ch0[base + i] = yL;
            if (ch1 != nullptr)
                ch1[base + i] = yR;
        }
    }
}

//==============================================================================
// This creates new instances of the plugin.

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YourPluginAudioProcessor();
}



