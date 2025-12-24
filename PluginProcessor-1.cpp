// PluginProcessor-1.cpp
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

// ✅ En este archivo debe quedar SIN static (link externo)
juce::Typeface::Ptr getEmbeddedPluginTypeface()
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

// ✅ inferSpriteGrid robusta con aspectPenalty
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
// TODO: makeLayout() vive en PluginProcessor-2.cpp

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

        // ✅ Fuerza la fuente embebida como default global (para TODO el texto)
        if (typeface != nullptr)
            juce::LookAndFeel::setDefaultSansSerifTypeface (typeface);

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

    // ✅ ComboBox: opción seleccionada -25% (0.75x)
    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        auto f = juce::LookAndFeel_V4::getComboBoxFont (box);
        f.setHeight (juce::jmax (6.0f, f.getHeight() * 0.75f));
        return f;
    }

    // ✅ PopupMenu: opciones -25% (0.75x)
    juce::Font getPopupMenuFont() override
    {
        auto f = juce::LookAndFeel_V4::getPopupMenuFont();
        f.setHeight (juce::jmax (6.0f, f.getHeight() * 0.75f));
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

        const float sx = (float) col * frameWf;
        const float sy = (float) row * frameHf;

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

        // usar overload clásico (src/dst en int), portable en todas las JUCE
        const int dxI = juce::roundToInt (dx);
        const int dyI = juce::roundToInt (dy);
        const int dwI = juce::jmax (1, juce::roundToInt (dw));
        const int dhI = juce::jmax (1, juce::roundToInt (dh));

        const int sxI = juce::roundToInt (sx + bleed);
        const int syI = juce::roundToInt (sy + bleed);
        const int swI = juce::jmax (1, juce::roundToInt (sw));
        const int shI = juce::jmax (1, juce::roundToInt (sh));

        g.drawImage (sheet,
                     dxI, dyI, dwI, dhI,
                     sxI, syI, swI, shI,
                     false);
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
class MinimalEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit MinimalEditor (YourPluginAudioProcessor& proc)
        : juce::AudioProcessorEditor (&proc)
        , processor (proc)
        , driveKnob  ("Drive")
        , toneKnob   ("Tone")
        , mixKnob    ("Mix")
        , outputKnob ("Output")
    {
        // ✅ Aplica mi_fuente.ttf (assets) a TODO el UI heredado de este editor
        setLookAndFeel (&knobLNF);

        // Look & Feel knobs
        driveKnob.slider.setLookAndFeel (&knobLNF);
        toneKnob .slider.setLookAndFeel (&knobLNF);
        mixKnob  .slider.setLookAndFeel (&knobLNF);
        outputKnob.slider.setLookAndFeel (&knobLNF);

        driveKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FF006700"));
        driveKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FF65FF65"));

        toneKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FFF9FF34"));
        toneKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FFCC66FF"));

        mixKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FF555555"));
        mixKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FFFFFFFF"));

        // Output knob (paleta neutra)
        outputKnob.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromString ("FF3A3A3A"));
        outputKnob.slider.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromString ("FFD0D0D0"));

        // Fonts/labels (usar FontOptions)
        driveKnob.label.setFont (juce::Font (juce::FontOptions (11.0f)));
        toneKnob .label.setFont (juce::Font (juce::FontOptions (11.0f)));
        mixKnob  .label.setFont (juce::Font (juce::FontOptions (11.0f)));
        outputKnob.label.setFont (juce::Font (juce::FontOptions (11.0f)));

        driveKnob.label.setLookAndFeel (&knobLNF);
        toneKnob .label.setLookAndFeel (&knobLNF);
        mixKnob  .label.setLookAndFeel (&knobLNF);
        outputKnob.label.setLookAndFeel (&knobLNF);

        addAndMakeVisible (driveKnob);
        addAndMakeVisible (toneKnob);
        addAndMakeVisible (mixKnob);
        addAndMakeVisible (outputKnob);

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
        // Drive/Tone/Mix labels
        driveKnob.setLabelImage (juce::ImageCache::getFromMemory (BinaryData::drive_png, BinaryData::drive_pngSize));
        toneKnob .setLabelImage (juce::ImageCache::getFromMemory (BinaryData::tone_png,  BinaryData::tone_pngSize));
        mixKnob  .setLabelImage (juce::ImageCache::getFromMemory (BinaryData::mix_png,   BinaryData::mix_pngSize));

        driveKnob.setLabelSlotHeights (12, 14);
        mixKnob  .setLabelSlotHeights (12, 14);
        toneKnob .setLabelSlotHeights (20, 14);

        // ✅ Output PNGs (on/off)
        plugin::ui::loadImageFromBinaryDataByFilename ("output.png",     outputLabelOn);
        plugin::ui::loadImageFromBinaryDataByFilename ("output_off.png", outputLabelOff);

        if (outputLabelOn.isValid())
            outputKnob.setLabelImage (outputLabelOn);

        // "igual que los otros knobs PNG"
        outputKnob.setLabelSlotHeights (12, 14);
       #else
        // Sin assets: al menos conserva los slots para texto
        outputKnob.setLabelSlotHeights (12, 14);
       #endif

        // Preamp
        preampBox.setJustificationType (juce::Justification::centredLeft);
        preampBox.setLookAndFeel (&knobLNF);

        int itemId = 1;
        for (const auto& it : PresetRegistry::items)
            preampBox.addItem (it.displayName, itemId++);

        if (preampBox.getNumItems() == 0)
            preampBox.addItem ("(none)", 1);

        addAndMakeVisible (preampBox);

        // Oversampling
        osBox.setJustificationType (juce::Justification::centredLeft);
        osBox.setLookAndFeel (&knobLNF);
        osBox.addItem ("x1", 1);
        osBox.addItem ("x2", 2);
        osBox.addItem ("x4", 3);
        osBox.addItem ("x8", 4);
        addAndMakeVisible (osBox);

        // ✅ AutoGain: ToggleButton SOLO tick + Label aparte
        autoGainButton.setButtonText ("");
        addAndMakeVisible (autoGainButton);

        autoGainLabel.setText ("AutoGain", juce::dontSendNotification);
        autoGainLabel.setJustificationType (juce::Justification::centredLeft);
        autoGainLabel.setInterceptsMouseClicks (false, false);
        autoGainLabel.setLookAndFeel (&knobLNF);
        autoGainLabel.setColour (juce::Label::textColourId, juce::Colours::white);

        // ✅ 50% más grande (1.5x)
        autoGainLabel.setFont (juce::Font (juce::FontOptions (11.0f * 1.5f)));

        addAndMakeVisible (autoGainLabel);

        // Attachments
        driveAtt   = std::make_unique<SliderAttachment>   (processor.apvts, "drive",  driveKnob.slider);
        toneAtt    = std::make_unique<SliderAttachment>   (processor.apvts, "tone",   toneKnob.slider);
        mixAtt     = std::make_unique<SliderAttachment>   (processor.apvts, "mix",    mixKnob.slider);
        outputAtt  = std::make_unique<SliderAttachment>   (processor.apvts, "output", outputKnob.slider);

        preampAtt  = std::make_unique<ComboBoxAttachment> (processor.apvts, "preamp", preampBox);
        osAtt      = std::make_unique<ComboBoxAttachment> (processor.apvts, "os",     osBox);

        autoGainAtt = std::make_unique<ButtonAttachment>  (processor.apvts, "autogain", autoGainButton);

        // Cache param ptr para enable/disable del output knob
        autoGainParam = processor.apvts.getRawParameterValue ("autogain");

        startTimerHz (30);
        timerCallback(); // aplica estado inicial (incluye PNG/enable)

        // ✅ Un poco más alto (solo lo necesario)
        setSize (410, 480);
    }

    ~MinimalEditor() override
    {
        stopTimer();
        header.stop();

        // Importante: evitar punteros colgantes (knobLNF vive como miembro)
        setLookAndFeel (nullptr);

        preampBox.setLookAndFeel (nullptr);
        osBox.setLookAndFeel (nullptr);

        autoGainLabel.setLookAndFeel (nullptr);

        driveKnob.label.setLookAndFeel (nullptr);
        toneKnob .label.setLookAndFeel (nullptr);
        mixKnob  .label.setLookAndFeel (nullptr);
        outputKnob.label.setLookAndFeel (nullptr);

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
        // ✅ Margen reducido para el nuevo ancho
        auto area = getLocalBounds().reduced (24);

        // -------------------------
        // HEADER
        int headerH = 72;
        const float ar = header.getFrameAspect();
        if (ar > 0.01f)
        {
            headerH = juce::roundToInt ((float) area.getWidth() / ar);
            headerH = juce::jlimit (56, 160, headerH);
        }

        auto headerArea = area.removeFromTop (headerH).reduced (6, 6);
        header.setBounds (headerArea);

        area.removeFromTop (10);

        // -------------------------
        // BOTTOM AREA (2 filas)
        // fila 1: preampBox (full)
        // fila 2: osBox + AutoGain (tick + label)
        const int bottomH = 96;
        auto bottomArea = area.removeFromBottom (juce::jmin (bottomH, area.getHeight()));
        bottomArea = bottomArea.reduced (0, 6);

        auto preampRow = bottomArea.removeFromTop (juce::jmin (40, bottomArea.getHeight() / 2));
        bottomArea.removeFromTop (6);
        auto secondRow = bottomArea;

        preampBox.setBounds (preampRow.reduced (0, 6));

        // secondRow: osBox + autogain
        const int gap = 8;
        auto osArea = secondRow.removeFromLeft ((int) juce::roundToInt (secondRow.getWidth() * 0.55f));
        secondRow.removeFromLeft (gap);
        auto agArea = secondRow;

        osBox.setBounds (osArea.reduced (0, 6));

        // AutoGain: tick chico + label al lado
        auto tickArea = agArea.removeFromLeft (26);
        autoGainButton.setBounds (tickArea.withSizeKeepingCentre (22, 22));
        autoGainLabel.setBounds (agArea.reduced (0, 6));

        // -------------------------
        // KNOB AREA (2x2)
        auto knobArea = area;
        const int kGap = 10;

        // Divide en 2 filas
        const int rowH = (knobArea.getHeight() - kGap) / 2;
        auto row1 = knobArea.removeFromTop (rowH);
        knobArea.removeFromTop (kGap);
        auto row2 = knobArea;

        // Cada fila en 2 columnas
        const int colW1 = (row1.getWidth() - kGap) / 2;
        auto r1c1 = row1.removeFromLeft (colW1);
        row1.removeFromLeft (kGap);
        auto r1c2 = row1;

        const int colW2 = (row2.getWidth() - kGap) / 2;
        auto r2c1 = row2.removeFromLeft (colW2);
        row2.removeFromLeft (kGap);
        auto r2c2 = row2;

        // ✅ más “aire” útil (menos padding) para que no queden mini
        driveKnob .setBounds (r1c1.reduced (4, 2));
        toneKnob  .setBounds (r1c2.reduced (4, 2));
        mixKnob   .setBounds (r2c1.reduced (4, 2));
        outputKnob.setBounds (r2c2.reduced (4, 2));
    }

