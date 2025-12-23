#include "PluginProcessor.h"
#include <cstring> // std::memset
#include <mutex>   // std::once_flag, std::call_once

//------------------------------------------------------------------------------
// Si CMake no define PLUGIN_HAS_ASSETS por alguna razón, no rompas el build:
#ifndef PLUGIN_HAS_ASSETS
 #define PLUGIN_HAS_ASSETS 0
#endif

#if PLUGIN_HAS_ASSETS
 #include "BinaryData.h"
#endif

//------------------------------------------------------------------------------
// Fuente embebida en assets/ (TTF/OTF) -> Typeface global (sin setTypefacePtr)
#ifndef PLUGIN_HAS_FONT
 #define PLUGIN_HAS_FONT 0
#endif

#if PLUGIN_HAS_ASSETS && PLUGIN_HAS_FONT
 #ifndef PLUGIN_PRIMARY_FONT_FILENAME
  #define PLUGIN_PRIMARY_FONT_FILENAME ""
 #endif
#endif

namespace plugin { namespace ui {

static juce::StringArray makeBinaryDataNameCandidates (juce::String fileName)
{
    // fileName: solo nombre (ej. "MiFuente.ttf")
    fileName = fileName.trim();

    auto sanitize = [] (juce::String s)
    {
        s = s.replaceCharacter ('.', '_')
             .replaceCharacter ('-', '_')
             .replaceCharacter (' ', '_');

        juce::String out;
        out.preallocateBytes ((size_t) s.getNumBytesAsUTF8());

        for (auto c : s)
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '_')
                out += juce::String::charToString ((juce::juce_wchar) c);
            else
                out += "_";
        }

        // JUCE a veces prefija '_' si el nombre no puede ser un identificador C++
        if (out.isNotEmpty() && ! (juce::CharacterFunctions::isLetter (out[0]) || out[0] == '_'))
            out = "_" + out;

        return out;
    };

    juce::StringArray cands;
    const auto base = sanitize (fileName);

    // variantes comunes (case + prefijos)
    cands.addIfNotAlreadyThere (base);
    cands.addIfNotAlreadyThere (base.toLowerCase());
    cands.addIfNotAlreadyThere (base.toUpperCase());

    cands.addIfNotAlreadyThere ("_" + base);
    cands.addIfNotAlreadyThere ("_" + base.toLowerCase());

    cands.addIfNotAlreadyThere ("f_" + base);
    cands.addIfNotAlreadyThere ("f_" + base.toLowerCase());

    return cands;
}

static juce::Typeface::Ptr getEmbeddedPluginTypeface()
{
    static juce::Typeface::Ptr tf;
    static bool tried = false;

    if (tried)
        return tf;

    tried = true;

   #if PLUGIN_HAS_ASSETS && PLUGIN_HAS_FONT
    const juce::String fontFile (PLUGIN_PRIMARY_FONT_FILENAME);

    if (fontFile.isNotEmpty())
    {
        const auto candidates = makeBinaryDataNameCandidates (fontFile);

        for (auto name : candidates)
        {
            int dataSize = 0;
            if (auto* data = BinaryData::getNamedResource (name.toRawUTF8(), dataSize))
            {
                tf = juce::Typeface::createSystemTypefaceFor (data, (size_t) dataSize);
                break;
            }
        }
    }
   #endif

    return tf;
}

}} // namespace plugin::ui


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

    // Oversampling selector (x1/x2/x4/x8). Default x8 para conservar el comportamiento anterior.
    juce::StringArray osChoices;
    osChoices.add ("x1");
    osChoices.add ("x2");
    osChoices.add ("x4");
    osChoices.add ("x8");

    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        "os", "Oversampling",
        osChoices,
        3));

    return { params.begin(), params.end() };
}

