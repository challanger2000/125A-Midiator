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
