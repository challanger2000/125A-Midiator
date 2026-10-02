#include "../src/BassBrain.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace midiator;

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

Phrase makeGuitarFixture() {
    GeneratorSettings s{};
    s.bars = 4;
    s.rootPitchClass = 9;
    s.scale = ScaleId::Phrygian;
    s.style = StyleId::NDHIndustrial;
    s.density = 0.56f;
    s.complexity = 0.42f;
    s.repetition = 0.72f;
    return RiffEngine::generate(s, 0xB455F17u);
}

void testDeterministicAndMonophonic() {
    const auto guitar = makeGuitarFixture();
    BassSettings s{};
    const auto a = BassBrain::generate(guitar, s, 12345u);
    const auto b = BassBrain::generate(guitar, s, 12345u);
    require(a == b, "Bass Brain must be deterministic for fixed settings and seed");

    for (int i = 0; i < a.usedSteps(); ++i) {
        const auto& st = a.steps[i];
        require(st.noteCount <= 1, "Bass Brain must remain monophonic");
        if (st.noteCount == 0) continue;
        require(st.notes[0].pitch >= 24 && st.notes[0].pitch <= 60,
                "Bass notes must stay in the defined bass register");
        require(RiffEngine::isScaleTone(st.notes[0].pitch, s.rootPitchClass, s.scale),
                "Bass primary notes must stay in the selected scale");
    }
}

void testFollowControlsGuitarLock() {
    const auto guitar = makeGuitarFixture();
    BassSettings low{};
    low.follow = 0.0f;
    BassSettings mid = low;
    mid.follow = 0.72f;
    BassSettings high = low;
    high.follow = 1.0f;

    long long lowCoincidence = 0, midCoincidence = 0, highCoincidence = 0;
    long long lowHits = 0, midHits = 0, highHits = 0;

    for (unsigned seed = 1; seed <= 256; ++seed) {
        const auto a = BassBrain::generate(guitar, low, 10000u + seed);
        const auto m = BassBrain::generate(guitar, mid, 10000u + seed);
        const auto b = BassBrain::generate(guitar, high, 10000u + seed);
        for (int i = 0; i < guitar.usedSteps(); ++i) {
            const bool gh = guitar.steps[i].noteCount > 0;
            if (a.steps[i].noteCount > 0) {
                ++lowHits;
                if (gh) ++lowCoincidence;
            }
            if (m.steps[i].noteCount > 0) {
                ++midHits;
                if (gh) ++midCoincidence;
            }
            if (b.steps[i].noteCount > 0) {
                ++highHits;
                if (gh) ++highCoincidence;
            }
        }
    }

    const double lowShare = static_cast<double>(lowCoincidence) / std::max<long long>(1, lowHits);
    const double midShare = static_cast<double>(midCoincidence) / std::max<long long>(1, midHits);
    const double highShare = static_cast<double>(highCoincidence) / std::max<long long>(1, highHits);
    std::cerr << "Bass Follow lock 0/72/100 = "
              << lowShare << ", " << midShare << ", " << highShare << "\n";
    require(highShare > lowShare + 0.25,
            "Bass Follow must materially increase lock to guitar onsets");
    require(lowShare < 0.65,
            "Bass Follow 0% must leave clearly independent rhythmic space");
    require(midShare > lowShare + 0.10 && midShare < 0.90,
            "default Bass Follow must be tight without becoming a guitar copy");
    require(highShare > 0.85,
            "Bass Follow 100% must remain strongly riff-locked");
}

void testMovementReducesRootDominance() {
    const auto guitar = makeGuitarFixture();
    BassSettings low{};
    low.movement = 0.0f;
    BassSettings high = low;
    high.movement = 1.0f;

    long long lowRoot = 0, highRoot = 0;
    long long lowNotes = 0, highNotes = 0;

    for (unsigned seed = 1; seed <= 256; ++seed) {
        const auto a = BassBrain::generate(guitar, low, 20000u + seed);
        const auto b = BassBrain::generate(guitar, high, 20000u + seed);
        for (int i = 0; i < guitar.usedSteps(); ++i) {
            if (a.steps[i].noteCount > 0) {
                ++lowNotes;
                if ((a.steps[i].notes[0].pitch % 12) == low.rootPitchClass) ++lowRoot;
            }
            if (b.steps[i].noteCount > 0) {
                ++highNotes;
                if ((b.steps[i].notes[0].pitch % 12) == high.rootPitchClass) ++highRoot;
            }
        }
    }

    const double lowShare = static_cast<double>(lowRoot) / std::max<long long>(1, lowNotes);
    const double highShare = static_cast<double>(highRoot) / std::max<long long>(1, highNotes);
    require(lowShare > highShare + 0.10,
            "Bass Movement must reduce pedal-root dominance");
}

