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
//     -> x += evenAmt * (x*abs(x)) con drift ultralento
//  C) Allpass “3D glue” (fase) post-trafo OUT y antes de loading (2 etapas)
//  D) Dynamic HF de-esser ultra rápido (por eventos) que modula preEdge Fc
//
// API requerida por PresetRegistry PRO:
//  - struct State
//  - static void prepare(State&, float)
//  - static void reset(State&)
//  - static float process(State&, float)

#include <cmath>
#include <cstdint>
#include <limits>

struct Preset_Neve1073
{
    static constexpr const char* kDisplayName = "Neve 1073 (PRO ULTIMATE HQ+ CAL/DRIFT/AP)";

    struct State
    {
        float sr = 192000.0f;

        // ---- Deterministic per-channel trim (tolerancias) ----
        float chanTrim = 0.0f; // [-1..+1] estable por instancia/canal

        // ---- RMS envelope (medición perceptual) ----
        float envP = 0.0f;   // potencia (EMA) (medida sobre señal HP)
        float env  = 0.0f;   // sqrt(envP) con atk/rel (sobre RMS)
        float sagEnv = 0.0f; // slow env (supply sag)

        // HPF states SOLO para medición del detector
        float meas_x1 = 0.0f;
        float meas_y1 = 0.0f;

        // ---- DC blocker (audio path) ----
        float dc_x1 = 0.0f;
        float dc_y1 = 0.0f;

        // ---- Asimetría pro: drift lento (temperatura/corriente) ----
        float drift   = 0.0f; // [-1..+1] aprox (muy lento)
        float driftLP = 0.0f; // lowpassed noise/source

        // ---- Subsonic HPF via LP (hp = x - lp) ----
        float sub_lp = 0.0f;

        // ---- Pre-emphasis shelf via LP (high = x - lp) ----
        float preShelf_lp = 0.0f;

        // ---- Split low/high via LP ----
        float split_lp = 0.0f;

        // ---- Transformer hysteresis (magnetización) ----
        float magIn  = 0.0f;
        float magOut = 0.0f;

        // Para eddy-loss: prev input por trafo
        float trafoIn_prevX  = 0.0f;
        float trafoOut_prevX = 0.0f;

        // ADAA memory for atan in transformers
        float atanIn_x1  = 0.0f;
        float atanOut_x1 = 0.0f;

        // ---- Dynamic HF de-esser (por eventos) para modular preEdge ----
        float hf_lp  = 0.0f; // LP para medir HF = x - LP(x, 10k)
        float hfEnv  = 0.0f; // env rápido de HF

        // ---- Pre-edge LP (antes de amp1) ----
        float preEdge_lp = 0.0f;

        // ---- Slew limiting (path amp) ----
        float slew_y = 0.0f;

        // ---- ADAA memory amp stages ----
        // Stage 1 usa ADAA2 -> necesita x1 y x2
        float amp1_x1 = 0.0f;
        float amp1_x2 = 0.0f;

        // Stage 2 usa ADAA1
        float amp2_x1 = 0.0f;

        // ---- Interstage RC (pegamento) ----
        float inter_lp = 0.0f; // para HP por sustracción
        float inter_hf = 0.0f; // LP alto para rounding

        // ---- Mid-forward pre / de-nasal post ----
        float mid_hi_lp = 0.0f;
        float mid_lo_lp = 0.0f;

        float post_mid_hi_lp = 0.0f;
        float post_mid_lo_lp = 0.0f;

        // ---- Allpass “3D glue” (post-trafo OUT) ----
        float ap1_x1 = 0.0f, ap1_y1 = 0.0f;
        float ap2_x1 = 0.0f, ap2_y1 = 0.0f;

        // ---- Loading / impedancias ----
        float load_lp = 0.0f;

        // Dip suave (sin biquads): band = LP_hi - LP_lo (18–30k aprox)
        float loadDip_hi_lp = 0.0f;
        float loadDip_lo_lp = 0.0f;

        // ---- HF control ----
        float deFizz_lp = 0.0f;
        float post1_lp  = 0.0f;
        float post2_lp  = 0.0f;

