cmake_minimum_required(VERSION 3.22)

project(ZynforgeRecording VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary" FORCE)
endif()

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(ZynforgeRecording
    PRODUCT_NAME                  "Zynforge Recording"
    COMPANY_NAME                  "ZynForge"
    BUNDLE_ID                     "com.zynforge.recording"
    VERSION                       0.1.0
    HARDENED_RUNTIME_ENABLED      TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "Zynforge Recording needs audio input access to record."
)

juce_generate_juce_header(ZynforgeRecording)

target_sources(ZynforgeRecording PRIVATE
    Source/Main.cpp
    Source/UI/MainComponent.cpp
    Source/UI/ChannelStrip.cpp
    Source/UI/LedMeter.cpp
    Source/UI/MiniSpectrum.cpp
    Source/UI/PhaseMeter.cpp
    Source/UI/TimelineStrip.cpp
    Source/UI/StripColourPicker.cpp
    Source/UI/ExportDialog.cpp
    Source/UI/BigClockPanel.cpp
    Source/Audio/AudioEngine.cpp
    Source/Audio/MultitrackRecorder.cpp
    Source/Audio/SessionPlayer.cpp
    Source/Audio/Markers.cpp
    Source/Audio/StripColours.cpp
    Source/Audio/StripGains.cpp
    Source/Audio/StripNames.cpp
    Source/Audio/TrackExporter.cpp
    Source/Theme/ZynForgeLookAndFeel.cpp
)

target_include_directories(ZynforgeRecording PRIVATE Source)

target_compile_definitions(ZynforgeRecording PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ZynforgeRecording,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:ZynforgeRecording,JUCE_VERSION>"
)

target_link_libraries(ZynforgeRecording PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
