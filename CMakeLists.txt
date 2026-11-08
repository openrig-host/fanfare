cmake_minimum_required(VERSION 3.15)
project(Fanfare VERSION 1.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to a JUCE checkout (cloned by CI, or C:/JUCE locally).
if(NOT DEFINED JUCE_ROOT)
    set(JUCE_ROOT "C:/JUCE" CACHE PATH "Path to JUCE root directory")
endif()

# Pull in JUCE's CMake helpers (juce_add_gui_app, the module targets, etc.).
add_subdirectory(${JUCE_ROOT} JUCE)

# Standalone GUI application target.
juce_add_gui_app(Fanfare
    PRODUCT_NAME    "Fanfare"
    COMPANY_NAME    "Fanfare"
    BUNDLE_ID       "com.fanfare.app"
    ICON_BIG        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Resources/Fanfare_Icon.png"
    ICON_SMALL      "${CMAKE_CURRENT_SOURCE_DIR}/Source/Resources/Fanfare_Icon.png"
)

# All compilation units (must match the Projucer source list).
target_sources(Fanfare PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/MainComponent_Setup.cpp
    Source/MainComponent_Menus.cpp
    Source/MainComponent_Windows.cpp
    Source/MainComponent_RigOps.cpp
    Source/RackSlotComponent.cpp
    Source/BoutiqueLookAndFeel.cpp
    Source/ConfigModalOverlay.cpp
    Source/LibraryPanel.cpp
    Source/QueueButtonComponent.cpp
    Source/SceneButtonComponent.cpp
    Source/RigSerializer.cpp
    Source/RigBuilder.cpp
    Source/RigTransitioner.cpp
    Source/AssetLoader.cpp
    Source/FanfareEngine.cpp
    Source/Web/OpenRigWebServer.cpp
    Source/Web/StageHotspotController.cpp
    Source/Web/StageRemoteModalOverlay.cpp
)

target_include_directories(Fanfare PRIVATE
    Source
    JuceLibraryCode
)

# Module configuration. Mirrors the Projucer AppConfig.h. JUCE 8+ bundles the
# Steinberg ASIO headers under juce_audio_devices/native/asio, so enable ASIO
# automatically on Windows when present.
target_compile_definitions(Fanfare PUBLIC
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
)

if(WIN32 AND EXISTS "${JUCE_ROOT}/modules/juce_audio_devices/native/asio")
    target_compile_definitions(Fanfare PUBLIC JUCE_ASIO=1)
endif()

target_link_libraries(Fanfare PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_processors_headless
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

if(WIN32)
    set_property(TARGET Fanfare PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    if(MSVC)
        target_compile_options(Fanfare PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(Fanfare PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    endif()

    add_custom_command(TARGET Fanfare POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Fanfare>" "${CMAKE_CURRENT_SOURCE_DIR}/Fanfare.exe"
    )
endif()