void testAllScalesStaySafeAtBassRegisterEdges() {
    for (int scaleIndex = 0;
         scaleIndex < static_cast<int>(ScaleId::Count);
         ++scaleIndex) {
        for (int root = 0; root < 12; ++root) {
            GeneratorSettings gs{};
            gs.rootPitchClass = root;
            gs.scale = static_cast<ScaleId>(scaleIndex);
            gs.bars = 4;
            gs.density = 1.0f;
            gs.complexity = 1.0f;

            for (unsigned seed = 1; seed <= 128; ++seed) {
                const auto guitar = RiffEngine::generate(
                    gs, 140000u + seed + static_cast<unsigned>(root * 1000 + scaleIndex * 20000));

                BassSettings bs{};
                bs.rootPitchClass = root;
                bs.scale = static_cast<ScaleId>(scaleIndex);
                bs.follow = 0.0f;
                bs.movement = 1.0f;
                bs.passing = 1.0f;
                bs.octaveChance = 1.0f;
                bs.sustain = 1.0f;

                const auto bass = BassBrain::generate(
                    guitar, bs, 240000u + seed + static_cast<unsigned>(root * 1000 + scaleIndex * 20000));

                for (int i = 0; i < bass.usedSteps(); ++i) {
                    if (bass.steps[i].noteCount <= 0)
                        continue;
                    const auto& n = bass.steps[i].notes[0];
                    require(n.pitch >= 24 && n.pitch <= 60,
                            "Bass register-edge test must remain in supported range");
                    require(RiffEngine::isScaleTone(n.pitch, root, bs.scale),
                            "Bass register-edge snapping must remain scale-safe for every root/scale");
                }
            }
        }
    }
}

void testStylesHaveDistinctBassRoles() {
    const auto guitar = makeGuitarFixture();

    struct Stats { double root=0.0, lock=0.0, longs=0.0, upper=0.0, hits=0.0; };
    auto measure=[&](StyleId style){
        long long hits=0,roots=0,locked=0,longs=0,upper=0;
        for(unsigned seed=1;seed<=256;++seed){
            BassSettings s{}; s.style=style; s.movement=0.45f; s.sustain=0.55f;
            const auto b=BassBrain::generate(guitar,s,80000u+seed);
            for(int i=0;i<b.usedSteps();++i){
                if(b.steps[i].noteCount<=0) continue;
                ++hits; const auto& n=b.steps[i].notes[0];
                if(((n.pitch%12)+12)%12==s.rootPitchClass) ++roots;
                if(guitar.steps[i].noteCount>0) ++locked;
                if(n.lengthSteps>1) ++longs;
                if(n.pitch>=40) ++upper;
            }
        }
        Stats st{}; const double hc=std::max(1.0,static_cast<double>(hits));
        st.hits=hits/256.0; st.root=roots/hc; st.lock=locked/hc;
        st.longs=longs/hc; st.upper=upper/hc; return st;
    };

    const auto ndh=measure(StyleId::NDHIndustrial);
    const auto dark=measure(StyleId::DarkRockGothic);
    const auto heavy=measure(StyleId::HeavyIndustrial);

    require(ndh.root > dark.root + 0.05,
            "NDH bass must remain more pedal-root focused than Dark Rock/Gothic");
    require(dark.longs > heavy.longs + 0.05,
            "Dark Rock/Gothic bass must sustain more than Heavy Industrial");
    require(heavy.lock > dark.lock + 0.05,
            "Heavy Industrial bass must lock to the riff more strongly than Dark Rock/Gothic");
    require(heavy.upper > ndh.upper + 0.02,
            "Heavy Industrial bass must use selective upper-octave reinforcement more than NDH");
}

void testNoOverlapAndDownbeatAnchor() {
    const auto guitar = makeGuitarFixture();
    BassSettings s{};
    s.sustain = 1.0f;
    const auto bass = BassBrain::generate(guitar, s, 777u);

    require(bass.steps[0].noteCount == 1,
            "Bass phrase must always contain a downbeat anchor");

    for (int i = 0; i < bass.usedSteps(); ++i) {
        if (bass.steps[i].noteCount == 0) continue;
        const auto& n = bass.steps[i].notes[0];
        for (int j = i + 1; j < std::min(bass.usedSteps(), i + n.lengthSteps); ++j)
            require(bass.steps[j].noteCount == 0,
                    "Bass notes must not overlap later bass onsets");
    }
}

} // namespace

int main() {
    testDeterministicAndMonophonic();
    testFollowControlsGuitarLock();
    testMovementReducesRootDominance();
    testAllScalesStaySafeAtBassRegisterEdges();
    testStylesHaveDistinctBassRoles();
    testNoOverlapAndDownbeatAnchor();

    std::cout << "Midiator Bass Brain tests: PASS\n";
    return 0;
}
