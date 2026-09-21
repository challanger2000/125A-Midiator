#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef MIDIATOR_UIDESC_PATH
#error MIDIATOR_UIDESC_PATH is not defined
#endif

static void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

int main() {
    std::ifstream in(MIDIATOR_UIDESC_PATH, std::ios::binary);
    require(static_cast<bool>(in), "midiator.uidesc must be readable");

    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string xml = ss.str();

    require(xml.find("<vstgui-ui-description") != std::string::npos,
            "UIDesc root element must exist");
    require(xml.find("template name=\"MidiatorView\"") != std::string::npos,
            "MidiatorView template must exist");

    for (int tag = 100; tag <= 110; ++tag) {
        const std::string needle = "tag=\"" + std::to_string(tag) + "\"";
        require(xml.find(needle) != std::string::npos,
                "all Midiator parameter tags 100..110 must be declared");
    }

    const char* theoryIds[] = {
        "midiator-id=\"theoryKey\"",
        "midiator-id=\"theoryNotes\"",
        "midiator-id=\"theoryCharacter\"",
        "midiator-id=\"theoryInterval\""
    };
    for (const char* id : theoryIds)
        require(xml.find(id) != std::string::npos,
                "all theory helper labels must be present");

    require(xml.find("control-tag=\"NewRiff\"") != std::string::npos,
            "NEW RIFF button must be bound");
    require(xml.find("control-tag=\"Variation\"") != std::string::npos,
            "VARIATION button must be bound");

    std::cout << "Midiator UI contract test: PASS\n";
    return 0;
}
