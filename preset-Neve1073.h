#pragma once

// preset-Neve1073.h (PRO ULTIMATE + HQ NEXT LEVEL + CAL/DRIFT/AP/DE-ESS)
// Diseñado para correr DENTRO de oversampling (4x o más). Ideal: 96k -> 384k interno.
//
// Ya tenías (HQ+):
//  1) Pre-edge LP real (anti-fizz) antes de amp1
//  2) Trafo más real: leak en magnetización + "eddy loss" por derivada
//  3) Slew limiting sutil en path de amp
//  4) ADAA2 SOLO en amp stage 1 (tanh)
//  5) Loading más real: LP dinámico + dip suave 18–30k
//  6) Tolerancias por canal (±1–3%) determinísticas
//  7) Sag más creíble: afecta drive/bias/softness
//  8) Detector perceptual interno: HPF para medición
//  9) Robustez: anti-denorm, NaN armor, clamps
//
// NUEVO (implementado en este archivo):
//  A) Calibración headroom real: -18 dBFS ≈ 0 VU (kInCal / kOutCal)
//  B) Asimetría “real”: dependiente de nivel + drift lento con leak (sin DC)
//  C) Allpass “3D glue” (fase) post-trafo OUT y antes de loading (2 etapas)
//  D) Dynamic HF de-esser ultra rápido (por eventos) que modula preEdge Fc
//
// ✅ IMPLEMENTADO AHORA:
//  1) Diffusion Bus paralelo (ambiente sin reverb): allpass cascade + feedback + damping
//  2) “Phase tilt” extra SOLO en el wet (más profundidad sin subir mix)
//  5) 0 noise real: drift determinístico (sin RNG) + anti-denorm determinístico
//
// ✅ PATCH “KNOBS MÁS AUDIBLES + 1073-ish” (este mensaje):
//  1) Smoothing + curvas audio taper para Tone1/Tone2 (más viaje sin zipper)
//  2) Macro-coherencia: Tone1/Tone2 mueven MÁS cosas (sag/even/de-esser/rangos)
//  3) Diffusion Bus mucho más sensible a Tone1 + smear más profundo
//  4) Air shelf tipo 1073 (sutil pero MUY audible) controlado por Tone2

#include "funciones.h"

#include <cmath>
#include <cstdint>
#include <limits>

struct Preset_Neve1073
{
    static constexpr const char* kDisplayName = "Neve 1073";

    static constexpr const char* kKnobBehavior =
        "Tone 2 (izq=dark, der=bright) altera: preEdge, de-esser, de-fizz y aire (air shelf 1073). "
        "Tone 1 (izq=agresivo, der=suavecito) cambia carácter y ‘espacio’ SIN cambiar ganancia. "
        "Incluye Diffusion Bus paralelo controlado por Tone1/Tone2. Knobs smoothed + audio taper.";

    using Real = double;

    struct State
    {
        float sr = 192000.0f;

        // ---- Deterministic per-channel trim (tolerancias) ----
        float chanTrim = 0.0f; // [-1..+1] estable por instancia/canal

        // ---- RMS envelope (medición perceptual) ----
        Real envP   = 0.0;
        Real env    = 0.0;
        Real sagEnv = 0.0;

        // HPF states SOLO para medición del detector
        Real meas_x1 = 0.0;
        Real meas_y1 = 0.0;

        // ---- DC blocker (audio path) ----
        Real dc_x1 = 0.0;
        Real dc_y1 = 0.0;

        // ---- Asimetría pro: drift lento (determinístico, sin RNG) ----
        Real drift      = 0.0; // [-1..+1] aprox (muy lento)
        Real driftLP    = 0.0; // lowpass (suaviza aún más)
        Real driftPhase = 0.0; // fase del LFO

        // ---- Subsonic HPF via LP (hp = x - lp) ----
        Real sub_lp = 0.0;

        // ---- Pre-emphasis shelf via LP (high = x - lp) ----
        Real preShelf_lp = 0.0;

        // ---- Split low/high via LP ----
        Real split_lp = 0.0;

        // ---- Transformer hysteresis ----
        Real magIn  = 0.0;
        Real magOut = 0.0;

        // Para eddy-loss: prev input por trafo
        Real trafoIn_prevX  = 0.0;
        Real trafoOut_prevX = 0.0;

        // ADAA memory for atan in transformers
        Real atanIn_x1  = 0.0;
        Real atanOut_x1 = 0.0;

        // ---- Dynamic HF de-esser ----
        Real hf_lp  = 0.0;
        Real hfEnv  = 0.0;

        // ---- Pre-edge LP (antes de amp1) ----
        Real preEdge_lp = 0.0;

        // ---- Slew limiting (path amp) ----
        Real slew_y = 0.0;

        // ---- ADAA memory amp stages ----
        Real amp1_x1 = 0.0;
        Real amp1_x2 = 0.0;
        Real amp2_x1 = 0.0;

        // ---- Interstage RC (pegamento) ----
        Real inter_lp = 0.0;
        Real inter_hf = 0.0;

        // ---- Mid-forward pre / de-nasal post ----
        Real mid_hi_lp      = 0.0;
        Real mid_lo_lp      = 0.0;
        Real post_mid_hi_lp = 0.0;
        Real post_mid_lo_lp = 0.0;

        // ---- Allpass “3D glue” (post-trafo OUT) ----
        Real ap1_x1 = 0.0, ap1_y1 = 0.0;
        Real ap2_x1 = 0.0, ap2_y1 = 0.0;

        // ===============================
        // ✅ (1) Diffusion Bus paralelo
        // ===============================
        static constexpr int kDiffN = 6; // 4..8 recomendado; 6 sweet spot
        Real diff_x1[kDiffN] = {0}, diff_y1[kDiffN] = {0};
        Real diffFb   = 0.0; // feedback sample
        Real diffDamp = 0.0; // LP dentro del loop (damping)