        // ---- Tiny RNG (anti-denorm + drift source) ----
        uint32_t rng = 0x12345678u;
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
        }

        reset (s);
    }

    static inline void reset (State& s) noexcept
    {
        s.envP = 0.0f;
        s.env  = 0.0f;
        s.sagEnv = 0.0f;

        s.meas_x1 = 0.0f;
        s.meas_y1 = 0.0f;

        s.dc_x1 = 0.0f;
        s.dc_y1 = 0.0f;

        s.drift   = 0.0f;
        s.driftLP = 0.0f;

        s.sub_lp = 0.0f;
        s.preShelf_lp = 0.0f;
        s.split_lp = 0.0f;

        s.magIn  = 0.0f;
        s.magOut = 0.0f;

        s.trafoIn_prevX  = 0.0f;
        s.trafoOut_prevX = 0.0f;

        s.atanIn_x1  = 0.0f;
        s.atanOut_x1 = 0.0f;

        s.hf_lp = 0.0f;
        s.hfEnv = 0.0f;

        s.preEdge_lp = 0.0f;
        s.slew_y = 0.0f;

        s.amp1_x1 = 0.0f;
        s.amp1_x2 = 0.0f;
        s.amp2_x1 = 0.0f;

        s.inter_lp = 0.0f;
        s.inter_hf = 0.0f;

        s.mid_hi_lp = 0.0f;
        s.mid_lo_lp = 0.0f;

        s.post_mid_hi_lp = 0.0f;
        s.post_mid_lo_lp = 0.0f;

        s.ap1_x1 = s.ap1_y1 = 0.0f;
        s.ap2_x1 = s.ap2_y1 = 0.0f;

        s.load_lp = 0.0f;
        s.loadDip_hi_lp = 0.0f;
        s.loadDip_lo_lp = 0.0f;

        s.deFizz_lp = 0.0f;
        s.post1_lp  = 0.0f;
        s.post2_lp  = 0.0f;

        // re-seed suave
        s.rng ^= 0xA5A5A5A5u;
        if (s.rng == 0) s.rng = 0x12345678u;
    }

    static inline float process (State& s, float x) noexcept
    {
        if (!std::isfinite (x))
            return 0.0f;

        // Anti-denorm: solo si cae a valores ultrapequeños
        if (std::fabs (x) < 1.0e-20f)
            x += tinyNoiseDenormSafe (s);

        // ============================================================
        // Arquitectura HQ:
        // DC → (CAL) → detector → asimetría(level+drift) → sub HP → trafo IN
        // → mid-forward → HF de-esser → pre-edge LP → slew → amp1 (ADAA2)
        // → inter RC → amp2 (ADAA1) → de-nasal → trafo OUT
        // → allpass glue → loading LP + dip → defizz/postLP → (CAL OUT)
        // ============================================================

        // ---- Tolerancias por canal (±1–3%) ----
        const float trimFc    = 1.0f + 0.015f * s.chanTrim; // ~±1.5%
        const float trimDrive = 1.0f + 0.020f * s.chanTrim; // ~±2.0%
        const float trimBias  = 1.0f + 0.030f * s.chanTrim; // ~±3.0%

        // ------------------------------------------------------------
        // 0) DC blocker (audio path)
        {
            const float R = alphaFromHz (5.0f, s.sr);
            const float y = (x - s.dc_x1) + R * s.dc_y1;
            s.dc_x1 = x;
            s.dc_y1 = y;
            x = y;
        }

        // ------------------------------------------------------------
        // ✅ A) Calibración headroom real (-18 dBFS ≈ 0 VU)
        // Mapea -18 dBFS RMS (~0.1259) a ~1.0 interno antes de “analógico”.
        // Luego devolvemos al final con kOutCal.
        x *= kInCal;

        // ------------------------------------------------------------
        // 1) Detector perceptual (HPF solo para medición) + env + sag
        {
            // HP de medición (evita que el low-end domine) ~90Hz
            const float aHP = std::exp (-2.0f * kPi * (90.0f / (float) s.sr));
            const float xm  = onePoleHP (s.meas_x1, s.meas_y1, x, aHP);

            const float p = xm * xm;

            const float measMs = 16.0f;
            const float aMeas  = alphaFromMs (measMs, s.sr);
            s.envP = aMeas * s.envP + (1.0f - aMeas) * p;

            // clamps inteligentes (evita runaway)
            s.envP = clampf (s.envP, 0.0f, 64.0f);

            const float inst = std::sqrt (s.envP + 1.0e-12f);

            const float atkMs = 2.7f;
            const float relMs = 105.0f;
            const float aAtk  = alphaFromMs (atkMs, s.sr);
            const float aRel  = alphaFromMs (relMs, s.sr);

            const float a = (inst > s.env) ? aAtk : aRel;
            s.env = a * s.env + (1.0f - a) * inst;

            // sag lento
            const float sagMs = 260.0f;
            const float aSag  = alphaFromMs (sagMs, s.sr);
            s.sagEnv = aSag * s.sagEnv + (1.0f - aSag) * inst;

            s.sagEnv = clampf (s.sagEnv, 0.0f, 16.0f);
        }

        const float env01 = clamp01 (s.env * 1.60f);
        const float sag01 = clamp01 (s.sagEnv * 1.15f);

        // Sag gain (sutil)
        {
            const float sagAmt = 0.18f;
            const float sagG   = 1.0f / (1.0f + sagAmt * (s.sagEnv * 1.25f));
            x *= sagG;
        }

        // ------------------------------------------------------------
        // ✅ B) Asimetría “real”: dependiente de nivel + drift lento con leak
        // - Drift ultralento (simula temperatura/corriente). Leak hacia 0.
        // - Pares sin DC: x += evenAmt * (x * abs(x))
        {
            // drift source (muy pequeño, cero-mean), reutiliza RNG
            float n = tinyNoiseBipolar (s); // [-1..+1] aprox

            // filtra MUY lento para que “camine” suave
            const float aLP = alphaFromHz (0.02f, s.sr);     // ~0.02 Hz (muy lento)
            s.driftLP = onePoleLP (s.driftLP, n, aLP);

            // leak a 0 (no se queda pegado)
            const float leakAlpha = std::exp (-1.0f / (35.0f * (float) s.sr)); // ~35s
            s.drift = s.drift * leakAlpha + (1.0f - leakAlpha) * s.driftLP;

            s.drift = clampf (s.drift, -1.0f, 1.0f);

            // even amount depende de nivel (env) y “supply” (sag), con micro variación por drift
            const float baseEven = (0.0045f + 0.0140f * env01 + 0.0060f * sag01) * (1.0f + 0.08f * s.chanTrim);
            float evenMul = 1.0f + 0.35f * s.drift;
            evenMul = clampf (evenMul, 0.65f, 1.35f);
            const float evenAmt = baseEven * evenMul;

            x += evenAmt * (x * std::fabs (x));
        }

        // ------------------------------------------------------------
        // 2) Subsonic HPF (hp = x - lp)
        {
            const float fc = 18.0f * trimFc;
            const float a  = alphaFromHz (fc, s.sr);
            s.sub_lp = onePoleLP (s.sub_lp, x, a);
            x = x - s.sub_lp;
        }

        // ------------------------------------------------------------
        // 3) Pre-emphasis shelf (menos brillante al empujar)
        {
            const float fc = (4100.0f - 1300.0f * env01) * trimFc;
            const float a  = alphaFromHz (clampHz (fc, 1800.0f, 6500.0f), s.sr);

            s.preShelf_lp = onePoleLP (s.preShelf_lp, x, a);
            const float high = x - s.preShelf_lp;

            const float shelfGain = 0.05f + 0.24f * env01;
            x = x + shelfGain * high;
        }

        // ------------------------------------------------------------
        // 4) Split low/high (Fc dinámica)
        float low = 0.0f;
        float high = 0.0f;
        {
            const float splitFc = (1850.0f - 720.0f * env01) * trimFc;
            const float a = alphaFromHz (clampHz (splitFc, 850.0f, 2600.0f), s.sr);

            s.split_lp = onePoleLP (s.split_lp, x, a);
            low  = s.split_lp;
            high = x - low;
        }

        // ------------------------------------------------------------
        // 5) Drives por banda
        {
            const float driveLo = (1.00f + 0.38f * env01) * trimDrive;
            const float driveHi = (1.00f + 0.92f * env01) * trimDrive;

            low  *= driveLo;
            high *= driveHi;
        }

        // ------------------------------------------------------------
        // 6) Trafo IN (hyst + ADAA atan) con leak + eddy + sag->drive/bias
        low = transformerHystADAA_HQ (
            s.magIn, s.trafoIn_prevX, s.atanIn_x1,
            low, env01, sag01, s.sr,
            55.0f * trimFc,
            0.24f,
            2.08f * trimDrive,
            0.012f * trimBias,
            0.14f
        );

        // ------------------------------------------------------------
        // 7) HIGH: mid-forward → HF de-esser → pre-edge LP → slew → amp1 → inter → amp2 → de-nasal

        // 7.a) Mid-forward pre-amp (band = LP_hi - LP_lo)
        {
            const float fcHi = (2200.0f - 500.0f * env01) * trimFc;
            const float fcLo = (350.0f  - 120.0f * env01) * trimFc;

            const float aHi = alphaFromHz (clampHz (fcHi, 1200.0f, 3200.0f), s.sr);
            const float aLo = alphaFromHz (clampHz (fcLo,  180.0f,  650.0f), s.sr);

            s.mid_hi_lp = onePoleLP (s.mid_hi_lp, high, aHi);
            s.mid_lo_lp = onePoleLP (s.mid_lo_lp, high, aLo);

            const float midBand = s.mid_hi_lp - s.mid_lo_lp;

            const float gDb = 0.15f + 1.35f * env01; // 0..~1.5dB
            const float g   = dbToLin (gDb) - 1.0f;

            high = high + midBand * g;
        }

        // 7.b) ✅ D) Dynamic HF de-esser ultra rápido (por eventos)
        // mide HF > ~10k y baja preEdge Fc cuando hay exceso (platos/sibilancias)
        float hf01 = 0.0f;
        {
            const float fcMeas = 10000.0f * trimFc;
            const float aMeas  = alphaFromHz (clampHz (fcMeas, 6500.0f, 18000.0f), s.sr);

            s.hf_lp = onePoleLP (s.hf_lp, high, aMeas);
            const float hf = high - s.hf_lp;        // “aire/agresivo”
            const float e  = std::fabs (hf);        // detector rápido (barato)

            const float aAtk = alphaFromMs (0.6f, s.sr);
            const float aRel = alphaFromMs (18.0f, s.sr);
            const float a    = (e > s.hfEnv) ? aAtk : aRel;

            s.hfEnv = a * s.hfEnv + (1.0f - a) * e;
            s.hfEnv = clampf (s.hfEnv, 0.0f, 8.0f);

            // escala: 0..1 solo cuando de verdad hay HF fuerte
            hf01 = clamp01 (s.hfEnv * 1.75f);
        }

        // 7.c) Pre-edge LP real (anti-fizz) antes del shaper
        {
            // base fc ~ 55k -> ~25–30k (env/sag) + mod por hf01 (solo cuando hace falta)
            float fc = (55000.0f - 26000.0f * env01 - 9000.0f * sag01) * trimFc;

            // ✅ modulación de-esser: baja fc hasta ~32% cuando hf01→1
            fc *= (1.0f - 0.32f * hf01);

            const float a  = alphaFromHz (clampHz (fc, 16000.0f, 90000.0f), s.sr);

            s.preEdge_lp = onePoleLP (s.preEdge_lp, high, a);
            high = s.preEdge_lp;
        }

        // 7.d) Slew limiting sutil (en oversampled)
        {
            float slewMax = 1.15f - 0.55f * env01 - 0.10f * sag01;
            slewMax = clampf (slewMax, 0.25f, 1.35f);

            const float dy = clampf (high - s.slew_y, -slewMax, +slewMax);
            s.slew_y += dy;
            high = s.slew_y;
        }

        // 7.e) Amp stage 1 (ADAA2 tanh) + sag afecta "softness"
        float y1 = ampStage1ADAA2 (s, high, env01, sag01);

        // 7.f) Interstage RC: HP suave + LP alto
        {
            const float aHP = alphaFromHz (35.0f * trimFc, s.sr);
            s.inter_lp = onePoleLP (s.inter_lp, y1, aHP);
            float z = y1 - s.inter_lp;

            const float aLP = alphaFromHz ((52000.0f - 18000.0f * env01) * trimFc, s.sr);
            s.inter_hf = onePoleLP (s.inter_hf, z, aLP);
            y1 = s.inter_hf;
        }

        // 7.g) Amp stage 2 (ADAA1) + hair refinado
        float y2 = ampStage2ADAA (s, y1, env01, sag01);

        // 7.h) Post de-nasal (suave)
        {
            const float fcHi = 2400.0f * trimFc;
            const float fcLo = 420.0f  * trimFc;

            const float aHi = alphaFromHz (fcHi, s.sr);
            const float aLo = alphaFromHz (fcLo, s.sr);

            s.post_mid_hi_lp = onePoleLP (s.post_mid_hi_lp, y2, aHi);
            s.post_mid_lo_lp = onePoleLP (s.post_mid_lo_lp, y2, aLo);

            const float midBand = s.post_mid_hi_lp - s.post_mid_lo_lp;

            const float gDb = 0.10f + 0.65f * env01;
            const float g   = dbToLin (gDb) - 1.0f;

            y2 = y2 - midBand * g;
        }

        high = y2;

        float y = low + high;

        // ------------------------------------------------------------
        // 8) Trafo OUT (hyst + ADAA atan) con leak + eddy + sag->drive/bias
        y = transformerHystADAA_HQ (
            s.magOut, s.trafoOut_prevX, s.atanOut_x1,
            y, env01, sag01, s.sr,
            42.0f * trimFc,
            0.19f,
            1.88f * trimDrive,
            0.008f * trimBias,
            0.12f
        );

        // ------------------------------------------------------------
        // ✅ C) Allpass “3D glue” (fase): después del trafo OUT y antes del loading
        {
            // Frecuencias suaves y dependientes de env/sag. 2 etapas para más “depth”.
            const float f1 = (650.0f  + 950.0f  * env01 + 250.0f * sag01) * trimFc;
            const float f2 = (1800.0f + 1400.0f * env01 + 350.0f * sag01) * trimFc;

            const float a1 = allpassCoefFromHz (clampHz (f1, 120.0f, 8000.0f),  s.sr);
            const float a2 = allpassCoefFromHz (clampHz (f2, 240.0f, 12000.0f), s.sr);

            y = allpass1 (y, s.ap1_x1, s.ap1_y1, a1);
            y = allpass1 (y, s.ap2_x1, s.ap2_y1, a2);
        }

        // ------------------------------------------------------------
        // 9) Loading LP dinámico (hardware top-end)
        {
            const float fc = (65000.0f - 38000.0f * (0.65f * env01 + 0.35f * sag01)) * trimFc;
            const float a  = alphaFromHz (clampHz (fc, 18000.0f, 90000.0f), s.sr);

            s.load_lp = onePoleLP (s.load_lp, y, a);
            y = s.load_lp;
        }

        // 10) Dip suave 18–30k (sin biquads)
        {
            const float fcHi = (30000.0f - 7000.0f * env01) * trimFc;
            const float fcLo = (18000.0f - 4000.0f * env01) * trimFc;

            const float aHi = alphaFromHz (clampHz (fcHi, 14000.0f, 60000.0f), s.sr);
            const float aLo = alphaFromHz (clampHz (fcLo,  9000.0f, 45000.0f), s.sr);

            s.loadDip_hi_lp = onePoleLP (s.loadDip_hi_lp, y, aHi);
            s.loadDip_lo_lp = onePoleLP (s.loadDip_lo_lp, y, aLo);

            const float band = s.loadDip_hi_lp - s.loadDip_lo_lp;

            const float dipDb = 0.05f + 0.75f * env01;
            const float g = dbToLin (dipDb) - 1.0f;

            y = y - band * g;
        }

        // ------------------------------------------------------------
        // 11) De-fizz dinámico
        {
            const float fc = (19500.0f - 12000.0f * env01) * trimFc;
            const float a  = alphaFromHz (clampHz (fc, 6000.0f, 24000.0f), s.sr);

            s.deFizz_lp = onePoleLP (s.deFizz_lp, y, a);
            y = s.deFizz_lp;
        }

        // ------------------------------------------------------------
        // 12) Post HF damping 2 polos
        {
            const float fc = (25500.0f - 16000.0f * env01) * trimFc;
            const float a  = alphaFromHz (clampHz (fc, 8000.0f, 32000.0f), s.sr);

            s.post1_lp = onePoleLP (s.post1_lp, y, a);
            s.post2_lp = onePoleLP (s.post2_lp, s.post1_lp, a);
            y = s.post2_lp;
        }

        // ------------------------------------------------------------
        // ✅ A) Calibración de salida (vuelve a dominio digital)
        y *= kOutCal;

        // Seguridad final
        if (!std::isfinite (y))
            return 0.0f;

        return clampf (y, -1.20f, 1.20f);
    }

    // Legacy stateless (por si alguien lo llama)
    static inline float process (float x) noexcept
    {
        if (!std::isfinite (x))
            return 0.0f;

        float y = (2.0f / 3.14159265358979323846f) * std::atan (2.0f * x);
        y = std::tanh (2.5f * y);
        y = (2.0f / 3.14159265358979323846f) * std::atan (1.7f * y);
        return clampf (y, -1.20f, 1.20f);
    }

