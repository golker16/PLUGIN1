#include "PluginProcessor.h"
#include <cstring> // std::memset, std::memcpy
#include <mutex>   // std::once_flag, std::call_once
#include <limits>  // std::numeric_limits

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

    // --- variantes comunes cuando CMake/JUCE incluye el path/carpeta en el nombre ---
    // ej: assets/header_sheet.png  -> assets_header_sheet_png
    const auto assetsSlash = sanitize ("assets/" + fileName);
    const auto assetsUnd   = sanitize ("assets_" + fileName);

    cands.addIfNotAlreadyThere (assetsSlash);
    cands.addIfNotAlreadyThere (assetsSlash.toLowerCase());
    cands.addIfNotAlreadyThere ("_" + assetsSlash);

    cands.addIfNotAlreadyThere (assetsUnd);
    cands.addIfNotAlreadyThere (assetsUnd.toLowerCase());
    cands.addIfNotAlreadyThere ("_" + assetsUnd);

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

#if PLUGIN_HAS_ASSETS

static bool loadImageFromBinaryDataByFilename (const juce::String& wantedFile, juce::Image& outImage)
{
    outImage = {};

    const void* data = nullptr;
    int dataSize = 0;

    // 1) intento por candidatos (incluye assets_ / assets/ por el cambio anterior)
    {
        const auto candidates = makeBinaryDataNameCandidates (wantedFile);
        for (auto name : candidates)
        {
            if ((data = BinaryData::getNamedResource (name.toRawUTF8(), dataSize)) != nullptr)
                break;
        }
    }

    // 2) fallback: escanear lista real de recursos embebidos (evita “no coincide el nombre”)
    if (data == nullptr || dataSize <= 0)
    {
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            const juce::String resName (BinaryData::namedResourceList[i]);

            // buscamos algo que contenga "header_sheet" y sea png
            if (resName.containsIgnoreCase ("header_sheet") && resName.endsWithIgnoreCase ("_png"))
            {
                int sz = 0;
                if (auto* d = BinaryData::getNamedResource (resName.toRawUTF8(), sz))
                {
                    data = d;
                    dataSize = sz;
                    break;
                }
            }
        }
    }

    if (data != nullptr && dataSize > 0)
    {
        outImage = juce::ImageCache::getFromMemory (data, dataSize);
        return outImage.isValid();
    }

   #if JUCE_DEBUG
    JUCE_DBG ("[HEADER] No se encontró recurso para '" + wantedFile + "'");
   #endif
    return false;
}

// ✅ 1) CAMBIO: inferSpriteGrid reemplazado por versión robusta con aspectPenalty
static std::pair<int, int> inferSpriteGrid (const juce::Image& sheet, int totalFrames)
{
    const int W = sheet.getWidth();
    const int H = sheet.getHeight();

    auto scoreGrid = [&] (int cols, int rows) -> long long
    {
        cols = juce::jmax (1, cols);
        rows = juce::jmax (1, rows);

        const int fwInt = W / cols;
        const int fhInt = H / rows;

        if (fwInt <= 0 || fhInt <= 0)
            return std::numeric_limits<long long>::max();

        const int rem = (W % cols) + (H % rows); // 0 = divisiones perfectas

        const float fw = (float) W / (float) cols;
        const float fh = (float) H / (float) rows;
        const float ar = (fh > 0.0001f ? fw / fh : 1.0f);

        // Area grande preferida
        const long long area = (long long) (fw * fh);

        // Penaliza aspectos “absurdos”
        long long aspectPenalty = 0;
        if (ar < 0.25f)  aspectPenalty += (long long) ((0.25f - ar) * 1.0e12f);
        if (ar > 40.0f)  aspectPenalty += (long long) ((ar - 40.0f) * 1.0e10f);

        return (long long) rem * 1000000LL - area + aspectPenalty;
    };

    long long bestScore = std::numeric_limits<long long>::max();
    int bestCols = 1;
    int bestRows = juce::jmax (1, totalFrames);

    for (int cols = 1; cols <= totalFrames; ++cols)
    {
        const int rows = (totalFrames + cols - 1) / cols; // ceil
        const long long s = scoreGrid (cols, rows);

        if (s < bestScore)
        {
            bestScore = s;
            bestCols = cols;
            bestRows = rows;
        }
    }

    return { bestCols, bestRows };
}

#endif // PLUGIN_HAS_ASSETS

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
        typeface = plugin::ui::getEmbeddedPluginTypeface();

        setColour (juce::ComboBox::backgroundColourId, juce::Colours::black);
        setColour (juce::ComboBox::textColourId,       juce::Colours::white);
        setColour (juce::ComboBox::arrowColourId,      juce::Colours::white);
        setColour (juce::ComboBox::outlineColourId,    juce::Colours::black);

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
        auto outer = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height)
                       .reduced (6.0f);

        const float baseRadius = juce::jmin (outer.getWidth(), outer.getHeight()) * 0.5f;

        auto bounds = outer.withSizeKeepingCentre (outer.getWidth() * 0.5f,
                                                   outer.getHeight() * 0.5f);

        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto  centre = bounds.getCentre();

        const float angle = rotaryStartAngle
                          + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        const auto emptyCol = slider.findColour (juce::Slider::rotarySliderOutlineColourId);
        const auto fillCol  = slider.findColour (juce::Slider::rotarySliderFillColourId);

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
    }