        // ✅ (2) Phase tilt extra SOLO en el wet (estados separados)
        Real wet_ap1_x1 = 0.0, wet_ap1_y1 = 0.0;
        Real wet_ap2_x1 = 0.0, wet_ap2_y1 = 0.0;

        // ---- Loading / impedancias ----
        Real load_lp = 0.0;

        // Dip suave 18–30k
        Real loadDip_hi_lp = 0.0;
        Real loadDip_lo_lp = 0.0;

        // ---- HF control ----
        Real deFizz_lp = 0.0;
        Real post1_lp  = 0.0;
        Real post2_lp  = 0.0;

        // ---- Deterministic denorm helper ----
        uint32_t denormCount = 0u;

        // ------------------------------------------------------------
        // ✅ (PATCH) Smoothed knobs (evita zipper y permite rangos más agresivos)
        Real tone1_sm = 0.5; // 0..1
        Real tone2_sm = 0.5; // 0..1

        // ✅ (PATCH) “Air shelf” tipo 1073 (estado LP para extra shelf)
        Real air_lp = 0.0;
    };

    static inline void prepare (State& s, float sampleRateHz) noexcept
    {
        s.sr = (sampleRateHz > 1000.0f ? sampleRateHz : 192000.0f);

        // Deterministic "tolerancia" por canal/instancia
        {
            const uintptr_t u = (uintptr_t) (&s);
            uint32_t x = (uint32_t) ((u >> 4) ^ (u >> 13) ^ (u >> 21));
            x ^= (x << 13);
            x ^= (x >> 17);
            x ^= (x << 5);
            const float r01 = (float) (x & 0xFFFFu) * (1.0f / 65535.0f);
            s.chanTrim = (2.0f * r01) - 1.0f; // [-1..+1]

            // driftPhase determinística por instancia/canal (sin RNG)
            s.driftPhase = (Real) ((r01) * 2.0f * 3.14159265358979323846f);
            s.denormCount = x ^ 0x9E3779B9u;
        }

        reset (s);
    }

    static inline void reset (State& s) noexcept
    {
        s.envP = 0.0;
        s.env  = 0.0;
        s.sagEnv = 0.0;

        s.meas_x1 = 0.0;
        s.meas_y1 = 0.0;

        s.dc_x1 = 0.0;
        s.dc_y1 = 0.0;

        s.drift   = 0.0;
        s.driftLP = 0.0;

        s.sub_lp = 0.0;
        s.preShelf_lp = 0.0;
        s.split_lp = 0.0;

        s.magIn  = 0.0;
        s.magOut = 0.0;

        s.trafoIn_prevX  = 0.0;
        s.trafoOut_prevX = 0.0;

        s.atanIn_x1  = 0.0;
        s.atanOut_x1 = 0.0;

        s.hf_lp = 0.0;
        s.hfEnv = 0.0;

        s.preEdge_lp = 0.0;
        s.slew_y = 0.0;

        s.amp1_x1 = 0.0;
        s.amp1_x2 = 0.0;
        s.amp2_x1 = 0.0;

        s.inter_lp = 0.0;
        s.inter_hf = 0.0;

        s.mid_hi_lp = 0.0;
        s.mid_lo_lp = 0.0;

        s.post_mid_hi_lp = 0.0;
        s.post_mid_lo_lp = 0.0;

        s.ap1_x1 = s.ap1_y1 = 0.0;
        s.ap2_x1 = s.ap2_y1 = 0.0;

        // ✅ Diffusion Bus reset
        for (int i = 0; i < State::kDiffN; ++i) { s.diff_x1[i] = 0.0; s.diff_y1[i] = 0.0; }
        s.diffFb = 0.0;
        s.diffDamp = 0.0;

        // ✅ Wet phase-tilt reset
        s.wet_ap1_x1 = s.wet_ap1_y1 = 0.0;
        s.wet_ap2_x1 = s.wet_ap2_y1 = 0.0;

        s.load_lp = 0.0;
        s.loadDip_hi_lp = 0.0;
        s.loadDip_lo_lp = 0.0;

        s.deFizz_lp = 0.0;
        s.post1_lp  = 0.0;
        s.post2_lp  = 0.0;

        // ✅ (PATCH) knob smoothing + air shelf reset
        s.tone1_sm = 0.5;
        s.tone2_sm = 0.5;
        s.air_lp   = 0.0;

        s.denormCount ^= 0xA5A5A5A5u;
    }

