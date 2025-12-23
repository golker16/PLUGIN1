// PluginProcessor-2.cpp
#include "PluginProcessor.h"
#include <cstring> // std::memset, std::memcpy
#include <mutex>   // std::once_flag, std::call_once
#include <limits>  // std::numeric_limits

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

    // ✅ NUEVO: AutoGain ON/OFF (default ON)
    params.push_back (std::make_unique<juce::AudioParameterBool>(
        "autogain", "AutoGain", true));

    // ✅ NUEVO: Output (dB) para uso manual cuando AutoGain está OFF
    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        "output", "Output",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
        0.0f));

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

    // ✅ NUEVO
    pAutoGain = apvts.getRawParameterValue ("autogain");
    pOutputDb = apvts.getRawParameterValue ("output");

    // Nota: el setDefaultSansSerifTypeface() queda en PluginProcessor-1.cpp
    // porque ahí vive getEmbeddedPluginTypeface() (UI/assets/typeface).
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

// ❌ createEditor() NO va aquí (vive en PluginProcessor-1.cpp)

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
// Tilt (legacy setter directo)
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
// ✅ Tone sin clicks: helpers rampa/interpolación de coeficientes

void YourPluginAudioProcessor::calcTiltCoeffArrays (float tone01,
                                                    std::array<float, 6>& low,
                                                    std::array<float, 6>& high)
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

    fcLow  = juce::jlimit (90.0f,   650.0f,  fcLow);
    fcHigh = juce::jlimit (1200.0f, 8000.0f, fcHigh);

    constexpr float q = 0.707f;

    auto lowC  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (
        sr, fcLow, q, juce::Decibels::decibelsToGain (-tiltDb));

    auto highC = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
        sr, fcHigh, q, juce::Decibels::decibelsToGain ( tiltDb));

    for (int i = 0; i < 6; ++i)
    {
        low [(size_t) i] = lowC ->coefficients[(size_t) i];
        high[(size_t) i] = highC->coefficients[(size_t) i];
    }
}

void YourPluginAudioProcessor::beginTiltRamp (float tone01, int rampSamples)
{
    const int N = juce::jmax (1, rampSamples);

    calcTiltCoeffArrays (tone01, tiltLowTgt, tiltHighTgt);

    for (int i = 0; i < 6; ++i)
    {
        tiltLowStep [(size_t) i]  = (tiltLowTgt [(size_t) i]  - tiltLowCur [(size_t) i])  / (float) N;
        tiltHighStep[(size_t) i]  = (tiltHighTgt[(size_t) i]  - tiltHighCur[(size_t) i])  / (float) N;
    }

    tiltRampRemaining = N;
}