private:
    juce::Typeface::Ptr typeface;
};

//------------------------------------------------------------------------------
// Animated header from sprite sheet (assets/header_sheet.png)
class AnimatedHeader final : public juce::Component,
                             private juce::Timer
{
public:
    void setSpriteSheet (juce::Image sheetImage, int totalFrames, int fps, int columns = 0, int rows = 0)
    {
        sheet = sheetImage;
        numFrames  = juce::jmax (1, totalFrames);
        frameIndex = 0;

        frameWf = 0.0f;
        frameHf = 0.0f;
        frameAspect = 1.0f;

        if (! sheet.isValid() || sheet.getWidth() <= 0 || sheet.getHeight() <= 0)
        {
            stopTimer();
            repaint();
            return;
        }

        cols = (columns > 0 ? columns : numFrames);
        cols = juce::jmax (1, cols);

        if (rows > 0)
            rowsCount = rows;
        else
            rowsCount = (numFrames + cols - 1) / cols;

        rowsCount = juce::jmax (1, rowsCount);

        frameWf = (float) sheet.getWidth()  / (float) cols;
        frameHf = (float) sheet.getHeight() / (float) rowsCount;

        frameWf = juce::jmax (1.0f, frameWf);
        frameHf = juce::jmax (1.0f, frameHf);

        frameAspect = frameWf / frameHf;

        startTimerHz (juce::jmax (1, fps));
        repaint();
    }

    void stop() { stopTimer(); }

    float getFrameAspect() const noexcept { return frameAspect; }

    void paint (juce::Graphics& g) override
    {
        if (! sheet.isValid() || numFrames <= 0 || frameWf <= 0.0f || frameHf <= 0.0f || cols <= 0 || rowsCount <= 0)
            return;

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

        const int col = frameIndex % cols;
        const int row = frameIndex / cols;

        if (row >= rowsCount)
            return;

        float sx = (float) col * frameWf;
        float sy = (float) row * frameHf;

        auto b = getLocalBounds().toFloat();
        const float ar = (frameAspect > 0.0001f ? frameAspect : 1.0f);

        float dw = b.getWidth();
        float dh = dw / ar;

        if (dh > b.getHeight())
        {
            dh = b.getHeight();
            dw = dh * ar;
        }

        const float dx = b.getX() + (b.getWidth()  - dw) * 0.5f;
        const float dy = b.getY() + (b.getHeight() - dh) * 0.5f;

        const float bleed = juce::jmin (bleedFixPx, 0.25f * juce::jmin (frameWf, frameHf));
        const float sw    = juce::jmax (1.0f, frameWf - 2.0f * bleed);
        const float sh    = juce::jmax (1.0f, frameHf - 2.0f * bleed);

        g.drawImage (sheet,
                     dx, dy, dw, dh,
                     sx + bleed, sy + bleed, sw, sh);
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

    float frameWf      = 0.0f;
    float frameHf      = 0.0f;
    float frameAspect  = 1.0f;

    float bleedFixPx   = 0.5f;
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
        driveKnob.slider.setLookAndFeel (&knobLNF);
        toneKnob .slider.setLookAndFeel (&knobLNF);
        mixKnob  .slider.setLookAndFeel (&knobLNF);

        driveKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FF006700"));
        driveKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FF65FF65"));

        toneKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FFF9FF34"));
        toneKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FFCC66FF"));

        mixKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FF555555"));
        mixKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FFFFFFFF"));

        driveKnob.label.setFont (juce::Font (11.0f));
        toneKnob .label.setFont (juce::Font (11.0f));
        mixKnob  .label.setFont (juce::Font (11.0f));

        driveKnob.label.setLookAndFeel (&knobLNF);
        toneKnob .label.setLookAndFeel (&knobLNF);
        mixKnob  .label.setLookAndFeel (&knobLNF);

        addAndMakeVisible (driveKnob);
        addAndMakeVisible (toneKnob);
        addAndMakeVisible (mixKnob);

        addAndMakeVisible (header);

       #if PLUGIN_HAS_ASSETS
        {
            juce::Image sheet;

            if (plugin::ui::loadImageFromBinaryDataByFilename ("header_sheet.png", sheet))
            {
                const int totalFrames = 45;
                const int cols = 1;
                const int rows = totalFrames;

                header.setSpriteSheet (sheet, totalFrames, 3, cols, rows);
            }
        }
       #endif

       #if PLUGIN_HAS_ASSETS
        driveKnob.setLabelImage (juce::ImageCache::getFromMemory (BinaryData::drive_png, BinaryData::drive_pngSize));
        toneKnob .setLabelImage (juce::ImageCache::getFromMemory (BinaryData::tone_png,  BinaryData::tone_pngSize));
        mixKnob  .setLabelImage (juce::ImageCache::getFromMemory (BinaryData::mix_png,   BinaryData::mix_pngSize));

        driveKnob.setLabelSlotHeights (12, 14);
        mixKnob  .setLabelSlotHeights (12, 14);
        toneKnob .setLabelSlotHeights (20, 14);
       #endif

        preampBox.setJustificationType (juce::Justification::centredLeft);
        preampBox.setLookAndFeel (&knobLNF);

        int itemId = 1;
        for (const auto& it : PresetRegistry::items)
            preampBox.addItem (it.displayName, itemId++);

        if (preampBox.getNumItems() == 0)
            preampBox.addItem ("(none)", 1);

        addAndMakeVisible (preampBox);

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

        setSize (820, 460);
    }

    ~MinimalEditor() override
    {
        header.stop();

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

        int headerH = 72;
        const float ar = header.getFrameAspect();
        if (ar > 0.01f)
        {
            headerH = juce::roundToInt ((float) area.getWidth() / ar);
            headerH = juce::jlimit (56, 160, headerH);
        }

        auto headerArea = area.removeFromTop (headerH).reduced (6, 6);
        header.setBounds (headerArea);

        area.removeFromTop (16);

        auto bottom = area.removeFromBottom (74);

        const int knobW = 90;
        const int knobH = 104;
        const int gap   = 8;

        const int startX = area.getX();
        const int y      = area.getY();

        driveKnob.setBounds (startX,                         y, knobW, knobH);
        toneKnob .setBounds (startX + knobW + gap,           y, knobW, knobH);
        mixKnob  .setBounds (startX + (knobW + gap) * 2,     y, knobW, knobH);

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
    const float t = juce::jlimit (0.0f, 1.0f, tone01);
    const float tiltDb = plugin::mapToneTiltDb (t);

    const float mag01 = juce::jlimit (0.0f, 1.0f, std::abs (tiltDb) / 9.0f);

    const float fcLowBase  = 240.0f;
    const float fcHighBase = 3200.0f;

    float fcLow  = fcLowBase;
    float fcHigh = fcHighBase;

    if (tiltDb < 0.0f) // dark
    {
        fcHigh = fcHighBase * (1.0f - 0.35f * mag01);
        fcLow  = fcLowBase  * (1.0f + 0.20f * mag01);
    }
    else if (tiltDb > 0.0f) // bright
    {
        fcHigh = fcHighBase * (1.0f + 0.55f * mag01);
        fcLow  = fcLowBase  * (1.0f - 0.15f * mag01);
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

    if (ch == oversamplerChannels && oversamplers[1] != nullptr)
        return;

    oversamplerChannels = ch;

    for (int i = 1; i <= kMaxOversamplingIndex; ++i)
    {
        oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
            (size_t) oversamplerChannels,
            i,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
            true);

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

// ⚠️ IMPORTANTE:
// Este método queda para “setear punteros / latency / buffers”.
// NO debe resetear oversampler en caliente por cambios de OS: ahora el cambio es via crossfade.
// Se usa solo en prepareToPlay() y en casos force (layout changes) donde no hay audio continuo.
void YourPluginAudioProcessor::applyOversamplingIndex (int newIndex, bool force)
{
    const int idx = juce::jlimit (0, kMaxOversamplingIndex, newIndex);

    if (! force && idx == currentOSIndex)
        return;

    currentOSIndex = idx;
    oversampling = (idx == 0 ? nullptr : oversamplers[idx].get());

    if (oversampling != nullptr)
    {
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
}

void YourPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

    driveSm.reset (sr, 0.02);
    toneSm .reset (sr, 0.02);
    mixSm  .reset (sr, 0.02);

    driveSm.setCurrentAndTargetValue (*pDrive);
    toneSm .setCurrentAndTargetValue (*pTone);
    mixSm  .setCurrentAndTargetValue (*pMix);

    lowShelfL.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 180.0f, 0.707f, 1.0f);
    lowShelfR.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 180.0f, 0.707f, 1.0f);
    highShelfL.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3800.0f, 0.707f, 1.0f);
    highShelfR.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3800.0f, 0.707f, 1.0f);

    updateTiltCoeffs (*pTone);

    autoGain.prepare (sr);

    maxBlockSizePrepared = juce::jmax (1, samplesPerBlock);

    const int wetCh = juce::jmax (1, juce::jmin (2, getTotalNumInputChannels()));

    ensureOversamplers (wetCh);
    initOversamplers (maxBlockSizePrepared);

    wetBuffer.setSize (wetCh, maxBlockSizePrepared, false, false, true);

    // Buffers crossfade (prealocados al máximo)
    wetOld.setSize (wetCh, maxBlockSizePrepared, false, false, true);
    wetNew.setSize (wetCh, maxBlockSizePrepared, false, false, true);

    // ---------------------------------------------------------------------
    // Destruir bancos A/B correctamente (usando el preset que “sabemos” que tienen):
    auto destructBank = [&] (std::array<PresetStateStorage, 2>& bank,
                             std::array<bool, 2>& flags,
                             int presetIndex)
    {
        if (presetIndex < 0 || presetIndex >= (int) PresetRegistry::items.size())
            return;

        const auto* pr = &PresetRegistry::items[(size_t) presetIndex];
        if (pr == nullptr || pr->destruct == nullptr)
            return;

        for (int ch = 0; ch < wetCh; ++ch)
        {
            if (flags[(size_t) ch])
                pr->destruct ((void*) &bank[(size_t) ch]);
        }
    };

    // activePresetIndex = banco activo (inicialmente activoState apunta a A)
    // pendingPresetIndex (FUERA de transición) lo usamos para “qué preset vive en el banco INACTIVO”
    destructBank (presetStateA, presetStateConstructedA, activePresetIndex);
    destructBank (presetStateB, presetStateConstructedB, pendingPresetIndex);

    presetStateConstructedA = {{ false, false }};
    presetStateConstructedB = {{ false, false }};
    for (auto& st : presetStateA) std::memset (&st, 0, sizeof (st));
    for (auto& st : presetStateB) std::memset (&st, 0, sizeof (st));

    // Reset transición
    transitioning = false;
    xfadeTotalSamples = 0;
    xfadePosSamples = 0;

    // ---- PRESET inicial ----
    activePresetIndex = -1;
    activePreset = nullptr;

    // OS inicial (forzado)
    applyOversamplingIndex (getDesiredOversamplingIndex(), true);

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

        // (re)apuntar bancos por defecto
        activeState  = &presetStateA;
        pendingState = &presetStateB;
        activeConstructed  = &presetStateConstructedA;
        pendingConstructed = &presetStateConstructedB;

        // inicializar “meta” del banco inactivo (fuera de transición)
        pendingPresetIndex = activePresetIndex;
        pendingOSIndex     = currentOSIndex;

        const float effectiveSr = (oversampling != nullptr ? osSr : (float) sr);
        stereoA.prepare (effectiveSr);
        stereoB.prepare (effectiveSr);
        stereoA.reset();
        stereoB.reset();

        for (int ch = 0; ch < wetCh; ++ch)
        {
            void* st = (void*) &getActiveStateStorage (ch);
            if (activePreset->construct != nullptr)
            {
                activePreset->construct (st);
                getActiveConstructed (ch) = true;
            }
            if (activePreset->prepare) activePreset->prepare (st, (oversampling != nullptr ? osSr : (float) sr));
            if (activePreset->reset)   activePreset->reset (st);
        }
    }
}

