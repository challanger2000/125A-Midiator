#pragma once

#include "RiffEngine.h"
#include "BassBrain.h"
#include "DrumBrain.h"
#include "PadBrain.h"
#include "SynthBrain.h"

#include <algorithm>

namespace midiator {

inline float sectionClampUnit(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

inline GeneratorSettings sectionGeneratorSettings(const GeneratorSettings& base) {
    auto s = base;
    switch (base.section) {
        case SectionType::Free: return s;
        case SectionType::Intro:
            s.density *= 0.62f; s.complexity *= 0.70f; s.repetition += 0.10f;
            s.powerChordChance *= 0.55f; s.palmMuteChance *= 0.85f; break;
        case SectionType::Verse:
            s.density *= 0.90f; s.complexity *= 0.88f; s.repetition += 0.06f;
            s.powerChordChance *= 0.78f; s.palmMuteChance *= 1.05f; break;
        case SectionType::PreChorus:
            s.density *= 1.00f; s.complexity *= 1.08f; s.repetition -= 0.06f;
            s.powerChordChance *= 0.95f; s.palmMuteChance *= 0.95f; break;
        case SectionType::Chorus:
            s.density *= 1.10f; s.complexity *= 1.00f; s.repetition += 0.04f;
            s.powerChordChance *= 1.25f; s.palmMuteChance *= 0.70f; break;
        case SectionType::Breakdown:
            s.density *= 0.78f; s.complexity *= 0.70f; s.repetition += 0.12f;
            s.powerChordChance *= 1.20f; s.palmMuteChance *= 1.15f; break;
        case SectionType::Outro:
            s.density *= 0.68f; s.complexity *= 0.65f; s.repetition += 0.10f;
            s.powerChordChance *= 0.70f; s.palmMuteChance *= 0.75f; break;
        case SectionType::Count: break;
    }
    s.density=sectionClampUnit(s.density);
    s.complexity=sectionClampUnit(s.complexity);
    s.repetition=sectionClampUnit(s.repetition);
    s.powerChordChance=sectionClampUnit(s.powerChordChance);
    s.palmMuteChance=sectionClampUnit(s.palmMuteChance);
    return s;
}

inline void applySectionPhraseShape(Phrase& phrase,
                                         SectionType section,
                                         uint32_t seed) {
    if (section == SectionType::Free ||
        section == SectionType::Chorus ||
        section == SectionType::PreChorus)
        return;

    float offbeatKeep = 1.0f;
    float strongKeep = 1.0f;
    switch (section) {
        case SectionType::Intro:
            offbeatKeep = 0.48f; strongKeep = 0.88f; break;
        case SectionType::Verse:
            offbeatKeep = 0.86f; strongKeep = 1.00f; break;
        case SectionType::Breakdown:
            offbeatKeep = 0.68f; strongKeep = 1.00f; break;
        case SectionType::Outro:
            offbeatKeep = 0.58f; strongKeep = 0.88f; break;
        default:
            return;
    }

    auto unitFor = [&](int step) {
        uint32_t x = seed ^ (0x9E3779B9u * static_cast<uint32_t>(step + 1));
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        return static_cast<float>(x & 0x00ffffffu) /
               static_cast<float>(0x01000000u);
    };

    for (int step=0; step<phrase.usedSteps(); ++step) {
        auto& st = phrase.steps[step];
        if (st.noteCount <= 0)
            continue;
        const int local = step % kStepsPerBar;
        const bool strong = (local % 4) == 0;
        const float keep = strong ? strongKeep : offbeatKeep;
        if (unitFor(step) > keep)
            st = {};
    }

    // Never let a section profile erase the opening musical anchor.
    if (phrase.steps[0].noteCount == 0) {
        // The raw generated phrase always owns a valid first step; section
        // thinning only reaches this path defensively.
        phrase.steps[0].noteCount = 1;
        phrase.steps[0].notes[0] = {33, 100, 1};
    }
}

inline BassSettings sectionBassSettings(const BassSettings& base, SectionType section) {
    auto s=base;
    switch(section) {
        case SectionType::Free: return s;
        case SectionType::Intro:
            s.follow*=0.90f; s.activity*=0.42f; s.movement*=0.80f;
            s.passing*=0.65f; s.octaveChance*=0.65f; s.sustain*=1.15f; break;
        case SectionType::Verse:
            s.follow*=1.00f; s.activity*=0.90f; s.movement*=0.95f;
            s.passing*=0.90f; break;
        case SectionType::PreChorus:
            s.follow*=1.03f; s.movement*=1.08f; s.passing*=1.10f; break;
        case SectionType::Chorus:
            s.follow*=1.05f; s.movement*=1.00f; s.octaveChance*=1.30f;
            s.sustain*=1.10f; break;
        case SectionType::Breakdown:
            s.follow*=1.12f; s.activity*=0.78f; s.movement*=0.70f;
            s.passing*=0.65f; s.sustain*=1.25f; break;
        case SectionType::Outro:
            s.follow*=0.92f; s.activity*=0.58f; s.movement*=0.85f;
            s.passing*=0.70f; s.sustain*=1.15f; break;
        case SectionType::Count: break;
    }
    s.follow=sectionClampUnit(s.follow); s.activity=sectionClampUnit(s.activity);
    s.movement=sectionClampUnit(s.movement); s.passing=sectionClampUnit(s.passing);
    s.octaveChance=sectionClampUnit(s.octaveChance);
    s.sustain=sectionClampUnit(s.sustain);
    return s;
}

inline DrumSettings sectionDrumSettings(const DrumSettings& base, SectionType section) {
    auto s=base;
    switch(section) {
        case SectionType::Free: return s;
        case SectionType::Intro:
            s.density*=0.55f; s.complexity*=0.55f; s.fillIntensity*=0.50f;
            s.crashOnDownbeat=false; break;
        case SectionType::Verse:
            s.density*=0.90f; s.complexity*=0.85f; s.fillIntensity*=0.75f; break;
        case SectionType::PreChorus:
            s.density*=1.05f; s.complexity*=1.10f; s.fillIntensity*=1.15f; break;
        case SectionType::Chorus:
            s.density*=1.12f; s.complexity*=1.00f; s.fillIntensity*=1.00f;
            s.crashOnDownbeat=true; break;
        case SectionType::Breakdown:
            s.density*=0.78f; s.complexity*=0.75f; s.fillIntensity*=0.70f; break;
        case SectionType::Outro:
            s.density*=0.65f; s.complexity*=0.65f; s.fillIntensity*=0.80f; break;
        case SectionType::Count: break;
    }
    s.density=sectionClampUnit(s.density);
    s.complexity=sectionClampUnit(s.complexity);
    s.fillIntensity=sectionClampUnit(s.fillIntensity);
    return s;
}

inline PadSettings sectionPadSettings(const PadSettings& base, SectionType section) {
    auto s=base;
    switch(section) {
        case SectionType::Free: return s;
        case SectionType::Intro:
            s.movement*=0.80f; s.spread*=1.10f; s.tension*=0.75f;
            s.sustain*=1.10f; s.contextFollow*=0.90f; break;
        case SectionType::Verse:
            s.movement*=0.90f; s.spread*=0.90f; s.tension*=0.90f; break;
        case SectionType::PreChorus:
            s.movement*=1.05f; s.spread*=1.05f; s.tension*=1.15f; break;
        case SectionType::Chorus:
            s.movement*=1.10f; s.spread*=1.18f; s.tension*=0.95f; break;
        case SectionType::Breakdown:
            s.movement*=0.70f; s.spread*=0.85f; s.tension*=1.10f; break;
        case SectionType::Outro:
            s.movement*=0.75f; s.spread*=1.10f; s.tension*=0.80f; break;
        case SectionType::Count: break;
    }
    s.movement=sectionClampUnit(s.movement); s.spread=sectionClampUnit(s.spread);
    s.tension=sectionClampUnit(s.tension); s.sustain=sectionClampUnit(s.sustain);
    s.contextFollow=sectionClampUnit(s.contextFollow);
    return s;
}

inline SynthSettings sectionSynthSettings(const SynthSettings& base, SectionType section) {
    auto s=base;
    switch(section) {
        case SectionType::Free: return s;
        case SectionType::Intro:
            s.activity*=0.55f; s.movement*=0.75f; s.repetition+=0.10f;
            s.syncopation*=0.65f; s.sustain*=1.20f; break;
        case SectionType::Verse:
            s.activity*=0.75f; s.movement*=0.90f; s.repetition+=0.06f; break;
        case SectionType::PreChorus:
            s.activity*=1.00f; s.movement*=1.10f; s.repetition-=0.05f;
            s.syncopation*=1.10f; break;
        case SectionType::Chorus:
            s.activity*=1.10f; s.movement*=1.05f; s.repetition+=0.05f;
            s.sustain*=1.05f; break;
        case SectionType::Breakdown:
            s.activity*=0.45f; s.movement*=0.80f; s.repetition+=0.12f;
            s.syncopation*=0.60f; break;
        case SectionType::Outro:
            s.activity*=0.55f; s.movement*=0.80f; s.repetition+=0.10f; break;
        case SectionType::Count: break;
    }
    s.activity=sectionClampUnit(s.activity); s.movement=sectionClampUnit(s.movement);
    s.repetition=sectionClampUnit(s.repetition); s.syncopation=sectionClampUnit(s.syncopation);
    s.sustain=sectionClampUnit(s.sustain); s.harmonicFollow=sectionClampUnit(s.harmonicFollow);
    return s;
}

} // namespace midiator
