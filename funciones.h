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
    // IMPORTANTE:
    // - Este es un "safety" FINAL para evitar hard-clamp (que genera aliasing).
    // - 100% lineal dentro de [-1, +1].
    // - Si |x| > 1, solo satura el "exceso" con una curva continua (C1) y con
    //   pendiente 1 en el umbral (no cambia el volumen en el borde).
    //
    // Curva:
    //   si |x| <= 1  -> x
    //   si |x| > 1   -> sign(x) * ( 1 + excess/(1+excess) )
    //
    // Nota: limita suavemente hacia ±2 (suficiente como "safety" para floats).
    const float ax = std::abs (x);
    if (ax <= 1.0f)
        return x;

    const float s = (x >= 0.0f ? 1.0f : -1.0f);
    const float excess = ax - 1.0f;
    const float softExcess = excess / (1.0f + excess); // suave, continuo, d/dx=1 en excess=0
    return s * (1.0f + softExcess);
}

// Drive 0..1 -> dB de "input drive" real (musical, no lineal)
//
// Requerimiento PRO:
// - Rango realista (0..30 dB típicamente 0..24/30).
// - Mucho control fino al inicio y más agresión al final.
// - Curva tipo potencia/log sin "explosión" repentina.
//
// Nota: esto solo mapea la perilla a dB.
// El carácter/etapas del drive se implementan en el bloque no-lineal (DriveSaturator).
inline float mapDriveDb (float drive01) noexcept
{
    const float x = juce::jlimit (0.0f, 1.0f, drive01);

    // potencia: fino al inicio, más decisión al final
    // (p > 1 => más resolución en valores bajos)
    constexpr float p = 2.35f;

    // pequeño componente log para que la parte media se sienta "continua"
    const float logCurve = std::log1p (9.0f * x) / std::log1p (9.0f); // 0..1

    const float shaped = 0.78f * std::pow (x, p) + 0.22f * logCurve;

    // rango real de input drive
    constexpr float maxDb = 30.0f;
    return maxDb * shaped;
}

//==============================================================================
// Tone 0..1 -> Tilt dB (neutral EXACTO en 0.5, pivot musical)
//
// Requerimiento PRO:
// - En el centro (0.5) NO hace nada.
// - Tilt tipo consola (no lowpass aburrido).
// - Curva dinámica: en el centro casi plano, hacia extremos más efecto.
//
// Nota:
// - La ubicación del pivot/frecuencias la define el procesador (filtros).
// - Aquí solo calculamos el tilt en dB.
inline float mapToneTiltDb (float tone01) noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, tone01);

    // signed: -0.5..+0.5 (izq..der)
    float s = t - 0.5f;

    // dead-zone alrededor del centro ("neutral real")
    constexpr float dead = 0.055f; // ~5.5% del recorrido total
    if (std::abs (s) <= dead * 0.5f)
        return 0.0f;

    const float sign = (s >= 0.0f ? 1.0f : -1.0f);
    const float mag  = (std::abs (s) - dead * 0.5f) / (0.5f - dead * 0.5f); // 0..1

    // curva musical (suave al inicio, más al final)
    constexpr float curve = 1.60f;
    const float a = std::pow (juce::jlimit (0.0f, 1.0f, mag), curve);

    // rango tilt (±12 dB)
    constexpr float maxDb = 12.0f;

    // convención: derecha = oscuro (tilt NEGATIVO), izquierda = brillante (tilt POSITIVO)
    return (sign > 0.0f) ? (-maxDb * a) : (maxDb * a);
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
//
// Objetivo: que el volumen se mantenga constante al mover cualquier knob, incluso con
// tamaños de buffer variables (FL Studio, etc.). Este modo NO depende del block-size:
// - Mide potencia (HP) de ENTRADA (dry alineado) y SALIDA (mixed PRE gain).
// - Mantiene envs RMS (EMA) con ventana configurable (ms).
// - Calcula targetGain = sqrt(inEnv/outEnv) con gate + clamp.
// - Suaviza la ganancia con attack/release por muestra.
// - Cuando no hay señal útil, vuelve lentamente a unity.
//
// Nota importante:
//   La medición de salida debe hacerse ANTES de aplicar la ganancia compensatoria
//   para evitar "auto-cancelación".
class AutoGainExact
{
public:
    void prepare (double sampleRate)
    {
        sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);

        // Rango amplio para que realmente compense presets/drive agresivos
        setClampDb (-60.0f, +24.0f);

        // ✅ Ajustes "full tiempo real" (más reactivo):
        // - gate más bajo para no "esperar" a que el RMS suba
        // - ventana de medición mucho más corta (el 'momento' que sentías venía de aquí)
        // - smoothing de ganancia rápido (sube/baja en pocos ms)
        //
        // Nota: estos valores son deliberadamente agresivos.
        // Si llegas a oír bombeo en material muy dinámico, sube measurementWindowMs
        // a 20–40ms y/o releaseMs a 10–30ms.
        setGateDb  (-90.0f);

        // Ventana de medición (RMS EMA).
        // 12ms ≈ comportamiento prácticamente instantáneo sin depender del tamaño de bloque.
        setMeasurementWindowMs (12.0f);

        // Suavizado de la GANANCIA (no de la medición):
        // attack: baja rápido cuando el preset/drive sube nivel
        // release: sube rápido para mantener el nivel constante
        setGainSmoothingMs (0.30f, 6.0f);

        // Vuelve a unity en silencio (más rápido, evita quedarse "pegado")
        setReturnToUnityMs (220.0f);

        // HP para no dejar que DC/subgrave dicte el autogain
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

    // Procesa una muestra estéreo y devuelve la ganancia ACTUAL (ya suavizada)
    inline float processStereo (float dryL, float dryR, float outLpre, float outRpre) noexcept
    {
        const float inL  = measureHP (dryL,    0, in_hp_x1,  in_hp_y1);
        const float inR  = measureHP (dryR,    1, in_hp_x1,  in_hp_y1);
        const float outL = measureHP (outLpre, 0, out_hp_x1, out_hp_y1);
        const float outR = measureHP (outRpre, 1, out_hp_x1, out_hp_y1);

        const float pIn  = 0.5f * (inL  * inL  + inR  * inR);
        const float pOut = 0.5f * (outL * outL + outR * outR);

        // RMS envs
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
            // vuelve a unity cuando no hay señal útil
            targetGain = returnAlpha * targetGain + (1.0f - returnAlpha) * 1.0f;
        }

        const bool needDown = (targetGain < compGain);
        const float a = needDown ? gainAttackAlpha : gainReleaseAlpha;
        compGain = a * compGain + (1.0f - a) * targetGain;

        return compGain;
    }

    // --- ajustes ---
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
        // 1-pole HP: y[n] = a*(y[n-1] + x[n] - x[n-1])
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