    // -------------------------------------------------------------------------
    static inline float process (State& s, float xIn, const plugin::Knobs& k) noexcept
    {
        if (!std::isfinite (xIn))
            return 0.0f;

        // Anti-denorm determinístico SOLO si cae a ultrapequeños
        if (std::fabs ((double) xIn) < 1.0e-20)
            xIn += (float) tinyNoiseDenormSafe (s);

        const Real sr = (Real) s.sr;
        Real x = (Real) xIn;

        // ------------------------------------------------------------
        // ✅ (PATCH) Knob smoothing (25–40 ms) + audio taper/curvas
        {
            const Real aK = alphaFromMsT ((Real) 35.0, sr);
            s.tone1_sm = onePoleLPT (s.tone1_sm, (Real) k.tone1_01, aK);
            s.tone2_sm = onePoleLPT (s.tone2_sm, (Real) k.tone2_01, aK);
        }

        const Real tone1 = clampT (s.tone1_sm, (Real) 0.0, (Real) 1.0);
        const Real tone2 = clampT (s.tone2_sm, (Real) 0.0, (Real) 1.0);

        const Real t1 = smoothstep01 (tone1);
        const Real t2 = smoothstep01 (tone2);

        // Tone1: izq agresivo, der suavecito
        const Real relax01 = pow01 (t1, (Real) 1.6);
        const Real agg01   = pow01 ((Real) 1.0 - t1, (Real) 1.35);

        // Tone2: izq dark, der bright
        const Real bright01 = pow01 (t2, (Real) 1.35);
        const Real dark01   = pow01 ((Real) 1.0 - t2, (Real) 1.35);

        // ---- Tolerancias por canal (±1–3%) ----
        const Real trimFc    = (Real) (1.0f + 0.015f * s.chanTrim);
        const Real trimDrive = (Real) (1.0f + 0.020f * s.chanTrim);
        const Real trimBias  = (Real) (1.0f + 0.030f * s.chanTrim);

        // ------------------------------------------------------------
        // 0) DC blocker (audio path)
        {
            const Real R = alphaFromHzT ((Real) 5.0, sr);
            const Real y0 = (x - s.dc_x1) + R * s.dc_y1;
            s.dc_x1 = x;
            s.dc_y1 = y0;
            x = y0;
        }

        // ------------------------------------------------------------
        // A) Calibración headroom real (-18 dBFS ≈ 0 VU)
        x *= (Real) kInCal;

        // ------------------------------------------------------------
        // 1) Detector perceptual + env + sag
        {
            const Real aHP = std::exp (-2.0 * kPi * ((Real) 90.0 / sr));
            const Real xm  = onePoleHPT (s.meas_x1, s.meas_y1, x, aHP);

            const Real p = xm * xm;

            const Real measMs = 16.0;
            const Real aMeas  = alphaFromMsT (measMs, sr);
            s.envP = aMeas * s.envP + (1.0 - aMeas) * p;
            s.envP = clampT (s.envP, (Real) 0.0, (Real) 64.0);

            const Real inst = std::sqrt (s.envP + 1.0e-18);

            const Real atkMs = 2.7;
            const Real relMs = 105.0;
            const Real aAtk  = alphaFromMsT (atkMs, sr);
            const Real aRel  = alphaFromMsT (relMs, sr);

            const Real a = (inst > s.env) ? aAtk : aRel;
            s.env = a * s.env + (1.0 - a) * inst;

            const Real sagMs = 260.0;
            const Real aSag  = alphaFromMsT (sagMs, sr);
            s.sagEnv = aSag * s.sagEnv + (1.0 - aSag) * inst;
            s.sagEnv = clampT (s.sagEnv, (Real) 0.0, (Real) 16.0);
        }

        const Real env01 = clamp01T (s.env * (Real) 1.60);
        const Real sag01 = clamp01T (s.sagEnv * (Real) 1.15);

        // ------------------------------------------------------------
        // ✅ (PATCH) Sag gain (más rango, más audible con Tone1 agresivo)
        {
            const Real sagAmt = (Real) 0.07 + (Real) 0.26 * agg01; // PATCH
            const Real sagG   = 1.0 / (1.0 + sagAmt * (s.sagEnv * 1.25));
            x *= sagG;
        }

        // ------------------------------------------------------------
        // B) Asimetría “real” con drift determinístico (0 noise, sin RNG)
        {
            // LFO ultra lento 0.005..0.03 Hz (determinístico)
            // suavecito -> un pelín más “vida”, agresivo -> más quieto
            Real fHz = (Real) 0.006 + (Real) 0.020 * ((Real) 0.25 + (Real) 0.75 * env01);
            fHz *= ((Real) 0.85 + (Real) 0.30 * relax01);
            fHz = clampT (fHz, (Real) 0.005, (Real) 0.030);

            s.driftPhase += (Real) (2.0 * kPi) * (fHz / sr);
            if (s.driftPhase > (Real) (2.0 * kPi)) s.driftPhase -= (Real) (2.0 * kPi);

            const Real n = std::sin (s.driftPhase); // [-1..+1] determinístico

            // Suaviza aún más
            const Real aLP = alphaFromHzT ((Real) 0.03, sr);
            s.driftLP = onePoleLPT (s.driftLP, n, aLP);

            // leak extra largo
            const Real leakAlpha = std::exp (-1.0 / ((Real) 35.0 * sr)); // ~35s
            s.drift = s.drift * leakAlpha + (1.0 - leakAlpha) * s.driftLP;
            s.drift = clampT (s.drift, (Real) -1.0, (Real) 1.0);

            const Real baseEven =
                ((Real) 0.0045 + (Real) 0.0140 * env01 + (Real) 0.0060 * sag01)
                * (Real) (1.0f + 0.08f * s.chanTrim);

            Real evenMul = 1.0 + (Real) 0.35 * s.drift;
            evenMul = clampT (evenMul, (Real) 0.65, (Real) 1.35);

            // ✅ (PATCH) Asimetría/even más contrastada por Tone1
            const Real evenAmt = baseEven * evenMul * ((Real) 0.55 + (Real) 1.25 * agg01);
            x += evenAmt * (x * std::fabs (x));
        }

        // ------------------------------------------------------------
        // 2) Subsonic HPF
        {
            const Real fc = (Real) 18.0 * trimFc;
            const Real a  = alphaFromHzT (fc, sr);
            s.sub_lp = onePoleLPT (s.sub_lp, x, a);
            x = x - s.sub_lp;
        }

        // ------------------------------------------------------------
        // 3) Pre-emphasis shelf
        {
            const Real fc = ((Real) 4100.0 - (Real) 1300.0 * env01) * trimFc;
            const Real a  = alphaFromHzT (clampT (fc, (Real) 1800.0, (Real) 6500.0), sr);

            s.preShelf_lp = onePoleLPT (s.preShelf_lp, x, a);
            const Real highSh = x - s.preShelf_lp;

            const Real shelfGain = (Real) 0.05 + (Real) 0.24 * env01;
            x = x + shelfGain * highSh;
        }

        // ------------------------------------------------------------
        // 4) Split low/high
        Real low = 0.0;
        Real high = 0.0;
        {
            const Real splitFc = ((Real) 1850.0 - (Real) 720.0 * env01) * trimFc;
            const Real a = alphaFromHzT (clampT (splitFc, (Real) 850.0, (Real) 2600.0), sr);

            s.split_lp = onePoleLPT (s.split_lp, x, a);
            low  = s.split_lp;
            high = x - low;
        }

        // ------------------------------------------------------------
        // 5) Drives por banda
        {
            const Real driveLo = ((Real) 1.00 + (Real) 0.38 * env01) * trimDrive;
            const Real driveHi = ((Real) 1.00 + (Real) 0.92 * env01) * trimDrive;

            low  *= driveLo;
            high *= driveHi;
        }

        // ------------------------------------------------------------
        // 6) Trafo IN
        low = transformerHystADAA_HQ (
            s.magIn, s.trafoIn_prevX, s.atanIn_x1,
            low, env01, sag01, sr,
            (Real) 55.0 * trimFc,
            (Real) 0.24,
            (Real) 2.08 * trimDrive,
            (Real) 0.012 * trimBias,
            (Real) 0.14
        );

        // ------------------------------------------------------------
        // 7) HIGH chain

        // 7.a) Mid-forward
        {
            const Real fcHi = ((Real) 2200.0 - (Real) 500.0 * env01) * trimFc;
            const Real fcLo = ((Real) 350.0  - (Real) 120.0 * env01) * trimFc;

            const Real aHi = alphaFromHzT (clampT (fcHi, (Real) 1200.0, (Real) 3200.0), sr);
            const Real aLo = alphaFromHzT (clampT (fcLo, (Real)  180.0, (Real)  650.0), sr);

            s.mid_hi_lp = onePoleLPT (s.mid_hi_lp, high, aHi);
            s.mid_lo_lp = onePoleLPT (s.mid_lo_lp, high, aLo);

            const Real midBand = s.mid_hi_lp - s.mid_lo_lp;

            const Real gDb = (Real) 0.15 + (Real) 1.35 * env01;
            const Real g   = dbToLinT (gDb) - 1.0;

            high = high + midBand * g;
        }

        // 7.b) HF de-esser ultra rápido
        Real hf01 = 0.0;
        {
            const Real fcMeas = (Real) 10000.0 * trimFc;
            const Real aMeas  = alphaFromHzT (clampT (fcMeas, (Real) 6500.0, (Real) 18000.0), sr);

            s.hf_lp = onePoleLPT (s.hf_lp, high, aMeas);
            const Real hf = high - s.hf_lp;
            const Real e  = std::fabs (hf);

            const Real aAtk = alphaFromMsT ((Real) 0.6, sr);
            const Real aRel = alphaFromMsT ((Real) 18.0, sr);
            const Real a    = (e > s.hfEnv) ? aAtk : aRel;

            s.hfEnv = a * s.hfEnv + (1.0 - a) * e;
            s.hfEnv = clampT (s.hfEnv, (Real) 0.0, (Real) 8.0);

            hf01 = clamp01T (s.hfEnv * (Real) 1.75);
        }

        // ✅ (PATCH) De-esser más contrastado con Tone2 (dark controla MUCHO más)
        hf01 *= (Real) (0.30 + 1.35 * dark01);

        // 7.c) Pre-edge LP
        {
            Real fc = ((Real) 55000.0 - (Real) 26000.0 * env01 - (Real) 9000.0 * sag01) * trimFc;

            // ✅ (PATCH) más rango bright/dark en preEdge
            fc *= (Real) (0.58 + 0.95 * bright01);

            fc *= (1.0 - (Real) 0.32 * hf01);

            const Real a  = alphaFromHzT (clampT (fc, (Real) 16000.0, (Real) 90000.0), sr);

            s.preEdge_lp = onePoleLPT (s.preEdge_lp, high, a);
            high = s.preEdge_lp;
        }

        // 7.d) Slew limiting
        {
            Real slewMax = (Real) 1.15 - (Real) 0.55 * env01 - (Real) 0.10 * sag01;
            slewMax = clampT (slewMax, (Real) 0.25, (Real) 1.35);
            slewMax *= ((Real) 0.85 + (Real) 0.35 * bright01);

            const Real dy = clampT (high - s.slew_y, -slewMax, +slewMax);
            s.slew_y += dy;
            high = s.slew_y;
        }

        // 7.e) Amp stage 1 (ADAA2 tanh)
        Real y1 = ampStage1ADAA2 (s, high, env01, sag01);

        // 7.f) Interstage RC
        {
            const Real aHP = alphaFromHzT ((Real) 35.0 * trimFc, sr);
            s.inter_lp = onePoleLPT (s.inter_lp, y1, aHP);
            Real z = y1 - s.inter_lp;

            const Real aLP = alphaFromHzT (((Real) 52000.0 - (Real) 18000.0 * env01) * trimFc, sr);
            s.inter_hf = onePoleLPT (s.inter_hf, z, aLP);
            y1 = s.inter_hf;
        }

        // 7.g) Amp stage 2 (ADAA1)
        Real y2 = ampStage2ADAA (s, y1, env01, sag01);

        // 7.h) De-nasal post
        {
            const Real fcHi = (Real) 2400.0 * trimFc;
            const Real fcLo = (Real)  420.0 * trimFc;

            const Real aHi = alphaFromHzT (fcHi, sr);
            const Real aLo = alphaFromHzT (fcLo, sr);

            s.post_mid_hi_lp = onePoleLPT (s.post_mid_hi_lp, y2, aHi);
            s.post_mid_lo_lp = onePoleLPT (s.post_mid_lo_lp, y2, aLo);

            const Real midBand = s.post_mid_hi_lp - s.post_mid_lo_lp;

            const Real gDb = (Real) 0.10 + (Real) 0.65 * env01;
            const Real g   = dbToLinT (gDb) - 1.0;

            y2 = y2 - midBand * g;
        }

        Real y = low + y2;

        // ------------------------------------------------------------
        // 8) Trafo OUT
        y = transformerHystADAA_HQ (
            s.magOut, s.trafoOut_prevX, s.atanOut_x1,
            y, env01, sag01, sr,
            (Real) 42.0 * trimFc,
            (Real) 0.19,
            (Real) 1.88 * trimDrive,
            (Real) 0.008 * trimBias,
            (Real) 0.12
        );

        // ------------------------------------------------------------
        // C) Allpass “3D glue” (base)
        {
            Real f1 = ((Real) 650.0  + (Real) 950.0  * env01 + (Real) 250.0 * sag01) * trimFc;
            Real f2 = ((Real) 1800.0 + (Real) 1400.0 * env01 + (Real) 350.0 * sag01) * trimFc;

            const Real apMul =
                ((Real) 1.15 - (Real) 0.45 * relax01) *
                ((Real) 0.90 + (Real) 0.20 * bright01);

            f1 *= apMul;
            f2 *= apMul;

            const Real a1 = allpassCoefFromHzT (clampT (f1, (Real) 120.0, (Real) 8000.0),  sr);
            const Real a2 = allpassCoefFromHzT (clampT (f2, (Real) 240.0, (Real) 12000.0), sr);

            y = allpass1T (y, s.ap1_x1, s.ap1_y1, a1);
            y = allpass1T (y, s.ap2_x1, s.ap2_y1, a2);
        }

        // ------------------------------------------------------------
        // ✅ Diffusion Bus paralelo (PATCH: mucho más sensible a Tone1 + halo más 1073)
        {
            const Real dry = y;

            const Real relax  = relax01;
            const Real bright = bright01;
            const Real dark   = dark01;

            const Real relax2 = relax * relax;

            // ✅ (PATCH) Mix y feedback con más viaje
            Real diffMix = (Real) 0.00 + (Real) 0.52 * relax2; // 0..0.52
            diffMix = clampT (diffMix, (Real) 0.0, (Real) 0.55);

            Real fb = (Real) 0.00 + (Real) 0.18 * relax2;      // 0..0.18
            fb *= (Real) (0.80 + 0.35 * dark);
            fb = clampT (fb, (Real) 0.0, (Real) 0.22);

            Real dampFc = (Real) 6500.0 + (Real) 14000.0 * bright;
            dampFc *= ((Real) 1.00 - (Real) 0.18 * env01);
            dampFc = clampT (dampFc, (Real) 3000.0, (Real) (0.45 * sr));
            const Real aDamp = alphaFromHzT (dampFc, sr);

            // ✅ (PATCH) SmearMul más profundo y sensible a Tone1
            const Real smearMul =
                ((Real) 1.20 - (Real) 0.55 * relax) *
                ((Real) 0.90 + (Real) 0.35 * bright);

            Real fcList[State::kDiffN] = {
                (Real) 250.0, (Real) 520.0, (Real) 930.0,
                (Real) 1500.0, (Real) 2400.0, (Real) 3800.0
            };

            for (int i = 0; i < State::kDiffN; ++i)
                fcList[i] = fcList[i] * smearMul * trimFc;

            // Loop input
            Real wetIn = dry + s.diffFb;

            // Damping inside loop
            s.diffDamp = onePoleLPT (s.diffDamp, wetIn, aDamp);
            wetIn = s.diffDamp;

            // Diffusion (loop signal)
            const Real wetLoop = runDiffuserAllpassCascade (s, wetIn, fcList, sr);

            // Update feedback from loop signal (no tilt aquí)
            s.diffFb = clampT (wetLoop * fb, (Real) -0.35, (Real) 0.35);

            // Wet output signal = loop + phase tilt extra (solo salida)
            Real wet = wetLoop;

            // Phase tilt extra solo en wet (más profundidad sin “room”)
            {
                Real fA = ((Real) 900.0  + (Real) 900.0  * relax) * trimFc;
                Real fB = ((Real) 2600.0 + (Real) 1400.0 * relax) * trimFc;

                fA *= ((Real) 0.95 + (Real) 0.20 * bright);
                fB *= ((Real) 0.95 + (Real) 0.20 * bright);

                const Real aA = allpassCoefFromHzT (clampT (fA, (Real) 120.0, (Real) 12000.0), sr);
                const Real aB = allpassCoefFromHzT (clampT (fB, (Real) 240.0, (Real) 16000.0), sr);

                wet = allpass1T (wet, s.wet_ap1_x1, s.wet_ap1_y1, aA);
                wet = allpass1T (wet, s.wet_ap2_x1, s.wet_ap2_y1, aB);
            }

            // Trim ligero para no inflar nivel
            const Real wetTrim = (Real) (0.92 + 0.08 * bright);
            wet *= wetTrim;

            y = dry * ((Real) 1 - diffMix) + wet * diffMix;
        }

        // ------------------------------------------------------------
        // 9) Loading LP dinámico
        {
            Real fc = ((Real) 65000.0 - (Real) 38000.0 * ((Real) 0.65 * env01 + (Real) 0.35 * sag01)) * trimFc;
            fc *= (Real) (0.90 + 0.25 * bright01);

            const Real a  = alphaFromHzT (clampT (fc, (Real) 18000.0, (Real) 90000.0), sr);

            s.load_lp = onePoleLPT (s.load_lp, y, a);
            y = s.load_lp;
        }

        // 10) Dip suave 18–30k
        {
            const Real fcHi = ((Real) 30000.0 - (Real) 7000.0 * env01) * trimFc;
            const Real fcLo = ((Real) 18000.0 - (Real) 4000.0 * env01) * trimFc;

            const Real aHi = alphaFromHzT (clampT (fcHi, (Real) 14000.0, (Real) 60000.0), sr);
            const Real aLo = alphaFromHzT (clampT (fcLo, (Real)  9000.0, (Real) 45000.0), sr);

            s.loadDip_hi_lp = onePoleLPT (s.loadDip_hi_lp, y, aHi);
            s.loadDip_lo_lp = onePoleLPT (s.loadDip_lo_lp, y, aLo);

            const Real band = s.loadDip_hi_lp - s.loadDip_lo_lp;

            const Real dipDb = (Real) 0.05 + (Real) 0.75 * env01;
            const Real g     = dbToLinT (dipDb) - 1.0;

            y = y - band * g;
        }

        // 11) De-fizz dinámico
        {
            Real fc = ((Real) 19500.0 - (Real) 12000.0 * env01) * trimFc;

            // ✅ (PATCH) más rango bright/dark en de-fizz
            fc *= (Real) (0.58 + 0.95 * bright01);

            const Real a  = alphaFromHzT (clampT (fc, (Real) 6000.0, (Real) 24000.0), sr);

            s.deFizz_lp = onePoleLPT (s.deFizz_lp, y, a);
            y = s.deFizz_lp;
        }

        // 12) Post HF damping 2 polos
        {
            Real fc = ((Real) 25500.0 - (Real) 16000.0 * env01) * trimFc;

            // ✅ (PATCH) post damping más contrastado
            fc *= (Real) (0.65 + 0.90 * bright01);

            const Real a  = alphaFromHzT (clampT (fc, (Real) 8000.0, (Real) 32000.0), sr);

            s.post1_lp = onePoleLPT (s.post1_lp, y, a);
            s.post2_lp = onePoleLPT (s.post2_lp, s.post1_lp, a);
            y = s.post2_lp;
        }

        // ------------------------------------------------------------
        // ✅ (PATCH) Air shelf tipo 1073 (sutil, MUY audible y “fiel”)
        {
            const Real fcAir = (Real) 12000.0 * trimFc;
            const Real aAir  = alphaFromHzT (clampT (fcAir, (Real) 6000.0, (Real) (0.45 * sr)), sr);

            s.air_lp = onePoleLPT (s.air_lp, y, aAir);
            const Real hi = y - s.air_lp;

            const Real airDb = ((Real) -0.8 * dark01) + ((Real) 1.8 * bright01); // -0.8..+1.8 dB
            const Real g = dbToLinT (airDb) - (Real) 1.0;

            y = y + hi * g;
        }

        // ------------------------------------------------------------
        // A) Calibración de salida
        y *= (Real) kOutCal;

        if (!std::isfinite (y))
            return 0.0f;

        y = clampT (y, (Real) -1.20, (Real) 1.20);
        return (float) y;
    }

