#include "SynthBrain.h"
#include "PadBrain.h"
#include "BassBrain.h"
#include "RiffEngine.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace midiator;

namespace {
void require(bool c,const char* m){ if(!c){std::cerr<<"FAIL: "<<m<<"\n";std::exit(1);} }

void makeContext(Phrase& g,Phrase& b,PadPhrase& p){
    GeneratorSettings gs{}; gs.bars=4;
    g=RiffEngine::generate(gs,0x51594E31u);
    BassSettings bs{};
    b=BassBrain::generate(g,bs,0x51594E32u);
    PadSettings ps{};
    p=PadBrain::generate(g,b,ps,0x51594E33u);
}

void testDeterministicCompactGestureScaleSafe(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);
    SynthSettings s{};
    const auto a=SynthBrain::generate(g,b,p,s,123u);
    const auto z=SynthBrain::generate(g,b,p,s,123u);
    require(a.bars==z.bars,"Synth Brain must be deterministic");
    int hits=0;
    for(int i=0;i<a.usedSteps();++i){
        require(a.steps[i].noteCount==z.steps[i].noteCount,"Synth topology must be deterministic");
        require(a.steps[i].noteCount<=2,"Synth gestures must stay compact at maximum two simultaneous notes");
        if(a.steps[i].noteCount<=0) continue;
        ++hits;
        for(int nidx=0;nidx<a.steps[i].noteCount;++nidx){
            const auto& n=a.steps[i].notes[nidx];
            require(n.pitch==z.steps[i].notes[nidx].pitch &&
                    n.velocity==z.steps[i].notes[nidx].velocity &&
                    n.lengthSteps==z.steps[i].notes[nidx].lengthSteps,
                    "Synth notes must be deterministic");
            require(n.pitch>=48&&n.pitch<=96,"Synth pitch must remain in melodic register");
            require(RiffEngine::isScaleTone(n.pitch,s.rootPitchClass,s.scale),
                    "Synth must remain scale-safe");
        }
    }
    require(hits>4,"Synth must generate a usable melodic part");
}

void testActivityRaisesHitCount(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);
    auto avg=[&](float activity){
        double hits=0;
        for(unsigned seed=1;seed<=256;++seed){
            SynthSettings s{}; s.activity=activity;
            const auto q=SynthBrain::generate(g,b,p,s,20000u+seed);
            for(int i=0;i<q.usedSteps();++i) if(q.steps[i].noteCount>0) ++hits;
        }
        return hits/256.0;
    };
    require(avg(1.0f)>avg(0.0f)+6.0,"Synth Activity must materially increase hit count");
}


void testMovementMateriallyIncreasesPitchTravel(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);

    auto avgJump=[&](float movement){
        long long jumps=0,total=0;
        for(unsigned seed=1;seed<=256;++seed){
            SynthSettings s{}; s.movement=movement;
            const auto q=SynthBrain::generate(g,b,p,s,23000u+seed);
            int prev=-1;
            for(int i=0;i<q.usedSteps();++i){
                if(q.steps[i].noteCount<=0) continue;
                const int pitch=q.steps[i].notes[0].pitch;
                if(prev>=0){ total+=std::abs(pitch-prev); ++jumps; }
                prev=pitch;
            }
        }
        return jumps?static_cast<double>(total)/jumps:0.0;
    };

    const double low=avgJump(0.0f);
    const double mid=avgJump(0.5f);
    const double high=avgJump(1.0f);

    require(mid>low+1.2,
            "Synth Movement 50% must materially increase melodic pitch travel over 0%");
    require(high>mid+0.7,
            "Synth Movement 100% must remain progressively stronger than 50%");
}


void testSyncopationRaisesOffbeatShare(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);

    auto offbeatShare=[&](float syncopation){
        long long hits=0,offbeats=0;
        for(unsigned seed=1;seed<=256;++seed){
            SynthSettings s{}; s.syncopation=syncopation;
            const auto q=SynthBrain::generate(g,b,p,s,25000u+seed);
            for(int i=0;i<q.usedSteps();++i){
                if(q.steps[i].noteCount<=0) continue;
                ++hits;
                if((i%2)!=0) ++offbeats;
            }
        }
        return hits?static_cast<double>(offbeats)/hits:0.0;
    };

    const double low=offbeatShare(0.0f);
    const double high=offbeatShare(1.0f);
    require(high>low+0.20,
            "Synth Syncopation must materially increase offbeat 16th-note activity");
}

