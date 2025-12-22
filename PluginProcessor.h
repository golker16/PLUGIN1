#pragma once

#include <JuceHeader.h>
#include <array>
#include <type_traits>
#include <cstring>
#include <cmath>
#include "funciones.h"
#include "PresetRegistry.h"

class YourPluginAudioProcessor : public juce::AudioProcessor
{
public:
    YourPluginAudioProcessor();
    ~YourPluginAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;

    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    void updateToneCoeffs (float tone01, float drive01);
    void rebuildOversampling (int newExponent);

    //==============================================================================
    // ✅ Anti-zipper PRO: smoothing + slew limiter dependiente de magnitud
    struct SmartSmoother
    {
        void prepare (double sampleRateHz, float fastMs_, float slowMs_, float slewPerSecond_)
        {
            sr = (sampleRateHz > 1000.0 ? (float) sampleRateHz : 48000.0f);
            fastMs = juce::jlimit (0.05f, 50.0f, fastMs_);
            slowMs = juce::jlimit (0.10f, 200.0f, slowMs_);
            slewPerSecond = juce::jmax (0.01f, slewPerSecond_);
        }

        void reset (float v) noexcept
        {
            current = v;
        }

        inline float process (float target) noexcept
        {
            const float t = target;
            const float diff = t - current;
            const float ad = std::abs (diff);

            // delta pequeño => más rápido
            const float w = juce::jlimit (0.0f, 1.0f, ad / 0.35f);
            const float ms = fastMs + (slowMs - fastMs) * w;
            const float tau = juce::jmax (1.0e-4f, ms * 0.001f);
            const float a = std::exp (-1.0f / (tau * sr));
            const float oneMinusA = 1.0f - a;

            // smoothing
            float y = a * current + oneMinusA * t;

            // slew limiter (sensación hardware)
            const float maxDelta = slewPerSecond / sr;
            const float d2 = y - current;
            y = current + juce::jlimit (-maxDelta, maxDelta, d2);

            current = y;
            return current;
        }

        float getCurrent() const noexcept { return current; }

        float sr = 48000.0f;
        float fastMs = 3.0f;
        float slowMs = 28.0f;
        float slewPerSecond = 4.0f; // unidades 0..1 por segundo
        float current = 0.0f;
    };

    //==============================================================================
    // ✅ Drive multi-etapa (3D) en oversampled
    // - Preamp soft sat
    // - Etapa más dura (soft clip)
    // - Transformer-ish (asimetría + compresión dependiente de nivel)
    // - Pre-emphasis/de-emphasis + softening de highs con drive
    // - DC blocker al final (sin DC)
    struct DriveSaturator
    {
        struct Params
        {
            float drive01 = 0.0f;
            float k1 = 1.0f;
            float k2 = 1.0f;
            float k3 = 1.0f;
            float stage2Mix = 0.0f;   // 0..1
            float xfMix = 0.0f;       // 0..1
            float bias = 0.0f;        // asimetría
            float emph = 0.0f;        // pre/de emphasis
            float compAmt = 0.0f;     // comp dependiente de nivel
            float hfSoft = 0.0f;      // suavizado highs
        };

        void prepare (float sampleRateHz) noexcept
        {
            sr = (sampleRateHz > 1000.0f ? sampleRateHz : 192000.0f);

            // pre/de emphasis (1-pole LP para derivar componente alta)
            emphA_in  = alphaFromHz (1800.0f);
            emphA_out = alphaFromHz (1800.0f);

            // envelope para transformer-ish comp (~8ms)
            envA = alphaFromMs (8.0f);

            // DC blocker (R cerca de 1)
            dcR = 0.9955f;

            reset();
        }

