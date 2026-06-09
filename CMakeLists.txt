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
    ICON_BIG                      "${CMAKE_CURRENT_SOURCE_DIR}/Source/Theme/AppIcon.png"
    # Register .zfproj as our session document so Finder shows the ZynForge
    # icon (JUCE points the document type's CFBundleTypeIconFile at the app
    # Icon) and double-clicking one opens the app, instead of a blank page.
    DOCUMENT_EXTENSIONS           "zfproj"
    VERSION                       0.1.0
    HARDENED_RUNTIME_ENABLED      TRUE
    HARDENED_RUNTIME_OPTIONS      "com.apple.security.device.audio-input"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "Zynforge Recording needs audio input access to record."
)

juce_generate_juce_header(ZynforgeRecording)

# Bundled fonts — Inter for UI text, JetBrains Mono for tabular numerals.
# Both ship with the binary so the brand identity is locked across macOS
# versions and per-user font settings. License: SIL Open Font License.
juce_add_binary_data(ZynforgeFonts SOURCES
    Source/Theme/Fonts/Inter-Regular.ttf
    Source/Theme/Fonts/Inter-Bold.ttf
    Source/Theme/Fonts/JetBrainsMono-Regular.ttf
    Source/Theme/Fonts/JetBrainsMono-Bold.ttf
)

target_sources(ZynforgeRecording PRIVATE
    Source/Main.cpp
    Source/UI/MainComponent.cpp
    Source/UI/MainComponentInit.cpp
    Source/UI/MainComponentTimer.cpp
    Source/UI/MainComponentKeys.cpp
    Source/UI/MainComponentLayout.cpp
    Source/UI/MainComponentCues.cpp
    Source/UI/MainComponentEdit.cpp
    Source/UI/MainComponentSessionIO.cpp
    Source/UI/MainComponentMenu.cpp
    Source/UI/MainComponentHelp.cpp
    Source/UI/MainComponentTools.cpp
    Source/UI/MainComponentStrips.cpp
    Source/UI/TrackSelectDialog.cpp
    Source/Tests/AutomationTests.cpp
    Source/Tests/MarkerTests.cpp
    Source/Tests/EngineStateTests.cpp
    Source/Tests/RecorderPlayerTests.cpp
    Source/Tests/RecordingIntegrityTests.cpp
    Source/Tests/TransientTests.cpp
    Source/Tests/TimecodeTests.cpp
    Source/Tests/TimelineExportTests.cpp
    Source/Tests/OscRemoteTests.cpp
    Source/Tests/ChannelCsvTests.cpp
    Source/Tests/McuProtocolTests.cpp
    Source/Tests/MenuDispatchTests.cpp
    Source/Tests/AudioCallbackTests.cpp
    Source/Tests/PlaceholderViewTests.cpp
    Source/Tests/ChannelStripAccessibilityTests.cpp
    Source/Tests/TransportBarAccessibilityTests.cpp
    Source/Tests/SetlistBarAccessibilityTests.cpp
    Source/Tests/EditToolsBarAccessibilityTests.cpp
    Source/Tests/MasterStripAccessibilityTests.cpp
    Source/Tests/VcaPanelAccessibilityTests.cpp
    Source/Tests/AutomationToolbarAccessibilityTests.cpp
    Source/Tests/EditPageAccessibilityTests.cpp
    Source/Tests/CompanionServerTests.cpp
    Source/Tests/CompoundFileTests.cpp
    Source/Tests/AafTests.cpp
    Source/Tests/FastHashTests.cpp
    Source/Tests/SessionBackupTests.cpp
    Source/Tests/PreflightTests.cpp
    Source/Tests/QcAnalyzerTests.cpp
    Source/Tests/SongDetectorTests.cpp
    Source/Tests/CrashScanTests.cpp
    Source/Tests/ConsoleLinkTests.cpp
    Source/Tests/StereoExportTests.cpp
    Source/Network/ConsoleLink.cpp
    Source/UI/ChannelStrip.cpp
    Source/UI/LedMeter.cpp
    Source/UI/MiniSpectrum.cpp
    Source/UI/TimelineStrip.cpp
    Source/UI/StripColourPicker.cpp
    Source/UI/ExportDialog.cpp
    Source/UI/PatchPage.cpp
    Source/UI/EditPage.cpp
    Source/UI/AudioDeviceDialog.cpp
    Source/UI/MasterStrip.cpp
    Source/UI/AddTracksDialog.cpp
    Source/UI/AutomationToolbar.cpp
    Source/UI/NewSessionDialog.cpp
    Source/UI/WelcomeDialog.cpp
    Source/UI/PerfDashboard.cpp
    Source/UI/SessionPropertiesDialog.cpp
    Source/UI/SetlistBar.cpp
    Source/UI/TempoBar.cpp
    Source/UI/ClickSettingsDialog.cpp
    Source/Network/CompanionServer.cpp
    Source/Network/SessionMirror.cpp
    Source/UI/Meterbridge.cpp
    Source/UI/TransportBar.cpp
    Source/UI/BigClockPanel.cpp
    Source/Audio/AudioEngine.cpp
    Source/Audio/AudioEngineAutomation.cpp
    Source/Audio/AudioEngineClips.cpp
    Source/Audio/ClickEngine.cpp
    Source/Audio/MultitrackRecorder.cpp
    Source/Audio/SessionPlayer.cpp
    Source/Audio/Markers.cpp
    Source/Audio/StripColours.cpp
    Source/Audio/StripGains.cpp
    Source/Audio/StripNames.cpp
    Source/Audio/StripRouting.cpp
    Source/Audio/TrackExporter.cpp
    Source/Audio/OscRemote.cpp
    Source/Audio/MidiControlSurface.cpp
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
    ZynforgeFonts
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_dsp
    juce::juce_osc
    juce::juce_cryptography
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