void testSustainRaisesLongNoteShare(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);
    auto share=[&](float sustain){
        long long hits=0,longs=0;
        for(unsigned seed=1;seed<=256;++seed){
            SynthSettings s{}; s.sustain=sustain;
            const auto q=SynthBrain::generate(g,b,p,s,30000u+seed);
            for(int i=0;i<q.usedSteps();++i) if(q.steps[i].noteCount>0){
                ++hits; if(q.steps[i].notes[0].lengthSteps>1) ++longs;
            }
        }
        return hits?static_cast<double>(longs)/hits:0.0;
    };
    require(share(1.0f)>share(0.0f)+0.35,"Synth Sustain must materially increase long notes");
}

void testPitchAndLengthControlsDoNotRewriteRhythm(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);

    auto sameTopology=[](const Phrase& a,const Phrase& z){
        if(a.usedSteps()!=z.usedSteps()) return false;
        for(int i=0;i<a.usedSteps();++i)
            if(a.steps[i].noteCount!=z.steps[i].noteCount) return false;
        return true;
    };

    for(unsigned seed=1;seed<=128;++seed){
        SynthSettings base{};
        const auto ref=SynthBrain::generate(g,b,p,base,50000u+seed);

        SynthSettings move=base; move.movement=1.0f;
        require(sameTopology(ref,SynthBrain::generate(g,b,p,move,50000u+seed)),
                "Synth Movement must change pitch behavior without rewriting rhythm");

        SynthSettings rep=base; rep.repetition=0.0f;
        require(sameTopology(ref,SynthBrain::generate(g,b,p,rep,50000u+seed)),
                "Synth Repetition must change motif pitch identity without rewriting rhythm");

        SynthSettings sus=base; sus.sustain=1.0f;
        require(sameTopology(ref,SynthBrain::generate(g,b,p,sus,50000u+seed)),
                "Synth Sustain must change note length without rewriting rhythm");
    }
}

void testRepetitionPreservesMotifPitchIdentity(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);

    auto motifAgreement=[&](float repetition){
        long long compared=0,matching=0;
        for(unsigned seed=1;seed<=256;++seed){
            SynthSettings s{}; s.repetition=repetition;
            const auto q=SynthBrain::generate(g,b,p,s,60000u+seed);
            for(int i=16;i<q.usedSteps();++i){
                const int ref=i%16;
                if(q.steps[i].noteCount<=0 || q.steps[ref].noteCount<=0) continue;
                ++compared;
                if((q.steps[i].notes[0].pitch%12+12)%12 ==
                   (q.steps[ref].notes[0].pitch%12+12)%12)
                    ++matching;
            }
        }
        return compared?static_cast<double>(matching)/compared:0.0;
    };

    const double low=motifAgreement(0.0f);
    const double high=motifAgreement(1.0f);
    require(high>low+0.12,
            "Synth Repetition must materially increase repeated motif pitch identity");
}

void testHarmonicFollowTargetsPadChordTones(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);

    auto chordToneShare=[&](float follow){
        long long hits=0,matches=0;
        for(unsigned seed=1;seed<=256;++seed){
            SynthSettings s{}; s.harmonicFollow=follow;
            const auto q=SynthBrain::generate(g,b,p,s,70000u+seed);

            int activeChord=-1;
            for(int i=0;i<q.usedSteps();++i){
                if(i<p.usedSteps() && p.steps[i].noteCount>0)
                    activeChord=i;
                if(q.steps[i].noteCount<=0 || activeChord<0) continue;

                ++hits;
                const int pc=(q.steps[i].notes[0].pitch%12+12)%12;
                const auto& chord=p.steps[activeChord];
                for(int n=0;n<chord.noteCount;++n){
                    if(((chord.notes[n].pitch%12+12)%12)==pc){
                        ++matches; break;
                    }
                }
            }
        }
        return hits?static_cast<double>(matches)/hits:0.0;
    };

    const double independent=chordToneShare(0.0f);
    const double guided=chordToneShare(1.0f);
    require(guided>independent+0.20,
            "Synth Harmonic Follow must materially increase pad-chord-tone targeting");
}

