#pragma once

// preset-API312512.h (PRO ULTIMATE + HQ NEXT LEVEL + CAL/DRIFT/AP/DE-ESS)
// Diseñado para correr DENTRO de oversampling (4x o más). Ideal: 96k -> 384k interno.
//
// ✅ Implementa (voicing API 312/512 = “americano, punch”):
// 1) Allpass “punch glue”: menos smear que Neve, fase “tight” (coef negativo permitido pero moderado)
// 2) Presence/air API-ish: biquad high-shelf (RBJ) + control de presencia (peak) con smoothing
// 3) “Firma” de hierro API: LF tight bump (80–120 Hz) + peak de presencia (2.8–4 kHz) dependiente de drive
// 4) Anti-aliasing extra: ADAA1 even + cubic + soft-slew rápido (preserva transiente)
// 5) Punch Bus paralelo: micro-diffusion corto (menos “space”, más “snap”) con HPF fb + governor
// 6) Macro Tone1/Tone2:
//    - Tone1 (izq=hard punch, der=suave): cambia agresividad/transiente y “opamp bite” SIN cambiar gain total
//    - Tone2 (izq=round, der=presence/air): controla presencia 3k + air shelf + de-esser
// 7) Sag “PSU”: más rápido y menor (API = más headroom / recovery rápido)
// 8) Tolerancias: trims separados (fc/drive/bias/trafo) con seeds determinísticas
//
// 🔧 OPTIMIZACIONES / MEJORAS (mantiene las del Neve):
// A) Biquads control-rate: update coefs cada 32 samples (airShelf + ironLF/Presence)
// B) Coef smoothing: rampa lineal de coefs durante 32 samples (evita zipper/phase wobble)
// C) Punch bus safety:
//    C1) RMS governor suave dentro del loop
//    C2) DC guard dentro del loop

#include <cmath>
#include <cstdint>
#include <limits>

// -----------------------------------------------------------------------------
// Minimal Knobs definition (si tu proyecto ya define plugin::Knobs, este bloque
// NO interferirá gracias al guard).
// -----------------------------------------------------------------------------
#ifndef PRESET_API312512_HAS_PLUGIN_KNOBS
#define PRESET_API312512_HAS_PLUGIN_KNOBS
namespace plugin
{
    struct Knobs
    {
        float tone1_01 = 0.5f; // 0..1
        float tone2_01 = 0.5f; // 0..1
    };
}
#endif

struct Preset_API312512
{
    static constexpr const char* kDisplayName = "API 312/512";

    static constexpr const char* kKnobBehavior =
        "Tone 2 (izq=round, der=presence/air) altera: peak 3k, air shelf RBJ, de-esser y edge HF. "
        "Tone 1 (izq=hard punch, der=smooth) controla: opamp bite, slew/transiente, sag rápido y punch bus. "
        "Incluye Punch Bus paralelo (corto/tight) + knobs smoothed + audio taper.";

    using Real = double;

    // ------------------ Biquad helper ------------------
    struct Biquad
    {
        // current coefs
        Real b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

        // targets + per-sample increments (coef smoothing)
        Real tb0 = 1, tb1 = 0, tb2 = 0, ta1 = 0, ta2 = 0;
        Real db0 = 0, db1 = 0, db2 = 0, da1 = 0, da2 = 0;
        uint32_t ramp = 0;

        // state
        Real z1 = 0, z2 = 0;

        inline void reset() noexcept
        {
            z1 = z2 = 0;
            tb0 = b0; tb1 = b1; tb2 = b2; ta1 = a1; ta2 = a2;
            db0 = db1 = db2 = da1 = da2 = 0;
            ramp = 0;
        }

        inline void setTargetCoeffs(Real nb0, Real nb1, Real nb2, Real na1, Real na2, uint32_t rampSamples) noexcept
        {
            tb0 = nb0; tb1 = nb1; tb2 = nb2; ta1 = na1; ta2 = na2;

            if (rampSamples == 0u)
            {
                b0 = tb0; b1 = tb1; b2 = tb2; a1 = ta1; a2 = ta2;
                db0 = db1 = db2 = da1 = da2 = 0;
                ramp = 0u;
                return;
            }

            // linear ramp from current to target
            const Real invN = (Real)(1.0 / (Real)rampSamples);
            db0 = (tb0 - b0) * invN;
            db1 = (tb1 - b1) * invN;
            db2 = (tb2 - b2) * invN;
            da1 = (ta1 - a1) * invN;
            da2 = (ta2 - a2) * invN;
            ramp = rampSamples;
        }

        inline void tick() noexcept
        {
            if (ramp == 0u) return;

            b0 += db0; b1 += db1; b2 += db2; a1 += da1; a2 += da2;
            ramp--;

            if (ramp == 0u)
            {
                // snap to exact target (avoid drift)
                b0 = tb0; b1 = tb1; b2 = tb2; a1 = ta1; a2 = ta2;
                db0 = db1 = db2 = da1 = da2 = 0;
            }
        }

