#pragma once

// funciones.h (PRO)
// Utilidades compartidas: mezcla, drive map, soft clip, level matching, UI.

#include <JuceHeader.h>
#include <cmath>
#include <complex>   // ✅ (A) requerido para mapeo de biquads (zeros/poles) entre sample rates

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
// (A) ✅ Knobs: estructura para pasar controles a los presets
struct Knobs
{
    // Tone 1: carácter (izquierda = agresivo, derecha = suavecito) 0..1
    float tone1_01 = 0.5f;

    // Tone 2: color (izquierda = dark, derecha = bright) 0..1
    float tone2_01 = 0.5f;

    // Mix: 0..1
    float mix01   = 1.0f;
};

//==============================================================================
// Helper clamp01 (para mapping bipolar)
inline float clamp01 (float x) noexcept
{
    return juce::jlimit (0.0f, 1.0f, x);
}

//==============================================================================
// Soft clip final "safety"
inline float softClipSafety (float x) noexcept
{
    constexpr float kneeStart = 0.98f;
    constexpr float one       = 1.0f;

    const float ax = std::abs (x);
    if (ax <= kneeStart)
        return x;

    const float sign = (x >= 0.0f ? 1.0f : -1.0f);

    const float kneeRange = juce::jmax (1.0e-6f, one - kneeStart);
    const float t = ax - kneeStart;

    const float scaled = juce::MathConstants<float>::halfPi * (t / kneeRange);
    const float soft   = (2.0f / juce::MathConstants<float>::pi) * std::atan (scaled);

    const float y = kneeStart + kneeRange * soft;
    return sign * juce::jlimit (0.0f, 1.0f, y);
}

//==============================================================================
// (B) ✅ Drive/Tone2 0..1 -> dB de pregain (BIPOLAR) (invertido como pediste)
// - IZQUIERDA  (s<0) => agresivo (+dB)
// - DERECHA    (s>=0)=> relajado (-dB)
inline float mapDriveDb (float x01) noexcept
{
    x01 = clamp01 (x01);
    const float s = x01 - 0.5f;
    const float a = std::fabs (s) * 2.0f;
    const float a2 = a * a;

    const float maxAggDb   = 36.0f; // izquierda agresivo
    const float maxRelaxDb = 12.0f; // derecha relajado

    if (s < 0.0f)
        return +maxAggDb * a2;

    return -maxRelaxDb * a2;
}