//==============================================================================
// ✅ Crossfade helpers (IMPLEMENTACIÓN)

// Render de cadena A (usa activePreset + active bank + stereoA)
void YourPluginAudioProcessor::renderChainA (juce::AudioBuffer<float>& io, int wetCh, int numSamples)
{
    if (activePreset == nullptr || activePreset->process == nullptr)
        return;

    const int osIdx = currentOSIndex;
    auto* osPtr = (osIdx == 0 ? nullptr : oversamplers[osIdx].get());

    if (osPtr != nullptr)
    {
        juce::dsp::AudioBlock<float> baseBlock (io);
        baseBlock = baseBlock.getSubBlock (0, (size_t) numSamples);

        auto osBlock = osPtr->processSamplesUp (baseBlock);
        const size_t osSamples = osBlock.getNumSamples();
        const size_t osCh = osBlock.getNumChannels();

        if (osCh >= 2 && wetCh >= 2)
        {
            float* L = osBlock.getChannelPointer (0);
            float* R = osBlock.getChannelPointer (1);

            void* stL = (void*) &getActiveStateStorage (0);
            void* stR = (void*) &getActiveStateStorage (1);

            for (size_t n = 0; n < osSamples; ++n)
            {
                L[n] = activePreset->process (stL, L[n]);
                R[n] = activePreset->process (stR, R[n]);
                stereoA.processSample (L[n], R[n]); // ✅ Stereo vida real ON
            }
        }
        else
        {
            // mono (o layout extraño)
            float* data = osBlock.getChannelPointer (0);
            void* st = (void*) &getActiveStateStorage (0);

            for (size_t n = 0; n < osSamples; ++n)
                data[n] = activePreset->process (st, data[n]);
        }

        osPtr->processSamplesDown (baseBlock);
    }
    else
    {
        // x1
        float* wetL = io.getWritePointer (0);
        float* wetR = (wetCh > 1) ? io.getWritePointer (1) : nullptr;

        if (wetCh >= 2 && wetR != nullptr)
        {
            void* stL = (void*) &getActiveStateStorage (0);
            void* stR = (void*) &getActiveStateStorage (1);

            for (int i = 0; i < numSamples; ++i)
            {
                wetL[i] = activePreset->process (stL, wetL[i]);
                wetR[i] = activePreset->process (stR, wetR[i]);
                stereoA.processSample (wetL[i], wetR[i]); // ✅
            }
        }
        else
        {
            void* st = (void*) &getActiveStateStorage (0);
            for (int i = 0; i < numSamples; ++i)
                wetL[i] = activePreset->process (st, wetL[i]);
        }
    }
}

