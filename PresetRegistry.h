#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "funciones.h" // para plugin::Knobs

// === Incluye aquí tus presets ===
#include "preset-Neve1073.h"
#include "preset-API312512.h"
// #include "preset-Otro.h" ...

// -----------------------------------------------------------------------------
// Preset registry "vtable"
// - Arregla C2078 (too many initializers) al incluir knobBehavior en el descriptor
// - Permite presets con State propio (size/align) y process con knobs
// -----------------------------------------------------------------------------
struct PresetDesc
{
    const char* displayName = "";
    const char* knobBehavior = "";     // <-- ESTE es el campo que te faltaba

    // Estado por instancia (cada canal / cada plugin instance)
    std::size_t stateSize  = 0;
    std::size_t stateAlign = alignof(std::max_align_t);

    // Funciones tipo vtable (operan sobre void* state)
    void  (*prepare)(void* state, float sampleRate) = nullptr;
    void  (*reset)(void* state) = nullptr;
    float (*process)(void* state, float x, const plugin::Knobs& k) = nullptr;
};

// Helpers para construir el PresetDesc desde un tipo PresetT
namespace preset_registry_detail
{
    template <typename PresetT>
    static void prepareThunk(void* st, float sr)
    {
        auto* s = reinterpret_cast<typename PresetT::State*>(st);
        PresetT::prepare(*s, sr);
    }

    template <typename PresetT>
    static void resetThunk(void* st)
    {
        auto* s = reinterpret_cast<typename PresetT::State*>(st);
        PresetT::reset(*s);
    }

    template <typename PresetT>
    static float processThunk(void* st, float x, const plugin::Knobs& k)
    {
        auto* s = reinterpret_cast<typename PresetT::State*>(st);
        return PresetT::process(*s, x, k);
    }

    // Fallback si un preset no define kKnobBehavior
    template <typename PresetT, typename = void>
    struct KnobBehaviorGetter
    {
        static constexpr const char* get() { return ""; }
    };

    template <typename PresetT>
    struct KnobBehaviorGetter<PresetT, std::void_t<decltype(PresetT::kKnobBehavior)>>
    {
        static constexpr const char* get() { return PresetT::kKnobBehavior; }
    };

    template <typename PresetT>
    constexpr PresetDesc make() noexcept
    {
        return PresetDesc{
            PresetT::kDisplayName,
            KnobBehaviorGetter<PresetT>::get(),
            sizeof(typename PresetT::State),
            alignof(typename PresetT::State),
            &prepareThunk<PresetT>,
            &resetThunk<PresetT>,
            &processThunk<PresetT>
        };
    }

    // Max helpers (para reservar buffer de estado con tamaño/alineación suficiente)
    constexpr std::size_t maxSz(std::size_t a, std::size_t b) { return a > b ? a : b; }
    constexpr std::size_t maxAl(std::size_t a, std::size_t b) { return a > b ? a : b; }
} // namespace preset_registry_detail

// -----------------------------------------------------------------------------
// Lista de presets disponibles
// Si agregas presets, solo añade preset_registry_detail::make<TuPreset>()
// -----------------------------------------------------------------------------
inline constexpr PresetDesc kPresets[] = {
    preset_registry_detail::make<Preset_Neve>(),
    preset_registry_detail::make<Preset_API312512>(),
    // preset_registry_detail::make<Preset_Otro>(),
};

inline constexpr std::size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

// -----------------------------------------------------------------------------
// Cálculo de máximo tamaño/alineación de State para reservar un buffer único
// -----------------------------------------------------------------------------
inline constexpr std::size_t kMaxPresetStateSize =
    []() constexpr {
        std::size_t m = 0;
        for (std::size_t i = 0; i < kPresetCount; ++i) m = preset_registry_detail::maxSz(m, kPresets[i].stateSize);
        return m;
    }();

inline constexpr std::size_t kMaxPresetStateAlign =
    []() constexpr {
        std::size_t m = alignof(std::max_align_t);
        for (std::size_t i = 0; i < kPresetCount; ++i) m = preset_registry_detail::maxAl(m, kPresets[i].stateAlign);
        return m;
    }();

// Helper para acceder por índice (sin tirar)
inline constexpr const PresetDesc& getPreset(std::size_t idx) noexcept
{
    return kPresets[(idx < kPresetCount) ? idx : 0];
}

