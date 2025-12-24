#pragma once

// funciones.h (PRO)
// Utilidades compartidas: mezcla, drive map, soft clip, level matching, UI.

#include <JuceHeader.h>
#include <cmath>


//------------------------------------------------------------------------------
// Assets / BinaryData
// - PLUGIN_HAS_ASSETS: definido desde CMake cuando existe al menos 1 archivo
//   dentro de /assets (PNGs y/o fuentes).
// - PLUGIN_HAS_FONT + PLUGIN_PRIMARY_FONT_FILENAME: definidos desde CMake cuando
//   existe al menos 1 .ttf/.otf en /assets.
//
// Si por alguna razón el build no define estas macros, no rompas la compilación.
#ifndef PLUGIN_HAS_ASSETS
 #define PLUGIN_HAS_ASSETS 0
#endif

#ifndef PLUGIN_HAS_FONT
 #define PLUGIN_HAS_FONT 0
#endif

#ifndef PLUGIN_PRIMARY_FONT_FILENAME
 #define PLUGIN_PRIMARY_FONT_FILENAME ""
#endif

#if PLUGIN_HAS_ASSETS
 #include "BinaryData.h"
#endif


namespace plugin
{
//==============================================================================
// Soft clip final "safety"
inline float softClipSafety (float x) noexcept
{
    // Safety final (NO saturador con make-up).
    // Objetivo:
    // - Mantener 1:1 (sin cambios) lejos del techo.
    // - Evitar hard-clip (jlimit) porque genera armónicos muy altos -> aliasing.
    // - Knee corto: empieza a comprimir un poco antes de ±1 y asintota a ±1.
    //
    // Mapeo:
    // - |x| <= kneeStart  -> y = x (perfectamente lineal)
    // - |x|  > kneeStart  -> y = kneeStart + (1-kneeStart) * (2/pi)*atan( (pi/2) * (|x|-kneeStart)/(1-kneeStart) )
    //
    // Nota: La escala (pi/2) asegura pendiente ~= 1 justo al inicio del knee,
    // evitando un "salto" brusco de derivada.

    constexpr float kneeStart = 0.98f;              // empieza a comprimir aquí
    constexpr float one       = 1.0f;

    const float ax = std::abs (x);
    if (ax <= kneeStart)
        return x;

    const float sign = (x >= 0.0f ? 1.0f : -1.0f);

    const float kneeRange = juce::jmax (1.0e-6f, one - kneeStart);
    const float t = ax - kneeStart; // excedente por encima del knee

    const float scaled = juce::MathConstants<float>::halfPi * (t / kneeRange); // (pi/2)*t/(1-kneeStart)
    const float soft   = (2.0f / juce::MathConstants<float>::pi) * std::atan (scaled); // 0..1

    const float y = kneeStart + kneeRange * soft; // -> 1 asintótico
    return sign * juce::jlimit (0.0f, 1.0f, y);
}

// Drive 0..1 -> dB de pregain
inline float mapDriveDb (float drive01) noexcept
{
    // Objetivo:
    // - Mejor resolución en valores bajos/medios ("sweet spot")
    // - Menos "explosión" al final (más controlable en presets agresivos)
    // - Misma idea: DRIVE es pregain para empujar el preset
    //
    // Curva exponencial normalizada (asintótica) + un pequeño componente lineal
    // para que el control cerca de 0 siga siendo predecible.
    const float x = juce::jlimit (0.0f, 1.0f, drive01);

    constexpr float k = 3.25f; // más alto = más resolución al inicio
    const float expNorm = 1.0f - std::exp (-k);
    const float expCurve = (1.0f - std::exp (-k * x)) / (expNorm > 0.0f ? expNorm : 1.0f);

    // Mezcla suave (evita que el final se sienta "todo o nada")
    const float shaped = 0.18f * x + 0.82f * expCurve; // 0..1

    // Rango de pregain (dB): un poco más de headroom para empujar presets, sin volverse inusable.
    return 36.0f * shaped; // 0..36 dB
}

//==============================================================================
// Tone 0..1 -> Tilt dB (neutral EXACTO en 0.5, con "dead-zone" alrededor)
//
// Requerimiento:
// - En el centro (0.5) NO debe hacer nada.
// - Hacia la derecha => sonido más oscuro.
// - Hacia la izquierda => sonido más brillante.
//
// Inspiración: el "Tone" tipo Decapitator, pero con una respuesta más robusta:
// - dead-zone para que el centro sea realmente neutro
// - curva no-lineal para que el movimiento se sienta musical
// - rango asimétrico (típicamente conviene menos boost de brillo que de dark)
inline float mapToneTiltDb (float tone01) noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, tone01);

    // signed: -0.5..+0.5 (izq..der)
    float s = t - 0.5f;

    // dead-zone alrededor del centro ("no hace nada")
    constexpr float dead = 0.06f; // ~6% del recorrido total
    if (std::abs (s) <= dead * 0.5f)
        return 0.0f;

    // remap quitando la zona muerta
    const float sign = (s >= 0.0f ? 1.0f : -1.0f);
    const float mag  = (std::abs (s) - dead * 0.5f) / (0.5f - dead * 0.5f); // 0..1

    // curva musical: suave al inicio, más decisión al final
    constexpr float curve = 1.55f;
    const float a = std::pow (juce::jlimit (0.0f, 1.0f, mag), curve);

    // Asimetría deliberada:
    // - Dark suele tolerar un poquito más (por distorsión/armónicos)
    // - Bright conviene ser más conservador para evitar harsh
    constexpr float maxDarkDb   = 9.0f;
    constexpr float maxBrightDb = 7.0f;

    // Nota: derecha = oscuro (tilt NEGATIVO), izquierda = brillante (tilt POSITIVO)
    if (sign > 0.0f)
        return -maxDarkDb * a;
    return  maxBrightDb * a;
}

