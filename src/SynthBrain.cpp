#include "SynthBrain.h"
#include "PadBrain.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace midiator {
namespace {

struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed ? seed : 0x13579bdfu) {}
    uint32_t nextU32() {
        uint32_t x = state;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        state = x;
        return x;
    }
    float unit() {
        return static_cast<float>(nextU32() & 0x00ffffffu) /
               static_cast<float>(0x01000000u);
    }
    bool chance(float p) { return unit() < std::clamp(p,0.0f,1.0f); }
};

int wrap12(int v){ v%=12; return v<0?v+12:v; }

int nearestScalePitch(int target, const SynthSettings& s) {
    int best=std::clamp(target,48,96), bestD=999;
    for(int p=48;p<=96;++p){
        if(!RiffEngine::isScaleTone(p,s.rootPitchClass,s.scale)) continue;
        int d=std::abs(p-target);
        if(d<bestD){best=p;bestD=d;}
    }
    return best;
}

int pitchFromDegree(int degree, const SynthSettings& s, int around) {
    const auto& scale=RiffEngine::scaleDefinition(s.scale);
    degree%=scale.count; if(degree<0) degree+=scale.count;
    const int pc=wrap12(s.rootPitchClass+scale.intervals[degree]);
    int best=around,bestD=999;
    for(int p=48;p<=96;++p){
        if(wrap12(p)!=pc) continue;
        int d=std::abs(p-around);
        if(d<bestD){best=p;bestD=d;}
    }
    return best;
}

int nearestPadChordPitch(const PadPhrase& pads, int step, int around) {
    if (pads.usedSteps() <= 0)
        return around;

    const int used = pads.usedSteps();
    int idx = ((step % used) + used) % used;

    // Find the harmony currently sounding: nearest pad onset at or before
    // this synth step, wrapping to the previous phrase cycle if necessary.
    int chordStep = -1;
    for (int delta = 0; delta < used; ++delta) {
        const int candidate = (idx - delta + used) % used;
        if (pads.steps[candidate].noteCount > 0) {
            chordStep = candidate;
            break;
        }
    }
    if (chordStep < 0)
        return around;

    const auto& chord = pads.steps[chordStep];
    int best = around;
    int bestDistance = 999;
    for (int n = 0; n < chord.noteCount; ++n) {
        const int pc = wrap12(chord.notes[n].pitch);
        for (int p = 48; p <= 96; ++p) {
            if (wrap12(p) != pc)
                continue;
            const int d = std::abs(p - around);
            if (d < bestDistance) {
                best = p;
                bestDistance = d;
            }
        }
    }
    return best;
}

int secondPadChordPitch(const PadPhrase& pads, int step, int firstPitch, int around) {
    if (pads.usedSteps() <= 0)
        return -1;

    const int used = pads.usedSteps();
    const int idx = ((step % used) + used) % used;
    int chordStep = -1;
    for (int delta = 0; delta < used; ++delta) {
        const int candidate = (idx - delta + used) % used;
        if (pads.steps[candidate].noteCount > 0) {
            chordStep = candidate;
            break;
        }
    }
    if (chordStep < 0)
        return -1;

    const int firstPc = wrap12(firstPitch);
    const auto& chord = pads.steps[chordStep];
    int best = -1;
    int bestDistance = 999;
    for (int n = 0; n < chord.noteCount; ++n) {
        const int pc = wrap12(chord.notes[n].pitch);
        if (pc == firstPc)
            continue;
        for (int p = 48; p <= 96; ++p) {
            if (wrap12(p) != pc)
                continue;
            const int d = std::abs(p - around);
            if (d < bestDistance && std::abs(p - firstPitch) <= 12) {
                best = p;
                bestDistance = d;
            }
        }
    }
    return best;
}

} // namespace