// Render de cadena B (usa pendingPresetIndex + pending bank + stereoB)
void YourPluginAudioProcessor::renderChainB (juce::AudioBuffer<float>& io, int wetCh, int numSamples)
{
    if (pendingPresetIndex < 0 || pendingPresetIndex >= (int) PresetRegistry::items.size())
        return;

    const auto* pendingPreset = &PresetRegistry::items[(size_t) pendingPresetIndex];
    if (pendingPreset == nullptr || pendingPreset->process == nullptr)
        return;

    const int osIdx = pendingOSIndex;
    auto* osPtr = (osIdx == 0 ? nullptr : oversamplers[osIdx].get());

    if (osPtr != nullptr)
    {
        juce::dsp::AudioBlock<float> baseBlock (io);
        baseBlock = baseBlock.getSubBlock (0, (size_t) numSamples);

        auto osBlock = osPtr->processSamplesUp (baseBlock);
        const size_t osSamples = osBlock.getNumSamples();
        const size_t osCh = osBlock.getNumChannels();

        if (osCh >= 2 && wetCh >= 2)
        {
            float* L = osBlock.getChannelPointer (0);
            float* R = osBlock.getChannelPointer (1);

            void* stL = (void*) &getPendingStateStorage (0);
            void* stR = (void*) &getPendingStateStorage (1);

            for (size_t n = 0; n < osSamples; ++n)
            {
                L[n] = pendingPreset->process (stL, L[n]);
                R[n] = pendingPreset->process (stR, R[n]);
                stereoB.processSample (L[n], R[n]); // ✅ Stereo vida real ON
            }
        }
        else
        {
            float* data = osBlock.getChannelPointer (0);
            void* st = (void*) &getPendingStateStorage (0);

            for (size_t n = 0; n < osSamples; ++n)
                data[n] = pendingPreset->process (st, data[n]);
        }

        osPtr->processSamplesDown (baseBlock);
    }
    else
    {
        float* wetL = io.getWritePointer (0);
        float* wetR = (wetCh > 1) ? io.getWritePointer (1) : nullptr;

        if (wetCh >= 2 && wetR != nullptr)
        {
            void* stL = (void*) &getPendingStateStorage (0);
            void* stR = (void*) &getPendingStateStorage (1);

            for (int i = 0; i < numSamples; ++i)
            {
                wetL[i] = pendingPreset->process (stL, wetL[i]);
                wetR[i] = pendingPreset->process (stR, wetR[i]);
                stereoB.processSample (wetL[i], wetR[i]); // ✅
            }
        }
        else
        {
            void* st = (void*) &getPendingStateStorage (0);
            for (int i = 0; i < numSamples; ++i)
                wetL[i] = pendingPreset->process (st, wetL[i]);
        }
    }
}