// Mezcla equal-power
inline float equalPowerMix (float dry, float wet, float mix01) noexcept
{
    const float m = juce::jlimit (0.0f, 1.0f, mix01);
    const float a = std::cos (0.5f * juce::MathConstants<float>::pi * m);
    const float b = std::sin (0.5f * juce::MathConstants<float>::pi * m);
    return dry * a + wet * b;
}

//==============================================================================
// LevelMatcher: RMS (EMA) + gate + clamp + smoothing attack/release
// (Lo dejamos por si lo quieres reutilizar, pero YA NO lo usaremos en el plugin)
class LevelMatcher
{
public:
    void prepare (double sampleRate)
    {
        sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

        setMeasurementWindowMs (160.0f);
        setGainSmoothingMs (8.0f, 120.0f);
        setGateDb (-60.0f);
        setClampDb (-18.0f, +18.0f);
        setMeasureHighpassHz (120.0f);
        setReturnToUnityMs (600.0f);

        reset();
    }

    void setSampleRate (double sampleRate) { prepare (sampleRate); }

    void reset()
    {
        refEnv = 0.0f;
        outEnv = 0.0f;
        compGain = 1.0f;
        targetGain = 1.0f;

        hp_x1[0] = hp_x1[1] = 0.0f;
        hp_y1[0] = hp_y1[1] = 0.0f;
    }

    void setGateDb (float gateDb_)
    {
        gateLin = juce::Decibels::decibelsToGain (gateDb_);
        gatePow = gateLin * gateLin;
    }

    void setClampDb (float minDb, float maxDb)
    {
        minGain = juce::Decibels::decibelsToGain (minDb);
        maxGain = juce::Decibels::decibelsToGain (maxDb);
    }

    void setMeasurementWindowMs (float ms)
    {
        const float tau = juce::jmax (1.0e-4f, ms * 0.001f);
        measAlpha = std::exp (-1.0f / (tau * (float) sr));
    }

    void setGainSmoothingMs (float attackMs, float releaseMs)
    {
        gainAttackAlpha  = alphaFromMs (attackMs);
        gainReleaseAlpha = alphaFromMs (releaseMs);
    }

    void setReturnToUnityMs (float ms)
    {
        returnAlpha = alphaFromMs (ms);
    }

    void setMeasureHighpassHz (float hz)
    {
        const float fc = juce::jlimit (5.0f, 1000.0f, hz);
        hpA = std::exp (-2.0f * juce::MathConstants<float>::pi * (fc / (float) sr));
    }

