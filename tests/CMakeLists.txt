add_executable(MidiatorCoreTests
    RiffEngineTests.cpp
    ../src/RiffEngine.cpp
)

target_include_directories(MidiatorCoreTests PRIVATE ../src)

add_test(NAME MidiatorCoreTests COMMAND MidiatorCoreTests)

add_executable(MidiatorMeasure
    RiffEngineMeasurement.cpp
    ../src/RiffEngine.cpp
)

target_include_directories(MidiatorMeasure PRIVATE ../src)

add_executable(MidiatorTriggerTests
    TriggerEdgeTests.cpp
)

target_include_directories(MidiatorTriggerTests PRIVATE ../src)
target_link_libraries(MidiatorTriggerTests PRIVATE sdk vstgui_support)
add_test(NAME MidiatorTriggerTests COMMAND MidiatorTriggerTests)

add_executable(MidiatorUIContractTests
    UIContractTests.cpp
)

target_compile_definitions(MidiatorUIContractTests PRIVATE
    MIDIATOR_UIDESC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../src/resource/midiator.uidesc"
)
add_test(NAME MidiatorUIContractTests COMMAND MidiatorUIContractTests)