Phrase SynthBrain::generate(const Phrase& guitar,
                            const Phrase& bass,
                            const PadPhrase& pads,
                            const SynthSettings& settings,
                            uint32_t seed) {
    SynthSettings s=settings;
    s.rootPitchClass=wrap12(s.rootPitchClass);
    s.activity=std::clamp(s.activity,0.0f,1.0f);
    s.movement=std::clamp(s.movement,0.0f,1.0f);
    s.repetition=std::clamp(s.repetition,0.0f,1.0f);
    s.syncopation=std::clamp(s.syncopation,0.0f,1.0f);
    s.sustain=std::clamp(s.sustain,0.0f,1.0f);
    s.harmonicFollow=std::clamp(s.harmonicFollow,0.0f,1.0f);
    s.centerMidi=std::clamp(s.centerMidi,60,84);

    Phrase out{};
    out.bars=std::clamp(std::max(guitar.bars,bass.bars),1,kMaxBars);
    Rng rhythmRng(seed);
    Rng pitchRng(seed ^ 0x50495443u);
    Rng lengthRng(seed ^ 0x4C454E47u);
    Rng harmonyRng(seed ^ 0x4841524Du);
    Rng gestureRng(seed ^ 0x47455354u);

    std::array<int,8> motifDegree{{0,2,4,1,0,3,2,5}};
    std::array<int,8> motifHit{{1,0,1,1,0,1,0,1}};

    if(s.style==StyleId::NDHIndustrial){
        motifDegree={{0,0,2,0,3,0,2,0}};
        motifHit={{1,0,1,0,1,1,0,1}};
    } else if(s.style==StyleId::DarkRockGothic){
        motifDegree={{0,2,4,5,4,2,1,3}};
        motifHit={{1,0,1,1,0,1,1,0}};
    } else if(s.style==StyleId::HeavyIndustrial){
        motifDegree={{0,3,0,4,2,0,5,0}};
        motifHit={{1,1,0,1,1,1,0,1}};
    }

    int previousPitch=nearestScalePitch(s.centerMidi,s);
    for(int step=0;step<out.usedSteps();++step){
        const int local=step%16;
        const int motifIndex=(step/2)%8;
        const bool offbeat=(local%2)!=0;

        // The base motif lives on the straight 8th-note skeleton. Syncopation
        // has a real job: it introduces the in-between 16ths instead of
        // starting from an already ~50% offbeat pattern.
        bool hit=!offbeat && motifHit[motifIndex]!=0;

        if(offbeat) {
            float syncChance=0.02f+0.68f*s.syncopation;
            if(s.style==StyleId::HeavyIndustrial)
                syncChance+=0.08f;
            else if(s.style==StyleId::DarkRockGothic)
                syncChance+=0.03f;

            if(motifHit[motifIndex]!=0 && rhythmRng.chance(syncChance))
                hit=true;
            else if(rhythmRng.chance(0.02f+0.16f*s.activity*s.syncopation))
                hit=true;
        } else if(!hit && rhythmRng.chance(0.04f+0.24f*s.activity)) {
            hit=true;
        }

        if(hit && !rhythmRng.chance(0.62f+0.33f*s.activity))
            hit=false;

        if(step%16==0)
            hit=true;

        if(!hit) continue;

        int degree=motifDegree[motifIndex];
        const float mutationProbability =
            (1.0f - s.repetition) * (0.25f + 0.70f * s.movement);
        if(pitchRng.chance(mutationProbability))
            degree += pitchRng.chance(0.5f)?1:-1;

        int target=s.centerMidi;
        if(s.style==StyleId::DarkRockGothic) target+=5;
        else if(s.style==StyleId::HeavyIndustrial) target+=2;

        int pitch=pitchFromDegree(degree,s,target);

        if(pitchRng.chance(0.10f+0.55f*s.movement)){
            int alt=pitch + (pitchRng.chance(0.5f)?12:-12);
            if(alt>=48 && alt<=96 && std::abs(alt-previousPitch)<=12)
                pitch=alt;
        }

        // Harmonic Follow does not turn the synth into a mechanical arpeggiator:
        // it probabilistically gravitates melodic notes toward the currently
        // sounding pad chord while preserving the role's own rhythm and motif.
        if(harmonyRng.chance(s.harmonicFollow)){
            const int chordTone = nearestPadChordPitch(pads, step, pitch);
            if(std::abs(chordTone-pitch)<=7 || s.harmonicFollow>0.85f)
                pitch=chordTone;
        }

        if(std::abs(pitch-previousPitch)>12)
            pitch=nearestScalePitch(previousPitch + (pitch>previousPitch?7:-7),s);

        auto& st=out.steps[step];
        st.noteCount=1;
        int velocity=82;
        if(local%4==0) velocity+=10;
        if(s.style==StyleId::HeavyIndustrial) velocity+=5;
        else if(s.style==StyleId::DarkRockGothic) velocity-=4;

        // Synth gestures stay deliberately short: arp/ostinato notes and
        // compact phrase fragments, not long lead lines.
        int len=1;
        if(lengthRng.chance(0.10f+0.55f*s.sustain))
            len=2;

        st.notes[0]={pitch,std::clamp(velocity,1,126),len};

        // Occasional short two-note chord stabs. The gesture decision has its
        // own RNG stream so Movement/Repetition/Sustain cannot rewrite whether
        // a step is a stab. Prefer a second active Pad chord tone.
        float stabChance = 0.0f;
        if ((local % 4) == 0) {
            if (s.style==StyleId::NDHIndustrial)
                stabChance=0.22f;
            else if (s.style==StyleId::DarkRockGothic)
                stabChance=0.12f;
            else
                stabChance=0.28f;
        } else if (s.style==StyleId::HeavyIndustrial) {
            stabChance=0.06f;
        }

        if (gestureRng.chance(stabChance)) {
            int second=secondPadChordPitch(pads,step,pitch,pitch+5);
            if(second<0) {
                second=pitchFromDegree(degree+2,s,pitch+4);
                if(second==pitch || std::abs(second-pitch)>12)
                    second=-1;
            }

            if(second>=48 && second<=96 &&
               second!=pitch &&
               RiffEngine::isScaleTone(second,s.rootPitchClass,s.scale)) {
                st.noteCount=2;
                const int stabLen=gestureRng.chance(0.72f)?1:2;
                st.notes[0].lengthSteps=stabLen;
                st.notes[1]={
                    second,
                    std::clamp(velocity-5,1,126),
                    stabLen
                };
            }
        }

        previousPitch=pitch;
    }

    return out;
}

} // namespace midiator