    float process (float drySample, float mixedSample)
    {
        return processStereo (drySample, drySample, mixedSample, mixedSample);
    }

    float processStereo (float dryL, float dryR, float outL, float outR)
    {
        const float mdL = measureHP (dryL, 0);
        const float mdR = measureHP (dryR, 1);
        const float moL = measureHP (outL, 0);
        const float moR = measureHP (outR, 1);

        const float refP = 0.5f * (mdL * mdL + mdR * mdR);
        const float outP = 0.5f * (moL * moL + moR * moR);

        refEnv = measAlpha * refEnv + (1.0f - measAlpha) * refP;
        outEnv = measAlpha * outEnv + (1.0f - measAlpha) * outP;

        const bool gateOk = (refEnv > gatePow && outEnv > gatePow);

        if (gateOk)
        {
            const float refRms = std::sqrt (refEnv + 1.0e-12f);
            const float outRms = std::sqrt (outEnv + 1.0e-12f);

            float t = refRms / outRms;
            t = juce::jlimit (minGain, maxGain, t);
            targetGain = t;
        }
        else
        {
            targetGain = returnAlpha * targetGain + (1.0f - returnAlpha) * 1.0f;
        }

        const bool needDown = (targetGain < compGain);
        const float a = needDown ? gainAttackAlpha : gainReleaseAlpha;
        compGain = a * compGain + (1.0f - a) * targetGain;

        return compGain;
    }

private:
    float alphaFromMs (float ms) const
    {
        const float tau = juce::jmax (1.0e-4f, ms * 0.001f);
        return std::exp (-1.0f / (tau * (float) sr));
    }

    float measureHP (float x, int ch) noexcept
    {
        const float y = hpA * (hp_y1[ch] + x - hp_x1[ch]);
        hp_x1[ch] = x;
        hp_y1[ch] = y;
        return y;
    }

    double sr = 48000.0;

    float measAlpha = 0.999f;
    float refEnv = 0.0f;
    float outEnv = 0.0f;

    float gateLin = juce::Decibels::decibelsToGain (-60.0f);
    float gatePow = gateLin * gateLin;

    float minGain = juce::Decibels::decibelsToGain (-18.0f);
    float maxGain = juce::Decibels::decibelsToGain (+18.0f);

    float gainAttackAlpha  = 0.999f;
    float gainReleaseAlpha = 0.9995f;

    float returnAlpha = 0.9999f;

    float hpA = 0.98f;
    float hp_x1[2] = { 0.0f, 0.0f };
    float hp_y1[2] = { 0.0f, 0.0f };

    float compGain = 1.0f;
    float targetGain = 1.0f;
};

//==============================================================================
// AutoGainExact (v3 ULTRA): iguala "volumen percibido" (RMS con HP) por MUESTRA.
class AutoGainExact
{
public:
    void prepare (double sampleRate)
    {
        sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

        setClampDb (-60.0f, +24.0f);
        setGateDb  (-90.0f);
        setMeasurementWindowMs (12.0f);
        setGainSmoothingMs (0.30f, 6.0f);
        setReturnToUnityMs (220.0f);
        setMeasureHighpassHz (70.0f);

        reset();
    }

    void reset()
    {
        inEnv  = 0.0f;
        outEnv = 0.0f;

        compGain   = 1.0f;
        targetGain = 1.0f;

        in_hp_x1[0] = in_hp_x1[1] = 0.0f;
        in_hp_y1[0] = in_hp_y1[1] = 0.0f;
        out_hp_x1[0] = out_hp_x1[1] = 0.0f;
        out_hp_y1[0] = out_hp_y1[1] = 0.0f;
    }

    float getGain() const noexcept { return compGain; }

