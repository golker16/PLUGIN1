// PresetRegistry.h
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// IMPORTANT: usa los nombres reales de tus headers:
#include "preset-Neve1073.h"
#include "preset-API312512.h"
#include "funciones.h" // plugin::Knobs

// -----------------------------------------------------------------------------
// 1) Ajusta aquí el nombre del struct del preset Neve si fuera distinto.
//    En tu caso más probable: preset-Neve1073.h define: struct Preset_Neve1073 { ... }
//    Si tu struct se llama Preset_Neve, cambia la línea de abajo.
// -----------------------------------------------------------------------------
using PresetNeveT = Preset_Neve1073; // <-- CAMBIA ESTO si tu struct real se llama distinto
using PresetApiT  = Preset_API312512;

// -----------------------------------------------------------------------------
// Helpers para detectar si un preset tiene kKnobBehavior
// -----------------------------------------------------------------------------
namespace preset_registry_detail
{
    template <typename T, typename = void>
    struct has_knob_behavior : std::false_type {};

    template <typename T>
    struct has_knob_behavior<T, std::void_t<decltype(T::kKnobBehavior)>> : std::true_type {};

    template <typename PresetT>
    constexpr const char* knobBehavior() noexcept
    {
        if constexpr (has_knob_behavior<PresetT>::value)
            return PresetT::kKnobBehavior;
        else
            return "";
    }

    // Func pointers (wrappers) para llamar a cada preset de forma uniforme
    using PrepareFn = void (*)(void*, float) noexcept;
    using ResetFn   = void (*)(void*) noexcept;
    using ProcessFn = float (*)(void*, float, const plugin::Knobs&) noexcept;

    struct PresetDesc
    {
        const char* displayName  = "";
        const char* knobBehavior = "";

        PrepareFn prepare = nullptr;
        ResetFn   reset   = nullptr;
        ProcessFn process = nullptr;

        std::size_t stateSize  = 0;
        std::size_t stateAlign = 0;
    };

    template <typename PresetT>
    static void prepareThunk(void* mem, float sr) noexcept
    {
        auto& st = *reinterpret_cast<typename PresetT::State*>(mem);
        PresetT::prepare(st, sr);
    }

    template <typename PresetT>
    static void resetThunk(void* mem) noexcept
    {
        auto& st = *reinterpret_cast<typename PresetT::State*>(mem);
        PresetT::reset(st);
    }

    template <typename PresetT>
    static float processThunk(void* mem, float x, const plugin::Knobs& k) noexcept
    {
        auto& st = *reinterpret_cast<typename PresetT::State*>(mem);
        return PresetT::process(st, x, k);
    }

    constexpr std::size_t maxSz(std::size_t a, std::size_t b) noexcept { return (a > b) ? a : b; }

    template <typename PresetT>
    constexpr PresetDesc make() noexcept
    {
        return PresetDesc{
            PresetT::kDisplayName,
            knobBehavior<PresetT>(),
            &prepareThunk<PresetT>,
            &resetThunk<PresetT>,
            &processThunk<PresetT>,
            sizeof(typename PresetT::State),
            alignof(typename PresetT::State)
        };
    }
} // namespace preset_registry_detail

// -----------------------------------------------------------------------------
// PresetRegistry
// -----------------------------------------------------------------------------
struct PresetRegistry
{
    using PresetDesc = preset_registry_detail::PresetDesc;

    // Lista de presets (agrega más aquí si tienes)
    inline static constexpr std::array<PresetDesc, 2> items = {
        preset_registry_detail::make<PresetNeveT>(),
        preset_registry_detail::make<PresetApiT>()
    };

    static constexpr std::size_t kCount = items.size();

    // Estos 2 deben ser compile-time (para aligned_storage_t)
    static constexpr std::size_t kMaxStateSize =
        preset_registry_detail::maxSz(sizeof(PresetNeveT::State), sizeof(PresetApiT::State));

    static constexpr std::size_t kMaxStateAlign =
        preset_registry_detail::maxSz(alignof(PresetNeveT::State), alignof(PresetApiT::State));

    // Búsqueda simple por índice (por si tu processor lo usa)
    static constexpr const PresetDesc& at(std::size_t i) noexcept
    {
        return items[i < kCount ? i : 0];
    }
};