    // Compatibilidad: sin knobs
    static inline float process (State& s, float xIn) noexcept
    {
        plugin::Knobs kDefault;
        return process (s, xIn, kDefault);
    }

    // Legacy stateless
    static inline float process (float x) noexcept
    {
        if (!std::isfinite (x))
            return 0.0f;

        float y = (2.0f / 3.14159265358979323846f) * std::atan (2.0f * x);
        y = std::tanh (2.5f * y);
        y = (2.0f / 3.14159265358979323846f) * std::atan (1.7f * y);
        y = clampf (y, -1.20f, 1.20f);
        return y;
    }

private:
    // ------------------ constants ------------------
    static constexpr Real kPi  = 3.141592653589793238462643383279502884;
    static constexpr Real kLn2 = 0.693147180559945309417232121458176568;

    // Calibración -18 dBFS ≈ 0 VU
    static constexpr float kInCal  = 7.943282347242814f;    // 10^(+18/20)
    static constexpr float kOutCal = 0.12589254117941673f;  // 10^(-18/20)

    // ------------------ float clamp for legacy/stateless ------------------
    static inline float clampf (float v, float lo, float hi) noexcept
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    // ------------------ templated/double helpers ------------------
    template <typename T>
    static inline T clampT (T v, T lo, T hi) noexcept
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    template <typename T>
    static inline T clamp01T (T v) noexcept
    {
        return (v < (T) 0) ? (T) 0 : (v > (T) 1) ? (T) 1 : v;
    }