// Inicia transición: construye preset/OS destino en el banco INACTIVO (pending*)
void YourPluginAudioProcessor::beginTransition (int newPreset, int newOS, int wetCh, int numSamples)
{
    if (transitioning)
        return;

    if (newPreset < 0 || newPreset >= (int) PresetRegistry::items.size())
        return;

    newOS = juce::jlimit (0, kMaxOversamplingIndex, newOS);

    // ------------- preparar fade (10ms clamp 5..20ms) -------------
    const int minS = juce::jmax (8, (int) std::lround (sr * 0.005));
    const int maxS = juce::jmax (minS + 1, (int) std::lround (sr * 0.020));
    const int defS = (int) std::lround (sr * 0.010);

    xfadeTotalSamples = juce::jlimit (minS, maxS, defS);
    xfadePosSamples   = 0;

    // ------------- limpiar/destruir el banco inactivo ANTES de reutilizarlo -------------
    // FUERA de transición, pendingPresetIndex/pendingOSIndex guardan “qué vive en el banco inactivo”.
    const int oldInactivePreset = pendingPresetIndex;

    if (oldInactivePreset >= 0 && oldInactivePreset < (int) PresetRegistry::items.size())
    {
        const auto* oldPr = &PresetRegistry::items[(size_t) oldInactivePreset];
        if (oldPr != nullptr && oldPr->destruct != nullptr)
        {
            for (int ch = 0; ch < wetCh; ++ch)
            {
                if (getPendingConstructed (ch))
                    oldPr->destruct ((void*) &getPendingStateStorage (ch));
            }
        }
    }

    for (int ch = 0; ch < wetCh; ++ch)
    {
        getPendingConstructed (ch) = false;
        std::memset (&getPendingStateStorage (ch), 0, sizeof (PresetStateStorage));
    }

    // ------------- setear target -------------
    pendingPresetIndex = newPreset;
    pendingOSIndex     = newOS;

    // ------------- preparar oversampler del destino (B) sin tocar el activo -------------
    if (pendingOSIndex != 0 && oversamplers[pendingOSIndex] != nullptr)
    {
        oversamplers[pendingOSIndex]->reset();
        oversamplers[pendingOSIndex]->initProcessing ((size_t) juce::jmax (1, maxBlockSizePrepared));
    }

    // ------------- construir el preset destino en el banco B -------------
    const auto* pr = &PresetRegistry::items[(size_t) pendingPresetIndex];

    const float osFactor = (pendingOSIndex == 0 ? 1.0f : (float) (1u << pendingOSIndex));
    const float pendingSr = (float) (sr * (double) osFactor);

    // ✅ StereoInteract: preparar a SR efectivo del dominio donde corre (OS si hay OS)
    stereoB.prepare (pendingSr);
    stereoB.reset();

    for (int ch = 0; ch < wetCh; ++ch)
    {
        void* st = (void*) &getPendingStateStorage (ch);
        if (pr->construct != nullptr)
        {
            pr->construct (st);
            getPendingConstructed (ch) = true;
        }
        if (pr->prepare) pr->prepare (st, pendingSr);
        if (pr->reset)   pr->reset (st);
    }

    // buffers crossfade (asegura tamaño)
    if (wetOld.getNumChannels() != wetCh || wetOld.getNumSamples() < maxBlockSizePrepared)
        wetOld.setSize (wetCh, maxBlockSizePrepared, false, false, true);

    if (wetNew.getNumChannels() != wetCh || wetNew.getNumSamples() < maxBlockSizePrepared)
        wetNew.setSize (wetCh, maxBlockSizePrepared, false, false, true);

    transitioning = true;
}

