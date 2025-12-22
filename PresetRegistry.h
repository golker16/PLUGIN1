#pragma once

// PresetRegistry.h (SOURCE VERSION)
// ------------------------------------------------------------
// Este archivo reemplaza la versión autogenerada por CMake.
// Motivo: en algunos builds/IDEs el header generado no estaba
// entrando en el include path, dejando PresetRegistry::items vacío
// y el ComboBox sin presets.
//
// Si agregas nuevos presets, inclúyelos aquí y añádelos al array
// 'items'.

#include <array>
#include <cstddef>
#include <new>
#include <type_traits>

// Presets disponibles
#include "preset-Neve1073.h"

struct PresetRegistry
{
    struct Item
    {
        const char* displayName = nullptr;
        std::size_t stateSize  = 1;
        std::size_t stateAlign = alignof (std::max_align_t);
        // Lifecycle del State (para soportar State con inicializadores / miembros no POD)
        void  (*construct)(void* state) = nullptr;
        void  (*destruct) (void* state) = nullptr;

        void  (*prepare)(void* state, float sr) = nullptr;
        void  (*reset)  (void* state) = nullptr;
        float (*process)(void* state, float x) = nullptr;
    };

    template <typename Preset>
    static Item make() noexcept
    {
        // Nota: muchos States usan in-class initializers (no son *trivially* constructible).
        // Por eso construimos/destruimos explícitamente con placement-new.
        static_assert (std::is_nothrow_default_constructible_v<typename Preset::State>,
                       "Preset::State must be nothrow default constructible");
        static_assert (std::is_nothrow_destructible_v<typename Preset::State>,
                       "Preset::State must be nothrow destructible");

        return Item {
            Preset::kDisplayName,
            sizeof (typename Preset::State),
            alignof (typename Preset::State),
            +[](void* st)
            {
                new (st) typename Preset::State();
            },
            +[](void* st)
            {
                reinterpret_cast<typename Preset::State*> (st)->~State();
            },
            +[](void* st, float sr)
            {
                auto& s = *reinterpret_cast<typename Preset::State*> (st);
                Preset::prepare (s, sr);
            },
            +[](void* st)
            {
                auto& s = *reinterpret_cast<typename Preset::State*> (st);
                Preset::reset (s);
            },
            +[](void* st, float x) -> float
            {
                auto& s = *reinterpret_cast<typename Preset::State*> (st);
                return Preset::process (s, x);
            }
        };
    }

    // Máximos para reservar storage (stereo): si agregas presets con State
    // más grande/alineación mayor, actualiza estos constexpr.
    static constexpr std::size_t kMaxStateSize  = sizeof (Preset_Neve1073::State);
    static constexpr std::size_t kMaxStateAlign = alignof (Preset_Neve1073::State);

    // Lista de presets disponibles
    static inline const std::array<Item, 1> items {{
        make<Preset_Neve1073>(),
    }};
};
