cmake_minimum_required(VERSION 3.15)
if(APPLE)
    option(FANFARE_MAC_UNIVERSAL "Build Universal (arm64;x86_64) binary on macOS" ON)
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment version")
    endif()
    if(FANFARE_MAC_UNIVERSAL)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
    else()
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS architectures" FORCE)
    endif()
endif()
project(Fanfare VERSION 1.7.40 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to a JUCE checkout (cloned by CI, C:/JUCE on Windows, or ~/JUCE on macOS).
if(NOT DEFINED JUCE_ROOT)
    if(WIN32)
        set(JUCE_ROOT "C:/JUCE" CACHE PATH "Path to JUCE root directory")
    elseif(EXISTS "$ENV{HOME}/JUCE")
        set(JUCE_ROOT "$ENV{HOME}/JUCE" CACHE PATH "Path to JUCE root directory")
    elseif(EXISTS "/opt/homebrew/opt/juce")
        set(JUCE_ROOT "/opt/homebrew/opt/juce" CACHE PATH "Path to JUCE root directory")
    else()
        set(JUCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to JUCE root directory")
    endif()
endif()

# Pull in JUCE's CMake helpers (juce_add_gui_app, the module targets, etc.).
add_subdirectory(${JUCE_ROOT} JUCE)

# Standalone GUI application target.
juce_add_gui_app(Fanfare
    PRODUCT_NAME                      "Fanfare"
    COMPANY_NAME                      "Fanfare"
    BUNDLE_ID                         "com.fanfare.app"
    ICON_BIG                          "${CMAKE_CURRENT_SOURCE_DIR}/Source/Resources/Fanfare_Icon.png"
    ICON_SMALL                        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Resources/Fanfare_Icon.png"
    MICROPHONE_PERMISSION_ENABLED     TRUE
    MICROPHONE_PERMISSION_TEXT        "Fanfare uses audio inputs for live instruments and monitor routing."
    LOCAL_NETWORK_PERMISSION_ENABLED  TRUE
    LOCAL_NETWORK_PERMISSION_TEXT     "Fanfare hosts a local Stage HUD web server for tablet remote control."
    PLIST_TO_MERGE                    "<plist><dict><key>NSHighResolutionCapable</key><true/></dict></plist>"
)

# The juceaide-generated resources.rc (embedded VERSIONINFO) only re-fires on
# icon changes, so it can go stale across version bumps. Remove it at
# configure time so the build regenerates it from the current PROJECT_VERSION.
if(WIN32)
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/Fanfare_artefacts/JuceLibraryCode/Fanfare_resources.rc")
endif()

# Shared compilation units (must match the Projucer source list, minus Main.cpp).
set(FANFARE_SHARED_SOURCES
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
    Source/Web/PluginMirrorCapture.cpp
    Source/Web/PluginMirrorInput.cpp
    Source/Web/PluginMirrorStreamer.cpp
    Source/Web/StageHotspotController.cpp
    Source/Web/StageRemoteModalOverlay.cpp
    Source/Synth/Fm/FmSupport.cpp
    Source/Synth/Fm/msfa/sin.cc
    Source/Synth/Fm/msfa/exp2.cc
    Source/Synth/Fm/msfa/freqlut.cc
    Source/Synth/Fm/msfa/env.cc
    Source/Synth/Fm/msfa/pitchenv.cc
    Source/Synth/Fm/msfa/lfo.cc
    Source/Synth/Fm/msfa/fm_op_kernel.cc
    Source/Synth/Fm/msfa/fm_core.cc
)

target_sources(Fanfare PRIVATE
    Source/Main.cpp
    ${FANFARE_SHARED_SOURCES}
)

juce_add_console_app(selftest
    PRODUCT_NAME    "selftest"
    COMPANY_NAME    "Fanfare"
)

target_sources(selftest PRIVATE
    tools/selftest.cpp
    ${FANFARE_SHARED_SOURCES}
)

set(FANFARE_INCLUDE_DIRS
    Source
)
if(WIN32)
    list(APPEND FANFARE_INCLUDE_DIRS JuceLibraryCode)
endif()

target_include_directories(Fanfare PRIVATE ${FANFARE_INCLUDE_DIRS})
target_include_directories(selftest PRIVATE ${FANFARE_INCLUDE_DIRS})

# Module configuration. Mirrors the Projucer AppConfig.h. JUCE 8+ bundles the
# Steinberg ASIO headers under juce_audio_devices/native/asio, so enable ASIO
# automatically on Windows when present.
set(FANFARE_COMPILE_DEFS
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    DONT_SET_USING_JUCE_NAMESPACE=1
)

if(WIN32 AND EXISTS "${JUCE_ROOT}/modules/juce_audio_devices/native/asio")
    list(APPEND FANFARE_COMPILE_DEFS JUCE_ASIO=1)
endif()

target_compile_definitions(Fanfare PUBLIC ${FANFARE_COMPILE_DEFS})
target_compile_definitions(selftest PRIVATE ${FANFARE_COMPILE_DEFS})

set(FANFARE_LINK_LIBS
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

if(NOT WIN32)
    find_package(SQLite3 REQUIRED)
    if(TARGET SQLite3::SQLite3)
        list(APPEND FANFARE_LINK_LIBS SQLite3::SQLite3)
    else()
        list(APPEND FANFARE_LINK_LIBS SQLite::SQLite3)
    endif()
endif()

target_link_libraries(Fanfare PRIVATE ${FANFARE_LINK_LIBS})
target_link_libraries(selftest PRIVATE ${FANFARE_LINK_LIBS})

if(WIN32)
    set_property(TARGET Fanfare PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(TARGET selftest PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    if(MSVC)
        target_compile_options(Fanfare PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(Fanfare PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
        target_compile_options(selftest PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(selftest PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    endif()

    add_custom_command(TARGET Fanfare POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Fanfare>" "${CMAKE_CURRENT_SOURCE_DIR}/Fanfare.exe"
    )
    add_custom_command(TARGET selftest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:selftest>" "${CMAKE_CURRENT_BINARY_DIR}/selftest.exe"
    )
endif()

if(APPLE)
    target_compile_definitions(Fanfare PUBLIC
        JUCE_PLUGINHOST_AU=1
    )
    target_compile_definitions(selftest PRIVATE
        JUCE_PLUGINHOST_AU=1
    )
    set_target_properties(Fanfare PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.fanfare.app"
        MACOSX_BUNDLE_BUNDLE_NAME "Fanfare"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
endif()

if(NOT WIN32)
    juce_generate_juce_header(Fanfare)
    juce_generate_juce_header(selftest)
endif()

enable_testing()
add_test(NAME selftest_unit COMMAND $<TARGET_FILE:selftest> --unit)
add_test(NAME selftest_offline_render COMMAND $<TARGET_FILE:selftest> --offline-render)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME audio_path_scan COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/scan_audio_path.py")
endif()