// Commit: swap de bancos + stereo, y aplicar nueva “config” sin resetear estados
void YourPluginAudioProcessor::commitTransition (int wetCh)
{
    if (!transitioning)
        return;

    const int oldActivePreset = activePresetIndex;
    const int oldActiveOS     = currentOSIndex;

    // swap bancos
    std::swap (activeState, pendingState);
    std::swap (activeConstructed, pendingConstructed);

    // swap stereo
    std::swap (stereoA, stereoB);

    // nuevo activo = target
    activePresetIndex = pendingPresetIndex;
    activePreset = (activePresetIndex >= 0 && activePresetIndex < (int) PresetRegistry::items.size())
                 ? &PresetRegistry::items[(size_t) activePresetIndex]
                 : nullptr;

    // OS activo = target (SIN resetear el oversampler: ya viene “caminado” durante la transición)
    currentOSIndex = juce::jlimit (0, kMaxOversamplingIndex, pendingOSIndex);
    oversampling   = (currentOSIndex == 0 ? nullptr : oversamplers[currentOSIndex].get());

    // latency/dry-delay acorde al OS nuevo
    const int latency = (oversampling != nullptr) ? (int) oversampling->getLatencyInSamples() : 0;
    setLatencySamples (latency);

    dryDelaySamples  = latency;
    dryDelayWritePos = 0;

    dryDelayBufferSize = juce::jmax (1, maxBlockSizePrepared + dryDelaySamples + 1);
    dryDelayBuffer.setSize (wetCh, dryDelayBufferSize, false, false, true);
    dryDelayBuffer.clear();

    const float osFactor = (currentOSIndex == 0 ? 1.0f : (float) (1u << currentOSIndex));
    osSr = (float) (sr * (double) osFactor);

    // fuera de transición, pendingPresetIndex/pendingOSIndex pasan a describir el banco inactivo (el viejo activo)
    pendingPresetIndex = oldActivePreset;
    pendingOSIndex     = oldActiveOS;

    transitioning = false;
    xfadePosSamples = 0;
}

