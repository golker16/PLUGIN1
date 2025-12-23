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
    void updateTiltCoeffs (float tone01);

    // ✅ Helpers para "Tone" sin clicks (rampa/interpolación de coeficientes)
    void calcTiltCoeffArrays (float tone01, std::array<float, 6>& low, std::array<float, 6>& high);
    void beginTiltRamp (float tone01, int rampSamples);
    inline void tickTiltRamp() noexcept;

    // Oversampling selector (0=x1, 1=x2, 2=x4, 3=x8)
    int  getDesiredOversamplingIndex() const noexcept;
    void ensureOversamplers (int numChannels);
    void initOversamplers (int maxBlockSize);
    void applyOversamplingIndex (int newIndex, bool force);

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
    std::atomic<float>* pDrive  = nullptr;
    std::atomic<float>* pTone   = nullptr;
    std::atomic<float>* pMix    = nullptr;
    std::atomic<float>* pPreamp = nullptr;

    std::atomic<float>* pOS     = nullptr;

    // ✅ NUEVO: AutoGain ON/OFF + Output (dB)
    std::atomic<float>* pAutoGain = nullptr;
    std::atomic<float>* pOutputDb = nullptr;

    // Smoothers
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> toneSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSm;

    // ✅ NUEVO: Output gain (lineal) + crossfade AutoGain (0..1)
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGainSm;      // ganancia lineal
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> autoGainBlendSm;   // 0..1 (crossfade ON/OFF)

    // Tilt EQ (pre)
    juce::dsp::IIR::Filter<float> lowShelfL, lowShelfR;
    juce::dsp::IIR::Filter<float> highShelfL, highShelfR;

    // ✅ NUEVO: estado para "Tone" sin clicks (rampa/interpolación de coeficientes)
    static constexpr int kTiltUpdateStride = 32;

    std::array<float, 6> tiltLowCur  {}, tiltLowTgt  {}, tiltLowStep  {};
    std::array<float, 6> tiltHighCur {}, tiltHighTgt {}, tiltHighStep {};
    int tiltRampRemaining = 0;

    // Oversampling (seleccionable en runtime)
    static constexpr int kMaxOversamplingIndex = 3; // x8
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 4> oversamplers; // [0]=nullptr, [1]=x2, [2]=x4, [3]=x8
    juce::dsp::Oversampling<float>* oversampling = nullptr; // non-owning, apunta al oversampler activo
    int currentOSIndex = 3;
    int oversamplerChannels = 2;
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

    //==============================================================================
    // ✅ (B) Dos banks de estado + punteros active/pending (evita memcpy UB)
    //     Bank A = actual, Bank B = pending durante transición
    std::array<PresetStateStorage, 2> presetStateA {}; // hasta estéreo
    std::array<PresetStateStorage, 2> presetStateB {}; // hasta estéreo

    std::array<bool, 2> presetStateConstructedA {{ false, false }};
    std::array<bool, 2> presetStateConstructedB {{ false, false }};

    std::array<PresetStateStorage, 2>* activeState  = &presetStateA;
    std::array<PresetStateStorage, 2>* pendingState = &presetStateB;

    std::array<bool, 2>* activeConstructed  = &presetStateConstructedA;
    std::array<bool, 2>* pendingConstructed = &presetStateConstructedB;

    // Helpers de acceso (cómodos)
    inline PresetStateStorage& getActiveStateStorage (int ch) noexcept  { return (*activeState)[(size_t) ch]; }
    inline PresetStateStorage& getPendingStateStorage(int ch) noexcept  { return (*pendingState)[(size_t) ch]; }
    inline bool& getActiveConstructed (int ch) noexcept                 { return (*activeConstructed)[(size_t) ch]; }
    inline bool& getPendingConstructed(int ch) noexcept                 { return (*pendingConstructed)[(size_t) ch]; }

    //==============================================================================
    plugin::AutoGainExact autoGain;

    double sr = 48000.0;
    float  osSr = 384000.0f; // sr * oversamplingFactor (se recalcula en prepareToPlay)

    //==============================================================================
    // ✅ (A) Crossfade de preset/OS (estructura de transición)
    bool transitioning = false;
    int  xfadeTotalSamples = 0;   // ej. 10ms en SR base (se calcula en beginTransition)
    int  xfadePosSamples   = 0;   // contador

    int pendingPresetIndex = -1;
    int pendingOSIndex     = -1;

    //==============================================================================
    // ✅ (C) StereoInteract siempre ON (dos instancias)
    StereoInteract stereoA; // activo
    StereoInteract stereoB; // pending durante transición

    //==============================================================================
    // ✅ (D) Buffers extra mínimos para crossfade
    juce::AudioBuffer<float> wetOld; // salida wet de cadena A
    juce::AudioBuffer<float> wetNew; // salida wet de cadena B
    juce::AudioBuffer<float> osTemp; // solo para "mismo OS + cambio preset" en dominio oversampled

    //==============================================================================
    // ✅ (E) Helpers privados (se implementan en .cpp)
    void beginTransition (int newPreset, int newOS, int wetCh, int numSamples);
    void commitTransition (int wetCh);

    // Render de cadenas A/B:
    // - Implementación típica:
    //   * si OS cambia -> render A y B a SR base en wetOld/wetNew
    //   * si OS igual  -> render A y B en oversampled (usando osTemp) y bajar una sola vez
    void renderChainA (juce::AudioBuffer<float>& io, int wetCh, int numSamples);
    void renderChainB (juce::AudioBuffer<float>& io, int wetCh, int numSamples);

    // Gains de crossfade (equal-power)
    inline float xfadeGainA (float t) const noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return std::cos (0.5f * juce::MathConstants<float>::pi * t);
    }

    inline float xfadeGainB (float t) const noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return std::sin (0.5f * juce::MathConstants<float>::pi * t);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (YourPluginAudioProcessor)
};






