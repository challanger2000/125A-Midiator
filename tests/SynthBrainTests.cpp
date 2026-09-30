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

void testDeterministicMonophonicScaleSafe(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);
    SynthSettings s{};
    const auto a=SynthBrain::generate(g,b,p,s,123u);
    const auto z=SynthBrain::generate(g,b,p,s,123u);
    require(a.bars==z.bars,"Synth Brain must be deterministic");
    int hits=0;
    for(int i=0;i<a.usedSteps();++i){
        require(a.steps[i].noteCount==z.steps[i].noteCount,"Synth topology must be deterministic");
        require(a.steps[i].noteCount<=1,"Synth foundation must remain monophonic");
        if(a.steps[i].noteCount<=0) continue;
        ++hits;
        const auto& n=a.steps[i].notes[0];
        require(n.pitch==z.steps[i].notes[0].pitch &&
                n.velocity==z.steps[i].notes[0].velocity &&
                n.lengthSteps==z.steps[i].notes[0].lengthSteps,
                "Synth notes must be deterministic");
        require(n.pitch>=48&&n.pitch<=96,"Synth pitch must remain in melodic register");
        require(RiffEngine::isScaleTone(n.pitch,s.rootPitchClass,s.scale),
                "Synth must remain scale-safe");
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
            if((a.steps[i].noteCount>0)!=(z.steps[i].noteCount>0)) return false;
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

void testStylesDiffer(){
    Phrase g{},b{}; PadPhrase p{}; makeContext(g,b,p);
    auto avgPitch=[&](StyleId style){
        long long count=0,sum=0;
        for(unsigned seed=1;seed<=128;++seed){
            SynthSettings s{}; s.style=style;
            const auto q=SynthBrain::generate(g,b,p,s,40000u+seed);
            for(int i=0;i<q.usedSteps();++i) if(q.steps[i].noteCount>0){
                sum+=q.steps[i].notes[0].pitch; ++count;
            }
        }
        return count?static_cast<double>(sum)/count:0.0;
    };
    const auto ndh=avgPitch(StyleId::NDHIndustrial);
    const auto dark=avgPitch(StyleId::DarkRockGothic);
    require(dark>ndh+2.0,"Dark Rock/Gothic synth should occupy a higher melodic register than NDH");
}
}

int main(){
    testDeterministicMonophonicScaleSafe();
    testActivityRaisesHitCount();
    testSustainRaisesLongNoteShare();
    testPitchAndLengthControlsDoNotRewriteRhythm();
    testRepetitionPreservesMotifPitchIdentity();
    testStylesDiffer();
    std::cout<<"Midiator Synth Brain tests: PASS\n";
    return 0;
}