        void reset() noexcept
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                lpIn[ch] = 0.0f;
                lpOut[ch] = 0.0f;
                env[ch] = 0.0f;
                hfLP[ch] = 0.0f;
                dc_x1[ch] = 0.0f;
                dc_y1[ch] = 0.0f;
                tanh_x1[ch] = 0.0f;
                tanh_x2[ch] = 0.0f;
                atan_x1[ch] = 0.0f;
            }
        }
        }

        static inline Params makeParams (float drive01) noexcept
        {
            Params p;
            p.drive01 = juce::jlimit (0.0f, 1.0f, drive01);

            // Etapas (crossfade interno):
            // stage2 empieza más tarde (cuando le pegas)
            const float d = p.drive01;
            auto smoothstep = [](float a, float b, float x)
            {
                const float t = juce::jlimit (0.0f, 1.0f, (x - a) / (b - a));
                return t * t * (3.0f - 2.0f * t);
            };

            p.stage2Mix = smoothstep (0.42f, 0.95f, d);
            p.xfMix     = smoothstep (0.25f, 0.85f, d);

            // ganancias internas por etapa (no solo "más dist")
            p.k1 = 1.0f + 7.0f * d;                    // preamp soft
            p.k2 = 1.0f + 10.0f * (d * d);             // etapa dura
            p.k3 = 1.0f + 5.5f * p.xfMix;              // transformer-ish

            // asimetría (armónicos pares) sube con drive
            p.bias = 0.07f * std::pow (d, 1.35f);

            // pre/de emphasis para densidad sin raspado
            p.emph = 0.55f * std::pow (d, 1.10f);

            
            // Limita la pre-emphasis cuando el drive es muy alto (reduce alias/fizz)
            p.emph *= (1.0f - 0.35f * smoothstep (0.70f, 1.00f, d));
// comp leve dependiente de nivel (solo cuando drive sube)
            p.compAmt = 0.35f * std::pow (d, 1.30f);

            // softening highs para evitar "digital fizz" con drive alto
            p.hfSoft = 0.22f * std::pow (d, 1.70f);

            return p;
        }


        // ---------------------------------------------------------------------
        // ADAA helpers (anti-alias)
        //
        // - ADAA2 para tanh(): reduce aliasing significativamente con poco costo.
        // - ADAA1 para atan(): buen balance CPU/calidad.
        //
        // Referencias: ADAA (Antiderivative Anti-Aliasing) para no-linealidades.
        // Implementación: segunda diferencia dividida de G(x), con fallback estable.
        //
        // f_tanh(x) = tanh(kx)/k
        // F_tanh(x) = ∫ f_tanh dx = log(cosh(kx)) / k^2
        // G_tanh(x) = ∫ F_tanh dx = (∫ log(cosh(u)) du)|_{u=kx} / k^3
        //
        // Para G0(t)=∫_0^t log(cosh(u)) du (función impar), usamos:
        // log(cosh(u)) = |u| - ln(2) + log(1 + exp(-2|u|))
        // log(1 + exp(-2v)) = Σ (-1)^{n+1} exp(-2nv)/n
        // => ∫ log(1 + exp(-2v)) dv = Σ (-1)^{n+1} (1 - exp(-2nv)) / (2 n^2)
        //
        // Truncamos la serie (N términos). Con N=8 es estable y suficiente para audio.
        static inline float logCoshStable (float t) noexcept
        {
            const float v = std::abs (t);
            // v + log1p(exp(-2v)) - ln(2) (estable para v grande)
            return v + std::log1p (std::exp (-2.0f * v)) - 0.6931471805599453094f;
        }

        static inline float intLogCoshStable (float t) noexcept
        {
            const float s = (t >= 0.0f ? 1.0f : -1.0f);
            const float v = std::abs (t);

            // I(v) = 0.5 v^2 - v ln(2) + Σ_{n=1..N} (-1)^{n+1} (1 - exp(-2nv)) / (2 n^2)
            float I = 0.5f * v * v - v * 0.6931471805599453094f;

            // Serie alternante: converge muy rápido porque exp(-2nv)
            constexpr int N = 8;
            for (int n = 1; n <= N; ++n)
            {
                const float sign = (n & 1) ? 1.0f : -1.0f; // (-1)^{n+1}
                const float nf = (float) n;
                const float term = (1.0f - std::exp (-2.0f * nf * v)) / (2.0f * nf * nf);
                I += sign * term;
            }

            return s * I; // función impar
        }

        inline float F_tanh (float x, float k) const noexcept
        {
            const float kk = juce::jmax (1.0e-6f, k);
            const float t = kk * x;
            return logCoshStable (t) / (kk * kk);
        }

        inline float G_tanh (float x, float k) const noexcept
        {
            const float kk = juce::jmax (1.0e-6f, k);
            const float t = kk * x;
            return intLogCoshStable (t) / (kk * kk * kk);
        }

        inline float f_tanh (float x, float k) const noexcept
        {
            const float kk = juce::jmax (1.0e-6f, k);
            return std::tanh (kk * x) / kk;
        }

        inline float adaa2Tanh (float x, int ch, float k) noexcept
        {
            constexpr float eps = 1.0e-6f;

            const float x1 = tanh_x1[ch];
            const float x2 = tanh_x2[ch];

            // precompute G
            const float G0 = G_tanh (x,  k);
            const float G1 = G_tanh (x1, k);
            const float G2 = G_tanh (x2, k);

            const float d1 = x  - x1;
            const float d0 = x1 - x2;
            const float d2 = x  - x2;

            float y = 0.0f;

            if (std::abs (d1) > eps && std::abs (d0) > eps && std::abs (d2) > eps)
            {
                const float dd1 = (G0 - G1) / d1;
                const float dd0 = (G1 - G2) / d0;
                y = 2.0f * (dd1 - dd0) / d2;
            }
            else
            {
                // fallback: ADAA1 (F) o directo
                const float denom = d1;
                if (std::abs (denom) > eps)
                    y = (F_tanh (x, k) - F_tanh (x1, k)) / denom;
                else
                    y = f_tanh (x, k);
            }

            // shift history
            tanh_x2[ch] = tanh_x1[ch];
            tanh_x1[ch] = x;

            return y;
        }

        // f_atan(x) = atan(kx)/k
        // F_atan(x) = ∫ f_atan dx
        //          = ( (u*atan(u) - 0.5 ln(1+u^2)) / k^2 ), u = kx
        inline float F_atan (float x, float k) const noexcept
        {
            const float kk = juce::jmax (1.0e-6f, k);
            const float u = kk * x;
            return (u * std::atan (u) - 0.5f * std::log (1.0f + u * u)) / (kk * kk);
        }

        inline float f_atan (float x, float k) const noexcept
        {
            const float kk = juce::jmax (1.0e-6f, k);
            return std::atan (kk * x) / kk;
        }

        inline float adaa1Atan (float x, int ch, float k) noexcept
        {
            constexpr float eps = 1.0e-6f;

            const float x1 = atan_x1[ch];
            const float denom = x - x1;

            float y = 0.0f;
            if (std::abs (denom) > eps)
                y = (F_atan (x, k) - F_atan (x1, k)) / denom;
            else
                y = f_atan (x, k);

            atan_x1[ch] = x;
            return y;
        }


        inline float processSample (float x, int ch, const Params& p) noexcept
        {
            // -----------------------------
            // 0) Pre-emphasis (high lift suave): x + emph*(x - LP(x))
            lpIn[ch] = onePoleLP (lpIn[ch], x, emphA_in);
            const float hiIn = x - lpIn[ch];
            float s = x + p.emph * hiIn;

            // -----------------------------
            // 1) Preamp soft sat (tanh con pendiente 1)
            const float y1 = adaa2Tanh (s, ch, p.k1);

            // 2) Etapa más dura (atan con pendiente 1)
            const float y2 = adaa1Atan (y1, ch, p.k2);
            float y = y1 + p.stage2Mix * (y2 - y1);

            // -----------------------------
            // 3) Transformer-ish: asimetría + comp dependiente de nivel
            // envelope rápido
            const float a = std::abs (y);
            env[ch] = envA * env[ch] + (1.0f - envA) * a;
            const float comp = 1.0f / (1.0f + p.compAmt * env[ch]);

            const float xb = y + p.bias * (1.0f + 0.45f * y);
            const float yt = (std::tanh (p.k3 * xb) / p.k3) * comp;

            y = y + p.xfMix * (yt - y);

            // -----------------------------
            // 4) De-emphasis (tame highs): y - emph*(y - LP(y))
            lpOut[ch] = onePoleLP (lpOut[ch], y, emphA_out);
            const float hiOut = y - lpOut[ch];
            y = y - (0.85f * p.emph) * hiOut;

            // 5) Softening highs (blend to LP)
            hfLP[ch] = onePoleLP (hfLP[ch], y, hfA);
            y = y + p.hfSoft * (hfLP[ch] - y);

            // 6) DC blocker
            const float dc = y - dc_x1[ch] + dcR * dc_y1[ch];
            dc_x1[ch] = y;
            dc_y1[ch] = dc;
            return dc;
        }

        void setHighSoftHz (float hz) noexcept
        {
            hfA = alphaFromHz (juce::jlimit (1200.0f, 20000.0f, hz));
        }

    private:
        inline float alphaFromHz (float fc) const noexcept
        {
            constexpr float twoPi = 6.2831853071795864769f;
            const float safeSr = (sr > 1.0f ? sr : 1.0f);
            return std::exp (-twoPi * (fc / safeSr));
        }

        inline float alphaFromMs (float ms) const noexcept
        {
            const float tau = juce::jmax (1.0e-4f, ms * 0.001f);
            const float safeSr = (sr > 1.0f ? sr : 1.0f);
            return std::exp (-1.0f / (tau * safeSr));
        }

        inline float onePoleLP (float y1, float x, float a) const noexcept
        {
            return (1.0f - a) * x + a * y1;
        }

        float sr = 192000.0f;

        // emphasis helpers
        float emphA_in  = 0.98f;
        float emphA_out = 0.98f;
        float lpIn[2]   = { 0.0f, 0.0f };
        float lpOut[2]  = { 0.0f, 0.0f };

        // transformer env
        float envA = 0.999f;
        float env[2] = { 0.0f, 0.0f };

        // high softening LP
        float hfA = 0.995f;
        float hfLP[2] = { 0.0f, 0.0f };

        // dc blocker
        float dcR = 0.9955f;
        float dc_x1[2] = { 0.0f, 0.0f };
        float dc_y1[2] = { 0.0f, 0.0f };

        // ADAA history (por canal)
        float tanh_x1[2] = { 0.0f, 0.0f };
        float tanh_x2[2] = { 0.0f, 0.0f };
        float atan_x1[2] = { 0.0f, 0.0f };
    };

    //==============================================================================
    // ✅ StereoInteract (PRO): crosstalk freq+nivel + micro allpass (fase/latencia muy leve)
    //
    // - Crosstalk dependiente de frecuencia:
    //     inyecta principalmente "mid band" (LP_hi - LP_lo) del canal opuesto
    //     y un poquito de low (LP) -> menos highs, menos fizz/IMD.
    //
    // - Crosstalk dependiente de nivel:
    //     amount sube suavemente con el nivel (envelope RMS rápido en oversampled).
    //
    // - Micro allpass:
    //     1er orden, coef ligeramente distinto L/R y ligeramente dependiente de nivel,
    //     para sensación "consola/analógica" (sin chorusing ni combs evidentes).
    struct StereoInteract
    {
        void prepare (float sampleRateHz) noexcept
        {
            sr = (sampleRateHz > 1000.0f ? sampleRateHz : 192000.0f);

            // envelope (nivel) ~ 12ms
            envAlpha = alphaFromMs (12.0f);

            // filtros para banda mid (aprox 350Hz..2.2kHz)
            setMidBandHz (2200.0f, 350.0f);

            // low para un poco de "glue" (más realista que fullband)
            lowAlpha = alphaFromHz (160.0f);

            // allpass micro (muy sutil)
            apBase = 0.62f;     // 0..1, más cerca de 1 => más fase/retardo
            apVar  = 0.010f;    // variación por nivel (muy pequeña)

            reset();
        }

        void reset() noexcept
        {
            envP = 0.0f;

            // Mid band LPs por canal (para generar band)
            midHiLP[0] = midHiLP[1] = 0.0f;
            midLoLP[0] = midLoLP[1] = 0.0f;

            lowLP[0] = lowLP[1] = 0.0f;

            // Allpass states (x1,y1) por canal
            ap_x1[0] = ap_x1[1] = 0.0f;
            ap_y1[0] = ap_y1[1] = 0.0f;
        }

        void setMidBandHz (float hiHz, float loHz) noexcept
        {
            midHiAlpha = alphaFromHz (juce::jlimit (600.0f, 6000.0f, hiHz));
            midLoAlpha = alphaFromHz (juce::jlimit (80.0f,  1200.0f, loHz));
        }

        // Procesa IN-PLACE una muestra estéreo oversampled
        inline void processSample (float& xL, float& xR) noexcept
        {
            // -------------------------
            // 1) Nivel (para amount dependiente de drive/saturación)
            const float p = 0.5f * (xL * xL + xR * xR);
            envP = envAlpha * envP + (1.0f - envAlpha) * p;

            // level01: curva suave
            const float rms = std::sqrt (envP + 1.0e-12f);
            float level01 = rms * 1.25f;                  // calibración heurística
            level01 = juce::jlimit (0.0f, 1.0f, level01);

            // amount base (muy sutil) + sube con nivel
            const float ctBase = 0.0012f;
            const float ctMax  = 0.0032f;
            const float ctAmt  = ctBase + (ctMax - ctBase) * (level01 * level01);

            // -------------------------
            // 2) Crosstalk dependiente de frecuencia
            // midBandOther = LP_hi(other) - LP_lo(other)
            // lowOther     = LP_low(other)
            const float midOtherFromR = midBandFrom (xR, 1);
            const float midOtherFromL = midBandFrom (xL, 0);

            const float lowOtherFromR = lowFrom (xR, 1);
            const float lowOtherFromL = lowFrom (xL, 0);

            // pesos: más mids, un poco de lows
            const float wMid = 1.0f;
            const float wLow = 0.55f;

            const float injL = (wMid * midOtherFromR + wLow * lowOtherFromR);
            const float injR = (wMid * midOtherFromL + wLow * lowOtherFromL);

            xL = xL + ctAmt * injL;
            xR = xR + ctAmt * injR;

            // -------------------------
            // 3) Micro allpass (fase) con tiny diferencia L/R
            // coef base + variación por nivel (opuesta entre canales)
            float aL = apBase + apVar * level01;
            float aR = apBase - apVar * level01;

            aL = juce::jlimit (0.10f, 0.95f, aL);
            aR = juce::jlimit (0.10f, 0.95f, aR);

            xL = allpass1 (xL, 0, aL);
            xR = allpass1 (xR, 1, aR);
        }

    private:
        inline float alphaFromHz (float fc) const noexcept
        {
            constexpr float twoPi = 6.2831853071795864769f;
            const float safeSr = (sr > 1.0f ? sr : 1.0f);
            return std::exp (-twoPi * (fc / safeSr));
        }

        inline float alphaFromMs (float ms) const noexcept
        {
            const float tau = juce::jmax (1.0e-4f, ms * 0.001f);
            const float safeSr = (sr > 1.0f ? sr : 1.0f);
            return std::exp (-1.0f / (tau * safeSr));
        }

        inline float onePoleLP (float y1, float x, float a) const noexcept
        {
            return (1.0f - a) * x + a * y1;
        }

        inline float midBandFrom (float x, int ch) noexcept
        {
            midHiLP[ch] = onePoleLP (midHiLP[ch], x, midHiAlpha);
            midLoLP[ch] = onePoleLP (midLoLP[ch], x, midLoAlpha);
            return (midHiLP[ch] - midLoLP[ch]);
        }

        inline float lowFrom (float x, int ch) noexcept
        {
            lowLP[ch] = onePoleLP (lowLP[ch], x, lowAlpha);
            return lowLP[ch];
        }

        inline float allpass1 (float x, int ch, float a) noexcept
        {
            // 1st-order allpass:
            // y = -a*x + x1 + a*y1
            const float y = (-a * x) + ap_x1[ch] + (a * ap_y1[ch]);
            ap_x1[ch] = x;
            ap_y1[ch] = y;
            return y;
        }

        float sr = 192000.0f;

        // Nivel
        float envAlpha = 0.999f;
        float envP = 0.0f;

        // Band shaping para crosstalk
        float midHiAlpha = 0.98f;
        float midLoAlpha = 0.995f;
        float lowAlpha   = 0.999f;

        float midHiLP[2] = { 0.0f, 0.0f };
        float midLoLP[2] = { 0.0f, 0.0f };
        float lowLP[2]   = { 0.0f, 0.0f };

        // Allpass
        float apBase = 0.62f;
        float apVar  = 0.010f;
        float ap_x1[2] = { 0.0f, 0.0f };
        float ap_y1[2] = { 0.0f, 0.0f };
    };

    //==============================================================================
    // Parameter pointers
    std::atomic<float>* pDrive   = nullptr;
    std::atomic<float>* pTone    = nullptr;
    std::atomic<float>* pMix     = nullptr;
    std::atomic<float>* pPreamp  = nullptr;
    std::atomic<float>* pQuality = nullptr;

    // Anti-zipper PRO
    SmartSmoother driveSm;
    SmartSmoother toneSm;
    SmartSmoother mixSm;

    // Tone Tilt (pre)
    juce::dsp::IIR::Filter<float> lowShelfL, lowShelfR;
    juce::dsp::IIR::Filter<float> highShelfL, highShelfR;

    // Tone Tilt (post) - para domar harshness cuando hay drive
    juce::dsp::IIR::Filter<float> lowShelfPostL, lowShelfPostR;
    juce::dsp::IIR::Filter<float> highShelfPostL, highShelfPostR;

    // Oversampling (solo bloque no lineal). Exponente: 1=2x, 2=4x, 3=8x
    int osExponent = 2; // default 4x
    int osExponentPrepared = -1;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    juce::AudioBuffer<float> wetBuffer;

    // En algunos hosts (incl. FL Studio) el tamaño de bloque puede variar.
    // Guardamos el máximo inicial para poder re-inicializar buffers/OS si crece.
    int maxBlockSizePrepared = 0;

    // ✅ 3.1) Dry delay para alinear con latencia del oversampling (solo necesario si usas MIX)
    juce::AudioBuffer<float> dryDelayBuffer;
    int dryDelayWritePos   = 0;
    int dryDelaySamples    = 0;
    int dryDelayBufferSize = 0;

    // Preset activo (stateful)
    const PresetRegistry::Item* activePreset = nullptr;
    int activePresetIndex = -1;

    using PresetStateStorage =
        std::aligned_storage_t<PresetRegistry::kMaxStateSize, PresetRegistry::kMaxStateAlign>;
    std::array<PresetStateStorage, 2> presetState {}; // hasta estéreo
    std::array<bool, 2> presetStateConstructed {{ false, false }};

    plugin::AutoGainExact autoGain;

    // ✅ Nuevo: interacción estéreo PRO para oversampled
    StereoInteract stereoInteract;

    // ✅ Nuevo: Drive multi-etapa (3D)
    DriveSaturator driveSat;

    // ✅ Nuevo: LP pre-distorsión dependiente de drive (en dominio oversampled)
    std::array<float, 2> preShaperLPz {{ 0.0f, 0.0f }};

    // ✅ Nuevo: LP anti-alias final (en dominio oversampled, antes de downsample)
    std::array<float, 2> osAaLPz {{ 0.0f, 0.0f }};


    double sr = 48000.0;
    float  osSr = 384000.0f; // sr * oversamplingFactor (se recalcula en prepareToPlay)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (YourPluginAudioProcessor)
};

