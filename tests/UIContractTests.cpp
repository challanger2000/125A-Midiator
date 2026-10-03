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
    require(xml.find("<bitmap name=\"BrandLogo\" path=\"125A_Logo.png\"") != std::string::npos,
            "authoritative 125A branding derivative must be declared");
    require(xml.find("title=\"MIDIATOR\"") != std::string::npos,
            "centered MIDIATOR title must exist");
    require(xml.find("title=\"125A  MIDIATOR\"") == std::string::npos,
            "125A must not be duplicated in the Midiator title");
    require(xml.find("minSize=\"645, 548\"") != std::string::npos &&
            xml.find("maxSize=\"1720, 1460\"") != std::string::npos,
            "resizable editor contract must expose non-identical min/max sizes");

    for (int tag = 100; tag <= 127; ++tag) {
        const std::string needle = "tag=\"" + std::to_string(tag) + "\"";
        require(xml.find(needle) != std::string::npos,
                "all Midiator parameter tags 100..127 must be declared");
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
    {
        const auto newPos = xml.find("control-tag=\"NewRiff\"");
        const auto varPos = xml.find("control-tag=\"Variation\"");
        require(newPos != std::string::npos && varPos != std::string::npos,
                "action button definitions must exist");
        require(xml.substr(newPos, 500).find("kick-style=\"true\"") != std::string::npos &&
                xml.substr(varPos, 500).find("kick-style=\"true\"") != std::string::npos,
                "action buttons must remain momentary GUI commands");
    }
    require(xml.find("control-tag=\"RootSource\"") != std::string::npos,
            "ROOT SOURCE selector must be bound");
    require(xml.find("<control-tag name=\"LegacyStyle\" tag=\"112\"") != std::string::npos,
            "legacy STYLE tag must remain declared for old automation");
    require(xml.find("<control-tag name=\"Style\" tag=\"126\"") != std::string::npos &&
            xml.find("control-tag=\"Style\"") != std::string::npos,
            "visible METAL STYLE selector must bind the new 12-style parameter");
    require(xml.find("control-tag=\"PalmMuteVelocity\"") != std::string::npos,
            "PM velocity threshold field must be bound");
    require(xml.find("control-tag=\"PowerChordsEnabled\"") != std::string::npos,
            "POWER CHORDS ON/OFF selector must be bound");
    require(xml.find("control-tag=\"DrumMap\"") != std::string::npos,
            "DRUM MAP selector must be bound");
    require(xml.find("control-tag=\"SectionLength\"") != std::string::npos,
            "SECTION LENGTH selector must be bound");
    require(xml.find("control-tag=\"SectionType\"") != std::string::npos,
            "SECTION selector must be bound");
    for (const char* roleTag : {"BassFollow", "BassMovement", "DrumDensity",
                                "DrumComplexity", "PadSpread", "PadTension",
                                "SynthActivity", "SynthMovement", "FillIntensity"}) {
        const std::string needle = std::string("control-tag=\"") + roleTag + "\"";
        require(xml.find(needle) != std::string::npos,
                "every focused role control must be bound in the GUI");
    }

    size_t sliderCount = 0;
    size_t pos = 0;
    while ((pos = xml.find("class=\"CSlider\"", pos)) != std::string::npos) {
        ++sliderCount;
        const auto end = xml.find("/>", pos);
        require(end != std::string::npos, "slider XML element must close");
        const auto slider = xml.substr(pos, end - pos);
        require(slider.find("draw-back=\"true\"") != std::string::npos &&
                slider.find("draw-frame=\"true\"") != std::string::npos &&
                slider.find("draw-value=\"true\"") != std::string::npos &&
                slider.find("draw-value-color=\"Accent\"") != std::string::npos,
                "every slider must use the visible native slider drawing");
        pos = end + 2;
    }
    require(sliderCount == 15, "exactly fifteen visible native sliders are required");

    size_t textEditCount = 0;
    pos = 0;
    while ((pos = xml.find("class=\"CTextEdit\"", pos)) != std::string::npos) {
        ++textEditCount;
        pos += 10;
    }
    require(textEditCount == 16,
            "fifteen percentage fields plus PM velocity must be editable");

    std::cout << "Midiator UI contract test: PASS\n";
    return 0;
}