    template <typename T>
    static inline T onePoleLPT (T y1, T x, T a) noexcept
    {
        return ((T) 1 - a) * x + a * y1;
    }

    template <typename T>
    static inline T onePoleHPT (T& x1, T& y1, T x, T a) noexcept
    {
        const T y = a * (y1 + x - x1);
        x1 = x;
        y1 = y;
        return y;
    }

    template <typename T>
    static inline T alphaFromHzT (T fc, T sr) noexcept
    {
        const T safeSr = (sr > (T) 1 ? sr : (T) 1);
        return std::exp ((T) (-2) * (T) kPi * (fc / safeSr));
    }

    template <typename T>
    static inline T alphaFromMsT (T ms, T sr) noexcept
    {
        const T tau = ms * (T) 0.001;
        const T safeSr = (sr > (T) 1 ? sr : (T) 1);
        const T denom = tau * safeSr;
        if (denom <= (T) 1.0e-12) return (T) 0;
        return std::exp ((T) (-1) / denom);
    }

    template <typename T>
    static inline T dbToLinT (T db) noexcept
    {
        return std::exp ((T) 0.1151292546497022842 * db); // ln(10)/20
    }

    // ------------------------------------------------------------
    // ✅ (PATCH) curvas para “audio taper” y extremos más marcados
    static inline Real smoothstep01 (Real x) noexcept
    {
        x = clampT (x, (Real) 0.0, (Real) 1.0);
        return x * x * ((Real) 3.0 - (Real) 2.0 * x);
    }