//==============================================================================
// Process
void YourPluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    const int wetCh = juce::jmax (1, juce::jmin (2, numCh));

    // layout change (mono<->stereo)
    if (wetCh != oversamplerChannels)
    {
        ensureOversamplers (wetCh);
        applyOversamplingIndex (currentOSIndex, true);

        // re-prealloc buffers crossfade
        wetBuffer.setSize (wetCh, juce::jmax (maxBlockSizePrepared, numSamples), false, false, true);
        wetOld.setSize    (wetCh, juce::jmax (maxBlockSizePrepared, numSamples), false, false, true);
        wetNew.setSize    (wetCh, juce::jmax (maxBlockSizePrepared, numSamples), false, false, true);
    }

    // block size variable
    if (numSamples > maxBlockSizePrepared)
    {
        maxBlockSizePrepared = numSamples;
        initOversamplers (maxBlockSizePrepared);

        wetBuffer.setSize (wetCh, maxBlockSizePrepared, false, false, true);
        wetOld.setSize    (wetCh, maxBlockSizePrepared, false, false, true);
        wetNew.setSize    (wetCh, maxBlockSizePrepared, false, false, true);

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

    // Selección de preset deseado
    int desiredPreset = 0;
    if (pPreamp != nullptr && PresetRegistry::items.size() > 0)
        desiredPreset = juce::jlimit (0, (int) PresetRegistry::items.size() - 1,
                                      (int) std::lround (*pPreamp));

    // OS deseado
    const int desiredOS = getDesiredOversamplingIndex();

    // ✅ A. Detectar cambios sin resetear/destruir de golpe
    if (!transitioning)
    {
        const bool needPresetChange = (activePreset == nullptr || desiredPreset != activePresetIndex);
        const bool needOSChange     = (desiredOS != currentOSIndex);

        if ((needPresetChange || needOSChange) && PresetRegistry::items.size() > 0)
        {
            // iniciar transición (no destruir ni resetear el activo)
            beginTransition (desiredPreset, desiredOS, wetCh, numSamples);
        }
    }

    // -------------------------------------------------------------------------
    // 0) Tone smoothing + tilt (SUB-BLOQUES)
    constexpr int kTiltUpdateStride = 32;

    if (wetBuffer.getNumChannels() != wetCh || wetBuffer.getNumSamples() < numSamples)
        wetBuffer.setSize (wetCh, numSamples, false, false, true);

    auto* wetL = wetBuffer.getWritePointer (0);
    auto* wetR = (wetCh > 1) ? wetBuffer.getWritePointer (1) : nullptr;

    int i0 = 0;
    while (i0 < numSamples)
    {
        const int chunk = juce::jmin (kTiltUpdateStride, numSamples - i0);

        updateTiltCoeffs (toneSm.getCurrentValue());

        for (int k = 0; k < chunk; ++k)
        {
            const int i = i0 + k;

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

        toneSm.skip (chunk);
        i0 += chunk;
    }

    // -------------------------------------------------------------------------
    // 1) PROCESO WET (con crossfade si corresponde)
    if (!transitioning)
    {
        // A) normal: render activo “in-place”
        renderChainA (wetBuffer, wetCh, numSamples);
    }
    else
    {
        // copiar base-prewet a wetOld y wetNew
        wetOld.makeCopyOf (wetBuffer, true);
        wetNew.makeCopyOf (wetBuffer, true);

        const bool osChanged = (pendingOSIndex != currentOSIndex);

        if (osChanged)
        {
            // ✅ Escenario 1: cambia OS => render A y B con oversamplers distintos, crossfade a SR base
            renderChainA (wetOld, wetCh, numSamples);
            renderChainB (wetNew, wetCh, numSamples);

            float* oL = wetOld.getWritePointer (0);
            float* nL = wetNew.getWritePointer (0);

            float* outL = wetBuffer.getWritePointer (0);

            float* oR = (wetCh > 1) ? wetOld.getWritePointer (1) : nullptr;
            float* nR = (wetCh > 1) ? wetNew.getWritePointer (1) : nullptr;
            float* outR = (wetCh > 1) ? wetBuffer.getWritePointer (1) : nullptr;

            for (int i = 0; i < numSamples; ++i)
            {
                const float t = (xfadeTotalSamples > 0)
                              ? ((float) (xfadePosSamples + i) / (float) xfadeTotalSamples)
                              : 1.0f;

                const float gA = xfadeGainA (t);
                const float gB = xfadeGainB (t);

                outL[i] = gA * oL[i] + gB * nL[i];
                if (wetCh > 1 && outR != nullptr && oR != nullptr && nR != nullptr)
                    outR[i] = gA * oR[i] + gB * nR[i];
            }

            xfadePosSamples += numSamples;
            if (xfadePosSamples >= xfadeTotalSamples)
                commitTransition (wetCh);
        }
        else
        {
            // ✅ Escenario 2: mismo OS, cambia preset => 1x up, duplicar osBlock, procesar A/B, crossfade en OS domain, 1x down
            const int osIdx = currentOSIndex;
            auto* osPtr = (osIdx == 0 ? nullptr : oversamplers[osIdx].get());

            if (osPtr != nullptr)
            {
                juce::dsp::AudioBlock<float> baseBlock (wetBuffer);
                baseBlock = baseBlock.getSubBlock (0, (size_t) numSamples);

                auto osBlockA = osPtr->processSamplesUp (baseBlock);
                const size_t osSamples = osBlockA.getNumSamples();
                const size_t osCh = osBlockA.getNumChannels();

                // osTemp: copia exacta del osBlockA (antes de procesar)
                if ((int) osCh <= 0)
                {
                    // nada
                }
                else
                {
                    if (osTemp.getNumChannels() != (int) osCh || osTemp.getNumSamples() < (int) osSamples)
                        osTemp.setSize ((int) osCh, (int) osSamples, false, false, true);

                    for (size_t ch = 0; ch < osCh; ++ch)
                        std::memcpy (osTemp.getWritePointer ((int) ch),
                                     osBlockA.getChannelPointer (ch),
                                     osSamples * sizeof (float));
                }

                // procesar A sobre osBlockA
                if (activePreset != nullptr && activePreset->process != nullptr)
                {
                    if (osCh >= 2 && wetCh >= 2)
                    {
                        float* L = osBlockA.getChannelPointer (0);
                        float* R = osBlockA.getChannelPointer (1);

                        void* stL = (void*) &getActiveStateStorage (0);
                        void* stR = (void*) &getActiveStateStorage (1);

                        for (size_t n = 0; n < osSamples; ++n)
                        {
                            L[n] = activePreset->process (stL, L[n]);
                            R[n] = activePreset->process (stR, R[n]);
                            stereoA.processSample (L[n], R[n]); // ✅
                        }
                    }
                    else
                    {
                        float* data = osBlockA.getChannelPointer (0);
                        void* st = (void*) &getActiveStateStorage (0);
                        for (size_t n = 0; n < osSamples; ++n)
                            data[n] = activePreset->process (st, data[n]);
                    }
                }

                // procesar B sobre osTemp
                const auto* pendingPreset = (pendingPresetIndex >= 0 && pendingPresetIndex < (int) PresetRegistry::items.size())
                                          ? &PresetRegistry::items[(size_t) pendingPresetIndex]
                                          : nullptr;

                if (pendingPreset != nullptr && pendingPreset->process != nullptr)
                {
                    if (osCh >= 2 && wetCh >= 2)
                    {
                        float* Lb = osTemp.getWritePointer (0);
                        float* Rb = osTemp.getWritePointer (1);

                        void* stL = (void*) &getPendingStateStorage (0);
                        void* stR = (void*) &getPendingStateStorage (1);

                        for (size_t n = 0; n < osSamples; ++n)
                        {
                            Lb[n] = pendingPreset->process (stL, Lb[n]);
                            Rb[n] = pendingPreset->process (stR, Rb[n]);
                            stereoB.processSample (Lb[n], Rb[n]); // ✅
                        }
                    }
                    else
                    {
                        float* data = osTemp.getWritePointer (0);
                        void* st = (void*) &getPendingStateStorage (0);
                        for (size_t n = 0; n < osSamples; ++n)
                            data[n] = pendingPreset->process (st, data[n]);
                    }
                }

                // crossfade oversampled domain
                const int osFactor = (osIdx == 0 ? 1 : (1 << osIdx));
                const float denom = (float) (juce::jmax (1, xfadeTotalSamples) * osFactor);

                if (osCh >= 2 && wetCh >= 2)
                {
                    float* L = osBlockA.getChannelPointer (0);
                    float* R = osBlockA.getChannelPointer (1);

                    float* Lb = osTemp.getWritePointer (0);
                    float* Rb = osTemp.getWritePointer (1);

                    const int baseStart = xfadePosSamples * osFactor;

                    for (size_t n = 0; n < osSamples; ++n)
                    {
                        const float t = (denom > 0.0f)
                                      ? ((float) (baseStart + (int) n) / denom)
                                      : 1.0f;

                        const float gA = xfadeGainA (t);
                        const float gB = xfadeGainB (t);

                        L[n] = gA * L[n] + gB * Lb[n];
                        R[n] = gA * R[n] + gB * Rb[n];
                    }
                }
                else if (osCh >= 1)
                {
                    float* a = osBlockA.getChannelPointer (0);
                    float* b = osTemp.getWritePointer (0);

                    const int baseStart = xfadePosSamples * osFactor;

                    for (size_t n = 0; n < osSamples; ++n)
                    {
                        const float t = (denom > 0.0f)
                                      ? ((float) (baseStart + (int) n) / denom)
                                      : 1.0f;

                        const float gA = xfadeGainA (t);
                        const float gB = xfadeGainB (t);

                        a[n] = gA * a[n] + gB * b[n];
                    }
                }

                // bajar una sola vez
                osPtr->processSamplesDown (baseBlock);

                xfadePosSamples += numSamples;
                if (xfadePosSamples >= xfadeTotalSamples)
                    commitTransition (wetCh);
            }
            else
            {
                // x1 (sin OS): hacemos crossfade a SR base renderizando 2 cadenas (igual que escenario 1)
                renderChainA (wetOld, wetCh, numSamples);
                renderChainB (wetNew, wetCh, numSamples);

                float* oL = wetOld.getWritePointer (0);
                float* nL = wetNew.getWritePointer (0);
                float* outL = wetBuffer.getWritePointer (0);

                float* oR = (wetCh > 1) ? wetOld.getWritePointer (1) : nullptr;
                float* nR = (wetCh > 1) ? wetNew.getWritePointer (1) : nullptr;
                float* outR = (wetCh > 1) ? wetBuffer.getWritePointer (1) : nullptr;

                for (int i = 0; i < numSamples; ++i)
                {
                    const float t = (xfadeTotalSamples > 0)
                                  ? ((float) (xfadePosSamples + i) / (float) xfadeTotalSamples)
                                  : 1.0f;

                    const float gA = xfadeGainA (t);
                    const float gB = xfadeGainB (t);

                    outL[i] = gA * oL[i] + gB * nL[i];
                    if (wetCh > 1 && outR != nullptr && oR != nullptr && nR != nullptr)
                        outR[i] = gA * oR[i] + gB * nR[i];
                }

                xfadePosSamples += numSamples;
                if (xfadePosSamples >= xfadeTotalSamples)
                    commitTransition (wetCh);
            }
        }
    }

    // -------------------------------------------------------------------------
    // 3) Mix + AUTO-LEVEL (por muestra)
    const float safetyHeadroom = juce::Decibels::decibelsToGain (-0.9f);
    const float maxAutoGain    = juce::Decibels::decibelsToGain (+12.0f);

    auto* dL = dryDelayBuffer.getWritePointer (0);
    auto* dR = (dryDelayBuffer.getNumChannels() > 1) ? dryDelayBuffer.getWritePointer (1) : nullptr;

    const int size = dryDelayBuffer.getNumSamples();

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix01 = mixSm.getNextValue();

        const int wp = dryDelayWritePos;
        int rp = wp - dryDelaySamples;
        if (rp < 0) rp += size;

        const float inL = ch0[i];
        const float inR = (ch1 != nullptr) ? ch1[i] : inL;

        dL[wp] = inL;
        if (dR != nullptr) dR[wp] = inR;

        const float dryL = dL[rp];
        const float dryR = (dR != nullptr) ? dR[rp] : dryL;

        dryDelayWritePos = wp + 1;
        if (dryDelayWritePos >= size) dryDelayWritePos = 0;

        const float wetOutL = wetL[i];
        const float wetOutR = (wetR != nullptr) ? wetR[i] : wetOutL;

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

        const float g = autoGain.processStereo (dryL, dryR, mixedL, mixedR);

        const float gSafe = juce::jlimit (0.0f, maxAutoGain, g);

        ch0[i] = plugin::softClipSafety (mixedL * gSafe * safetyHeadroom);
        if (ch1 != nullptr)
            ch1[i] = plugin::softClipSafety (mixedR * gSafe * safetyHeadroom);
    }
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YourPluginAudioProcessor();
}


