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
        void  (*prepare)(void* state, float sr) = nullptr;
        void  (*reset)  (void* state) = nullptr;
        float (*process)(void* state, float x) = nullptr;
    };

    template <typename Preset>
    static Item make() noexcept
    {
        static_assert (std::is_trivially_default_constructible_v<typename Preset::State>,
                       "Preset::State must be trivially default constructible");
        static_assert (std::is_trivially_destructible_v<typename Preset::State>,
                       "Preset::State must be trivially destructible");

        return Item {
            Preset::kDisplayName,
            sizeof (typename Preset::State),
            alignof (typename Preset::State),
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
