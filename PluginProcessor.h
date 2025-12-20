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

    // Smoothers
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> toneSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSm;

    // Tilt EQ (pre)
    juce::dsp::IIR::Filter<float> lowShelfL, lowShelfR;
    juce::dsp::IIR::Filter<float> highShelfL, highShelfR;

    // Oversampling
    static constexpr int kOversamplingExponent = 3; // 8x
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    juce::AudioBuffer<float> wetBuffer;

    // Preset activo (stateful)
    const PresetRegistry::Item* activePreset = nullptr;
    int activePresetIndex = -1;

    using PresetStateStorage =
        std::aligned_storage_t<PresetRegistry::kMaxStateSize, PresetRegistry::kMaxStateAlign>;
    std::array<PresetStateStorage, 2> presetState {}; // hasta estéreo

    plugin::LevelMatcher levelMatch;

    // ✅ Nuevo: interacción estéreo PRO para oversampled
    StereoInteract stereoInteract;

    double sr = 48000.0;
    float  osSr = 384000.0f; // sr * oversamplingFactor (se recalcula en prepareToPlay)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (YourPluginAudioProcessor)
};