private:
    // ------------------ utilities ------------------

    static constexpr float kPi  = 3.14159265358979323846f;
    static constexpr float kLn2 = 0.69314718055994530942f;

    // ✅ Calibración -18 dBFS ≈ 0 VU
    static constexpr float kInCal  = 7.943282347242814f;   // 10^(+18/20)
    static constexpr float kOutCal = 0.12589254117941673f; // 10^(-18/20)

    static inline float clampf (float v, float lo, float hi) noexcept
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    static inline float clamp01 (float v) noexcept
    {
        return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    }

    static inline float clampHz (float v, float lo, float hi) noexcept
    {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    static inline float onePoleLP (float y1, float x, float a) noexcept
    {
        return (1.0f - a) * x + a * y1;
    }

    static inline float onePoleHP (float& x1, float& y1, float x, float a) noexcept
    {
        const float y = a * (y1 + x - x1);
        x1 = x;
        y1 = y;
        return y;
    }

    static inline float alphaFromHz (float fc, float sr) noexcept
    {
        const float safeSr = (sr > 1.0f ? sr : 1.0f);
        return std::exp (-2.0f * kPi * (fc / safeSr));
    }

    static inline float alphaFromMs (float ms, float sr) noexcept
    {
        const float tau = ms * 0.001f;
        const float safeSr = (sr > 1.0f ? sr : 1.0f);
        const float denom = tau * safeSr;
        if (denom <= 1.0e-6f) return 0.0f;
        return std::exp (-1.0f / denom);
    }

    static inline float dbToLin (float db) noexcept
    {
        return std::exp (0.1151292546497022842f * db); // ln(10)/20
    }

    // ------------------ anti-denorm + tiny RNG helpers ------------------

    static inline uint32_t xorshift32 (uint32_t& state) noexcept
    {
        uint32_t x = state;
        x ^= (x << 13);
        x ^= (x >> 17);
        x ^= (x << 5);
        state = (x != 0 ? x : 0x12345678u);
        return state;
    }

    static inline float tinyNoiseDenormSafe (State& s) noexcept
    {
        const uint32_t r = xorshift32 (s.rng);
        const float r01 = (float) (r & 0xFFFFu) * (1.0f / 65535.0f);
        const float n = (r01 - 0.5f) * 2.0f;     // [-1..+1]
        return n * 1.0e-20f;                     // ultra pequeño
    }

    static inline float tinyNoiseBipolar (State& s) noexcept
    {
        const uint32_t r = xorshift32 (s.rng);
        const float r01 = (float) (r & 0xFFFFu) * (1.0f / 65535.0f);
        return (r01 - 0.5f) * 2.0f; // [-1..+1]
    }

    // ------------------ allpass “3D glue” ------------------

    static inline float allpassCoefFromHz (float fc, float sr) noexcept
    {
        // 1st-order allpass coef from fc:
        // a = (1 - tan(w/2)) / (1 + tan(w/2)), w = 2*pi*fc/sr
        const float safeSr = (sr > 1.0f ? sr : 1.0f);
        const float w = kPi * (fc / safeSr); // w/2 = pi*fc/sr
        const float t = std::tan (w);
        const float a = (1.0f - t) / (1.0f + t);
        return clampf (a, 0.0f, 0.9999f);
    }

    static inline float allpass1 (float x, float& x1, float& y1, float a) noexcept
    {
        // y = -a*x + x1 + a*y1
        const float y = (-a * x) + x1 + (a * y1);
        x1 = x;
        y1 = y;
        return y;
    }

    // ------------------ ADAA helpers ------------------

    static inline float logCoshStable (float z) noexcept
    {
        const float az = std::fabs (z);
        if (az > 10.0f)
            return az - kLn2;
        return std::log (std::cosh (z));
    }

    static inline float adaaTanh1 (float x, float& x1, float k) noexcept
    {
        const float denom = (x - x1);
        float y;

        if (std::fabs (denom) > 1.0e-8f)
        {
            const float kx  = k * x;
            const float kx1 = k * x1;

            const float Fx  = logCoshStable (kx)  / k;
            const float Fx1 = logCoshStable (kx1) / k;

            y = (Fx - Fx1) / denom;
        }
        else
        {
            y = std::tanh (k * x);
        }

        x1 = x;

        const float d = std::tanh (k);
        return (d > 1.0e-12f) ? (y / d) : y;
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
            const double inv = 1.0 / x; // in (-1, 0)
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

    static inline float adaaTanh2 (float x, float& x1, float& x2, float k) noexcept
    {
        const float z = k * x;
        const bool use2 = (std::fabs (z) > 0.25f);

        const float d01 = (x - x1);
        const float d12 = (x1 - x2);
        const float d02 = (x - x2);

        float y;

        if (!use2 || std::fabs (d01) < 1.0e-7f || std::fabs (d12) < 1.0e-7f || std::fabs (d02) < 1.0e-7f)
        {
            y = adaaTanh1 (x, x1, k);
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
        if (std::fabs (norm) > 1.0e-12)
            yd /= norm;

        y = (float) yd;

        x2 = x1;
        x1 = x;

        return y;
    }

    static inline float adaaAtan1 (float x, float& x1, float k) noexcept
    {
        constexpr float twoOverPi = 0.63661977236758134308f; // 2/pi

        const float denom = (x - x1);
        float y;

        if (std::fabs (denom) > 1.0e-8f)
        {
            const float kx  = k * x;
            const float kx1 = k * x1;

            const float Fx  = x  * std::atan (kx)  - (0.5f / k) * std::log1p (kx  * kx);
            const float Fx1 = x1 * std::atan (kx1) - (0.5f / k) * std::log1p (kx1 * kx1);

            y = (Fx - Fx1) / denom;
        }
        else
        {
            y = std::atan (k * x);
        }

        x1 = x;
        return twoOverPi * y;
    }

    static inline float lowLevelBlend (float x, float y, float knee) noexcept
    {
        const float ax = std::fabs (x);
        const float b  = ax / (ax + knee);
        return x + b * (y - x);
    }

    static inline float asymTanhZero (float x, float k, float bias) noexcept
    {
        const float y  = std::tanh (k * (x + bias)) - std::tanh (k * bias);

        const float yp = std::tanh (k * (1.0f + bias)) - std::tanh (k * bias);
        const float yn = std::tanh (k * (-1.0f + bias)) - std::tanh (k * bias);
        const float m  = 0.5f * (std::fabs (yp) + std::fabs (yn));

        return (m > 1.0e-12f) ? (y / m) : y;
    }

    // ------------------ transformer HQ ------------------

    static inline float transformerCoreADAA_HQ (float x, float& atan_x1, float env01, float sag01,
                                                float driveBase, float biasBase) noexcept
    {
        const float drive = (driveBase * (1.0f - 0.06f * sag01)) + 0.22f * env01;
        const float bias  = (biasBase  + 0.018f * env01) + 0.010f * sag01;

        const float core  = adaaAtan1 (x, atan_x1, drive);

        const float x2 = x * x;
        const float fluxIn = x + (0.12f + 0.05f * env01) * x * x2;
        const float flux   = std::tanh ((1.15f + 0.10f * env01) * fluxIn);

        const float k    = (1.70f + 0.95f * env01) * (1.0f - 0.05f * sag01);
        const float asym = asymTanhZero (x, k, bias);

        float y = 0.70f * core + 0.23f * flux + 0.07f * asym;

        y = lowLevelBlend (x, y, 0.18f);

        return clampf (y, -1.25f, 1.25f);
    }

    static inline float transformerHystADAA_HQ (float& m, float& prevX, float& atan_x1,
                                                float x, float env01, float sag01, float sr,
                                                float fcBase, float hystAmt,
                                                float driveBase, float biasBase,
                                                float injectBase) noexcept
    {
        const float tau = 0.7f + 0.9f * (1.0f - env01);
        const float leakAlpha = std::exp (-1.0f / (tau * (float) sr));
        m *= leakAlpha;

        const float dx = x - prevX;
        prevX = x;

        const float eddyAmt = (0.35f + 0.55f * env01);
        const float eddy = 1.0f / (1.0f + eddyAmt * std::fabs (dx));

        const float fc = (fcBase + (40.0f * env01));
        const float a  = alphaFromHz (fc, sr);

        const float d = (1.10f + 0.60f * env01) * (1.0f - 0.06f * sag01);
        const float h = hystAmt + 0.12f * env01;

        const float target = std::tanh (d * (x - h * m));
        m = onePoleLP (m, target, a);

        float inject = (injectBase + 0.10f * env01 + 0.05f * sag01);
        inject *= eddy;

        const float xin = x + inject * m;

        return transformerCoreADAA_HQ (xin, atan_x1, env01, sag01, driveBase, biasBase);
    }

    // ------------------ amp stages ------------------

    static inline float ampStage1ADAA2 (State& s, float x, float env01, float sag01) noexcept
    {
        const float drive = (1.55f + 0.58f * env01) * (1.0f - 0.08f * sag01);
        const float k1    = (2.85f + 0.90f * env01) * (1.0f - 0.06f * sag01);

        // (opcional) bias por sag MUY sutil (ya tienes asimetría pro arriba)
        const float bias = (0.0005f + 0.0009f * env01) * sag01;

        const float pre = (x + bias) * drive;

        float y = adaaTanh2 (pre, s.amp1_x1, s.amp1_x2, k1);

        y = lowLevelBlend (x, y, 0.26f);

        return clampf (y, -1.35f, 1.35f);
    }

    static inline float ampStage2ADAA (State& s, float x, float env01, float sag01) noexcept
    {
        const float drive = (1.10f + 0.34f * env01) * (1.0f - 0.06f * sag01);
        const float k2    = (2.05f + 0.78f * env01) * (1.0f - 0.05f * sag01);

        const float pre = x * drive;

        float y = adaaTanh1 (pre, s.amp2_x1, k2);

        const float t = pre;
        y += (0.004f + 0.014f * env01) * (t * t * t);

        y = 0.88f * y + 0.12f * (0.63661977236758134308f * std::atan (2.3f * y)); // 2/pi

        y = lowLevelBlend (x, y, 0.24f);

        return clampf (y, -1.35f, 1.35f);
    }
};