    static inline Real pow01 (Real x, Real p) noexcept
    {
        x = clampT (x, (Real) 0.0, (Real) 1.0);
        return std::pow (x, p);
    }

    // ------------------ deterministic anti-denorm (no RNG) ------------------
    static inline Real tinyNoiseDenormSafe (State& s) noexcept
    {
        s.denormCount += 1u;
        const Real sign = (s.denormCount & 1u) ? (Real) 1.0 : (Real) -1.0;
        return sign * (Real) 1.0e-20;
    }

    // ------------------ allpass helpers ------------------
    template <typename T>
    static inline T allpassCoefFromHzT (T fc, T sr) noexcept
    {
        const T safeSr = (sr > (T) 1 ? sr : (T) 1);
        const T w = (T) kPi * (fc / safeSr);
        const T t = std::tan (w);
        const T a = ((T) 1 - t) / ((T) 1 + t);
        return clampT (a, (T) 0, (T) 0.9999);
    }

    template <typename T>
    static inline T allpass1T (T x, T& x1, T& y1, T a) noexcept
    {
        const T y = (-a * x) + x1 + (a * y1);
        x1 = x;
        y1 = y;
        return y;
    }

    // ✅ Diffuser cascade helper
    static inline Real runDiffuserAllpassCascade (State& s, Real x, const Real* fcHz, Real sr) noexcept
    {
        for (int i = 0; i < State::kDiffN; ++i)
        {
            const Real fc = clampT (fcHz[i], (Real) 60.0, (Real) (0.45 * sr));
            const Real a  = allpassCoefFromHzT (fc, sr);
            x = allpass1T (x, s.diff_x1[i], s.diff_y1[i], a);
        }
        return x;
    }

