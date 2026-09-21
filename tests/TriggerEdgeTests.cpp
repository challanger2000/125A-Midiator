#include "PluginProcessor.h"

#include <cstdlib>
#include <iostream>

using Steinberg::Vst::RisingEdgeTrigger;

static void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

int main() {
    RisingEdgeTrigger t{};

    require(!t.update(0.0), "initial low state must not trigger");
    require(t.update(1.0), "low-to-high transition must trigger");
    require(!t.update(1.0), "held high state must not retrigger");
    require(!t.update(0.0), "button release must not trigger");
    require(t.update(1.0), "second press must trigger once");
    require(!t.update(0.0), "second release must not trigger");

    std::cout << "Midiator trigger edge test: PASS\n";
    return 0;
}