    inline float processStereo (float dryL, float dryR, float outLpre, float outRpre) noexcept
    {
        const float inL  = measureHP (dryL,    0, in_hp_x1,  in_hp_y1);
        const float inR  = measureHP (dryR,    1, in_hp_x1,  in_hp_y1);
        const float outL = measureHP (outLpre, 0, out_hp_x1, out_hp_y1);
        const float outR = measureHP (outRpre, 1, out_hp_x1, out_hp_y1);

        const float pIn  = 0.5f * (inL  * inL  + inR  * inR);
        const float pOut = 0.5f * (outL * outL + outR * outR);

        inEnv  = measAlpha * inEnv  + (1.0f - measAlpha) * pIn;
        outEnv = measAlpha * outEnv + (1.0f - measAlpha) * pOut;

        const bool gateOk = (inEnv > gatePow) && (outEnv > gatePow);

        if (gateOk)
        {
            const float ratio = inEnv / (outEnv + 1.0e-20f);
            float g = std::sqrt (ratio);
            g = juce::jlimit (minGain, maxGain, g);
            targetGain = g;
        }
        else
        {
            targetGain = returnAlpha * targetGain + (1.0f - returnAlpha) * 1.0f;
        }

        const bool needDown = (targetGain < compGain);
        const float a = needDown ? gainAttackAlpha : gainReleaseAlpha;
        compGain = a * compGain + (1.0f - a) * targetGain;

        return compGain;
    }

    void setClampDb (float minDb, float maxDb)
    {
        minGain = juce::Decibels::decibelsToGain (minDb);
        maxGain = juce::Decibels::decibelsToGain (maxDb);
    }

    void setGateDb (float gateDb)
    {
        const float gateLin = juce::Decibels::decibelsToGain (gateDb);
        gatePow = gateLin * gateLin;
    }

    void setMeasurementWindowMs (float ms)
    {
        const float tau = juce::jmax (1.0e-4f, ms * 0.001f);
        measAlpha = std::exp (-1.0f / (tau * (float) sr));
    }

    void setGainSmoothingMs (float attackMs, float releaseMs)
    {
        gainAttackAlpha  = alphaFromMsSample (attackMs);
        gainReleaseAlpha = alphaFromMsSample (releaseMs);
    }

    void setReturnToUnityMs (float ms)
    {
        returnAlpha = alphaFromMsSample (ms);
    }

    void setMeasureHighpassHz (float hz)
    {
        const float fc = juce::jlimit (5.0f, 1000.0f, hz);
        hpA = std::exp (-2.0f * juce::MathConstants<float>::pi * (fc / (float) sr));
    }

private:
    inline float alphaFromMsSample (float ms) const noexcept
    {
        const float tau = juce::jmax (1.0e-4f, ms * 0.001f);
        return std::exp (-1.0f / (tau * (float) sr));
    }

    inline float measureHP (float x, int ch, float* x1, float* y1) noexcept
    {
        const float y = hpA * (y1[ch] + x - x1[ch]);
        x1[ch] = x;
        y1[ch] = y;
        return y;
    }

    double sr = 48000.0;

    float minGain = juce::Decibels::decibelsToGain (-60.0f);
    float maxGain = juce::Decibels::decibelsToGain (+24.0f);

    float gatePow = juce::Decibels::decibelsToGain (-80.0f) * juce::Decibels::decibelsToGain (-80.0f);

    float hpA = 0.98f;
    float in_hp_x1[2]  = { 0.0f, 0.0f };
    float in_hp_y1[2]  = { 0.0f, 0.0f };
    float out_hp_x1[2] = { 0.0f, 0.0f };
    float out_hp_y1[2] = { 0.0f, 0.0f };

    float measAlpha = 0.999f;
    float inEnv  = 0.0f;
    float outEnv = 0.0f;

    float gainAttackAlpha  = 0.99f;
    float gainReleaseAlpha = 0.9995f;
    float returnAlpha      = 0.9999f;

    float compGain   = 1.0f;
    float targetGain = 1.0f;
};