    // ------------------ ADAA helpers ------------------
    template <typename T>
    static inline T logCoshStableT (T z) noexcept
    {
        const T az = std::fabs (z);
        if (az > (T) 10)
            return az - (T) kLn2;
        return std::log (std::cosh (z));
    }

    template <typename T>
    static inline T adaaTanh1T (T x, T& x1, T k) noexcept
    {
        const T denom = (x - x1);
        T y;

        if (std::fabs (denom) > (T) 1.0e-12)
        {
            const T kx  = k * x;
            const T kx1 = k * x1;

            const T Fx  = logCoshStableT (kx)  / k;
            const T Fx1 = logCoshStableT (kx1) / k;

            y = (Fx - Fx1) / denom;
        }
        else
        {
            y = std::tanh (k * x);
        }

        x1 = x;

        const T d = std::tanh (k);
        return (std::fabs (d) > (T) 1.0e-18) ? (y / d) : y;
    }

    static inline double li2_series_neg1_0 (double x) noexcept
    {
        if (x == -1.0) return -(kPi * kPi) / 12.0;

        double sum = 0.0;
        double term = x;
        for (int n = 1; n <= 64; ++n)
        {
            sum += term / (double) (n * n);
            term *= x;
            if (std::fabs (term) < 1.0e-12)
                break;
        }
        return sum;
    }

    static inline double li2_real_neg (double x) noexcept
    {
        if (x == 0.0)  return 0.0;
        if (x == -1.0) return -(kPi * kPi) / 12.0;

        if (x < -1.0)
        {
            const double inv = 1.0 / x;
            const double L = std::log (-x);
            const double li2inv = li2_series_neg1_0 (inv);
            return -li2inv - (kPi * kPi) / 6.0 - 0.5 * L * L;
        }

        return li2_series_neg1_0 (x);
    }

    static inline double F2_tanh (double x, double k) noexcept
    {
        double z = k * x;
        if (z >  12.0) z =  12.0;
        if (z < -12.0) z = -12.0;

        const double arg = -std::exp (-2.0 * z);
        const double I = 0.5 * z * z - z * (double) kLn2 + 0.5 * li2_real_neg (arg);

        const double kk = k * k;
        return (kk > 1.0e-18) ? (I / kk) : 0.0;
    }

    template <typename T>
    static inline T adaaTanh2T (T x, T& x1, T& x2, T k) noexcept
    {
        const T z = k * x;
        const bool use2 = (std::fabs (z) > (T) 0.25);

        const T d01 = (x  - x1);
        const T d12 = (x1 - x2);
        const T d02 = (x  - x2);

        T y;

        if (!use2
            || std::fabs (d01) < (T) 1.0e-12
            || std::fabs (d12) < (T) 1.0e-12
            || std::fabs (d02) < (T) 1.0e-12)
        {
            y = adaaTanh1T (x, x1, k);
            x2 = x1;
            return y;
        }

        const double F0 = F2_tanh ((double) x,  (double) k);
        const double F1 = F2_tanh ((double) x1, (double) k);
        const double F2 = F2_tanh ((double) x2, (double) k);

        const double s0 = (F0 - F1) / (double) d01;
        const double s1 = (F1 - F2) / (double) d12;

        double yd = 2.0 * (s0 - s1) / (double) d02;

        const double norm = std::tanh ((double) k);
        if (std::fabs (norm) > 1.0e-18)
            yd /= norm;

        y = (T) yd;

        x2 = x1;
        x1 = x;

        return y;
    }

