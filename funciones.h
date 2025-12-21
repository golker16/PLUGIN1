#pragma once

// funciones.h (PRO)
// Utilidades compartidas: mezcla, drive map, soft clip, level matching, UI.

#include <JuceHeader.h>
#include <cmath>

namespace plugin
{
//==============================================================================
// Soft clip final "safety"
inline float softClipSafety (float x) noexcept
{
    const float k = 1.6f;
    return std::tanh (k * x) / std::tanh (k);
}

// Drive 0..1 -> dB de pregain
inline float mapDriveDb (float drive01) noexcept
{
    const float d = juce::jlimit (0.0f, 1.0f, drive01);
    const float shaped = std::pow (d, 0.65f);
    return 30.0f * shaped; // 0..30 dB
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
// AutoGainExact (v2): match RMS "percibido" (HP) por bloque entre ENTRADA y SALIDA.
//
// Objetivo: que el volumen NO cambie aunque cambien Drive/Tone/Mix/Preset.
// - Se mide una versión filtrada (HP) de la señal para evitar que subgraves/DC engañen.
// - Se ajusta una ganancia compensatoria con attack/release por BLOQUE.
class AutoGainExact
{
public:
    void prepare (double sampleRate)
    {
        sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

        // Para distorsión casi siempre quieres atenuar mucho y subir poquito.
        // Para que REALMENTE no suba/baje al mover knobs, necesitas permitir más rango.
        // (Si lo dejas en +6 dB, cualquier preset/drive que suba más que eso jamás se compensará.)
        setClampDb (-60.0f, +18.0f);
        setGateDb  (-70.0f);

        // Responde rápido a picos/subidas (baja), y más lento al recuperar (sube).
        setSmoothingMs      (2.5f, 140.0f);
        setReturnToUnityMs  (650.0f);

        // Medición: HP para que DC/subgrave no engañe, pero no tan alto que ignore el cuerpo.
        // 60 Hz suele dar un "volumen percibido" más estable cuando mueves Tone.
        setMeasureHighpassHz (60.0f);

        reset();
    }

    void reset()
    {
        compGain   = 1.0f;
        targetGain = 1.0f;

        in_hp_x1[0] = in_hp_x1[1] = 0.0f;
        in_hp_y1[0] = in_hp_y1[1] = 0.0f;
        out_hp_x1[0] = out_hp_x1[1] = 0.0f;
        out_hp_y1[0] = out_hp_y1[1] = 0.0f;
    }

    // Ganancia actual (la que debes aplicar al audio en este bloque)
    float getGain() const noexcept { return compGain; }

    // Medición por muestra (HP) con estados separados para entrada y salida
    inline float measureIn (float x, int ch) noexcept  { return measureHP (x, ch, in_hp_x1,  in_hp_y1);  }
    inline float measureOut (float x, int ch) noexcept { return measureHP (x, ch, out_hp_x1, out_hp_y1); }

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

    void setSmoothingMs (float newAttackMs, float newReleaseMs)
    {
        attackMs  = juce::jmax (0.1f, newAttackMs);
        releaseMs = juce::jmax (0.1f, newReleaseMs);
    }

    void setReturnToUnityMs (float ms)
    {
        returnMs = juce::jmax (1.0f, ms);
    }

    void setMeasureHighpassHz (float hz)
    {
        const float fc = juce::jlimit (5.0f, 1000.0f, hz);
        hpA = std::exp (-2.0f * juce::MathConstants<float>::pi * (fc / (float) sr));
    }

    // Actualiza la ganancia para el PRÓXIMO bloque.
    // inPow/outPow deben venir ya promediadas (potencia media del bloque).
    float updateFromBlockPowers (double inPow, double outPow, int numSamples)
    {
        numSamples = juce::jmax (1, numSamples);

        const bool gateOk = (inPow > (double) gatePow) && (outPow > (double) gatePow);

        if (gateOk)
        {
            const double ratio = inPow / (outPow + 1.0e-20);
            float g = (float) std::sqrt (ratio);

            g = juce::jlimit (minGain, maxGain, g);
            targetGain = g;
        }
        else
        {
            // vuelve a 1.0 suavemente cuando no hay señal útil
            const float aRet = alphaFromMsBlock (returnMs, numSamples);
            targetGain = aRet * targetGain + (1.0f - aRet) * 1.0f;
        }

        const bool needDown = (targetGain < compGain);
        const float a = alphaFromMsBlock (needDown ? attackMs : releaseMs, numSamples);

        compGain = a * compGain + (1.0f - a) * targetGain;
        return compGain;
    }

private:
    // alpha correcto cuando actualizas 1 vez por BLOQUE (no por sample)
    float alphaFromMsBlock (float ms, int numSamples) const
    {
        const float tau = juce::jmax (1.0e-4f, ms * 0.001f);      // segundos
        const float dt  = (float) numSamples / (float) sr;        // segundos por bloque
        return std::exp (-dt / tau);
    }

    inline float measureHP (float x, int ch, float* x1, float* y1) noexcept
    {
        // 1-pole HP: y[n] = a*(y[n-1] + x[n] - x[n-1])
        const float y = hpA * (y1[ch] + x - x1[ch]);
        x1[ch] = x;
        y1[ch] = y;
        return y;
    }

    double sr = 48000.0;

    float minGain = juce::Decibels::decibelsToGain (-48.0f);
    float maxGain = juce::Decibels::decibelsToGain (+6.0f);

    float gatePow = juce::Decibels::decibelsToGain (-70.0f) * juce::Decibels::decibelsToGain (-70.0f);

    float attackMs  = 2.5f;
    float releaseMs = 140.0f;
    float returnMs  = 650.0f;

    float compGain   = 1.0f;
    float targetGain = 1.0f;

    // HP states separados para entrada/salida
    float hpA = 0.98f;
    float in_hp_x1[2]  = { 0.0f, 0.0f };
    float in_hp_y1[2]  = { 0.0f, 0.0f };
    float out_hp_x1[2] = { 0.0f, 0.0f };
    float out_hp_y1[2] = { 0.0f, 0.0f };
};

//==============================================================================
// UI helpers
namespace ui
{
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

    // ✅ 2.1: alturas configurables por instancia
    int pngTopH  = 16;
    int textTopH = 14;

    explicit LabeledKnob (const juce::String& name)
    {
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);

        // fallback label más pequeño si no hay PNG
        label.setFont (juce::Font (9.0f));

        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setMouseDragSensitivity (140);

        imageComp.setInterceptsMouseClicks (false, false);
        imageComp.setVisible (false); // por defecto NO hay PNG

        addAndMakeVisible (label);
        addAndMakeVisible (imageComp);
        addAndMakeVisible (slider);
    }

    // ✅ 2.1: setter para controlar alto del "slot" por knob
    void setLabelSlotHeights (int pngTop, int textTop)
    {
        pngTopH  = juce::jlimit (6, 48, pngTop);   // mínimo 6 (más chico)
        textTopH = juce::jlimit (6, 48, textTop);
        resized();
        repaint();
    }

    // Pone un PNG encima del knob
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
            imageComp.setImage (juce::Image()); // limpia
        }

        resized();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();

        // ✅ 2.1: usa tamaños configurables
        const int topH = imageComp.isVisible() ? pngTopH : textTopH;

        if (imageComp.isVisible())
            imageComp.setBounds (r.removeFromTop (topH).reduced (1, 2));
        else
            label.setBounds (r.removeFromTop (topH));

        // ✅ 2.2: slider interno más chico (moderado y consistente)
        slider.setBounds (r.reduced (16, 14));
    }
};
} // namespace ui

} // namespace plugin

