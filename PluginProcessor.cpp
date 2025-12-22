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

    // helpers RT-safe: shelf RBJ, sin asignaciones
    auto dbToGain = [] (float db) noexcept
    {
        // gain = 10^(db/20) = exp2(db/6.0206)
        return std::exp2 (db / 6.0205999132796239f);
    };

    auto setCoeffs = [] (juce::dsp::IIR::Coefficients<float>& c,
                         float b0, float b1, float b2, float a0, float a1, float a2) noexcept
    {
        // Normalizar a0 -> 1
        const float invA0 = 1.0f / a0;
        b0 *= invA0; b1 *= invA0; b2 *= invA0;
        a1 *= invA0; a2 *= invA0;

        // JUCE dsp IIR coefficients: {b0,b1,b2,a0,a1,a2}
        c.coefficients[0] = b0;
        c.coefficients[1] = b1;
        c.coefficients[2] = b2;
        c.coefficients[3] = 1.0f;
        c.coefficients[4] = a1;
        c.coefficients[5] = a2;
    };

    auto calcLowShelf = [&] (float fc, float slope, float gainLin,
                             juce::dsp::IIR::Coefficients<float>& dst) noexcept
    {
        const float A = std::sqrt (juce::jmax (1.0e-12f, gainLin));
        const float w0 = 2.0f * juce::MathConstants<float>::pi * (fc / (float) sr);
        const float cosw0 = std::cos (w0);
        const float sinw0 = std::sin (w0);

        const float S = juce::jlimit (0.10f, 4.00f, slope);
        const float tmp = (A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f;
        const float alpha = 0.5f * sinw0 * std::sqrt (juce::jmax (0.0f, tmp));
        const float twoSqrtAAlpha = 2.0f * std::sqrt (A) * alpha;

        const float b0 =    A * ((A + 1.0f) - (A - 1.0f) * cosw0 + twoSqrtAAlpha);
        const float b1 =  2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
        const float b2 =    A * ((A + 1.0f) - (A - 1.0f) * cosw0 - twoSqrtAAlpha);
        const float a0 =          (A + 1.0f) + (A - 1.0f) * cosw0 + twoSqrtAAlpha;
        const float a1 =   -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
        const float a2 =          (A + 1.0f) + (A - 1.0f) * cosw0 - twoSqrtAAlpha;

        setCoeffs (dst, b0, b1, b2, a0, a1, a2);
    };

    auto calcHighShelf = [&] (float fc, float slope, float gainLin,
                              juce::dsp::IIR::Coefficients<float>& dst) noexcept
    {
        const float A = std::sqrt (juce::jmax (1.0e-12f, gainLin));
        const float w0 = 2.0f * juce::MathConstants<float>::pi * (fc / (float) sr);
        const float cosw0 = std::cos (w0);
        const float sinw0 = std::sin (w0);

        const float S = juce::jlimit (0.10f, 4.00f, slope);
        const float tmp = (A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f;
        const float alpha = 0.5f * sinw0 * std::sqrt (juce::jmax (0.0f, tmp));
        const float twoSqrtAAlpha = 2.0f * std::sqrt (A) * alpha;

        const float b0 =    A * ((A + 1.0f) + (A - 1.0f) * cosw0 + twoSqrtAAlpha);
        const float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
        const float b2 =    A * ((A + 1.0f) + (A - 1.0f) * cosw0 - twoSqrtAAlpha);
        const float a0 =          (A + 1.0f) - (A - 1.0f) * cosw0 + twoSqrtAAlpha;
        const float a1 =    2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
        const float a2 =          (A + 1.0f) - (A - 1.0f) * cosw0 - twoSqrtAAlpha;

        setCoeffs (dst, b0, b1, b2, a0, a1, a2);
    };

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

    const float preGainLow  = dbToGain (-preTiltDb);
    const float preGainHigh = dbToGain ( preTiltDb);

    calcLowShelf  (fcLowPre,  q, preGainLow,  *lowShelfL.coefficients);
    calcLowShelf  (fcLowPre,  q, preGainLow,  *lowShelfR.coefficients);
    calcHighShelf (fcHighPre, q, preGainHigh, *highShelfL.coefficients);
    calcHighShelf (fcHighPre, q, preGainHigh, *highShelfR.coefficients);

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

    const float postGainLow  = dbToGain (-postTiltDb);
    const float postGainHigh = dbToGain ( postTiltDb);

    calcLowShelf  (fcLowPost,  qPost, postGainLow,  *lowShelfPostL.coefficients);
    calcLowShelf  (fcLowPost,  qPost, postGainLow,  *lowShelfPostR.coefficients);
    calcHighShelf (fcHighPost, qPost, postGainHigh, *highShelfPostL.coefficients);
    calcHighShelf (fcHighPost, qPost, postGainHigh, *highShelfPostR.coefficients);

    // Drive ↔ Tone: softening highs (anti-fizz) a más drive
    const float softPos = std::pow (d, 1.20f);
    const float hfHz = 18000.0f - 9000.0f * softPos; // 18k -> 9k
    driveSat.setHighSoftHz (hfHz);
}

//==============================================================================
// Process
void YourPluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    // Limpiar canales extra
    for (int ch = 2; ch < numCh; ++ch)
        buffer.clear (ch, 0, numSamples);

    auto* ch0 = buffer.getWritePointer (0);
    auto* ch1 = (numCh > 1) ? buffer.getWritePointer (1) : nullptr;

    // Targets (blindado por si el host llama sin parámetros aún)
    const float driveTarget = (pDrive != nullptr ? pDrive->load() : 0.25f);
    const float toneTarget  = (pTone  != nullptr ? pTone ->load() : 0.5f);
    const float mixTarget   = (pMix   != nullptr ? pMix  ->load() : 1.0f);

    // Quality: 0=Eco(2x), 1=HQ(4x), 2=Insane(8x)
    const int choice = (pQuality != nullptr ? (int) std::lround (pQuality->load()) : 1);
    const int newOSIndex = juce::jlimit (0, 2, choice);

    // RT-safe: cambiar punteros / recomputar SR interno (sin alloc)
    if (newOSIndex != activeOSIndex && oversamplingConfigs[(size_t) newOSIndex] != nullptr)
    {
        activeOSIndex = newOSIndex;

        currentOSLatencySamples = (int) oversamplingConfigs[(size_t) activeOSIndex]->getLatencyInSamples();
        dryDelaySamples = currentOSLatencySamples;

        outputPadSamples = juce::jmax (0, maxLatencySamples - currentOSLatencySamples);

        const float osFactor = (float) (1u << (1 + activeOSIndex));
        osSr = (float) (sr * osFactor);

        // módulos oversampled (RT-safe: solo floats + reset)
        stereoInteract.prepare (osSr);
        driveSat.prepare (osSr);

        // reset estados oversampled para evitar basura / NaN al cambiar Quality
        preShaperLPz = {{ 0.0f, 0.0f }};
        osAaLPz      = {{ 0.0f, 0.0f }};

        // preset: re-prepare al SR interno nuevo
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

    // Selección de preset (puede cambiar por bloque)
    int preampIndex = 0;
    if (pPreamp != nullptr && PresetRegistry::items.size() > 0)
        preampIndex = juce::jlimit (0, (int) PresetRegistry::items.size() - 1,
                                    (int) std::lround (pPreamp->load()));

    if ((activePreset == nullptr || preampIndex != activePresetIndex) && PresetRegistry::items.size() > 0)
    {
        // Destruir estado anterior antes de cambiar de preset
        if (activePreset != nullptr && activePreset->destruct != nullptr)
        {
            for (int ch = 0; ch < 2; ++ch)
                if (presetStateConstructed[(size_t) ch])
                    activePreset->destruct ((void*) &presetState[(size_t) ch]);
        }

        presetStateConstructed = {{ false, false }};

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

    // Actualiza Tone (tilt) en base al valor suavizado actual (evita zipper por bloques)
    updateToneCoeffs (toneSm.getCurrent(), driveSm.getCurrent());

    // Punteros de delays (una vez por bloque)
    auto* dL = dryDelayBuffer.getWritePointer (0);
    auto* dR = (dryDelayBuffer.getNumChannels() > 1) ? dryDelayBuffer.getWritePointer (1) : nullptr;
    const int drySize = dryDelayBuffer.getNumSamples();

    auto* oL = outputDelayBuffer.getWritePointer (0);
    auto* oR = (outputDelayBuffer.getNumChannels() > 1) ? outputDelayBuffer.getWritePointer (1) : nullptr;
    const int outSize = outputDelayBuffer.getNumSamples();

    // Procesamos en chunks para evitar realloc en hosts con bloques gigantes
    int offset = 0;
    while (offset < numSamples)
    {
        const int block = juce::jmin (maxBlockSizePrepared, numSamples - offset);
        const int wetCh = juce::jmax (1, juce::jmin (2, numCh));

        auto* wetL = wetBuffer.getWritePointer (0);
        auto* wetR = (wetCh > 1) ? wetBuffer.getWritePointer (1) : nullptr;

        // -------------------------------------------------------------------------
        // 1) WET base SR: pregain + Tone PRE (tilt)
        float drive01ForBlock = driveSm.getCurrent();

        for (int i = 0; i < block; ++i)
        {
            const float drive01 = driveSm.process (driveTarget);
            (void) toneSm.process (toneTarget); // smoothing para próximos bloques
            drive01ForBlock = drive01;

            const float pregain = plugin::drive01ToPregainFast (drive01);

            float xL = plugin::sanitize (ch0[offset + i] * pregain);
            xL = lowShelfL.processSample (xL);
            xL = highShelfL.processSample (xL);
            wetL[i] = plugin::sanitize (xL);

            if (wetR != nullptr)
            {
                const float inR = (ch1 != nullptr) ? ch1[offset + i] : ch0[offset + i];
                float xR = plugin::sanitize (inR * pregain);
                xR = lowShelfR.processSample (xR);
                xR = highShelfR.processSample (xR);
                wetR[i] = plugin::sanitize (xR);
            }
        }

        // -------------------------------------------------------------------------
        // 2) Oversampling SOLO donde duele (bloque no-lineal)
        auto* os = oversamplingConfigs[(size_t) activeOSIndex].get();
        if (os != nullptr)
        {
            juce::dsp::AudioBlock<float> baseBlock (wetBuffer);
            baseBlock = baseBlock.getSubBlock (0, (size_t) block);

            auto osBlock = os->processSamplesUp (baseBlock);

            const size_t osSamples = osBlock.getNumSamples();
            const size_t osCh = osBlock.getNumChannels();

            const auto satParams = DriveSaturator::makeParams (drive01ForBlock);

            // Anti-alias LP dependiente de drive (pre) + LP final (post)
            const float d = juce::jlimit (0.0f, 1.0f, drive01ForBlock);
            auto smoothstep = [] (float a, float b, float x)
            {
                const float t = juce::jlimit (0.0f, 1.0f, (x - a) / (b - a));
                return t * t * (3.0f - 2.0f * t);
            };

            const float preAmt = smoothstep (0.55f, 1.00f, d);          // 0..1
            const float baseNyq = 0.5f * (float) sr;

            const float preHzTarget = 20000.0f - 12000.0f * preAmt;     // 20k -> 8k
            const float preHz = juce::jlimit (5000.0f, 0.49f * baseNyq, preHzTarget);
            const float preA  = std::exp (-2.0f * juce::MathConstants<float>::pi * preHz / osSr);

            const float aaHz = 0.47f * baseNyq;
            const float aaA  = std::exp (-2.0f * juce::MathConstants<float>::pi * aaHz / osSr);

            auto preLP = [&] (float x, int chIdx) noexcept
            {
                const int ch = juce::jlimit (0, 1, chIdx);
                if (preAmt <= 1.0e-4f)
                {
                    preShaperLPz[(size_t) ch] = x;
                    return x;
                }

                const float y = (1.0f - preA) * x + preA * preShaperLPz[(size_t) ch];
                preShaperLPz[(size_t) ch] = y;
                return x + preAmt * (y - x);
            };

            if (osCh == 2)
            {
                float* L = osBlock.getChannelPointer (0);
                float* R = osBlock.getChannelPointer (1);

                void* stL = (void*) &presetState[0];
                void* stR = (void*) &presetState[1];

                for (size_t n = 0; n < osSamples; ++n)
                {
                    float xL = plugin::sanitize (L[n]);
                    float xR = plugin::sanitize (R[n]);

                    xL = preLP (xL, 0);
                    xR = preLP (xR, 1);

                    stereoInteract.processSample (xL, xR);

                    xL = driveSat.processSample (xL, 0, satParams);
                    xR = driveSat.processSample (xR, 1, satParams);

                    if (activePreset != nullptr && activePreset->process != nullptr)
                    {
                        xL = activePreset->process (stL, xL);
                        xR = activePreset->process (stR, xR);
                    }

                    L[n] = plugin::sanitize (xL);
                    R[n] = plugin::sanitize (xR);
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
                        float x = plugin::sanitize (data[n]);
                        x = preLP (x, (int) ch);
                        x = driveSat.processSample (x, (int) ch, satParams);
                        if (activePreset != nullptr && activePreset->process != nullptr)
                            x = activePreset->process (st, x);
                        data[n] = plugin::sanitize (x);
                    }
                }
            }

            // Anti-alias LP final en dominio oversampled
            for (size_t ch = 0; ch < osCh; ++ch)
            {
                float* data = osBlock.getChannelPointer (ch);
                float z = osAaLPz[juce::jlimit ((size_t) 0, (size_t) 1, ch)];

                for (size_t n = 0; n < osSamples; ++n)
                {
                    const float y = (1.0f - aaA) * data[n] + aaA * z;
                    z = y;
                    data[n] = y;
                }

                osAaLPz[juce::jlimit ((size_t) 0, (size_t) 1, ch)] = z;
            }

            os->processSamplesDown (baseBlock);
        }

        // -------------------------------------------------------------------------
        // 2.5) Tone POST
        for (int i = 0; i < block; ++i)
        {
            float yL = wetL[i];
            yL = lowShelfPostL.processSample (yL);
            yL = highShelfPostL.processSample (yL);
            wetL[i] = plugin::sanitize (yL);

            if (wetR != nullptr)
            {
                float yR = wetR[i];
                yR = lowShelfPostR.processSample (yR);
                yR = highShelfPostR.processSample (yR);
                wetR[i] = plugin::sanitize (yR);
            }
        }

        // -------------------------------------------------------------------------
        // 3) Mix + AutoGain (por muestra) + latency padding fijo
        for (int i = 0; i < block; ++i)
        {
            const float mix01 = mixSm.process (mixTarget);

            // ✅ DRY retrasado/alineado con oversampling (latencia REAL del modo activo)
            const int wp = dryDelayWritePos;
            int rp = wp - dryDelaySamples;
            if (rp < 0) rp += drySize;

            const float inL = ch0[offset + i];
            const float inR = (ch1 != nullptr) ? ch1[offset + i] : inL;

            dL[wp] = inL;
            if (dR != nullptr) dR[wp] = inR;

            const float dryL = dL[rp];
            const float dryR = (dR != nullptr) ? dR[rp] : dryL;

            dryDelayWritePos = wp + 1;
            if (dryDelayWritePos >= drySize) dryDelayWritePos = 0;

            const float wetOutL = wetL[i];
            const float wetOutR = (wetR != nullptr) ? wetR[i] : wetOutL;

            float outLpre = dryL;
            float outRpre = dryR;

            if (mix01 > 1.0e-4f)
            {
                outLpre = plugin::equalPowerMix (dryL, wetOutL, mix01);
                outRpre = plugin::equalPowerMix (dryR, wetOutR, mix01);
            }

            const float g = autoGain.processStereo (dryL, dryR, outLpre, outRpre);

            float outL = plugin::softClipSafety (outLpre * g);
            float outR = plugin::softClipSafety (outRpre * g);

            outL = plugin::sanitize (outL);
            outR = plugin::sanitize (outR);

            // Latency padding (para mantener latency fija = maxLatencySamples)
            const int owp = outputDelayWritePos;
            int orp = owp - outputPadSamples;
            if (orp < 0) orp += outSize;

            oL[owp] = outL;
            if (oR != nullptr) oR[owp] = outR;

            const float delayedL = oL[orp];
            const float delayedR = (oR != nullptr) ? oR[orp] : delayedL;

            outputDelayWritePos = owp + 1;
            if (outputDelayWritePos >= outSize) outputDelayWritePos = 0;

            ch0[offset + i] = delayedL;
            if (ch1 != nullptr)
                ch1[offset + i] = delayedR;
        }

        offset += block;
    }
}
//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YourPluginAudioProcessor();
}