    template <typename T>
    static inline T adaaAtan1T (T x, T& x1, T k) noexcept
    {
        constexpr double twoOverPi = 0.63661977236758134308;

        const T denom = (x - x1);
        T y;

        if (std::fabs (denom) > (T) 1.0e-12)
        {
            const T kx  = k * x;
            const T kx1 = k * x1;

            const T Fx  = x  * std::atan (kx)  - ((T) 0.5 / k) * std::log1p (kx  * kx);
            const T Fx1 = x1 * std::atan (kx1) - ((T) 0.5 / k) * std::log1p (kx1 * kx1);

            y = (Fx - Fx1) / denom;
        }
        else
        {
            y = std::atan (k * x);
        }

        x1 = x;
        return (T) (twoOverPi) * y;
    }

    template <typename T>
    static inline T lowLevelBlendT (T x, T y, T knee) noexcept
    {
        const T ax = std::fabs (x);
        const T b  = ax / (ax + knee);
        return x + b * (y - x);
    }

    template <typename T>
    static inline T asymTanhZeroT (T x, T k, T bias) noexcept
    {
        const T y  = std::tanh (k * (x + bias)) - std::tanh (k * bias);

        const T yp = std::tanh (k * ((T) 1 + bias)) - std::tanh (k * bias);
        const T yn = std::tanh (k * ((T) -1 + bias)) - std::tanh (k * bias);
        const T m  = (T) 0.5 * (std::fabs (yp) + std::fabs (yn));

        return (m > (T) 1.0e-18) ? (y / m) : y;
    }

    // ------------------ transformer HQ ------------------
    static inline Real transformerCoreADAA_HQ (Real x, Real& atan_x1, Real env01, Real sag01,
                                               Real driveBase, Real biasBase) noexcept
    {
        const Real drive = (driveBase * (1.0 - 0.06 * sag01)) + 0.22 * env01;
        const Real bias  = (biasBase  + 0.018 * env01) + 0.010 * sag01;

        const Real core  = adaaAtan1T (x, atan_x1, drive);

        const Real x2 = x * x;
        const Real fluxIn = x + ((Real) 0.12 + (Real) 0.05 * env01) * x * x2;
        const Real flux   = std::tanh (((Real) 1.15 + (Real) 0.10 * env01) * fluxIn);

        const Real k    = ((Real) 1.70 + (Real) 0.95 * env01) * (1.0 - (Real) 0.05 * sag01);
        const Real asym = asymTanhZeroT (x, k, bias);

        Real y = (Real) 0.70 * core + (Real) 0.23 * flux + (Real) 0.07 * asym;
        y = lowLevelBlendT (x, y, (Real) 0.18);

        return clampT (y, (Real) -1.25, (Real) 1.25);
    }

    static inline Real transformerHystADAA_HQ (Real& m, Real& prevX, Real& atan_x1,
                                               Real x, Real env01, Real sag01, Real sr,
                                               Real fcBase, Real hystAmt,
                                               Real driveBase, Real biasBase,
                                               Real injectBase) noexcept
    {
        const Real tau = (Real) 0.7 + (Real) 0.9 * (1.0 - env01);
        const Real leakAlpha = std::exp (-1.0 / (tau * sr));
        m *= leakAlpha;

        const Real dx = x - prevX;
        prevX = x;

        const Real eddyAmt = ((Real) 0.35 + (Real) 0.55 * env01);
        const Real eddy = 1.0 / (1.0 + eddyAmt * std::fabs (dx));

        const Real fc = (fcBase + ((Real) 40.0 * env01));
        const Real a  = alphaFromHzT (fc, sr);

        const Real d = ((Real) 1.10 + (Real) 0.60 * env01) * (1.0 - (Real) 0.06 * sag01);
        const Real h = hystAmt + (Real) 0.12 * env01;

        const Real target = std::tanh (d * (x - h * m));
        m = onePoleLPT (m, target, a);

        Real inject = (injectBase + (Real) 0.10 * env01 + (Real) 0.05 * sag01);
        inject *= eddy;

        const Real xin = x + inject * m;

        return transformerCoreADAA_HQ (xin, atan_x1, env01, sag01, driveBase, biasBase);
    }

    // ------------------ amp stages ------------------
    static inline Real ampStage1ADAA2 (State& s, Real x, Real env01, Real sag01) noexcept
    {
        const Real drive = ((Real) 1.55 + (Real) 0.58 * env01) * (1.0 - (Real) 0.08 * sag01);
        const Real k1    = ((Real) 2.85 + (Real) 0.90 * env01) * (1.0 - (Real) 0.06 * sag01);

        const Real bias = ((Real) 0.0005 + (Real) 0.0009 * env01) * sag01;

        const Real pre = (x + bias) * drive;

        Real y = adaaTanh2T (pre, s.amp1_x1, s.amp1_x2, k1);
        y = lowLevelBlendT (x, y, (Real) 0.26);

        return clampT (y, (Real) -1.35, (Real) 1.35);
    }

    static inline Real ampStage2ADAA (State& s, Real x, Real env01, Real sag01) noexcept
    {
        const Real drive = ((Real) 1.10 + (Real) 0.34 * env01) * (1.0 - (Real) 0.06 * sag01);
        const Real k2    = ((Real) 2.05 + (Real) 0.78 * env01) * (1.0 - (Real) 0.05 * sag01);

        const Real pre = x * drive;

        Real y = adaaTanh1T (pre, s.amp2_x1, k2);

        const Real t = pre;
        y += ((Real) 0.004 + (Real) 0.014 * env01) * (t * t * t);

        y = (Real) 0.88 * y + (Real) 0.12 * ((Real) 0.63661977236758134308 * std::atan ((Real) 2.3 * y));
        y = lowLevelBlendT (x, y, (Real) 0.24);

        return clampT (y, (Real) -1.35, (Real) 1.35);
    }
};