        inline Real process(Real x) noexcept
        {
            tick();

            // Transposed Direct Form II
            const Real y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    struct State
    {
        float sr = 192000.0f;

        // ---- Legacy (compat) ----
        float chanTrim = 0.0f;

        // ---- Independent deterministic trims ----
        float trimA = 0.0f; // filtro (fc)
        float trimB = 0.0f; // drive
        float trimC = 0.0f; // bias/asim
        float trimT = 0.0f; // hierro/signature

        // ---- RMS envelope ----
        Real envP = 0.0;
        Real env  = 0.0;

        // ---- Sag (más rápido y menos) ----
        Real sagFast = 0.0;
        Real sagSlow = 0.0;
        Real sagEnv  = 0.0;
        Real lf_lp   = 0.0;

        // HPF detector states
        Real meas_x1 = 0.0;
        Real meas_y1 = 0.0;

        // ---- DC blocker (audio path) ----
        Real dc_x1 = 0.0;
        Real dc_y1 = 0.0;

        // ---- Drift determinístico ----
        Real drift      = 0.0;
        Real driftLP    = 0.0;
        Real driftPhase = 0.0;

        // ---- ADAA memory ----
        Real even_x1 = 0.0;
        Real cub_x1  = 0.0;

        // ---- Subsonic HPF via LP (hp = x - lp) ----
        Real sub_lp = 0.0;

        // ---- Presence pre-emphasis via LP ----
        Real preShelf_lp = 0.0;

        // ---- Split low/high via LP ----
        Real split_lp = 0.0;

        // ---- Transformer hysteresis ----
        Real magIn  = 0.0;
        Real magOut = 0.0;

        Real trafoIn_prevX  = 0.0;
        Real trafoOut_prevX = 0.0;

        // ADAA memory for atan in transformers
        Real atanIn_x1  = 0.0;
        Real atanOut_x1 = 0.0;

        // ---- “Firma” API: hierro (LF tight + presence peak) ----
        Biquad ironLF;
        Biquad presencePk;
        Real   ironSig_sm = 0.0;

        // ---- Dynamic HF de-esser (menos agresivo que Neve) ----
        Real hf_lp  = 0.0;
        Real hfEnv  = 0.0;

        // ---- Pre-edge LP ----
        Real preEdge_lp = 0.0;

        // ---- Slew limiting (más rápido) ----
        Real slew_y = 0.0;

        // ---- ADAA memory amp stages ----
        Real amp1_x1 = 0.0;
        Real amp1_x2 = 0.0;
        Real amp2_x1 = 0.0;

        // ---- Interstage RC (tight) ----
        Real inter_lp = 0.0;
        Real inter_hf = 0.0;

        // ---- Presence band (2–5k) shaping ----
        Real pres_hi_lp = 0.0;
        Real pres_lo_lp = 0.0;

        // ---- Allpass “punch glue” ----
        Real ap1_x1 = 0.0, ap1_y1 = 0.0;
        Real ap2_x1 = 0.0, ap2_y1 = 0.0;

        // ===============================
        // Punch Bus paralelo (micro diffusion corto)
        // ===============================
        static constexpr int kDiffN = 4; // más corto/tight
        Real diff_x1[kDiffN] = {0}, diff_y1[kDiffN] = {0};
        Real diffFb   = 0.0;
        Real diffDamp = 0.0;

        // feedback HPF
        Real diffHP_lp = 0.0;

        // safety: RMS governor + DC guard
        Real diffRms    = 0.0;
        Real diffDC_x1  = 0.0;
        Real diffDC_y1  = 0.0;

        // ---- Loading / HF control ----
        Real load_lp = 0.0;
        Real deFizz_lp = 0.0;
        Real post1_lp  = 0.0;
        Real post2_lp  = 0.0;

        // ---- Denorm helper ----
        uint32_t denormCount = 0u;

        // ---- Smoothed knobs ----
        Real tone1_sm = 0.5;
        Real tone2_sm = 0.5;

        // ---- Air shelf biquad (RBJ) ----
        Biquad airShelf;
        Real   airGain_sm = 0.0;

        // control-rate counter
        uint32_t biquadCtr = 0u;
    };

    static inline void prepare(State& s, float sampleRateHz) noexcept
    {
        s.sr = (sampleRateHz > 1000.0f ? sampleRateHz : 192000.0f);

        // Deterministic seeds
        {
            const uintptr_t u = (uintptr_t)(&s);
            uint32_t x = (uint32_t)((u >> 4) ^ (u >> 13) ^ (u >> 21));
            x ^= (x << 13);
            x ^= (x >> 17);
            x ^= (x << 5);

            const float r01 = (float)(x & 0xFFFFu) * (1.0f / 65535.0f);
            s.chanTrim = (2.0f * r01) - 1.0f;

            s.driftPhase = (Real)(r01 * 2.0f * 3.14159265358979323846f);

            const uint32_t seed = x;
            s.trimA = u32_to_bipolar(splitmix32(seed ^ 0xA1B2C3D4u));
            s.trimB = u32_to_bipolar(splitmix32(seed ^ 0x1F2E3D4Cu));
            s.trimC = u32_to_bipolar(splitmix32(seed ^ 0x55AA55AAu));
            s.trimT = u32_to_bipolar(splitmix32(seed ^ 0xDEADBEEFu));

            s.denormCount = x ^ 0x9E3779B9u;
        }

        reset(s);
    }

    static inline void reset(State& s) noexcept
    {
        s.envP = 0.0;
        s.env  = 0.0;

        s.sagFast = 0.0;
        s.sagSlow = 0.0;
        s.sagEnv  = 0.0;
        s.lf_lp   = 0.0;

        s.meas_x1 = 0.0;
        s.meas_y1 = 0.0;

        s.dc_x1 = 0.0;
        s.dc_y1 = 0.0;

        s.drift   = 0.0;
        s.driftLP = 0.0;

        s.even_x1 = 0.0;
        s.cub_x1  = 0.0;

        s.sub_lp = 0.0;
        s.preShelf_lp = 0.0;
        s.split_lp = 0.0;

        s.magIn  = 0.0;
        s.magOut = 0.0;

        s.trafoIn_prevX  = 0.0;
        s.trafoOut_prevX = 0.0;

        s.atanIn_x1  = 0.0;
        s.atanOut_x1 = 0.0;

        s.ironLF.reset();
        s.presencePk.reset();
        s.ironSig_sm = 0.0;

        s.hf_lp = 0.0;
        s.hfEnv = 0.0;

        s.preEdge_lp = 0.0;
        s.slew_y = 0.0;

        s.amp1_x1 = 0.0;
        s.amp1_x2 = 0.0;
        s.amp2_x1 = 0.0;

        s.inter_lp = 0.0;
        s.inter_hf = 0.0;

        s.pres_hi_lp = 0.0;
        s.pres_lo_lp = 0.0;

        s.ap1_x1 = s.ap1_y1 = 0.0;
        s.ap2_x1 = s.ap2_y1 = 0.0;

        for (int i = 0; i < State::kDiffN; ++i) { s.diff_x1[i] = 0.0; s.diff_y1[i] = 0.0; }
        s.diffFb    = 0.0;
        s.diffDamp  = 0.0;
        s.diffHP_lp = 0.0;

        s.diffRms   = 0.0;
        s.diffDC_x1 = 0.0;
        s.diffDC_y1 = 0.0;

        s.load_lp  = 0.0;
        s.deFizz_lp = 0.0;
        s.post1_lp  = 0.0;
        s.post2_lp  = 0.0;

        s.tone1_sm = 0.5;
        s.tone2_sm = 0.5;

        s.airShelf.reset();
        s.airGain_sm = 0.0;

        s.biquadCtr = 0u;

        s.denormCount ^= 0xA5A5A5A5u;
    }

    // -------------------------------------------------------------------------
    static inline float process(State& s, float xIn, const plugin::Knobs& k) noexcept
    {
        if (!std::isfinite(xIn))
            return 0.0f;

        if (std::fabs((double)xIn) < 1.0e-20)
            xIn += (float)tinyNoiseDenormSafe(s);

        const Real sr = (Real)s.sr;
        Real x = (Real)xIn;

        // Knob smoothing (API = feeling más “directo”: un poco más rápido)
        {
            const Real aK = alphaFromMsT((Real)9.0, sr);
            s.tone1_sm = onePoleLPT(s.tone1_sm, (Real)k.tone1_01, aK);
            s.tone2_sm = onePoleLPT(s.tone2_sm, (Real)k.tone2_01, aK);
        }

        const Real tone1 = clampT(s.tone1_sm, (Real)0.0, (Real)1.0);
        const Real tone2 = clampT(s.tone2_sm, (Real)0.0, (Real)1.0);

        auto shapeMacro = [&](Real v, Real kShape) noexcept -> Real
        {
            v = clampT(v, (Real)0.0, (Real)1.0);
            const Real xx = (v - (Real)0.5) * (Real)2.0;
            const Real denom = std::tanh(kShape);
            const Real y = (std::fabs(denom) > (Real)1.0e-18) ? (std::tanh(kShape * xx) / denom) : xx;
            return (Real)0.5 + (Real)0.5 * y;
        };

        const Real t1m = shapeMacro(tone1, (Real)3.2);
        const Real t2m = shapeMacro(tone2, (Real)3.6);

        // Tone1: izq hard punch, der smooth
        const Real smooth01 = pow01(t1m, (Real)0.85);
        const Real punch01  = pow01((Real)1.0 - t1m, (Real)0.85);

        // Tone2: izq round, der presence/air
        const Real pres01   = pow01(t2m, (Real)0.78);
        const Real round01  = pow01((Real)1.0 - t2m, (Real)0.85);

        const Real t1Dist = std::fabs(tone1 - (Real)0.5) * (Real)2.0;
        const Real t2Dist = std::fabs(tone2 - (Real)0.5) * (Real)2.0;
        const Real macroI = (Real)1.0 + (Real)1.7 * pow01((t1Dist + t2Dist) * (Real)0.5, (Real)1.15);

        // ---- Independent trims ----
        const Real trimFc    = (Real)(1.0f + 0.012f * s.trimA);
        const Real trimDrive = (Real)(1.0f + 0.022f * s.trimB);
        const Real trimBias  = (Real)(1.0f + 0.022f * s.trimC);
        const Real trimTrafo = (Real)(1.0f + 0.022f * s.trimT);

        const bool doBiquadUpdate = ((++s.biquadCtr & 31u) == 0u);
        constexpr uint32_t kBiquadRampN = 32u;

        // 0) DC blocker
        {
            const Real R = alphaFromHzT((Real)6.0, sr);
            const Real y0 = (x - s.dc_x1) + R * s.dc_y1;
            s.dc_x1 = x;
            s.dc_y1 = y0;
            x = y0;
        }

        // Calibración headroom (-18 dBFS ≈ 0 VU)
        x *= (Real)kInCal;

        // 1) Detector + env + sag (API: más rápido y menos)
        Real xm = 0.0;
        {
            // LF weighting (API = tight): menor peso
            Real lfInst = 0.0;
            {
                const Real aLF = alphaFromHzT((Real)220.0 * trimFc, sr);
                s.lf_lp = onePoleLPT(s.lf_lp, x, aLF);
                lfInst = std::fabs(s.lf_lp);
            }

            const Real aHP = std::exp(-2.0 * kPi * ((Real)95.0 / sr));
            xm = onePoleHPT(s.meas_x1, s.meas_y1, x, aHP);

            const Real p = xm * xm;

            const Real measMs = 12.0;
            const Real aMeas  = alphaFromMsT(measMs, sr);
            s.envP = aMeas * s.envP + (1.0 - aMeas) * p;
            s.envP = clampT(s.envP, (Real)0.0, (Real)64.0);

            const Real inst = std::sqrt(s.envP + 1.0e-18);

            // env más rápido
            const Real atkMs = 1.4;
            const Real relMs = 72.0;
            const Real aAtk  = alphaFromMsT(atkMs, sr);
            const Real aRel  = alphaFromMsT(relMs, sr);
            const Real a = (inst > s.env) ? aAtk : aRel;
            s.env = a * s.env + (1.0 - a) * inst;

            // sag rápido + menor (y algo de LF)
            {
                const Real weighted = inst + (Real)0.32 * lfInst;

                const Real aF = alphaFromMsT((Real)18.0, sr);
                const Real aS = alphaFromMsT((Real)220.0, sr);

                s.sagFast = aF * s.sagFast + (1.0 - aF) * weighted;
                s.sagSlow = aS * s.sagSlow + (1.0 - aS) * weighted;

                s.sagFast = clampT(s.sagFast, (Real)0, (Real)16);
                s.sagSlow = clampT(s.sagSlow, (Real)0, (Real)16);

                s.sagEnv = (Real)0.62 * s.sagFast + (Real)0.38 * s.sagSlow;
            }
        }

        const Real env01 = clamp01T(s.env * (Real)1.55);
        const Real sag01 = clamp01T(s.sagEnv * (Real)0.050);

        // Sag gain (API: menor, recovery rápido; Tone1 punch lo enfatiza un poco)
        {
            const Real sagAmt = (Real)0.035 + (Real)0.095 * punch01;
            const Real sagG   = 1.0 / (1.0 + sagAmt * (s.sagEnv * 1.00));
            x *= sagG;
        }

        // Drift + even (API: menos even “warmth”, más limpio)
        {
            Real fHz = (Real)0.010 + (Real)0.030 * ((Real)0.25 + (Real)0.75 * env01);
            fHz *= ((Real)0.90 + (Real)0.25 * smooth01);
            fHz = clampT(fHz, (Real)0.008, (Real)0.060);

            s.driftPhase += (Real)(2.0 * kPi) * (fHz / sr);
            if (s.driftPhase > (Real)(2.0 * kPi)) s.driftPhase -= (Real)(2.0 * kPi);

            const Real n = std::sin(s.driftPhase);

            const Real aLP = alphaFromHzT((Real)0.05, sr);
            s.driftLP = onePoleLPT(s.driftLP, n, aLP);

            const Real leakAlpha = std::exp(-1.0 / ((Real)22.0 * sr));
            s.drift = s.drift * leakAlpha + (1.0 - leakAlpha) * s.driftLP;
            s.drift = clampT(s.drift, (Real)-1.0, (Real)1.0);

            const Real baseEven =
                ((Real)0.0020 + (Real)0.0075 * env01 + (Real)0.0025 * sag01)
                * (Real)(1.0f + 0.06f * s.trimC);

            Real evenMul = 1.0 + (Real)0.25 * s.drift;
            evenMul = clampT(evenMul, (Real)0.75, (Real)1.25);

            const Real evenAmt = baseEven * evenMul * ((Real)0.60 + (Real)0.80 * smooth01);

            x += evenAmt * adaaEven1(x, s.even_x1);
        }

        // 2) Subsonic HPF (API tighter: un poco más alto)
        {
            const Real fc = (Real)24.0 * trimFc;
            const Real a  = alphaFromHzT(fc, sr);
            s.sub_lp = onePoleLPT(s.sub_lp, x, a);
            x = x - s.sub_lp;
        }

        // 3) Presence pre-emphasis (API: 2–5k, más “punch”)
        {
            const Real fc = ((Real)3300.0 - (Real)700.0 * env01) * trimFc;
            const Real a  = alphaFromHzT(clampT(fc, (Real)1800.0, (Real)5200.0), sr);

            s.preShelf_lp = onePoleLPT(s.preShelf_lp, x, a);
            const Real highSh = x - s.preShelf_lp;

            const Real presGain = ((Real)0.06 + (Real)0.28 * env01) * ((Real)0.85 + (Real)0.45 * pres01);
            x = x + presGain * highSh;
        }

        // 4) Split low/high (API: split un poco más alto)
        Real low = 0.0;
        Real high = 0.0;
        {
            const Real splitFc = ((Real)2450.0 - (Real)650.0 * env01) * trimFc;
            const Real a = alphaFromHzT(clampT(splitFc, (Real)1300.0, (Real)3600.0), sr);

            s.split_lp = onePoleLPT(s.split_lp, x, a);
            low  = s.split_lp;
            high = x - low;
        }

        // 5) Drives por banda (API = más “mid punch”, menos low bloom)
        {
            Real driveLo = ((Real)0.95 + (Real)0.30 * env01) * trimDrive;
            Real driveHi = ((Real)1.10 + (Real)0.95 * env01) * trimDrive;

            // Tone1 punch empuja transiente/odd en high, Tone1 smooth relaja
            driveHi *= (Real)(0.95 + 0.75 * punch01);
            driveLo *= (Real)(0.95 + 0.20 * smooth01);

            // Tone2 presence empuja high drive sutil
            driveHi *= (Real)(0.95 + 0.22 * pres01);

            low  *= driveLo;
            high *= driveHi;
        }

        // 6) Trafo IN (API input iron: más tight, menos hyst)
        low = transformerHystADAA_HQ(
            s.magIn, s.trafoIn_prevX, s.atanIn_x1,
            low, env01, sag01, sr,
            (Real)72.0 * trimFc * trimTrafo,  // fc más alto
            (Real)0.16,                       // menos hyst
            (Real)1.85 * trimDrive,
            (Real)0.007 * trimBias,
            (Real)(0.16 * (0.95 + 0.10 * trimTrafo)) // un poco más inyección
        );

        // 7) HIGH chain
        // 7.a) Presence band shaping (2–5k) para “punch” (modulado por Tone2/env)
        {
            const Real fcHi = ((Real)5200.0 - (Real)900.0 * env01) * trimFc;
            const Real fcLo = ((Real)1800.0 - (Real)400.0 * env01) * trimFc;

            const Real aHi = alphaFromHzT(clampT(fcHi, (Real)2800.0, (Real)7000.0), sr);
            const Real aLo = alphaFromHzT(clampT(fcLo, (Real) 900.0, (Real)2600.0), sr);

            s.pres_hi_lp = onePoleLPT(s.pres_hi_lp, high, aHi);
            s.pres_lo_lp = onePoleLPT(s.pres_lo_lp, high, aLo);

            const Real presBand = s.pres_hi_lp - s.pres_lo_lp;

            const Real gDb = ((Real)0.10 + (Real)1.10 * env01) * ((Real)0.75 + (Real)0.55 * pres01);
            const Real g   = dbToLinT(gDb) - 1.0;

            high = high + presBand * g;
        }

        // 7.b) HF de-esser rápido pero más sutil (Tone2 round modula release)
        Real hf01 = 0.0;
        {
            const Real fcMeas = (Real)10500.0 * trimFc;
            const Real aMeas  = alphaFromHzT(clampT(fcMeas, (Real)6500.0, (Real)19000.0), sr);

            s.hf_lp = onePoleLPT(s.hf_lp, high, aMeas);
            const Real hf = high - s.hf_lp;
            const Real e  = std::fabs(hf);

            const Real aAtk = alphaFromMsT((Real)0.7, sr);
            const Real relMsBase = (Real)22.0;
            const Real relMs = relMsBase * ((Real)0.85 + (Real)0.90 * round01);
            const Real aRel = alphaFromMsT(relMs, sr);

            const Real a = (e > s.hfEnv) ? aAtk : aRel;

            s.hfEnv = a * s.hfEnv + (1.0 - a) * e;
            s.hfEnv = clampT(s.hfEnv, (Real)0.0, (Real)8.0);

            hf01 = clamp01T(s.hfEnv * (Real)1.45);
        }

        // de-esser amount: más con round, menos con presence
        hf01 *= (Real)(0.20 + 1.05 * round01);

        // 7.c) Pre-edge LP (API = edge más alto, transiente más “snap”)
        {
            Real fc = ((Real)72000.0 - (Real)26000.0 * env01 - (Real)7000.0 * sag01) * trimFc;

            fc *= (Real)(0.82 + 0.55 * pres01);
            fc *= (1.0 - (Real)0.26 * hf01);

            const Real a  = alphaFromHzT(clampT(fc, (Real)22000.0, (Real)98000.0), sr);

            s.preEdge_lp = onePoleLPT(s.preEdge_lp, high, a);
            high = s.preEdge_lp;
        }

        // 7.d) Soft-slew limiting (más rápido, mantiene golpe)
        {
            Real slewMax = (Real)1.30 - (Real)0.45 * env01 - (Real)0.08 * sag01;
            slewMax = clampT(slewMax, (Real)0.35, (Real)1.55);
            slewMax *= ((Real)0.95 + (Real)0.25 * pres01);

            const Real dx = high - s.slew_y;
            const Real dy = slewMax * std::tanh(dx / (slewMax + (Real)1.0e-12));
            s.slew_y += dy;
            high = s.slew_y;
        }

        // 7.e) Amp stage 1 (ADAA2): opamp bite (más odd, más punch)
        Real y1 = ampStage1ADAA2(s, high, env01, sag01, punch01);

        // 7.f) Interstage RC (tight)
        {
            const Real aHP = alphaFromHzT((Real)55.0 * trimFc, sr);
            s.inter_lp = onePoleLPT(s.inter_lp, y1, aHP);
            Real z = y1 - s.inter_lp;

            const Real aLP = alphaFromHzT(((Real)64000.0 - (Real)18000.0 * env01) * trimFc, sr);
            s.inter_hf = onePoleLPT(s.inter_hf, z, aLP);
            y1 = s.inter_hf;
        }

        // 7.g) Amp stage 2 (ADAA1 + cubic ADAA1): presencia/edge
        Real y2 = ampStage2ADAA(s, y1, env01, sag01, pres01);

        Real y = low + y2;

        // 8) Trafo OUT (API output iron: tight + punch)
        y = transformerHystADAA_HQ(
            s.magOut, s.trafoOut_prevX, s.atanOut_x1,
            y, env01, sag01, sr,
            (Real)58.0 * trimFc * trimTrafo,
            (Real)0.15,
            (Real)1.70 * trimDrive,
            (Real)0.006 * trimBias,
            (Real)(0.14 * (0.95 + 0.10 * trimTrafo))
        );

        // 8.b) “Firma” API: LF tight bump + Presence peak (dependiente de drive/env)
        {
            const Real sigTarget = clamp01T((Real)0.18 + (Real)0.82 * ((Real)0.70*env01 + (Real)0.30*sag01));
            const Real aS = alphaFromMsT((Real)45.0, sr);
            s.ironSig_sm = onePoleLPT(s.ironSig_sm, sigTarget, aS);

            if (doBiquadUpdate)
            {
                // LF tight bump 80–120 Hz, +0..~0.8 dB
                const Real lfFc   = ((Real)92.0 + (Real)28.0 * s.ironSig_sm) * trimFc;
                const Real lfGain = (Real)0.10 + (Real)0.70 * s.ironSig_sm;
                biquadPeakRBJ(s.ironLF, sr, lfFc, lfGain, (Real)0.85, kBiquadRampN);

                // Presence peak 2.8–4 kHz (Tone2 presence lo sube)
                const Real pFc   = ((Real)3000.0 + (Real)1200.0 * pres01) * trimFc;
                const Real pGain = ((Real)0.10 + (Real)1.05 * s.ironSig_sm) * ((Real)0.65 + (Real)0.70 * pres01)
                                 - ((Real)0.25 * round01);
                biquadPeakRBJ(s.presencePk, sr, pFc, pGain, (Real)1.10, kBiquadRampN);
            }

            y = s.ironLF.process(y);
            y = s.presencePk.process(y);
        }

        // C) Allpass “punch glue” (menos smear, más arriba)
        {
            Real f1 = ((Real)1100.0 + (Real) 900.0 * env01 + (Real)220.0 * sag01) * trimFc;
            Real f2 = ((Real)2600.0 + (Real)1500.0 * env01 + (Real)260.0 * sag01) * trimFc;

            const Real apMul =
                ((Real)1.02 - (Real)0.28 * smooth01) *
                ((Real)0.95 + (Real)0.18 * pres01);

            f1 *= apMul;
            f2 *= apMul;

            const Real a1 = allpassCoefFromHzT(clampT(f1, (Real)240.0, (Real)11000.0), sr);
            const Real a2 = allpassCoefFromHzT(clampT(f2, (Real)420.0, (Real)16000.0), sr);

            y = allpass1T(y, s.ap1_x1, s.ap1_y1, a1);
            y = allpass1T(y, s.ap2_x1, s.ap2_y1, a2);
        }

        // Punch Bus paralelo (micro diffusion corto)
        {
            const Real dry = y;

            // API = menos “space”: solo cuando Tone1 smooth lo pide un poco
            Real busMix = (Real)0.00 + (Real)0.35 * (smooth01 * smooth01);
            busMix = clampT(busMix, (Real)0.0, (Real)0.35);

            Real fb = (Real)0.00 + (Real)0.12 * (smooth01 * smooth01);
            fb *= (Real)(0.85 + (Real)0.25 * round01);
            fb = clampT(fb, (Real)0.0, (Real)0.12);

            busMix = clampT(busMix * macroI, (Real)0.0, (Real)0.38);
            fb     = clampT(fb     * macroI, (Real)0.0, (Real)0.13);

            Real dampFc = (Real)9000.0 + (Real)16000.0 * pres01;
            dampFc *= ((Real)1.00 - (Real)0.14 * env01);
            dampFc = clampT(dampFc, (Real)4500.0, (Real)(0.45 * sr));
            const Real aDamp = alphaFromHzT(dampFc, sr);

            // frecuencias cortas (early-ish)
            Real fcList[State::kDiffN] = { (Real)520.0, (Real)980.0, (Real)1700.0, (Real)2900.0 };
            const Real smearMul =
                ((Real)1.15 - (Real)0.75 * smooth01) *
                ((Real)0.90 + (Real)0.35 * pres01);

            for (int i = 0; i < State::kDiffN; ++i)
                fcList[i] = fcList[i] * smearMul * trimFc;

            // HPF en feedback (API = tight): 35 Hz
            {
                const Real fcHP = (Real)35.0 * trimFc;
                const Real aHP  = alphaFromHzT(fcHP, sr);
                s.diffHP_lp = onePoleLPT(s.diffHP_lp, s.diffFb, aHP);
                const Real fbHP = s.diffFb - s.diffHP_lp;
                s.diffFb = fbHP;
            }

            Real wetIn = dry + s.diffFb;

            s.diffDamp = onePoleLPT(s.diffDamp, wetIn, aDamp);
            wetIn = s.diffDamp;

            Real wetLoop = runDiffuserAllpassCascade(s, wetIn, fcList, sr);

            // soft safety
            {
                const Real ksc = (Real)1.05 + (Real)0.70 * smooth01;
                wetLoop = softClipTanh(wetLoop, ksc);
            }

            // RMS governor
            {
                const Real aR = alphaFromMsT((Real)45.0, sr);
                s.diffRms = onePoleLPT(s.diffRms, wetLoop * wetLoop, aR);
                const Real rms = std::sqrt(s.diffRms + (Real)1.0e-18);

                const Real target = (Real)0.22;
                const Real g = (rms > target) ? (target / rms) : (Real)1.0;
                wetLoop *= g;
            }

            // DC guard HPF 10 Hz
            {
                const Real aHPdc = alphaFromHzT((Real)10.0 * trimFc, sr);
                wetLoop = onePoleHPT(s.diffDC_x1, s.diffDC_y1, wetLoop, aHPdc);
            }

            s.diffFb = clampT(wetLoop * fb, (Real)-0.22, (Real)0.22);
            if (std::fabs(s.diffFb) < (Real)1.0e-20)
                s.diffFb += (Real)tinyNoiseDenormSafe(s);

            const Real m  = clampT(busMix, (Real)0, (Real)0.38);
            const Real gW = std::sqrt(m);
            const Real gD = std::sqrt((Real)1 - m);
            y = dry * gD + wetLoop * gW;
        }

        // 9) Loading LP (API: deja más top, menos damping)
        {
            Real fc = ((Real)78000.0 - (Real)32000.0 * ((Real)0.70 * env01 + (Real)0.30 * sag01)) * trimFc;
            fc *= (Real)(0.95 + (Real)0.25 * pres01);

            const Real a  = alphaFromHzT(clampT(fc, (Real)22000.0, (Real)98000.0), sr);

            s.load_lp = onePoleLPT(s.load_lp, y, a);
            y = s.load_lp;
        }

        // 10) De-fizz dinámico (API: menos agresivo)
        {
            Real fc = ((Real)23000.0 - (Real)12000.0 * env01) * trimFc;
            fc *= (Real)(0.85 + (Real)0.45 * pres01);

            const Real a  = alphaFromHzT(clampT(fc, (Real)9000.0, (Real)30000.0), sr);

            s.deFizz_lp = onePoleLPT(s.deFizz_lp, y, a);
            y = s.deFizz_lp;
        }

        // 11) Post HF damping 2 polos (API: más arriba)
        {
            Real fc = ((Real)30000.0 - (Real)17000.0 * env01) * trimFc;
            fc *= (Real)(0.85 + (Real)0.35 * pres01);

            const Real a  = alphaFromHzT(clampT(fc, (Real)11000.0, (Real)38000.0), sr);

            s.post1_lp = onePoleLPT(s.post1_lp, y, a);
            s.post2_lp = onePoleLPT(s.post2_lp, s.post1_lp, a);
            y = s.post2_lp;
        }

        // 12) Air shelf (RBJ) API-ish
        {
            // round = recorta un poco, presence = abre
            const Real airDbTarget = ((Real)-2.0 * round01) + ((Real)5.0 * pres01);

            const Real aG = alphaFromMsT((Real)28.0, sr);
            s.airGain_sm = onePoleLPT(s.airGain_sm, airDbTarget, aG);

            const Real fcAir = (Real)10500.0 * trimFc;
            const Real slope = (Real)0.65;

            if (doBiquadUpdate)
                biquadHighShelfRBJ(s.airShelf, sr, fcAir, s.airGain_sm, slope, kBiquadRampN);

            y = s.airShelf.process(y);
        }

        // Calibración de salida
        y *= (Real)kOutCal;

        if (!std::isfinite(y))
            return 0.0f;

        y = clampT(y, (Real)-1.20, (Real)1.20);
        return (float)y;
    }

    static inline float process(State& s, float xIn) noexcept
    {
        plugin::Knobs kDefault;
        return process(s, xIn, kDefault);
    }

    static inline float process(float x) noexcept
    {
        if (!std::isfinite(x))
            return 0.0f;

        float y = (2.0f / 3.14159265358979323846f) * std::atan(2.0f * x);
        y = std::tanh(2.5f * y);
        y = (2.0f / 3.14159265358979323846f) * std::atan(1.7f * y);
        y = clampf(y, -1.20f, 1.20f);
        return y;
    }

private:
    // ------------------ constants ------------------
    static constexpr Real kPi  = 3.141592653589793238462643383279502884;
    static constexpr Real kLn2 = 0.693147180559945309417232121458176568;

    // Calibración -18 dBFS ≈ 0 VU
    static constexpr float kInCal  = 7.943282347242814f;      // 10^(18/20)
    static constexpr float kOutCal = 0.12589254117941673f;    // 10^(-18/20)

    // ------------------ splitmix + bipolar helpers ------------------
    static inline uint32_t splitmix32(uint32_t x) noexcept
    {
        x += 0x9E3779B9u;
        x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
        x = (x ^ (x >> 13)) * 0xC2B2AE35u;
        return x ^ (x >> 16);
    }

    static inline float u32_to_bipolar(uint32_t x) noexcept
    {
        const float r01 = (float)(x & 0xFFFFu) * (1.0f / 65535.0f);
        return 2.0f * r01 - 1.0f;
    }

    static inline float clampf(float v, float lo, float hi) noexcept
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    template <typename T>
    static inline T clampT(T v, T lo, T hi) noexcept
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    template <typename T>
    static inline T clamp01T(T v) noexcept
    {
        return (v < (T)0) ? (T)0 : (v > (T)1) ? (T)1 : v;
    }

    template <typename T>
    static inline T onePoleLPT(T y1, T x, T a) noexcept
    {
        return ((T)1 - a) * x + a * y1;
    }

    template <typename T>
    static inline T onePoleHPT(T& x1, T& y1, T x, T a) noexcept
    {
        const T y = a * (y1 + x - x1);
        x1 = x;
        y1 = y;
        return y;
    }

    template <typename T>
    static inline T alphaFromHzT(T fc, T sr) noexcept
    {
        const T safeSr = (sr > (T)1 ? sr : (T)1);
        return std::exp((T)(-2) * (T)kPi * (fc / safeSr));
    }

    template <typename T>
    static inline T alphaFromMsT(T ms, T sr) noexcept
    {
        const T tau = ms * (T)0.001;
        const T safeSr = (sr > (T)1 ? sr : (T)1);
        const T denom = tau * safeSr;
        if (denom <= (T)1.0e-12) return (T)0;
        return std::exp((T)(-1) / denom);
    }

    template <typename T>
    static inline T dbToLinT(T db) noexcept
    {
        return std::exp((T)0.1151292546497022842 * db); // ln(10)/20
    }

    static inline Real pow01(Real x, Real p) noexcept
    {
        x = clampT(x, (Real)0.0, (Real)1.0);
        return std::pow(x, p);
    }

    static inline Real tinyNoiseDenormSafe(State& s) noexcept
    {
        s.denormCount += 1u;
        const Real sign = (s.denormCount & 1u) ? (Real)1.0 : (Real)-1.0;
        return sign * (Real)1.0e-20;
    }

    // ------------------ allpass helpers ------------------
    template <typename T>
    static inline T allpassCoefFromHzT(T fc, T sr) noexcept
    {
        const T safeSr = (sr > (T)1 ? sr : (T)1);
        const T ratio = clampT(fc / safeSr, (T)0, (T)0.49);
        const T w = (T)kPi * ratio;

        const T t = std::tan(w);
        const T a = ((T)1 - t) / ((T)1 + t);

        return clampT(a, (T)-0.9999, (T)0.9999);
    }

    template <typename T>
    static inline T allpass1T(T x, T& x1, T& y1, T a) noexcept
    {
        const T y = (-a * x) + x1 + (a * y1);
        x1 = x;
        y1 = y;
        return y;
    }

    static inline Real runDiffuserAllpassCascade(State& s, Real x, const Real* fcHz, Real sr) noexcept
    {
        for (int i = 0; i < State::kDiffN; ++i)
        {
            const Real fc = clampT(fcHz[i], (Real)80.0, (Real)(0.45 * sr));
            const Real a  = allpassCoefFromHzT(fc, sr);
            x = allpass1T(x, s.diff_x1[i], s.diff_y1[i], a);
        }
        return x;
    }

    static inline Real softClipTanh(Real x, Real k) noexcept
    {
        const Real kk = clampT(k, (Real)0.25, (Real)8.0);
        const Real n  = std::tanh(kk);
        if (std::fabs(n) < (Real)1.0e-18) return x;
        return std::tanh(kk * x) / n;
    }

    // ------------------ RBJ biquads ------------------
    static inline void biquadHighShelfRBJ(Biquad& q, Real sr, Real fc, Real gainDb, Real slopeS, uint32_t rampSamples) noexcept
    {
        fc = clampT(fc, (Real)10.0, (Real)(0.49 * sr));
        slopeS = clampT(slopeS, (Real)0.2, (Real)2.0);

        const Real A  = std::exp((Real)0.057564627324851142 * gainDb);
        const Real w0 = (Real)2.0 * kPi * (fc / sr);
        const Real cw = std::cos(w0);
        const Real sw = std::sin(w0);

        const Real alpha = sw * (Real)0.5 * std::sqrt((A + (Real)1.0/A) * ((Real)1.0/slopeS - (Real)1.0) + (Real)2.0);
        const Real beta  = (Real)2.0 * std::sqrt(A) * alpha;

        const Real b0 =    A*((A+1) + (A-1)*cw + beta);
        const Real b1 = -2*A*((A-1) + (A+1)*cw);
        const Real b2 =    A*((A+1) + (A-1)*cw - beta);
        const Real a0 =       (A+1) - (A-1)*cw + beta;
        const Real a1 =  2*((A-1) - (A+1)*cw);
        const Real a2 =       (A+1) - (A-1)*cw - beta;

        const Real invA0 = (Real)1.0 / a0;

        q.setTargetCoeffs(
            b0 * invA0,
            b1 * invA0,
            b2 * invA0,
            a1 * invA0,
            a2 * invA0,
            rampSamples
        );
    }

    static inline void biquadPeakRBJ(Biquad& q, Real sr, Real fc, Real gainDb, Real Q, uint32_t rampSamples) noexcept
    {
        fc = clampT(fc, (Real)10.0, (Real)(0.49 * sr));
        Q  = clampT(Q,  (Real)0.2,  (Real)8.0);

        const Real A  = std::exp((Real)0.057564627324851142 * gainDb);
        const Real w0 = (Real)2.0 * kPi * (fc / sr);
        const Real cw = std::cos(w0);
        const Real sw = std::sin(w0);
        const Real alpha = sw / ((Real)2.0 * Q);

        const Real b0 = (Real)1.0 + alpha * A;
        const Real b1 = (Real)-2.0 * cw;
        const Real b2 = (Real)1.0 - alpha * A;
        const Real a0 = (Real)1.0 + alpha / A;
        const Real a1 = (Real)-2.0 * cw;
        const Real a2 = (Real)1.0 - alpha / A;

        const Real invA0 = (Real)1.0 / a0;

        q.setTargetCoeffs(
            b0 * invA0,
            b1 * invA0,
            b2 * invA0,
            a1 * invA0,
            a2 * invA0,
            rampSamples
        );
    }

    // ------------------ ADAA for even/cubic ------------------
    static inline Real F_even(Real x) noexcept
    {
        return ((Real)1.0 / 3.0) * std::fabs(x) * x * x;
    }

    static inline Real adaaEven1(Real x, Real& x1) noexcept
    {
        const Real d = x - x1;
        Real y;
        if (std::fabs(d) > (Real)1.0e-12)
            y = (F_even(x) - F_even(x1)) / d;
        else
            y = x * std::fabs(x);

        x1 = x;
        return y;
    }

    static inline Real F_cubic(Real x) noexcept { return ((Real)0.25) * x * x * x * x; }

    static inline Real adaaCubic1(Real x, Real& x1) noexcept
    {
        const Real d = x - x1;
        Real y;
        if (std::fabs(d) > (Real)1.0e-12)
            y = (F_cubic(x) - F_cubic(x1)) / d;
        else
            y = x * x * x;

        x1 = x;
        return y;
    }

    // ------------------ ADAA helpers (tanh/atan) ------------------
    template <typename T>
    static inline T logCoshStableT(T z) noexcept
    {
        const T az = std::fabs(z);
        if (az > (T)10)
            return az - (T)kLn2;
        return std::log(std::cosh(z));
    }

    template <typename T>
    static inline T adaaTanh1T(T x, T& x1, T k) noexcept
    {
        const T denom = (x - x1);
        T y;

        if (std::fabs(denom) > (T)1.0e-12)
        {
            const T kx  = k * x;
            const T kx1 = k * x1;

            const T Fx  = logCoshStableT(kx)  / k;
            const T Fx1 = logCoshStableT(kx1) / k;

            y = (Fx - Fx1) / denom;
        }
        else
        {
            y = std::tanh(k * x);
        }

        x1 = x;

        const T d = std::tanh(k);
        return (std::fabs(d) > (T)1.0e-18) ? (y / d) : y;
    }

    static inline double li2_series_neg1_0(double x) noexcept
    {
        if (x == -1.0) return -(kPi * kPi) / 12.0;

        double sum = 0.0;
        double term = x;
        for (int n = 1; n <= 64; ++n)
        {
            sum += term / (double)(n * n);
            term *= x;
            if (std::fabs(term) < 1.0e-12)
                break;
        }
        return sum;
    }

    static inline double li2_real_neg(double x) noexcept
    {
        if (x == 0.0)  return 0.0;
        if (x == -1.0) return -(kPi * kPi) / 12.0;

        if (x < -1.0)
        {
            const double inv = 1.0 / x;
            const double L = std::log(-x);
            const double li2inv = li2_series_neg1_0(inv);
            return -li2inv - (kPi * kPi) / 6.0 - 0.5 * L * L;
        }

        return li2_series_neg1_0(x);
    }

    static inline double F2_tanh(double x, double k) noexcept
    {
        double z = k * x;
        if (z >  12.0) z =  12.0;
        if (z < -12.0) z = -12.0;

        const double arg = -std::exp(-2.0 * z);
        const double I = 0.5 * z * z - z * (double)kLn2 + 0.5 * li2_real_neg(arg);

        const double kk = k * k;
        return (kk > 1.0e-18) ? (I / kk) : 0.0;
    }

    template <typename T>
    static inline T adaaTanh2T(T x, T& x1, T& x2, T k) noexcept
    {
        const T z = k * x;
        const bool use2 = (std::fabs(z) > (T)0.25);

        const T d01 = (x  - x1);
        const T d12 = (x1 - x2);
        const T d02 = (x  - x2);

        T y;

        if (!use2
            || std::fabs(d01) < (T)1.0e-12
            || std::fabs(d12) < (T)1.0e-12
            || std::fabs(d02) < (T)1.0e-12)
        {
            y = adaaTanh1T(x, x1, k);
            x2 = x1;
            return y;
        }

        const double F0 = F2_tanh((double)x,  (double)k);
        const double F1 = F2_tanh((double)x1, (double)k);
        const double F2 = F2_tanh((double)x2, (double)k);

        const double s0 = (F0 - F1) / (double)d01;
        const double s1 = (F1 - F2) / (double)d12;

        double yd = 2.0 * (s0 - s1) / (double)d02;

        const double norm = std::tanh((double)k);
        if (std::fabs(norm) > 1.0e-18)
            yd /= norm;

        y = (T)yd;

        x2 = x1;
        x1 = x;

        return y;
    }

    template <typename T>
    static inline T adaaAtan1T(T x, T& x1, T k) noexcept
    {
        constexpr double twoOverPi = 0.63661977236758134308;

        const T denom = (x - x1);
        T y;

        if (std::fabs(denom) > (T)1.0e-12)
        {
            const T kx  = k * x;
            const T kx1 = k * x1;

            const T Fx  = x  * std::atan(kx)  - ((T)0.5 / k) * std::log1p(kx  * kx);
            const T Fx1 = x1 * std::atan(kx1) - ((T)0.5 / k) * std::log1p(kx1 * kx1);

            y = (Fx - Fx1) / denom;
        }
        else
        {
            y = std::atan(k * x);
        }

        x1 = x;
        return (T)(twoOverPi) * y;
    }

    template <typename T>
    static inline T lowLevelBlendT(T x, T y, T knee) noexcept
    {
        const T ax = std::fabs(x);
        const T b  = ax / (ax + knee);
        return x + b * (y - x);
    }

    template <typename T>
    static inline T asymTanhZeroT(T x, T k, T bias) noexcept
    {
        const T y  = std::tanh(k * (x + bias)) - std::tanh(k * bias);

        const T yp = std::tanh(k * ((T)1 + bias)) - std::tanh(k * bias);
        const T yn = std::tanh(k * ((T)-1 + bias)) - std::tanh(k * bias);
        const T m  = (T)0.5 * (std::fabs(yp) + std::fabs(yn));

        return (m > (T)1.0e-18) ? (y / m) : y;
    }

    // ------------------ transformer HQ ------------------
    static inline Real transformerCoreADAA_HQ(Real x, Real& atan_x1, Real env01, Real sag01,
                                              Real driveBase, Real biasBase) noexcept
    {
        const Real drive = (driveBase * (1.0 - 0.05 * sag01)) + 0.24 * env01;
        const Real bias  = (biasBase  + 0.015 * env01) + 0.008 * sag01;

        const Real core  = adaaAtan1T(x, atan_x1, drive);

        const Real x2 = x * x;
        const Real fluxIn = x + ((Real)0.10 + (Real)0.04 * env01) * x * x2;
        const Real flux   = std::tanh(((Real)1.05 + (Real)0.10 * env01) * fluxIn);

        const Real k    = ((Real)1.85 + (Real)1.05 * env01) * (1.0 - (Real)0.05 * sag01);
        const Real asym = asymTanhZeroT(x, k, bias);

        Real y = (Real)0.66 * core + (Real)0.26 * flux + (Real)0.08 * asym;
        y = lowLevelBlendT(x, y, (Real)0.20);

        return clampT(y, (Real)-1.25, (Real)1.25);
    }

    static inline Real transformerHystADAA_HQ(Real& m, Real& prevX, Real& atan_x1,
                                              Real x, Real env01, Real sag01, Real sr,
                                              Real fcBase, Real hystAmt,
                                              Real driveBase, Real biasBase,
                                              Real injectBase) noexcept
    {
        const Real tau = (Real)0.55 + (Real)0.65 * (1.0 - env01);
        const Real leakAlpha = std::exp(-1.0 / (tau * sr));
        m *= leakAlpha;

        const Real dx = x - prevX;
        prevX = x;

        const Real eddyAmt = ((Real)0.30 + (Real)0.45 * env01);
        const Real eddy = 1.0 / (1.0 + eddyAmt * std::fabs(dx));

        const Real fc = (fcBase + ((Real)55.0 * env01));
        const Real a  = alphaFromHzT(fc, sr);

        const Real d = ((Real)1.06 + (Real)0.65 * env01) * (1.0 - (Real)0.05 * sag01);
        const Real h = hystAmt + (Real)0.10 * env01;

        const Real target = std::tanh(d * (x - h * m));
        m = onePoleLPT(m, target, a);

        Real inject = (injectBase + (Real)0.10 * env01 + (Real)0.04 * sag01);
        inject *= eddy;

        const Real xin = x + inject * m;

        return transformerCoreADAA_HQ(xin, atan_x1, env01, sag01, driveBase, biasBase);
    }

    // ------------------ amp stages (API opamp vibe) ------------------
    static inline Real ampStage1ADAA2(State& s, Real x, Real env01, Real sag01, Real punch01) noexcept
    {
        const Real drive = ((Real)1.65 + (Real)0.75 * env01) * (1.0 - (Real)0.06 * sag01);
        const Real k1    = ((Real)3.15 + (Real)1.10 * env01) * (1.0 - (Real)0.05 * sag01);

        // un pelín de bias con punch
        const Real bias = ((Real)0.00025 + (Real)0.00085 * env01) * ((Real)0.25 + (Real)0.75 * punch01);

        const Real pre = (x + bias) * drive;

        Real y = adaaTanh2T(pre, s.amp1_x1, s.amp1_x2, k1);
        y = lowLevelBlendT(x, y, (Real)0.24);

        return clampT(y, (Real)-1.35, (Real)1.35);
    }

    static inline Real ampStage2ADAA(State& s, Real x, Real env01, Real sag01, Real pres01) noexcept
    {
        const Real drive = ((Real)1.20 + (Real)0.40 * env01) * (1.0 - (Real)0.05 * sag01);
        const Real k2    = ((Real)2.25 + (Real)0.95 * env01) * (1.0 - (Real)0.04 * sag01);

        const Real pre = x * drive;

        Real y = adaaTanh1T(pre, s.amp2_x1, k2);

        // ADAA1 cubic: más “edge” cuando hay presence
        const Real c = ((Real)0.004 + (Real)0.018 * env01) * ((Real)0.80 + (Real)0.60 * pres01);
        y += c * adaaCubic1(pre, s.cub_x1);

        y = (Real)0.90 * y + (Real)0.10 * ((Real)0.63661977236758134308 * std::atan((Real)2.5 * y));
        y = lowLevelBlendT(x, y, (Real)0.22);

        return clampT(y, (Real)-1.35, (Real)1.35);
    }
};