namespace
{
//------------------------------------------------------------------------------
// Per-knob LookAndFeel: usa colours del Slider para (vacío/lleno)
struct KnobLNF : public juce::LookAndFeel_V4
{
public:
    KnobLNF()
    {
        // Carga Typeface embebida (si existe).
        typeface = plugin::ui::getEmbeddedPluginTypeface();

        // ComboBox (recuadro)
        setColour (juce::ComboBox::backgroundColourId, juce::Colours::black);
        setColour (juce::ComboBox::textColourId,       juce::Colours::white);
        setColour (juce::ComboBox::arrowColourId,      juce::Colours::white);
        setColour (juce::ComboBox::outlineColourId,    juce::Colours::black);

        // PopupMenu (menú desplegable)
        setColour (juce::PopupMenu::backgroundColourId, juce::Colours::black);
        setColour (juce::PopupMenu::textColourId,       juce::Colours::white);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour::fromRGB (20, 20, 20));
        setColour (juce::PopupMenu::highlightedTextColourId,       juce::Colours::white);
    }

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& f) override
    {
        if (typeface != nullptr)
            return typeface;
        return juce::LookAndFeel_V4::getTypefaceForFont (f);
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        auto f = juce::LookAndFeel_V4::getComboBoxFont (box);
        f.setHeight (juce::jmax (6.0f, f.getHeight() * 0.5f));
        return f;
    }

    juce::Font getPopupMenuFont() override
    {
        auto f = juce::LookAndFeel_V4::getPopupMenuFont();
        f.setHeight (juce::jmax (6.0f, f.getHeight() * 0.5f));
        return f;
    }

    void drawComboBox (juce::Graphics& g, int width, int height,
                       bool /*isButtonDown*/,
                       int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                       juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
        const float corner = 4.0f;

        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, corner);

        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);

        // flecha
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        auto arrowArea = r.removeFromRight (22.0f).reduced (6.0f, 6.0f);

        juce::Path p;
        p.addTriangle (arrowArea.getCentreX() - 4.0f, arrowArea.getCentreY() - 2.0f,
                       arrowArea.getCentreX() + 4.0f, arrowArea.getCentreY() - 2.0f,
                       arrowArea.getCentreX(),        arrowArea.getCentreY() + 4.0f);
        g.fillPath (p);
    }

    void drawPopupMenuBackground (juce::Graphics& g, int /*width*/, int /*height*/) override
    {
        g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    }

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

private:
    juce::Typeface::Ptr typeface;
};