void testShortChordStabsOccur(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);
    long long hits=0,dyads=0;

    for(unsigned seed=1;seed<=512;++seed){
        SynthSettings s{};
        const auto q=SynthBrain::generate(g,b,p,s,80000u+seed);
        for(int i=0;i<q.usedSteps();++i){
            const auto& st=q.steps[i];
            if(st.noteCount<=0) continue;
            ++hits;
            if(st.noteCount==2){
                ++dyads;
                require(st.notes[0].lengthSteps<=2 && st.notes[1].lengthSteps<=2,
                        "Synth chord stabs must remain short");
                require(st.notes[0].pitch!=st.notes[1].pitch,
                        "Synth chord stab notes must be distinct");
            }
            for(int n=0;n<st.noteCount;++n)
                require(st.notes[n].lengthSteps<=2,
                        "Synth arp/phrase notes must remain short");
        }
    }

    require(dyads>0,"Synth must generate occasional short two-note chord stabs");
    const double share=hits?static_cast<double>(dyads)/hits:0.0;
    require(share<0.30,
            "Synth chord stabs must remain occasional rather than dominating the role");
}

void testStylesDiffer(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);

    struct Stats {
        double hits=0.0;
        double avgPitch=0.0;
        double offbeatShare=0.0;
        double dyadShare=0.0;
    };

    auto measure=[&](StyleId style){
        long long hits=0,pitchSum=0,offbeats=0,dyads=0;
        for(unsigned seed=1;seed<=256;++seed){
            SynthSettings s{}; s.style=style;
            const auto q=SynthBrain::generate(
                g,b,p,s,
                40000u+seed+
                static_cast<unsigned>(static_cast<int>(style))*10000u);
            for(int i=0;i<q.usedSteps();++i){
                if(q.steps[i].noteCount<=0) continue;
                ++hits;
                pitchSum+=q.steps[i].notes[0].pitch;
                if((i%2)!=0) ++offbeats;
                if(q.steps[i].noteCount>1) ++dyads;
            }
        }
        Stats out{};
        out.hits=static_cast<double>(hits)/256.0;
        const double hc=std::max<long long>(1,hits);
        out.avgPitch=pitchSum/hc;
        out.offbeatShare=offbeats/hc;
        out.dyadShare=dyads/hc;
        return out;
    };

    const auto ndh=measure(StyleId::NDHIndustrial);
    const auto dark=measure(StyleId::DarkRockGothic);
    const auto thrash=measure(StyleId::Thrash);
    const auto groove=measure(StyleId::Groove);
    const auto death=measure(StyleId::Death);
    const auto melodicDeath=measure(StyleId::MelodicDeath);
    const auto metalcore=measure(StyleId::Metalcore);
    const auto nu=measure(StyleId::NuMetal);
    const auto doom=measure(StyleId::Doom);
    const auto djent=measure(StyleId::DjentProgressive);

    require(dark.avgPitch>ndh.avgPitch+2.0,
            "Dark Rock/Gothic synth should occupy a higher melodic register than NDH");
    require(melodicDeath.avgPitch>nu.avgPitch+3.0,
            "Melodic Death synth must retain a higher melodic register than Nu Metal");
    require(djent.offbeatShare>doom.offbeatShare+0.05,
            "Djent/Progressive synth must retain more syncopated offbeat activity than Doom");
    require(groove.hits>thrash.hits+7.0,
            "Groove Metal synth must remain more active than the sparse Thrash support role");
    require(metalcore.dyadShare>death.dyadShare+0.05,
            "Metalcore synth must retain more chord-stab support than Death Metal");
}
}

int main(){
    testDeterministicCompactGestureScaleSafe();
    testActivityRaisesHitCount();
    testMovementMateriallyIncreasesPitchTravel();
    testSyncopationRaisesOffbeatShare();
    testSustainRaisesLongNoteShare();
    testPitchAndLengthControlsDoNotRewriteRhythm();
    testRepetitionPreservesMotifPitchIdentity();
    testHarmonicFollowTargetsPadChordTones();
    testShortChordStabsOccur();
    testStylesDiffer();
    std::cout<<"Midiator Synth Brain tests: PASS\n";
    return 0;
}
