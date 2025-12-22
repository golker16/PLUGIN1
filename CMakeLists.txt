cmake_minimum_required(VERSION 3.15)

project(PluginSaturador1 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# MSVC runtime
# -----------------------------------------------------------------------------
# Nota: en plugins (VST3) normalmente se recomienda /MD (runtime dinámico)
# para evitar problemas de CRT al cargar en hosts. Si mantienes /MT, y ves
# inestabilidad/crashes raros, este es un sospechoso #1.
if (MSVC)
  # Release: /MT  | Debug: /MTd
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

# -----------------------------
# Fetch JUCE
# -----------------------------
FetchContent_Declare(
  juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.10
)
FetchContent_MakeAvailable(juce)

# -----------------------------
# Plugin Target (VST3)
# -----------------------------
juce_add_plugin(PluginSaturador1
  COMPANY_NAME "golk"
  PRODUCT_NAME "Plugin Saturador 1"

  # IDs únicos para evitar colisión/caché en hosts (VST3 ClassID)
  PLUGIN_MANUFACTURER_CODE G1K1
  PLUGIN_CODE S7A1

  FORMATS VST3

  VST3_CATEGORIES "Fx" "Distortion"

  IS_SYNTH FALSE
  IS_MIDI_EFFECT FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE

  COPY_PLUGIN_AFTER_BUILD FALSE

  # Deja que JUCE genere manifest/moduleinfo.json
  VST3_AUTO_MANIFEST TRUE
)

juce_generate_juce_header(PluginSaturador1)

# -----------------------------------------------------------------------------
# Assets (PNG + Fonts) -> JUCE BinaryData (DIAGNÓSTICO + GLOB ROBUSTO)
# -----------------------------------------------------------------------------

# --- Assets: carpeta base ---
set(PLUGIN_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/assets")

message(STATUS "PLUGIN_ASSETS_DIR        = ${PLUGIN_ASSETS_DIR}")

# 1) Verifica que el directorio exista
if(NOT EXISTS "${PLUGIN_ASSETS_DIR}")
  message(FATAL_ERROR "No existe PLUGIN_ASSETS_DIR: '${PLUGIN_ASSETS_DIR}'")
endif()

# 2) Lista contenido (CI debug)
message(STATUS "Listando contenido de assets (CI debug):")

file(GLOB _ASSET_DEBUG_LIST
  RELATIVE "${PLUGIN_ASSETS_DIR}"
  "${PLUGIN_ASSETS_DIR}/*"
)

if(_ASSET_DEBUG_LIST)
  foreach(_f IN LISTS _ASSET_DEBUG_LIST)
    message(STATUS "  - ${_f}")
  endforeach()
else()
  message(STATUS "  (vacío)")
endif()

unset(_ASSET_DEBUG_LIST)

# 3) Globs robustos (incluyen mayúsculas, y reconfiguran si cambian archivos)
file(GLOB_RECURSE PLUGIN_ASSET_PNGS
  CONFIGURE_DEPENDS
  "${PLUGIN_ASSETS_DIR}/*.png"
  "${PLUGIN_ASSETS_DIR}/*.PNG"
)

file(GLOB_RECURSE PLUGIN_ASSET_FONTS
  CONFIGURE_DEPENDS
  "${PLUGIN_ASSETS_DIR}/*.ttf"
  "${PLUGIN_ASSETS_DIR}/*.TTF"
  "${PLUGIN_ASSETS_DIR}/*.otf"
  "${PLUGIN_ASSETS_DIR}/*.OTF"
)

list(LENGTH PLUGIN_ASSET_PNGS  PLUGIN_ASSET_PNGS_COUNT)
list(LENGTH PLUGIN_ASSET_FONTS PLUGIN_ASSET_FONTS_COUNT)

message(STATUS "PLUGIN_ASSET_PNGS        = ${PLUGIN_ASSET_PNGS}")
message(STATUS "PLUGIN_ASSET_PNGS_COUNT  = ${PLUGIN_ASSET_PNGS_COUNT}")
message(STATUS "PLUGIN_ASSET_FONTS       = ${PLUGIN_ASSET_FONTS}")
message(STATUS "PLUGIN_ASSET_FONTS_COUNT = ${PLUGIN_ASSET_FONTS_COUNT}")

# 4) Unifica lista de assets embebibles
set(PLUGIN_ASSET_FILES ${PLUGIN_ASSET_PNGS} ${PLUGIN_ASSET_FONTS})
list(LENGTH PLUGIN_ASSET_FILES PLUGIN_ASSET_FILES_COUNT)

if(PLUGIN_ASSET_FILES_COUNT EQUAL 0)
  message(WARNING
    "No se encontraron assets (PNGs/Fuentes) en '${PLUGIN_ASSETS_DIR}'. "
    "Si en tu repo existen, entonces NO llegaron al checkout del runner (revisar Git LFS/submodules)."
  )
endif()

# 5) Si hay assets -> embebe en BinaryData + define macros
if (PLUGIN_ASSET_FILES_COUNT GREATER 0)
  juce_add_binary_data(PluginSaturador1_BinaryData
    SOURCES ${PLUGIN_ASSET_FILES}
  )

  target_link_libraries(PluginSaturador1 PRIVATE
    PluginSaturador1_BinaryData
  )

  target_compile_definitions(PluginSaturador1 PRIVATE PLUGIN_HAS_ASSETS=1)
else()
  target_compile_definitions(PluginSaturador1 PRIVATE PLUGIN_HAS_ASSETS=0)
endif()

# 6) Fuente primaria: si hay al menos una, define su nombre de archivo para que el código la cargue.
#    Prioridad: si existe /assets/mi_fuente.ttf, úsala como default.
#    Nota: el nombre de recurso interno de BinaryData es sanitizado; en código probamos varias variantes.
if (PLUGIN_ASSET_FONTS_COUNT GREATER 0)
  list(SORT PLUGIN_ASSET_FONTS)

  # Busca mi_fuente.ttf (case-insensitive). Si no está, cae al primer font ordenado.
  set(PLUGIN_PRIMARY_FONT_PATH "")
  foreach(_font_path IN LISTS PLUGIN_ASSET_FONTS)
    get_filename_component(_font_name "${_font_path}" NAME)
    string(TOLOWER "${_font_name}" _font_name_lc)
    if (_font_name_lc STREQUAL "mi_fuente.ttf")
      set(PLUGIN_PRIMARY_FONT_PATH "${_font_path}")
      break()
    endif()
  endforeach()

  if (PLUGIN_PRIMARY_FONT_PATH STREQUAL "")
    list(GET PLUGIN_ASSET_FONTS 0 PLUGIN_PRIMARY_FONT_PATH)
  endif()

  get_filename_component(PLUGIN_PRIMARY_FONT_FILENAME "${PLUGIN_PRIMARY_FONT_PATH}" NAME)

  message(STATUS "PLUGIN_PRIMARY_FONT_PATH     = ${PLUGIN_PRIMARY_FONT_PATH}")
  message(STATUS "PLUGIN_PRIMARY_FONT_FILENAME = ${PLUGIN_PRIMARY_FONT_FILENAME}")

  target_compile_definitions(PluginSaturador1 PRIVATE
    PLUGIN_HAS_FONT=1
    PLUGIN_PRIMARY_FONT_FILENAME="${PLUGIN_PRIMARY_FONT_FILENAME}"
  )
else()
  target_compile_definitions(PluginSaturador1 PRIVATE PLUGIN_HAS_FONT=0)
endif()

# Presets (SOURCE VERSION)
#
# En vez de autogenerar PresetRegistry.h en el build dir, mantenemos un
# PresetRegistry.h en el source tree. Esto evita el caso en que el header
# generado no entra al include path (y el menú de presets queda vacío).
#
# Si agregas más presets, basta con ponerlos en archivos preset-*.h
# y añadirlos en PresetRegistry.h.
#---------------------------------------------------------------------------

# Para IDEs: añade los headers de presets al target
file(GLOB CONFIGURE_DEPENDS PRESET_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/preset-*.h")
if(PRESET_HEADERS)
  target_sources(PluginSaturador1 PRIVATE ${PRESET_HEADERS})
endif()

# Preset registry (source)
target_sources(PluginSaturador1 PRIVATE PresetRegistry.h)

# Includes: build dir (JuceHeader.h generado) + source dir
target_include_directories(PluginSaturador1 PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_sources(PluginSaturador1 PRIVATE
  PluginProcessor.cpp
  PluginProcessor.h
  funciones.h
)

target_compile_definitions(PluginSaturador1 PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
)

# OJO: NO meter PluginSaturador1_BinaryData aquí.
# Ya se linkea arriba dentro del if() cuando existen PNGs.
target_link_libraries(PluginSaturador1 PRIVATE
  juce::juce_audio_processors
  juce::juce_dsp
  juce::juce_gui_basics
  juce::juce_gui_extra

  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags
  juce::juce_recommended_lto_flags
)