//==============================================================================
// UI helpers
namespace ui
{

//------------------------------------------------------------------------------
// ✅ Declaración: esta función está implementada en tu PluginProcessor-1.cpp
// (o donde la hayas definido) y devuelve la fuente embebida desde BinaryData.
//
// Importante: la declaramos aquí para que GlobalFontLookAndFeel use EXACTAMENTE
// la misma fuente que ya te funciona en los knobs.
juce::Typeface::Ptr getEmbeddedPluginTypeface();

//==============================================================================
// GlobalFontLookAndFeel (JUCE 8 compatible)
struct GlobalFontLookAndFeel : juce::LookAndFeel_V4
{
    GlobalFontLookAndFeel()
    {
        // ✅ 1) Intenta usar la MISMA fuente embebida "oficial"
        customTypeface = getEmbeddedPluginTypeface();

        // ✅ 2) Fallback robusto: tu loader por macros + scan
        if (customTypeface == nullptr)
            customTypeface = loadTypefaceFromAssets();
    }

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& f) override
    {
        if (customTypeface != nullptr)
            return customTypeface;

        return juce::LookAndFeel_V4::getTypefaceForFont (f);
    }

private:
    static juce::String sanitizeResourceName (const juce::String& s)
    {
        juce::String out;
        out.preallocateBytes ((size_t) s.getNumBytesAsUTF8());

        for (auto c : s)
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '_')
                out += juce::String::charToString ((juce::juce_wchar) c);
            else
                out += "_";
        }

        if (out.isNotEmpty() && ! (juce::CharacterFunctions::isLetter (out[0]) || out[0] == '_'))
            out = "_" + out;

        return out;
    }

    // ✅ Reemplazado por tu versión robusta + fallback scan
    static juce::Typeface::Ptr loadTypefaceFromAssets()
    {
       #if PLUGIN_HAS_ASSETS && PLUGIN_HAS_FONT
        const juce::String fontFile (PLUGIN_PRIMARY_FONT_FILENAME);
        if (fontFile.isEmpty())
            return {};

        // Ej: "mi_fuente.ttf"
        const auto sanitizedFull = sanitizeResourceName (fontFile); // "mi_fuente_ttf"

        // También probamos sin extensión + con sufijos típicos
        juce::String baseNoExt = fontFile.upToLastOccurrenceOf (".", false, false);
        if (baseNoExt.isEmpty())
            baseNoExt = fontFile;

        const auto sanitizedNoExt = sanitizeResourceName (baseNoExt);

        juce::StringArray candidates;

        // Candidatos más comunes (JUCE/CMake suelen generar algo tipo mi_fuente_ttf)
        candidates.add (sanitizedFull);
        candidates.add ("_" + sanitizedFull);
        candidates.add ("f_" + sanitizedFull);

        // Si PLUGIN_PRIMARY_FONT_FILENAME vino sin extensión, probamos sufijos
        candidates.add (sanitizedNoExt + "_ttf");
        candidates.add (sanitizedNoExt + "_otf");
        candidates.add ("_" + sanitizedNoExt + "_ttf");
        candidates.add ("_" + sanitizedNoExt + "_otf");

        // Variantes lower
        candidates.add (sanitizedFull.toLowerCase());
        candidates.add ("_" + sanitizedFull.toLowerCase());
        candidates.add ("f_" + sanitizedFull.toLowerCase());
        candidates.add ((sanitizedNoExt + "_ttf").toLowerCase());
        candidates.add ((sanitizedNoExt + "_otf").toLowerCase());

        for (auto name : candidates)
        {
            int dataSize = 0;
            if (auto* data = BinaryData::getNamedResource (name.toRawUTF8(), dataSize))
            {
                if (dataSize > 0)
                    return juce::Typeface::createSystemTypefaceFor (data, (size_t) dataSize);
            }
        }

        // Fallback: escanea TODOS los recursos y agarra el primero que parezca la fuente
        // (esto ayuda cuando el nombre real en BinaryData no coincide con tus macros)
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            const juce::String resName (BinaryData::namedResourceList[i]);

            const bool looksLikeFont =
                resName.containsIgnoreCase (sanitizedNoExt) &&
                (resName.containsIgnoreCase ("ttf") || resName.containsIgnoreCase ("otf"));

            if (! looksLikeFont)
                continue;

            int dataSize = 0;
            if (auto* data = BinaryData::getNamedResource (resName.toRawUTF8(), dataSize))
            {
                if (dataSize > 0)
                    return juce::Typeface::createSystemTypefaceFor (data, (size_t) dataSize);
            }
        }
       #endif

        return {};
    }

    juce::Typeface::Ptr customTypeface;
};


