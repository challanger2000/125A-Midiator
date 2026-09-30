smtg_add_vst3plugin(Midiator
    PluginProcessor.cpp
    PluginProcessor.h
    RiffEngine.cpp
    GuitarBrain.cpp
    BassBrain.cpp
    BassBrain.h
    DrumBrain.cpp
    DrumBrain.h
    PadBrain.cpp
    PadBrain.h
    SynthBrain.cpp
    SynthBrain.h
    RiffEngine.h
)

target_link_libraries(Midiator PRIVATE sdk vstgui_support)

smtg_target_add_plugin_resources(Midiator
    RESOURCES
        resource/midiator.uidesc
        resource/125A_Logo.png
)

if(WIN32)
    target_sources(Midiator PRIVATE
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp
    )
endif()

smtg_target_configure_version_file(Midiator)

target_compile_definitions(Midiator
    PRIVATE
        $<$<CONFIG:Debug>:VSTGUI_LIVE_EDITING=1>
)