private:
    void timerCallback() override
    {
        const bool agOn = (autoGainParam != nullptr && autoGainParam->load() >= 0.5f);

        outputKnob.setEnabled (!agOn);
        outputKnob.setAlpha (agOn ? 0.35f : 1.0f);

        // ✅ Cambiar PNG solo cuando cambia el estado (no 30 veces/seg)
        if (agOn != lastAgOn)
        {
           #if PLUGIN_HAS_ASSETS
            if (agOn)
            {
                if (outputLabelOff.isValid())
                    outputKnob.setLabelImage (outputLabelOff);
            }
            else
            {
                if (outputLabelOn.isValid())
                    outputKnob.setLabelImage (outputLabelOn);
            }
           #endif
            lastAgOn = agOn;
        }
    }

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;

    YourPluginAudioProcessor& processor;

    KnobLNF knobLNF;

    AnimatedHeader header;

    plugin::ui::LabeledKnob driveKnob;
    plugin::ui::LabeledKnob toneKnob;
    plugin::ui::LabeledKnob mixKnob;
    plugin::ui::LabeledKnob outputKnob;

    juce::ToggleButton autoGainButton;

    // ✅ AutoGain texto separado
    juce::Label autoGainLabel;

    juce::ComboBox preampBox;
    juce::ComboBox osBox;

    std::unique_ptr<SliderAttachment> driveAtt, toneAtt, mixAtt, outputAtt;
    std::unique_ptr<ComboBoxAttachment> preampAtt, osAtt;
    std::unique_ptr<ButtonAttachment> autoGainAtt;

    std::atomic<float>* autoGainParam = nullptr;

    // ✅ Output label PNGs y flag de estado
    juce::Image outputLabelOn, outputLabelOff;
    bool lastAgOn = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MinimalEditor)
};
} // namespace

//==============================================================================
// Editor factory (vive aquí porque MinimalEditor vive aquí)
juce::AudioProcessorEditor* YourPluginAudioProcessor::createEditor()
{
    return new MinimalEditor (*this);
}