//------------------------------------------------------------------------------
// Animated header from sprite sheet (assets/header_sheet.png)
// Soporta:
// - tira 1xN (default)
// - grilla CxR (ej. 9x5 = 45 frames)
class AnimatedHeader final : public juce::Component,
                             private juce::Timer
{
public:
    // totalFrames: cantidad total de frames (ej. 45)
    // fps: frames por segundo
    // columns: si es 0 -> asume tira 1xN (columns = totalFrames, rows = 1)
    // rows: si es 0 -> calcula rows = ceil(totalFrames / columns)
    void setSpriteSheet (juce::Image sheetImage, int totalFrames, int fps, int columns = 0, int rows = 0)
    {
        sheet = sheetImage;
        numFrames = juce::jmax (1, totalFrames);
        frameIndex = 0;

        if (! sheet.isValid() || sheet.getWidth() <= 0 || sheet.getHeight() <= 0)
        {
            stopTimer();
            return;
        }

        // Default: tira horizontal 1xN
        cols = (columns > 0 ? columns : numFrames);
        cols = juce::jmax (1, cols);

        // Auto rows si no lo pasan
        if (rows > 0)
            rowsCount = rows;
        else
            rowsCount = (numFrames + cols - 1) / cols; // ceil

        rowsCount = juce::jmax (1, rowsCount);

        frameW = juce::jmax (1, sheet.getWidth()  / cols);
        frameH = juce::jmax (1, sheet.getHeight() / rowsCount);

        startTimerHz (juce::jmax (1, fps));
        repaint();
    }

    void stop() { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        if (! sheet.isValid() || numFrames <= 0 || frameW <= 0 || frameH <= 0 || cols <= 0 || rowsCount <= 0)
            return;

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

        const int col = frameIndex % cols;
        const int row = frameIndex / cols;

        // Si el sheet tiene más celdas que frames (grilla incompleta), corta seguro
        if (row >= rowsCount)
            return;

        const int sx = col * frameW;
        const int sy = row * frameH;

        auto b = getLocalBounds().toFloat();
        const float ar = (float) frameW / (float) frameH;

        // contain dentro del componente (permitiendo agrandar/reducir)
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

    int cols      = 1;
    int rowsCount = 1;

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

        // Fuente embebida: aplica a texto de labels (y otros componentes que usen este L&F)
        driveKnob.label.setLookAndFeel (&knobLNF);
        toneKnob .label.setLookAndFeel (&knobLNF);
        mixKnob  .label.setLookAndFeel (&knobLNF);

        addAndMakeVisible (driveKnob);
        addAndMakeVisible (toneKnob);
        addAndMakeVisible (mixKnob);

        addAndMakeVisible (header);

       #if PLUGIN_HAS_ASSETS
        // header_sheet.png -> sprite sheet animado (45 frames)
        {
            const juce::String wantedFile = "header_sheet.png";
            const auto candidates = plugin::ui::makeBinaryDataNameCandidates (wantedFile);

            const void* data = nullptr;
            int dataSize = 0;

            for (auto name : candidates)
            {
                if ((data = BinaryData::getNamedResource (name.toRawUTF8(), dataSize)) != nullptr)
                    break;
            }

            if (data != nullptr && dataSize > 0)
            {
                auto sheet = juce::ImageCache::getFromMemory (data, dataSize);

                // ✅ Caso típico: grilla 9x5 = 45
                // Si tu sheet fuese tira 1x45, cambia a: header.setSpriteSheet (sheet, 45, 3);
                header.setSpriteSheet (sheet, /*totalFrames*/ 45, /*fps*/ 3, /*columns*/ 9);
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

        // Fuente embebida: ComboBox + menú desplegable usan el LookAndFeel del ComboBox
        preampBox.setLookAndFeel (&knobLNF);

        int itemId = 1;
        for (const auto& it : PresetRegistry::items)
            preampBox.addItem (it.displayName, itemId++);

        if (preampBox.getNumItems() == 0)
            preampBox.addItem ("(none)", 1);

        addAndMakeVisible (preampBox);

        // Oversampling selector (para ahorrar recursos en proyectos no-mix)
        osBox.setJustificationType (juce::Justification::centredLeft);
        osBox.setLookAndFeel (&knobLNF);
        osBox.addItem ("x1", 1);
        osBox.addItem ("x2", 2);
        osBox.addItem ("x4", 3);
        osBox.addItem ("x8", 4);
        addAndMakeVisible (osBox);

        using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

        driveAtt  = std::make_unique<SliderAttachment>   (processor.apvts, "drive",  driveKnob.slider);
        toneAtt   = std::make_unique<SliderAttachment>   (processor.apvts, "tone",   toneKnob.slider);
        mixAtt    = std::make_unique<SliderAttachment>   (processor.apvts, "mix",    mixKnob.slider);
        preampAtt = std::make_unique<ComboBoxAttachment> (processor.apvts, "preamp", preampBox);
        osAtt     = std::make_unique<ComboBoxAttachment> (processor.apvts, "os",     osBox);

        // ✅ UI más grande
        setSize (820, 460);
    }

    ~MinimalEditor() override
    {
        header.stop();

        // Limpieza L&F (importante para evitar dangling pointers)
        preampBox.setLookAndFeel (nullptr);
        osBox.setLookAndFeel (nullptr);
        driveKnob.label.setLookAndFeel (nullptr);
        toneKnob .label.setLookAndFeel (nullptr);
        mixKnob  .label.setLookAndFeel (nullptr);

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

        // --- Bottom row: [Oversampling] [Preamp] ---
        bottom.reduce (0, 10);

        auto osArea = bottom.removeFromLeft (juce::jmax (120, bottom.getWidth() / 3));
        osBox.setBounds (osArea.reduced (0, 12));
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
    juce::ComboBox osBox;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> driveAtt, toneAtt, mixAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> preampAtt, osAtt;

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
    pOS     = apvts.getRawParameterValue ("os");

#if PLUGIN_HAS_ASSETS && PLUGIN_HAS_FONT
    // Aplica una sola vez la fuente embebida como Sans Serif por defecto.
    // Esto afecta a menús/listas/popup menus y textos que usen el LookAndFeel default.
    static std::once_flag sFontOnce;
    std::call_once (sFontOnce, []()
    {
        if (auto tf = plugin::ui::getEmbeddedPluginTypeface())
            juce::Desktop::getInstance().getDefaultLookAndFeel().setDefaultSansSerifTypeface (tf);
    });
#endif
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
    // Tone "robusto" tipo tilt:
    // - centro exacto (0.5) = neutro
    // - derecha = oscuro, izquierda = brillante
    // - curva + rango asimétrico (ver plugin::mapToneTiltDb)
    const float t = juce::jlimit (0.0f, 1.0f, tone01);
    const float tiltDb = plugin::mapToneTiltDb (t);

    // Hacemos el tilt más estable/musical variando un poquito los puntos de shelf
    // según cuánto te alejes del centro. Esto evita que se sienta como dos shelves
    // rígidos y ayuda a que el control sea "piola" en cualquier preset.
    const float mag01 = juce::jlimit (0.0f, 1.0f, std::abs (tiltDb) / 9.0f);

    // Valores base (calibrados para que no suene nasal/harsh)
    const float fcLowBase  = 240.0f;
    const float fcHighBase = 3200.0f;

    // Con más tilt, acercamos un poco el high shelf hacia el medio (dark) o lo
    // alejamos (bright). Low shelf también se mueve levemente para mantener pivot.
    float fcLow  = fcLowBase;
    float fcHigh = fcHighBase;

    if (tiltDb < 0.0f) // dark
    {
        fcHigh = fcHighBase * (1.0f - 0.35f * mag01); // baja hasta ~65%
        fcLow  = fcLowBase  * (1.0f + 0.20f * mag01); // sube levemente
    }
    else if (tiltDb > 0.0f) // bright
    {
        fcHigh = fcHighBase * (1.0f + 0.55f * mag01); // sube hasta ~155%
        fcLow  = fcLowBase  * (1.0f - 0.15f * mag01); // baja levemente
    }

    fcLow  = juce::jlimit (90.0f,  650.0f, fcLow);
    fcHigh = juce::jlimit (1200.0f, 8000.0f, fcHigh);

    constexpr float q = 0.707f;
    auto low  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (
        sr, fcLow, q, juce::Decibels::decibelsToGain (-tiltDb));
    auto high = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
        sr, fcHigh, q, juce::Decibels::decibelsToGain ( tiltDb));

    *lowShelfL.coefficients  = *low;
    *lowShelfR.coefficients  = *low;
    *highShelfL.coefficients = *high;
    *highShelfR.coefficients = *high;
}

//==============================================================================
// Prepare

int YourPluginAudioProcessor::getDesiredOversamplingIndex() const noexcept
{
    if (pOS == nullptr)
        return kMaxOversamplingIndex;

    const int idx = (int) std::lround ((double) pOS->load());
    return juce::jlimit (0, kMaxOversamplingIndex, idx);
}

void YourPluginAudioProcessor::ensureOversamplers (int numChannels)
{
    const int ch = juce::jmax (1, juce::jmin (2, numChannels));

    // Ya existen y coinciden
    if (ch == oversamplerChannels && oversamplers[1] != nullptr)
        return;

    oversamplerChannels = ch;

    // Recrear oversamplers para este layout de canales (evita accesos fuera de rango)
    for (int i = 1; i <= kMaxOversamplingIndex; ++i)
    {
        oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
            (size_t) oversamplerChannels,
            i,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
            true /* max quality */);

        oversamplers[i]->reset();
    }

    oversamplers[0].reset();
    initOversamplers (maxBlockSizePrepared);
}

void YourPluginAudioProcessor::initOversamplers (int maxBlockSize)
{
    const size_t bs = (size_t) juce::jmax (1, maxBlockSize);

    for (int i = 1; i <= kMaxOversamplingIndex; ++i)
        if (oversamplers[i] != nullptr)
            oversamplers[i]->initProcessing (bs);
}

void YourPluginAudioProcessor::applyOversamplingIndex (int newIndex, bool force)
{
    const int idx = juce::jlimit (0, kMaxOversamplingIndex, newIndex);

    if (! force && idx == currentOSIndex)
        return;

    currentOSIndex = idx;
    oversampling = (idx == 0 ? nullptr : oversamplers[idx].get());

    if (oversampling != nullptr)
    {
        // Evita heredar estados del filtro anterior al cambiar de modo
        oversampling->reset();
        oversampling->initProcessing ((size_t) juce::jmax (1, maxBlockSizePrepared));
    }

    const int latency = (oversampling != nullptr) ? (int) oversampling->getLatencyInSamples() : 0;
    setLatencySamples (latency);

    dryDelaySamples  = latency;
    dryDelayWritePos = 0;

    const int wetCh = juce::jmax (1, juce::jmin (2, oversamplerChannels));
    dryDelayBufferSize = juce::jmax (1, maxBlockSizePrepared + dryDelaySamples + 1);
    dryDelayBuffer.setSize (wetCh, dryDelayBufferSize, false, false, true);
    dryDelayBuffer.clear();

    const float osFactor = (idx == 0 ? 1.0f : (float) (1u << idx));
    osSr = (float) (sr * (double) osFactor);

    if (activePreset != nullptr)
    {
        for (int ch = 0; ch < wetCh; ++ch)
        {
            if (! presetStateConstructed[(size_t) ch])
                continue;

            void* st = (void*) &presetState[(size_t) ch];
            if (activePreset->prepare) activePreset->prepare (st, osSr);
            if (activePreset->reset)   activePreset->reset   (st);
        }
    }
}

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

    // ✅ AutoGain por bloque (entrada alineada vs salida real) para volumen constante
    autoGain.prepare (sr);

    // Guardar block size inicial (si el host luego usa uno mayor, creceremos buffers/OS)
    maxBlockSizePrepared = juce::jmax (1, samplesPerBlock);

    // Canales reales (mono/stereo) para respetar distribución original del input
    const int wetCh = juce::jmax (1, juce::jmin (2, getTotalNumInputChannels()));

    // Oversampling (seleccionable en runtime)
    // Nota: pre-creamos oversamplers x2/x4/x8 para evitar reallocs al cambiar el modo.
    ensureOversamplers (wetCh);
    initOversamplers (maxBlockSizePrepared);

    // buffers
    wetBuffer.setSize (wetCh, maxBlockSizePrepared, false, false, true);

    // ---------------------------------------------------------------------
    // Preset state lifecycle
    //
    // prepareToPlay() puede llamarse múltiples veces. Si ya teníamos un preset
    // activo, destruimos los estados construidos para evitar UB.
    if (activePreset != nullptr && activePreset->destruct != nullptr)
    {
        for (int ch = 0; ch < wetCh; ++ch)
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
    applyOversamplingIndex (getDesiredOversamplingIndex(), true);

    // delay para DRY alineado con latencia de oversampling
    dryDelayBufferSize = juce::jmax (1, maxBlockSizePrepared + dryDelaySamples + 1);
    dryDelayBuffer.setSize (wetCh, dryDelayBufferSize, false, false, true);
    dryDelayBuffer.clear();
    dryDelayWritePos = 0;

    int preampIndex = 0;
    if (pPreamp != nullptr && PresetRegistry::items.size() > 0)
        preampIndex = juce::jlimit (0, (int) PresetRegistry::items.size() - 1,
                                    (int) std::lround (*pPreamp));

    if (PresetRegistry::items.size() > 0)
    {
        activePresetIndex = preampIndex;
        activePreset = &PresetRegistry::items[(size_t) preampIndex];

        for (int ch = 0; ch < wetCh; ++ch)
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

    const int wetCh = juce::jmax (1, juce::jmin (2, numCh));

    // Si el host cambia a mono/estéreo, recrea oversamplers para evitar accesos inválidos.
    if (wetCh != oversamplerChannels)
    {
        ensureOversamplers (wetCh);
        applyOversamplingIndex (currentOSIndex, true);
    }

    // Aplicar cambio de oversampling (x1/x2/x4/x8) en runtime
    applyOversamplingIndex (getDesiredOversamplingIndex(), false);

    // FL Studio y otros hosts pueden variar el block size. Si crece,
    // re-inicializamos oversampling y buffers para evitar desalineaciones.
    if (numSamples > maxBlockSizePrepared)
    {
        maxBlockSizePrepared = numSamples;
        initOversamplers (maxBlockSizePrepared);

        // crecer wetBuffer (pre OS)
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
        // Destruir estado anterior antes de cambiar de preset
        if (activePreset != nullptr && activePreset->destruct != nullptr)
        {
            for (int ch = 0; ch < wetCh; ++ch)
            {
                if (presetStateConstructed[(size_t) ch])
                    activePreset->destruct ((void*) &presetState[(size_t) ch]);
            }
        }

        presetStateConstructed = {{ false, false }};

        activePresetIndex = preampIndex;
        activePreset = &PresetRegistry::items[(size_t) preampIndex];

        const float osFactor = (currentOSIndex == 0 ? 1.0f : (float) (1u << currentOSIndex));
        osSr = (float) (sr * (double) osFactor);

        for (int ch = 0; ch < wetCh; ++ch)
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

    // Tone smoothing real (por bloque) antes de usar tilt
    toneSm.setTargetValue (*pTone);
    toneSm.skip (numSamples);
    updateTiltCoeffs (toneSm.getCurrentValue());

    // Asegura wetBuffer sin realocar cada bloque
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
    // 2) Oversampling (opcional) -> preset PRO (stateful) -> (downsample)
    //    ESTÉREO "DUAL-MONO": cada lado independiente (sin cross-talk L<->R)
    if (activePreset != nullptr && activePreset->process != nullptr)
    {
        if (oversampling != nullptr)
        {
            juce::dsp::AudioBlock<float> baseBlock (wetBuffer);

            // procesamos SOLO numSamples (aunque wetBuffer sea más grande)
            baseBlock = baseBlock.getSubBlock (0, (size_t) numSamples);

            auto osBlock = oversampling->processSamplesUp (baseBlock);

            const size_t osSamples = osBlock.getNumSamples();
            const size_t osCh = osBlock.getNumChannels();

            if (osCh == 2)
            {
                float* L = osBlock.getChannelPointer (0);
                float* R = osBlock.getChannelPointer (1);

                void* stL = (void*) &presetState[0];
                void* stR = (void*) &presetState[1];

                for (size_t n = 0; n < osSamples; ++n)
                {
                    const float xL = L[n];
                    const float xR = R[n];

                    L[n] = activePreset->process (stL, xL);
                    R[n] = activePreset->process (stR, xR);
                }
            }
            else
            {
                // Mono
                for (size_t ch = 0; ch < osCh; ++ch)
                {
                    float* data = osBlock.getChannelPointer (ch);
                    void* st = (void*) &presetState[0];

                    for (size_t n = 0; n < osSamples; ++n)
                        data[n] = activePreset->process (st, data[n]);
                }
            }

            oversampling->processSamplesDown (baseBlock);
        }
        else
        {
            // x1: sin oversampling (ahorra CPU) - DUAL MONO
            if (wetCh >= 2 && wetR != nullptr)
            {
                void* stL = (void*) &presetState[0];
                void* stR = (void*) &presetState[1];

                for (int i = 0; i < numSamples; ++i)
                {
                    wetL[i] = activePreset->process (stL, wetL[i]);
                    wetR[i] = activePreset->process (stR, wetR[i]);
                }
            }
            else
            {
                void* st = (void*) &presetState[0];
                for (int i = 0; i < numSamples; ++i)
                    wetL[i] = activePreset->process (st, wetL[i]);
            }
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
        const float mix01 = mixSm.getNextValue();

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