//==============================================================================
// (C) ✅ Tone 0..1 -> Tilt dB (invertido como pediste)
// - DERECHA (s>0) => BRIGHT (+dB)
// - IZQUIERDA (s<=0) => DARK (-dB)
inline float mapToneTiltDb (float t01) noexcept
{
    t01 = clamp01 (t01);
    const float s = t01 - 0.5f;
    const float a = std::fabs (s) * 2.0f;

    const float maxBrightDb = 5.0f; // derecha
    const float maxDarkDb   = 6.0f; // izquierda

    if (s > 0.0f)
        return +maxBrightDb * a;

    return -maxDarkDb * a;
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
// AutoGainExact (v4 MASTERING): K-weighting + ventana momentary (~400ms) + gate con hold
//
// - K-weighting real (2 biquads): shelf + HP (coef base ITU a 48k)
// - Recalcula coef para cualquier SR via mapeo z/p -> s (inverse bilinear @48k) -> z (bilinear @SR)
// - Ventana tipo momentary: EMA con tau=400ms (IIR cercano)
// - Gate loudness style -70 dB + HOLD 150 ms para evitar “respirar” en pausas
class AutoGainExact
{
public:
    void prepare (double sampleRate)
    {
        sr = (sampleRate > 1000.0 ? sampleRate : 48000.0);
        // defaults “match rápido” (más agresivo que el perfil mastering)
        // Objetivo: que al mover Drive/Preamp el match se note al toque.
        setClampDb (-24.0f, +24.0f);
        setGateDb  (-90.0f);              // gate muy bajo: casi siempre mide
        setGateHoldMs (80.0f);            // hold corto
        setMeasurementWindowMs (120.0f);  // ventana más rápida que 400ms

        // smoothing más reactivo
        setGainSmoothingMs (12.0f, 120.0f);
        setReturnToUnityMs (450.0f);

        // preparar K-weighting (coef dependientes de SR)
        inK.prepare (sr);
        outK.prepare (sr);

        reset();
    }

    void reset()
    {
        inEnv  = 0.0f;
        outEnv = 0.0f;

        compGain   = 1.0f;
        targetGain = 1.0f;

        gateHoldRemaining = 0;

        inK.reset();
        outK.reset();
    }

    float getGain() const noexcept { return compGain; }

    inline float processStereo (float dryL, float dryR, float outLpre, float outRpre) noexcept
    {
        // 1) K-weighting aplicado a DRY y a OUT (pre makeup)
        const float inL  = inK.process (dryL,    0);
        const float inR  = inK.process (dryR,    1);
        const float outL = outK.process (outLpre, 0);
        const float outR = outK.process (outRpre, 1);

        // 2) energía (power) y ventana momentary (EMA)
        const float pIn  = 0.5f * (inL  * inL  + inR  * inR);
        const float pOut = 0.5f * (outL * outL + outR * outR);

        inEnv  = measAlpha * inEnv  + (1.0f - measAlpha) * pIn;
        outEnv = measAlpha * outEnv + (1.0f - measAlpha) * pOut;

        const bool gateOk = (inEnv > gatePow) && (outEnv > gatePow);

        if (gateOk)
        {
            // refresca hold
            gateHoldRemaining = gateHoldSamples;

            // objetivo = sqrt(in/out)
            const float ratio = inEnv / (outEnv + 1.0e-20f);
            float g = std::sqrt (ratio);
            g = juce::jlimit (minGain, maxGain, g);
            targetGain = g;
        }
        else
        {
            // hold: no “respires” con silencios/pausas cortas
            if (gateHoldRemaining > 0)
            {
                --gateHoldRemaining;
                // targetGain se mantiene (no vuelve a unity todavía)
            }
            else
            {
                // return to unity suave
                targetGain = returnAlpha * targetGain + (1.0f - returnAlpha) * 1.0f;
            }
        }

        // smoothing attack/release
        const bool needDown = (targetGain < compGain);
        const float a = needDown ? gainAttackAlpha : gainReleaseAlpha;
        compGain = a * compGain + (1.0f - a) * targetGain;

        return compGain;
    }

    // ---------------------- setters ----------------------
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

    void setGateHoldMs (float ms)
    {
        const float t = juce::jmax (0.0f, ms) * 0.001f;
        gateHoldSamples = (int) std::lround (t * (double) sr);
        gateHoldSamples = juce::jmax (0, gateHoldSamples);
        gateHoldRemaining = juce::jmin (gateHoldRemaining, gateHoldSamples);
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

private:
    //==============================================================================
    // Util: alpha para 1-pole EMA por muestra
    inline float alphaFromMsSample (float ms) const noexcept
    {
        const float tau = juce::jmax (1.0e-4f, ms * 0.001f);
        return std::exp (-1.0f / (tau * (float) sr));
    }

    //==============================================================================
    // K-Weighting filter (2 biquads) con mapeo de coef desde SR=48k -> SR actual
    struct KWeightingFilter
    {
        struct Coeff
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0;
            double a1 = 0.0, a2 = 0.0; // denom: 1 + a1 z^-1 + a2 z^-2
        };

        struct Biquad2ch
        {
            void set (const Coeff& c)
            {
                b0 = (float) c.b0; b1 = (float) c.b1; b2 = (float) c.b2;
                a1 = (float) c.a1; a2 = (float) c.a2;
            }

            void reset()
            {
                x1[0]=x1[1]=0.0f; x2[0]=x2[1]=0.0f;
                y1[0]=y1[1]=0.0f; y2[0]=y2[1]=0.0f;
            }

            inline float process (float x, int ch) noexcept
            {
                // DF1:
                // y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
                const float y = (b0 * x) + (b1 * x1[ch]) + (b2 * x2[ch])
                              - (a1 * y1[ch]) - (a2 * y2[ch]);

                x2[ch] = x1[ch];
                x1[ch] = x;
                y2[ch] = y1[ch];
                y1[ch] = y;
                return y;
            }

            float b0=1.0f,b1=0.0f,b2=0.0f,a1=0.0f,a2=0.0f;
            float x1[2]{}, x2[2]{}, y1[2]{}, y2[2]{};
        };

        void prepare (double sampleRate)
        {
            fs = (sampleRate > 1000.0 ? sampleRate : 48000.0);

            // Coef base ITU a 48k (lo que pediste)
            // Stage 1 (shelf)
            const Coeff shelf48 {
                1.53512485958697, -2.69169618940638, 1.19839281085285,
               -1.69065929318241,  0.73248077421585
            };

            // Stage 2 (HP)
            const Coeff hp48 {
                1.0, -2.0, 1.0,
               -1.99004745483398, 0.99007225036621
            };

            Coeff shelfSR{}, hpSR{};
            mapFrom48kToFs (shelf48, fs, shelfSR);
            mapFrom48kToFs (hp48,    fs, hpSR);

            shelf.set (shelfSR);
            hp.set (hpSR);

            reset();
        }

        void reset()
        {
            shelf.reset();
            hp.reset();
        }

        inline float process (float x, int ch) noexcept
        {
            x = shelf.process (x, ch);
            x = hp.process (x, ch);
            return x;
        }

    private:
        static constexpr double fs0 = 48000.0;

        static inline std::array<std::complex<double>, 2> quadRoots (double A, double B, double C)
        {
            // A z^2 + B z + C = 0
            const std::complex<double> disc = std::sqrt (std::complex<double> (B*B - 4.0*A*C, 0.0));
            const std::complex<double> twoA = 2.0 * A;
            return { (-B + disc) / twoA, (-B - disc) / twoA };
        }

        static inline std::complex<double> evalH (const Coeff& c, double w)
        {
            // z = e^{jw}, z^{-1} = e^{-jw}
            const std::complex<double> zinv = std::exp (std::complex<double> (0.0, -w));
            const std::complex<double> zinv2 = zinv * zinv;

            const std::complex<double> num = c.b0 + c.b1 * zinv + c.b2 * zinv2;
            const std::complex<double> den = 1.0 + c.a1 * zinv + c.a2 * zinv2;

            return num / den;
        }

        static inline std::complex<double> invBilinear (std::complex<double> z, double fs)
        {
            // s = 2fs * (z - 1)/(z + 1)
            const std::complex<double> one (1.0, 0.0);
            return (2.0 * fs) * (z - one) / (z + one);
        }

        static inline std::complex<double> fwdBilinear (std::complex<double> s, double fs)
        {
            // z = (1 + s/(2fs)) / (1 - s/(2fs))
            const std::complex<double> one (1.0, 0.0);
            const std::complex<double> k = s / (2.0 * fs);
            return (one + k) / (one - k);
        }

        static inline void mapFrom48kToFs (const Coeff& c48, double fsNew, Coeff& out)
        {
            // 1) zeros/poles digitales @48k
            const auto zZeros48 = quadRoots (c48.b0, c48.b1, c48.b2);
            const auto zPoles48 = quadRoots (1.0,    c48.a1, c48.a2);

            // 2) map to analog s via inverse bilinear (fs0)
            const std::complex<double> sZ0 = invBilinear (zZeros48[0], fs0);
            const std::complex<double> sZ1 = invBilinear (zZeros48[1], fs0);
            const std::complex<double> sP0 = invBilinear (zPoles48[0], fs0);
            const std::complex<double> sP1 = invBilinear (zPoles48[1], fs0);

            // 3) map back to digital z via bilinear (fsNew)
            const std::complex<double> zZ0 = fwdBilinear (sZ0, fsNew);
            const std::complex<double> zZ1 = fwdBilinear (sZ1, fsNew);
            const std::complex<double> zP0 = fwdBilinear (sP0, fsNew);
            const std::complex<double> zP1 = fwdBilinear (sP1, fsNew);

            // 4) construir denom real: 1 + a1 z^-1 + a2 z^-2
            const std::complex<double> sumP = zP0 + zP1;
            const std::complex<double> prodP = zP0 * zP1;

            out.a1 = -(sumP.real());
            out.a2 =  (prodP.real());

            // 5) construir num (sin ganancia aún)
            const std::complex<double> sumZ = zZ0 + zZ1;
            const std::complex<double> prodZ = zZ0 * zZ1;

            out.b0 = 1.0;
            out.b1 = -(sumZ.real());
            out.b2 =  (prodZ.real());

            // 6) ajustar ganancia para match con la respuesta original (referencia)
            // usamos 1kHz, pero si SR es muy baja, bajamos la referencia.
            double fRef = 1000.0;
            const double nyqNew = 0.5 * fsNew;
            if (fRef > 0.35 * nyqNew)
                fRef = 0.25 * fsNew; // bien debajo de Nyquist

            const double w0 = 2.0 * juce::MathConstants<double>::pi * (fRef / fs0);
            const double w1 = 2.0 * juce::MathConstants<double>::pi * (fRef / fsNew);

            const double mag48  = std::abs (evalH (c48, w0));
            const double magTmp = std::abs (evalH (out, w1));

            const double k = (magTmp > 1.0e-18 ? (mag48 / magTmp) : 1.0);

            out.b0 *= k;
            out.b1 *= k;
            out.b2 *= k;
        }

        double fs = 48000.0;
        Biquad2ch shelf;
        Biquad2ch hp;
    };

    //==============================================================================
    double sr = 48000.0;

    float minGain = juce::Decibels::decibelsToGain (-60.0f);
    float maxGain = juce::Decibels::decibelsToGain (+24.0f);

    float gatePow = juce::Decibels::decibelsToGain (-70.0f) * juce::Decibels::decibelsToGain (-70.0f);

    float measAlpha = 0.999f;
    float inEnv  = 0.0f;
    float outEnv = 0.0f;

    float gainAttackAlpha  = 0.99f;
    float gainReleaseAlpha = 0.9995f;
    float returnAlpha      = 0.9999f;

    float compGain   = 1.0f;
    float targetGain = 1.0f;

    int gateHoldSamples    = 0;
    int gateHoldRemaining  = 0;

    KWeightingFilter inK;
    KWeightingFilter outK;
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
            imageComp.setBounds (top);              // <-- NO lo reduzco
        else
            label.setBounds (top);

        // 2) Gap mínimo (PEGADO al knob)
        r.removeFromTop (1);

        // 3) Mantén tu "baseline" anterior para el knob
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
