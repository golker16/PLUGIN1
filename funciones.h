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
// Mejoras:
//  - Medición por POTENCIA estéreo (L^2 + R^2) (sin cancelación por fase)
//  - HPF solo para la medición (más perceptual, menos dependencia de graves/tone)
//  - "Return-to-unity" lento cuando el gate no se cumple (evita subir ruido)
class LevelMatcher
{
public:
    void prepare (double sampleRate)
    {
        sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

        setMeasurementWindowMs (160.0f);      // más reactivo que 300ms, menos “lag”
        setGainSmoothingMs (8.0f, 120.0f);    // baja rápido si te pasas, sube suave
        setGateDb (-60.0f);
        setClampDb (-18.0f, +18.0f);          // un poco más margen para drive fuerte
        setMeasureHighpassHz (120.0f);        // clave para que tone/low-end no “mueva” el match
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

        // HP states (medición)
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

    // Cuando no hay señal (gate), targetGain vuelve lentamente hacia 1.0
    void setReturnToUnityMs (float ms)
    {
        returnAlpha = alphaFromMs (ms);
    }

    // HPF 1er orden SOLO para medición (no afecta audio)
    void setMeasureHighpassHz (float hz)
    {
        const float fc = juce::jlimit (5.0f, 1000.0f, hz);
        hpA = std::exp (-2.0f * juce::MathConstants<float>::pi * (fc / (float) sr));
    }

    // Mantengo tu API mono por compat
    float process (float drySample, float mixedSample)
    {
        // trata el mono como estéreo linkeado (L=R)
        return processStereo (drySample, drySample, mixedSample, mixedSample);
    }

    // ✅ Nuevo: match estéreo correcto (sin cancelación por fase)
    float processStereo (float dryL, float dryR, float outL, float outR)
    {
        // Medición ponderada (HPF) solo para el detector
        const float mdL = measureHP (dryL, 0);
        const float mdR = measureHP (dryR, 1);
        const float moL = measureHP (outL, 0);
        const float moR = measureHP (outR, 1);

        // Potencia estéreo linkeada (promedio de potencias)
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
            // sin señal: vuelve lento a unity para no “quedarte” amplificando ruido
            targetGain = returnAlpha * targetGain + (1.0f - returnAlpha) * 1.0f;
        }

        // Suavizado de ganancia (baja rápido, sube más lento)
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
        // 1er orden HP: y = a*(y1 + x - x1)
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

    // HPF detector states
    float hpA = 0.98f;
    float hp_x1[2] = { 0.0f, 0.0f };
    float hp_y1[2] = { 0.0f, 0.0f };

    float compGain = 1.0f;
    float targetGain = 1.0f;
};

//==============================================================================
// AutoGainExact: match RMS por bloque (dry vs mixed) + clamp + smoothing.
// - "Exacto" en el sentido de RMS/energía: mover Drive/Tone/Mix no cambia nivel.
// - Gate: si hay silencio, no persigue ruido (return-to-unity).
class AutoGainExact
{
public:
    void prepare (double sampleRate)
    {
        sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

        setClampDb (-12.0f, +12.0f);
        setGateDb (-65.0f);
        setSmoothingMs (4.0f, 80.0f);   // baja rápido, sube suave
        setReturnToUnityMs (500.0f);

        reset();
    }

    void reset()
    {
        compGain   = 1.0f;
        targetGain = 1.0f;
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

    void setSmoothingMs (float attackMs, float releaseMs)
    {
        attackAlpha  = alphaFromMs (attackMs);
        releaseAlpha = alphaFromMs (releaseMs);
    }

    void setReturnToUnityMs (float ms)
    {
        returnAlpha = alphaFromMs (ms);
    }

    // Calcula/actualiza el gain desde energía por bloque.
    // dryPow / mixedPow son POTENCIAS medias (o sumas, ratio es el mismo).
    float updateFromBlockPowers (double dryPow, double mixedPow)
    {
        const bool gateOk = (dryPow > (double) gatePow) && (mixedPow > (double) gatePow);

        if (gateOk)
        {
            const double ratio = dryPow / (mixedPow + 1.0e-20);
            float g = (float) std::sqrt (ratio);

            g = juce::jlimit (minGain, maxGain, g);
            targetGain = g;
        }
        else
        {
            // sin señal -> vuelve lento a unity
            targetGain = returnAlpha * targetGain + (1.0f - returnAlpha) * 1.0f;
        }

        // smoothing: si baja, ataque; si sube, release
        const bool needDown = (targetGain < compGain);
        const float a = needDown ? attackAlpha : releaseAlpha;
        compGain = a * compGain + (1.0f - a) * targetGain;

        return compGain;
    }

private:
    float alphaFromMs (float ms) const
    {
        const float tau = juce::jmax (1.0e-4f, ms * 0.001f);
        return std::exp (-1.0f / (tau * (float) sr));
    }

    double sr = 48000.0;

    float minGain = juce::Decibels::decibelsToGain (-12.0f);
    float maxGain = juce::Decibels::decibelsToGain (+12.0f);

    float gatePow = juce::Decibels::decibelsToGain (-65.0f)
                  * juce::Decibels::decibelsToGain (-65.0f);

    float attackAlpha  = 0.999f;
    float releaseAlpha = 0.9995f;
    float returnAlpha  = 0.9999f;

    float compGain = 1.0f;
    float targetGain = 1.0f;
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
        // auto c = bounds.getCentre();
        // juce::Point<float> p (c.x + std::cos (angle) * (radius - lineW),
        //                       c.y + std::sin (angle) * (radius - lineW));
        // g.fillEllipse (p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f);
    }
};

struct LabeledKnob : juce::Component
{
    juce::Label  label;
    juce::Slider slider;

    // ✅ Nuevo: imagen opcional como "label" encima del knob
    juce::Image labelImage;
    juce::ImageComponent imageComp;

    explicit LabeledKnob (const juce::String& name)
    {
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);

        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setMouseDragSensitivity (140);

        imageComp.setInterceptsMouseClicks (false, false);
        imageComp.setVisible (false); // por defecto NO hay PNG

        addAndMakeVisible (label);
        addAndMakeVisible (imageComp);
        addAndMakeVisible (slider);
    }

    // Llama a esto para poner un PNG encima del knob
    void setLabelImage (juce::Image img)
    {
        labelImage = img;
        const bool hasImg = labelImage.isValid();

        imageComp.setVisible (hasImg);
        label.setVisible (!hasImg);

        if (hasImg)
        {
            imageComp.setImage (labelImage, juce::RectanglePlacement::centred);
        }

        resized();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();

        // Más espacio arriba si hay imagen
        const int topH = imageComp.isVisible() ? 26 : 18;

        if (imageComp.isVisible())
            imageComp.setBounds (r.removeFromTop (topH).reduced (2));
        else
            label.setBounds (r.removeFromTop (topH));

        // Knob un poco más pequeño visualmente:
        slider.setBounds (r.reduced (6));
    }
};
} // namespace ui

} // namespace plugin