struct SimpleKnobLookAndFeel : juce::LookAndFeel_V4
{
    juce::Colour trackColour  = juce::Colour::fromRGB (45, 45, 45);
    juce::Colour valueColour  = juce::Colour::fromRGB (90, 255, 130);
    float thickness = 0.14f;

    void drawRotarySlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPosProportional,
                           float rotaryStartAngle,
                           float rotaryEndAngle,
                           juce::Slider&) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (6.0f);

        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float lineW  = juce::jmax (2.0f, radius * thickness);
        const float angle  = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        juce::Path arc;
        arc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(),
                           radius - lineW * 0.5f, radius - lineW * 0.5f,
                           0.0f, rotaryStartAngle, rotaryEndAngle, true);

        g.setColour (trackColour);
        g.strokePath (arc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path valueArc;
        valueArc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(),
                                radius - lineW * 0.5f, radius - lineW * 0.5f,
                                0.0f, rotaryStartAngle, angle, true);

        g.setColour (valueColour);
        g.strokePath (valueArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // ✅ Eliminado: marcador/punto (fillEllipse) para que quede solo rueda + llenado
    }
};

// ✅ LabeledKnob: permite PNG encima del knob sin romper lo actual
struct LabeledKnob : juce::Component
{
    juce::Label  label;
    juce::Slider slider;

    // imagen opcional como "label" encima del knob
    juce::Image labelImage;
    juce::ImageComponent imageComp;

    // ✅ Dejar defaults como estaban (para no reescalar raro el PNG)
    int pngTopH  = 16;
    int textTopH = 14;

    explicit LabeledKnob (const juce::String& name)
    {
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);

        // fallback label más pequeño si no hay PNG
        label.setFont (juce::Font (juce::FontOptions (9.0f)));

        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setMouseDragSensitivity (140);

        imageComp.setInterceptsMouseClicks (false, false);
        imageComp.setVisible (false); // por defecto NO hay PNG

        addAndMakeVisible (label);
        addAndMakeVisible (imageComp);
        addAndMakeVisible (slider);
    }

    void setLabelSlotHeights (int pngTop, int textTop)
    {
        pngTopH  = juce::jlimit (6, 48, pngTop);
        textTopH = juce::jlimit (6, 48, textTop);
        resized();
        repaint();
    }

    void setLabelImage (juce::Image img)
    {
        labelImage = img;
        const bool hasImg = labelImage.isValid();

        imageComp.setVisible (hasImg);
        label.setVisible (!hasImg);

        if (hasImg)
        {
            imageComp.setImage (labelImage);
            imageComp.setImagePlacement (juce::RectanglePlacement::centred
                                       | juce::RectanglePlacement::onlyReduceInSize);
        }
        else
        {
            imageComp.setImage (juce::Image());
        }

        resized();
        repaint();
    }

    // ✅ Reemplazado por el resized() que pediste (baseline + 0.75 real, gap 1px, PNG sin reduced)
    void resized() override
    {
        auto r = getLocalBounds();

        const bool hasImg = imageComp.isVisible();
        const int topH = hasImg ? pngTopH : textTopH;

        // 1) Slot superior (PNG o texto)
        auto top = r.removeFromTop (topH);

        if (hasImg)
            imageComp.setBounds (top);              // <-- NO lo reduzco: no lo agranda ni lo deforma
        else
            label.setBounds (top);

        // 2) Gap mínimo (PEGADO al knob)
        r.removeFromTop (1);                        // <-- casi pegado

        // 3) Mantén tu "baseline" anterior para el knob (esto es clave)
        auto knobBase = r.reduced (16, 14);

        // 4) ...y recién ahí lo hacemos 25% más chico (0.75x)
        const int baseSide = juce::jmin (knobBase.getWidth(), knobBase.getHeight());
        const int side     = juce::jmax (10, juce::roundToInt (baseSide * 0.75f));

        // 5) Alineación: centrado en X y "pegado arriba" dentro de knobBase
        auto knobBounds = juce::Rectangle<int> (0, 0, side, side)
                            .withCentre (juce::Point<int> (knobBase.getCentreX(),
                                                           knobBase.getY() + side / 2));

        slider.setBounds (knobBounds);
    }
};

} // namespace ui

} // namespace plugin