inline void YourPluginAudioProcessor::tickTiltRamp() noexcept
{
    if (tiltRampRemaining <= 0)
        return;

    for (int i = 0; i < 6; ++i)
    {
        tiltLowCur [(size_t) i]  += tiltLowStep [(size_t) i];
        tiltHighCur[(size_t) i]  += tiltHighStep[(size_t) i];
    }

    --tiltRampRemaining;

    if (tiltRampRemaining == 0)
    {
        // “snap” final para evitar drift por floating error
        tiltLowCur  = tiltLowTgt;
        tiltHighCur = tiltHighTgt;
    }

    // ✅ FIX C2106: NO escribir por índice. Construir Coefficients y asignar completo.
    if (lowShelfL.coefficients != nullptr)
        *lowShelfL.coefficients = juce::dsp::IIR::Coefficients<float> (
            tiltLowCur[0], tiltLowCur[1], tiltLowCur[2],
            tiltLowCur[3], tiltLowCur[4], tiltLowCur[5]);

    if (lowShelfR.coefficients != nullptr)
        *lowShelfR.coefficients = juce::dsp::IIR::Coefficients<float> (
            tiltLowCur[0], tiltLowCur[1], tiltLowCur[2],
            tiltLowCur[3], tiltLowCur[4], tiltLowCur[5]);

    if (highShelfL.coefficients != nullptr)
        *highShelfL.coefficients = juce::dsp::IIR::Coefficients<float> (
            tiltHighCur[0], tiltHighCur[1], tiltHighCur[2],
            tiltHighCur[3], tiltHighCur[4], tiltHighCur[5]);

    if (highShelfR.coefficients != nullptr)
        *highShelfR.coefficients = juce::dsp::IIR::Coefficients<float> (
            tiltHighCur[0], tiltHighCur[1], tiltHighCur[2],
            tiltHighCur[3], tiltHighCur[4], tiltHighCur[5]);
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

    driveSm.setCurrentAndTargetValue (pDrive ? pDrive->load() : 0.25f);
    toneSm .setCurrentAndTargetValue (pTone  ? pTone ->load() : 0.5f);
    mixSm  .setCurrentAndTargetValue (pMix   ? pMix  ->load() : 1.0f);

    // ✅ NUEVO: Output gain + blend autogain
    outputGainSm.reset (sr, 0.02);
    outputGainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pOutputDb ? pOutputDb->load() : 0.0f));

    autoGainBlendSm.reset (sr, 0.01);
    const bool agOn = (pAutoGain != nullptr && pAutoGain->load() >= 0.5f);
    autoGainBlendSm.setCurrentAndTargetValue (agOn ? 1.0f : 0.0f);

    lowShelfL.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 180.0f, 0.707f, 1.0f);
    lowShelfR.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 180.0f, 0.707f, 1.0f);
    highShelfL.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3800.0f, 0.707f, 1.0f);
    highShelfR.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 3800.0f, 0.707f, 1.0f);

    updateTiltCoeffs (pTone ? pTone->load() : 0.5f);

    // ✅ Inicializar arrays de coef para ramp
    auto read6 = [] (const juce::dsp::IIR::Coefficients<float>& c, std::array<float, 6>& dst)
    {
        for (int i = 0; i < 6; ++i)
            dst[(size_t) i] = c.coefficients[(size_t) i];
    };

    if (lowShelfL.coefficients != nullptr)  read6 (*lowShelfL.coefficients,  tiltLowCur);
    if (highShelfL.coefficients != nullptr) read6 (*highShelfL.coefficients, tiltHighCur);

    tiltLowTgt = tiltLowCur;
    tiltHighTgt = tiltHighCur;
    tiltLowStep.fill (0.0f);
    tiltHighStep.fill (0.0f);
    tiltRampRemaining = 0;

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
                                    (int) std::lround (pPreamp->load()));

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
void YourPluginAudioProcessor::beginTransition (int newPreset, int newOS, int wetCh, int /*numSamples*/)
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

    // targets de smoothers
    driveSm.setTargetValue (pDrive ? pDrive->load() : 0.25f);
    toneSm .setTargetValue (pTone  ? pTone ->load() : 0.5f);
    mixSm  .setTargetValue (pMix   ? pMix  ->load() : 1.0f);

    // ✅ AutoGain ON/OFF + Output targets
    const bool agOn = (pAutoGain != nullptr && pAutoGain->load() >= 0.5f);
    autoGainBlendSm.setTargetValue (agOn ? 1.0f : 0.0f);

    const float outDb = (pOutputDb != nullptr ? pOutputDb->load() : 0.0f);
    outputGainSm.setTargetValue (juce::Decibels::decibelsToGain (outDb));

    // Selección de preset deseado
    int desiredPreset = 0;
    if (pPreamp != nullptr && PresetRegistry::items.size() > 0)
        desiredPreset = juce::jlimit (0, (int) PresetRegistry::items.size() - 1,
                                      (int) std::lround (pPreamp->load()));

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
    // 0) Tone smoothing + tilt SIN clicks (rampa por coef, stride=32)
    if (wetBuffer.getNumChannels() != wetCh || wetBuffer.getNumSamples() < numSamples)
        wetBuffer.setSize (wetCh, numSamples, false, false, true);

    auto* wetL = wetBuffer.getWritePointer (0);
    auto* wetR = (wetCh > 1) ? wetBuffer.getWritePointer (1) : nullptr;

    int strideLeft = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float toneVal = toneSm.getNextValue();

        if (strideLeft <= 0)
        {
            const int rampN = juce::jmin (kTiltUpdateStride, numSamples - i);
            beginTiltRamp (toneVal, rampN);
            strideLeft = kTiltUpdateStride;
        }

        tickTiltRamp();
        --strideLeft;

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
                if ((int) osCh > 0)
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
                    else if (osCh >= 1)
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
                    else if (osCh >= 1)
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
    // 3) Mix + AUTO-LEVEL (por muestra) + Output manual cuando AutoGain OFF
    const float safetyHeadroom = juce::Decibels::decibelsToGain (-0.9f);
    const float maxAutoGain    = juce::Decibels::decibelsToGain (+12.0f);

    auto* dL = dryDelayBuffer.getWritePointer (0);
    auto* dR = (dryDelayBuffer.getNumChannels() > 1) ? dryDelayBuffer.getWritePointer (1) : nullptr;

    const int size = dryDelayBuffer.getNumSamples();

    constexpr float epsBlend = 1.0e-5f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix01  = mixSm.getNextValue();
        const float blend  = autoGainBlendSm.getNextValue();  // 0..1
        const float gManual = outputGainSm.getNextValue();    // lineal

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

        const bool useAuto = (blend > epsBlend);

        // Si mix ~ 0, la entrada útil es dry
        if (mix01 <= 1.0e-4f)
        {
            float gAuto = 1.0f;
            if (useAuto)
            {
                gAuto = autoGain.processStereo (dryL, dryR, dryL, dryR);
                gAuto = juce::jlimit (0.0f, maxAutoGain, gAuto);
            }

            const float gFinal = blend * gAuto + (1.0f - blend) * gManual;
            const float head   = (1.0f - blend) + blend * safetyHeadroom;

            float outL = dryL * gFinal * head;
            float outR = dryR * gFinal * head;

            if (useAuto)
            {
                outL = plugin::softClipSafety (outL);
                outR = plugin::softClipSafety (outR);
            }

            ch0[i] = outL;
            if (ch1 != nullptr)
                ch1[i] = outR;

            continue;
        }

        const float mixedL = plugin::equalPowerMix (dryL, wetOutL, mix01);
        const float mixedR = plugin::equalPowerMix (dryR, wetOutR, mix01);

        float gAuto = 1.0f;
        if (useAuto)
        {
            gAuto = autoGain.processStereo (dryL, dryR, mixedL, mixedR);
            gAuto = juce::jlimit (0.0f, maxAutoGain, gAuto);
        }

        const float gFinal = blend * gAuto + (1.0f - blend) * gManual;
        const float head   = (1.0f - blend) + blend * safetyHeadroom;

        float outL = mixedL * gFinal * head;
        float outR = mixedR * gFinal * head;

        if (useAuto)
        {
            outL = plugin::softClipSafety (outL);
            outR = plugin::softClipSafety (outR);
        }

        ch0[i] = outL;
        if (ch1 != nullptr)
            ch1[i] = outR;
    }
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new YourPluginAudioProcessor();
}
