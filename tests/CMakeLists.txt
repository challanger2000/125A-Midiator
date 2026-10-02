add_executable(MidiatorCoreTests
    RiffEngineTests.cpp
    ../src/GuitarBrain.cpp
)

target_include_directories(MidiatorCoreTests PRIVATE ../src)

add_test(NAME MidiatorCoreTests COMMAND MidiatorCoreTests)

add_executable(MidiatorMeasure
    RiffEngineMeasurement.cpp
    ../src/GuitarBrain.cpp
    ../src/BassBrain.cpp
    ../src/DrumBrain.cpp
    ../src/PadBrain.cpp
    ../src/SynthBrain.cpp
)

target_include_directories(MidiatorMeasure PRIVATE ../src)


add_executable(MidiatorUIContractTests
    UIContractTests.cpp
)

target_compile_definitions(MidiatorUIContractTests PRIVATE
    MIDIATOR_UIDESC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../src/resource/midiator.uidesc"
)
add_test(NAME MidiatorUIContractTests COMMAND MidiatorUIContractTests)

add_executable(MidiatorStateRoundtripTests
    StateRoundtripTests.cpp
    ../src/PluginProcessor.cpp
    ../src/GuitarBrain.cpp
    ../src/BassBrain.cpp
    ../src/DrumBrain.cpp
    ../src/PadBrain.cpp
    ../src/SynthBrain.cpp
)

target_include_directories(MidiatorStateRoundtripTests PRIVATE ../src)
target_compile_definitions(MidiatorStateRoundtripTests PRIVATE MIDIATOR_NO_FACTORY=1)
target_link_libraries(MidiatorStateRoundtripTests PRIVATE sdk vstgui_support)
add_test(NAME MidiatorStateRoundtripTests COMMAND MidiatorStateRoundtripTests)


add_executable(MidiatorProcessorLiveTests
    ProcessorLiveTests.cpp
    ../src/PluginProcessor.cpp
    ../src/GuitarBrain.cpp
    ../src/BassBrain.cpp
    ../src/DrumBrain.cpp
    ../src/PadBrain.cpp
    ../src/SynthBrain.cpp
)

target_include_directories(MidiatorProcessorLiveTests PRIVATE ../src)
target_compile_definitions(MidiatorProcessorLiveTests PRIVATE MIDIATOR_NO_FACTORY=1)
target_link_libraries(MidiatorProcessorLiveTests PRIVATE sdk vstgui_support)
add_test(NAME MidiatorProcessorLiveTests COMMAND MidiatorProcessorLiveTests)

add_executable(MidiatorBassBrainTests
    BassBrainTests.cpp
    ../src/GuitarBrain.cpp
    ../src/BassBrain.cpp
)

target_include_directories(MidiatorBassBrainTests PRIVATE ../src)
add_test(NAME MidiatorBassBrainTests COMMAND MidiatorBassBrainTests)

add_executable(MidiatorDrumBrainTests
    DrumBrainTests.cpp
    ../src/GuitarBrain.cpp
    ../src/BassBrain.cpp
    ../src/DrumBrain.cpp
)

target_include_directories(MidiatorDrumBrainTests PRIVATE ../src)
add_test(NAME MidiatorDrumBrainTests COMMAND MidiatorDrumBrainTests)


add_executable(MidiatorPadBrainTests
    PadBrainTests.cpp
    ../src/GuitarBrain.cpp
    ../src/BassBrain.cpp
    ../src/PadBrain.cpp
)

target_include_directories(MidiatorPadBrainTests PRIVATE ../src)
add_test(NAME MidiatorPadBrainTests COMMAND MidiatorPadBrainTests)


add_executable(MidiatorSynthBrainTests
    SynthBrainTests.cpp
    ../src/GuitarBrain.cpp
    ../src/BassBrain.cpp
    ../src/PadBrain.cpp
    ../src/SynthBrain.cpp
)

target_include_directories(MidiatorSynthBrainTests PRIVATE ../src)
add_test(NAME MidiatorSynthBrainTests COMMAND MidiatorSynthBrainTests)


add_executable(MidiatorGoldenArrangementTests
    GoldenArrangementTests.cpp
    ../src/GuitarBrain.cpp
    ../src/BassBrain.cpp
    ../src/DrumBrain.cpp
    ../src/PadBrain.cpp
    ../src/SynthBrain.cpp
)
target_include_directories(MidiatorGoldenArrangementTests PRIVATE ../src)
add_test(NAME MidiatorGoldenArrangementTests COMMAND MidiatorGoldenArrangementTests)


add_executable(MidiatorArrangementPropertyTests
    ArrangementPropertyTests.cpp
    ../src/GuitarBrain.cpp
    ../src/BassBrain.cpp
    ../src/DrumBrain.cpp
    ../src/PadBrain.cpp
    ../src/SynthBrain.cpp
)
target_include_directories(MidiatorArrangementPropertyTests PRIVATE ../src)
add_test(NAME MidiatorArrangementPropertyTests COMMAND MidiatorArrangementPropertyTests)
