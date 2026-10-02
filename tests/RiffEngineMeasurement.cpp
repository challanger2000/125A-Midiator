#include "RiffEngine.h"
#include "BassBrain.h"
#include "DrumBrain.h"
#include "PadBrain.h"
#include "SynthBrain.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <vector>
#include <iostream>

using namespace midiator;

static const char* pitchClassName(int pc) {
    static const char* names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    pc %= 12;
    if (pc < 0) pc += 12;
    return names[pc];
}

static void printPhraseGrid(const Phrase& p, const char* title) {
    std::cout << "\n" << title << "\n";
    std::cout << "Legend: P=palm/mute-like, O=open-like, 5=power chord\n";

    for (int bar = 0; bar < p.bars; ++bar) {
        std::cout << "Bar " << (bar + 1) << ": ";
        for (int i = 0; i < 16; ++i) {
            const auto& step = p.steps[bar * 16 + i];
            if (step.noteCount <= 0) {
                std::cout << "[----]";
                continue;
            }

            const auto& n = step.notes[0];
            const int octave = (n.pitch / 12) - 1;
            std::cout << "[" << pitchClassName(n.pitch) << octave
                      << (n.velocity <= 40 ? "P" : "O")
                      << (step.noteCount == 2 ? "5" : " ")
                      << "]";
        }
        std::cout << "\n";
    }
}

static int countHits(const Phrase& p) {
    int hits = 0;
    for (int i = 0; i < p.usedSteps(); ++i)
        hits += p.steps[i].noteCount > 0 ? 1 : 0;
    return hits;
}

static int countDiffSteps(const Phrase& a, const Phrase& b) {
    int d = 0;
    const int n = std::min(a.usedSteps(), b.usedSteps());
    for (int i = 0; i < n; ++i)
        if (!(a.steps[i] == b.steps[i]))
            ++d;
    return d + std::abs(a.usedSteps() - b.usedSteps());
}

static double onsetJaccard(const Phrase& a, const Phrase& b) {
    const int n = std::min(a.usedSteps(), b.usedSteps());
    int intersection = 0;
    int unionCount = 0;
    for (int i = 0; i < n; ++i) {
        const bool ha = a.steps[i].noteCount > 0;
        const bool hb = b.steps[i].noteCount > 0;
        if (ha || hb) ++unionCount;
        if (ha && hb) ++intersection;
    }
    return unionCount > 0
        ? static_cast<double>(intersection) / static_cast<double>(unionCount)
        : 1.0;
}

static int longestHitRun(const Phrase& p) {
    int longest = 0;
    int current = 0;
    for (int i = 0; i < p.usedSteps(); ++i) {
        if (p.steps[i].noteCount > 0) {
            ++current;
            longest = std::max(longest, current);
        } else {
            current = 0;
        }
    }
    return longest;
}


struct SweepMetrics {
    double hits = 0.0;
    double rootShare = 0.0;
    double muteShare = 0.0;
    double chordShare = 0.0;
    double longNoteShare = 0.0;
    double distinctPitchClasses = 0.0;
    double avgAbsJump = 0.0;
    double adjacentBarJaccard = 0.0;
    double fast4Share = 0.0;
    double longRestShare = 0.0;
};

static SweepMetrics measureSettings(const GeneratorSettings& settings,
                                    unsigned seedBase,
                                    int samples = 256) {
    SweepMetrics m{};
    long long totalHits = 0;
    long long rootNotes = 0;
    long long muteNotes = 0;
    long long chordHits = 0;
    long long longNotes = 0;
    long long jumpCount = 0;
    long long jumpSum = 0;
    long long distinctPcTotal = 0;
    long long barPairs = 0;
    double barJaccardSum = 0.0;
    int fast4Phrases = 0;
    int longRestPhrases = 0;

    for (int sidx = 0; sidx < samples; ++sidx) {
        const auto p = RiffEngine::generate(settings, seedBase + static_cast<unsigned>(sidx));
        bool pcs[12] = {};
        int previousPitch = -1;
        int run = 0;
        int longestRun = 0;
        int rest = 0;
        int longestRest = 0;

        for (int i = 0; i < p.usedSteps(); ++i) {
            const auto& st = p.steps[i];
            if (st.noteCount <= 0) {
                run = 0;
                ++rest;
                longestRest = std::max(longestRest, rest);
                continue;
            }

            rest = 0;
            ++run;
            longestRun = std::max(longestRun, run);
            ++totalHits;
            if (st.noteCount > 1) ++chordHits;

            const auto& n = st.notes[0];
            const int pc = (n.pitch % 12 + 12) % 12;
            pcs[pc] = true;
            if (pc == settings.rootPitchClass) ++rootNotes;
            if (n.velocity <= 40) ++muteNotes;
            if (n.lengthSteps > 1) ++longNotes;

            if (previousPitch >= 0) {
                jumpSum += std::abs(n.pitch - previousPitch);
                ++jumpCount;
            }
            previousPitch = n.pitch;
        }

        if (longestRun >= 4) ++fast4Phrases;
        if (longestRest >= 4) ++longRestPhrases;

        int distinct = 0;
        for (bool used : pcs) distinct += used ? 1 : 0;
        distinctPcTotal += distinct;

        for (int bar = 1; bar < p.bars; ++bar) {
            int intersection = 0;
            int unionCount = 0;
            for (int i = 0; i < kStepsPerBar; ++i) {
                const bool a = p.steps[(bar - 1) * kStepsPerBar + i].noteCount > 0;
                const bool b = p.steps[bar * kStepsPerBar + i].noteCount > 0;
                if (a || b) ++unionCount;
                if (a && b) ++intersection;
            }
            barJaccardSum += unionCount > 0
                ? static_cast<double>(intersection) / unionCount : 1.0;
            ++barPairs;
        }
    }

    const double phraseCount = static_cast<double>(samples);
    const double hitCount = std::max(1.0, static_cast<double>(totalHits));
    m.hits = totalHits / phraseCount;
    m.rootShare = rootNotes / hitCount;
    m.muteShare = muteNotes / hitCount;
    m.chordShare = chordHits / hitCount;
    m.longNoteShare = longNotes / hitCount;
    m.distinctPitchClasses = distinctPcTotal / phraseCount;
    m.avgAbsJump = jumpCount > 0 ? static_cast<double>(jumpSum) / jumpCount : 0.0;
    m.adjacentBarJaccard = barPairs > 0 ? barJaccardSum / barPairs : 1.0;
    m.fast4Share = static_cast<double>(fast4Phrases) / phraseCount;
    m.longRestShare = static_cast<double>(longRestPhrases) / phraseCount;
    return m;
}

static void printSweepLine(const char* label, double value, const SweepMetrics& m) {
    std::cout << label << " " << std::setw(5) << value * 100.0 << "%:"
              << " hits=" << m.hits
              << " root=" << m.rootShare * 100.0 << "%"
              << " mute=" << m.muteShare * 100.0 << "%"
              << " chords=" << m.chordShare * 100.0 << "%"
              << " longNotes=" << m.longNoteShare * 100.0 << "%"
              << " pitchClasses=" << m.distinctPitchClasses
              << " avgJump=" << m.avgAbsJump
              << " barJaccard=" << m.adjacentBarJaccard * 100.0 << "%"
              << " fast4=" << m.fast4Share * 100.0 << "%"
              << " longRest=" << m.longRestShare * 100.0 << "%"
              << "\n";
}


struct BassSweepMetrics {
    double hits = 0.0;
    double rootShare = 0.0;
    double guitarCoincidence = 0.0;
    double longNoteShare = 0.0;
    double octaveShare = 0.0;
    double avgAbsJump = 0.0;
    double avgPitch = 0.0;
};

static BassSweepMetrics measureBass(const Phrase& guitar,
                                    const BassSettings& settings,
                                    unsigned seedBase,
                                    int samples = 256) {
    BassSweepMetrics m{};
    long long hits = 0, roots = 0, coincident = 0, longNotes = 0;
    long long octaves = 0, jumps = 0, jumpSum = 0, pitchSum = 0;

    for (int sidx = 0; sidx < samples; ++sidx) {
        const auto bass = BassBrain::generate(
            guitar, settings, seedBase + static_cast<unsigned>(sidx));
        int previous = -1;

        for (int i = 0; i < bass.usedSteps(); ++i) {
            const auto& st = bass.steps[i];
            if (st.noteCount <= 0) continue;

            ++hits;
            const auto& n = st.notes[0];
            const int pc = (n.pitch % 12 + 12) % 12;
            if (pc == settings.rootPitchClass) ++roots;
            if (guitar.steps[i].noteCount > 0) ++coincident;
            if (n.lengthSteps > 1) ++longNotes;
            if (n.pitch >= 40) ++octaves;
            pitchSum += n.pitch;

            if (previous >= 0) {
                jumpSum += std::abs(n.pitch - previous);
                ++jumps;
            }
            previous = n.pitch;
        }
    }

    const double h = std::max(1.0, static_cast<double>(hits));
    m.hits = hits / static_cast<double>(samples);
    m.rootShare = roots / h;
    m.guitarCoincidence = coincident / h;
    m.longNoteShare = longNotes / h;
    m.octaveShare = octaves / h;
    m.avgAbsJump = jumps > 0 ? static_cast<double>(jumpSum) / jumps : 0.0;
    m.avgPitch = pitchSum / h;
    return m;
}

static void printBassSweepLine(const char* label, double value, const BassSweepMetrics& m) {
    std::cout << label << " " << std::setw(5) << value * 100.0 << "%:"
              << " hits=" << m.hits
              << " root=" << m.rootShare * 100.0 << "%"
              << " guitarLock=" << m.guitarCoincidence * 100.0 << "%"
              << " longNotes=" << m.longNoteShare * 100.0 << "%"
              << " upperRegister=" << m.octaveShare * 100.0 << "%"
              << " avgJump=" << m.avgAbsJump
              << " avgPitch=" << m.avgPitch
              << "\n";
}


struct DrumSweepMetrics {
    double hits = 0.0;
    double kicks = 0.0;
    double kickLock = 0.0;
    double snares = 0.0;
    double hats = 0.0;
    double ghosts = 0.0;
    double toms = 0.0;
    double crashes = 0.0;
    double avgVelocity = 0.0;
    double velocityStdDev = 0.0;
};

static DrumSweepMetrics measureDrums(const Phrase& guitar,
                                     const Phrase& bass,
                                     const DrumSettings& settings,
                                     unsigned seedBase,
                                     int samples = 256) {
    DrumSweepMetrics m{};
    long long hits=0,kicks=0,locked=0,snares=0,hats=0,ghosts=0,toms=0,crashes=0,vel=0,velSq=0;
    for(int sidx=0;sidx<samples;++sidx){
        const auto d=DrumBrain::generate(guitar,bass,settings,seedBase+static_cast<unsigned>(sidx));
        for(int i=0;i<d.usedSteps();++i){
            const bool contextHit =
                (i<guitar.usedSteps() && guitar.steps[i].noteCount>0) ||
                (i<bass.usedSteps() && bass.steps[i].noteCount>0);
            for(int n=0;n<d.steps[i].hitCount;++n){
                const auto& hit=d.steps[i].hits[n];
                ++hits; vel+=hit.velocity; velSq+=hit.velocity*hit.velocity;
                switch(hit.voice){
                    case DrumVoice::Kick: ++kicks; if(contextHit) ++locked; break;
                    case DrumVoice::Snare: ++snares; break;
                    case DrumVoice::ClosedHat:
                    case DrumVoice::OpenHat: ++hats; break;
                    case DrumVoice::GhostSnare: ++ghosts; break;
                    case DrumVoice::LowTom:
                    case DrumVoice::MidTom:
                    case DrumVoice::HighTom: ++toms; break;
                    case DrumVoice::Crash: ++crashes; break;
                    default: break;
                }
            }
        }
    }
    const double phrases=static_cast<double>(samples);
    const double hcount=std::max(1.0,static_cast<double>(hits));
    const double kcount=std::max(1.0,static_cast<double>(kicks));
    m.hits=hits/phrases; m.kicks=kicks/phrases; m.kickLock=locked/kcount;
    m.snares=snares/phrases; m.hats=hats/phrases; m.ghosts=ghosts/phrases;
    m.toms=toms/phrases; m.crashes=crashes/phrases; m.avgVelocity=vel/hcount;
    {
        const double mean = m.avgVelocity;
        const double variance = static_cast<double>(velSq) / hcount - mean * mean;
        m.velocityStdDev = variance > 0.0 ? std::sqrt(variance) : 0.0;
    }
    return m;
}

static void printDrumSweepLine(const char* label,double value,const DrumSweepMetrics& m){
    std::cout<<label<<" "<<std::setw(5)<<value*100.0<<"%:"
             <<" hits="<<m.hits<<" kicks="<<m.kicks
             <<" kickLock="<<m.kickLock*100.0<<"%"
             <<" snare="<<m.snares<<" hats="<<m.hats
             <<" ghost="<<m.ghosts<<" toms="<<m.toms
             <<" crash="<<m.crashes<<" avgVel="<<m.avgVelocity
             <<" velStd="<<m.velocityStdDev<<"\n";
}

static double measureSectionFillActivity(const Phrase& guitar,
                                         const Phrase& bass,
                                         const DrumSettings& settings,
                                         int targetBar,
                                         unsigned seedBase,
                                         int samples = 256) {
    long long activity = 0;
    for (int sidx = 0; sidx < samples; ++sidx) {
        const auto d = DrumBrain::generate(
            guitar, bass, settings, seedBase + static_cast<unsigned>(sidx));
        if (targetBar < 0 || targetBar >= d.bars)
            continue;
        for (int local = 12; local < 16; ++local) {
            const auto& st = d.steps[targetBar * kStepsPerBar + local];
            for (int n = 0; n < st.hitCount; ++n) {
                const auto v = st.hits[n].voice;
                if (v == DrumVoice::Kick || v == DrumVoice::Snare ||
                    v == DrumVoice::LowTom || v == DrumVoice::MidTom ||
                    v == DrumVoice::HighTom)
                    ++activity;
            }
        }
    }
    return static_cast<double>(activity) / std::max(1, samples);
}


struct PadSweepMetrics {
    double chords = 0.0;
    double avgVoices = 0.0;
    double avgDurationSteps = 0.0;
    double avgSpanSemitones = 0.0;
    double avgVoiceJump = 0.0;
    double octaveDoubleShare = 0.0;
    double colorVoiceShare = 0.0;
    double contextToneShare = 0.0;
};

static PadSweepMetrics measurePads(const Phrase& guitar,
                                   const Phrase& bass,
                                   const PadSettings& settings,
                                   unsigned seedBase,
                                   int samples = 256) {
    PadSweepMetrics m{};
    long long chords=0, voices=0, durations=0, octaveDoubles=0, colorVoices=0;
    long long spans=0, jumps=0, jumpSum=0, contextCompared=0, contextMatched=0;

    for(int sidx=0;sidx<samples;++sidx){
        const unsigned seed=seedBase+static_cast<unsigned>(sidx);
        const auto p=PadBrain::generate(guitar,bass,settings,seed);

        PadSettings neutralSettings=settings;
        neutralSettings.tension=0.0f;
        const auto neutral=PadBrain::generate(guitar,bass,neutralSettings,seed);

        std::array<int,kMaxPadVoices> previous{{-1,-1,-1,-1}};
        int activeChord=-1;

        for(int i=0;i<p.usedSteps();++i){
            const auto& st=p.steps[i];
            if(st.noteCount>0)
                activeChord=i;

            // Measure Guitar/Bass agreement against the currently active pad
            // harmony across the whole harmonic section, not only at onset.
            if(activeChord>=0){
                auto matchContext=[&](int pitch){
                    const int pc=(pitch%12+12)%12;
                    const auto& chord=p.steps[activeChord];
                    for(int cn=0;cn<chord.noteCount;++cn)
                        if(((chord.notes[cn].pitch%12+12)%12)==pc) return true;
                    return false;
                };
                if(i<guitar.usedSteps() && guitar.steps[i].noteCount>0){
                    ++contextCompared;
                    if(matchContext(guitar.steps[i].notes[0].pitch)) ++contextMatched;
                }
                if(i<bass.usedSteps() && bass.steps[i].noteCount>0){
                    ++contextCompared;
                    if(matchContext(bass.steps[i].notes[0].pitch)) ++contextMatched;
                }
            }

            if(st.noteCount<=0)
                continue;

            ++chords;
            voices+=st.noteCount;
            if(st.noteCount==4) ++octaveDoubles;

            // Isolate the effect of Tension against the exact same seed,
            // context-follow choice and all other settings with tension=0.
            const auto& baseline=neutral.steps[i];
            if(baseline.noteCount>0){
                unsigned actualMask=0, baseMask=0;
                for(int n=0;n<st.noteCount;++n)
                    actualMask |= 1u << ((st.notes[n].pitch%12+12)%12);
                for(int n=0;n<baseline.noteCount;++n)
                    baseMask |= 1u << ((baseline.notes[n].pitch%12+12)%12);
                if(actualMask!=baseMask)
                    ++colorVoices;
            }

            int lo=127,hi=0;
            for(int n=0;n<st.noteCount;++n){
                const auto& note=st.notes[n];
                durations+=note.lengthSteps;
                lo=std::min(lo,note.pitch); hi=std::max(hi,note.pitch);
                if(previous[n]>=0){
                    jumpSum+=std::abs(note.pitch-previous[n]);
                    ++jumps;
                }
                previous[n]=note.pitch;
            }
            spans+=hi-lo;
        }
    }

    const double phraseCount=static_cast<double>(samples);
    const double chordCount=std::max(1.0,static_cast<double>(chords));
    const double voiceCount=std::max(1.0,static_cast<double>(voices));
    m.chords=chords/phraseCount;
    m.avgVoices=voices/chordCount;
    m.avgDurationSteps=durations/voiceCount;
    m.avgSpanSemitones=spans/chordCount;
    m.avgVoiceJump=jumps>0?static_cast<double>(jumpSum)/jumps:0.0;
    m.octaveDoubleShare=octaveDoubles/chordCount;
    m.colorVoiceShare=colorVoices/chordCount;
    m.contextToneShare=contextCompared?static_cast<double>(contextMatched)/contextCompared:0.0;
    return m;
}

static void printPadSweepLine(const char* label,double value,const PadSweepMetrics& m){
    std::cout<<label<<" "<<std::setw(5)<<value*100.0<<"%:"
             <<" chords="<<m.chords
             <<" voices="<<m.avgVoices
             <<" duration="<<m.avgDurationSteps
             <<" span="<<m.avgSpanSemitones
             <<" voiceJump="<<m.avgVoiceJump
             <<" octaveDouble="<<m.octaveDoubleShare*100.0<<"%"
             <<" colorVoice="<<m.colorVoiceShare*100.0<<"%"
             <<" contextTone="<<m.contextToneShare*100.0<<"%\n";
}


struct SynthSweepMetrics {
    double hits=0.0, longShare=0.0, offbeatShare=0.0, avgJump=0.0, avgPitch=0.0;
    double motifPitchAgreement=0.0;
    double padChordToneShare=0.0;
    double dyadShare=0.0;
};
static SynthSweepMetrics measureSynth(const Phrase& guitar,const Phrase& bass,const PadPhrase& pads,
                                      const SynthSettings& s,unsigned seedBase,int samples=256){
    SynthSweepMetrics m{}; long long hits=0,longs=0,off=0,jumps=0,jumpSum=0,pitchSum=0;
    long long motifCompared=0,motifMatched=0,padCompared=0,padMatched=0,dyads=0;
    for(int si=0;si<samples;++si){
        const auto p=SynthBrain::generate(guitar,bass,pads,s,seedBase+static_cast<unsigned>(si));
        int prev=-1;
        int activeChord=-1;
        for(int i=0;i<p.usedSteps();++i){
            if(i<pads.usedSteps() && pads.steps[i].noteCount>0)
                activeChord=i;
            const auto& st=p.steps[i]; if(st.noteCount<=0) continue;
            ++hits; if(st.noteCount==2) ++dyads;
            const auto& n=st.notes[0]; pitchSum+=n.pitch;
            if(n.lengthSteps>1) ++longs; if((i%2)!=0) ++off;
            if(prev>=0){jumpSum+=std::abs(n.pitch-prev);++jumps;} prev=n.pitch;
            if(activeChord>=0){
                const auto& chord=pads.steps[activeChord];
                for(int sn=0;sn<st.noteCount;++sn){
                    ++padCompared;
                    const int pc=(st.notes[sn].pitch%12+12)%12;
                    for(int cn=0;cn<chord.noteCount;++cn){
                        if(((chord.notes[cn].pitch%12+12)%12)==pc){
                            ++padMatched; break;
                        }
                    }
                }
            }
            if(i>=16){
                const int ref=i%16;
                if(p.steps[ref].noteCount>0){
                    ++motifCompared;
                    const int a=(n.pitch%12+12)%12;
                    const int b=(p.steps[ref].notes[0].pitch%12+12)%12;
                    if(a==b) ++motifMatched;
                }
            }
        }
    }
    const double hc=std::max(1.0,static_cast<double>(hits));
    m.hits=hits/static_cast<double>(samples); m.longShare=longs/hc; m.offbeatShare=off/hc;
    m.avgJump=jumps?static_cast<double>(jumpSum)/jumps:0.0; m.avgPitch=pitchSum/hc;
    m.motifPitchAgreement=motifCompared?static_cast<double>(motifMatched)/motifCompared:0.0;
    m.padChordToneShare=padCompared?static_cast<double>(padMatched)/padCompared:0.0;
    m.dyadShare=dyads/hc;
    return m;
}
static void printSynthSweepLine(const char* label,double value,const SynthSweepMetrics& m){
    std::cout<<label<<" "<<std::setw(5)<<value*100.0<<"%:"
             <<" hits="<<m.hits<<" long="<<m.longShare*100.0<<"%"
             <<" offbeat="<<m.offbeatShare*100.0<<"%"
             <<" avgJump="<<m.avgJump<<" avgPitch="<<m.avgPitch
             <<" motifMatch="<<m.motifPitchAgreement*100.0<<"%"
             <<" chordTone="<<m.padChordToneShare*100.0<<"%"
             <<" dyad="<<m.dyadShare*100.0<<"%\n";
}



static uint32_t benchmarkNextSeed(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x ? x : 0x125A2026u;
}

struct NewRiffBenchResult {
    double avgAttempts = 0.0;
    int p95Attempts = 0;
    int p99Attempts = 0;
    int maxAttempts = 0;
    double full32Share = 0.0;
    double p50Us = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
    double maxUs = 0.0;
};

static NewRiffBenchResult benchmarkNewRiffPath(StyleId style,
                                               int samples = 256) {
    std::vector<int> attempts;
    std::vector<double> micros;
    attempts.reserve(static_cast<size_t>(samples));
    micros.reserve(static_cast<size_t>(samples));

    long long attemptSum = 0;
    int full32 = 0;

    GeneratorSettings settings{};
    settings.bars = 4;
    settings.rootPitchClass = 9;
    settings.scale = ScaleId::Phrygian;
    settings.style = style;

    for (int sample = 0; sample < samples; ++sample) {
        uint32_t stateSeed = 0x600D0000u + static_cast<uint32_t>(sample * 977 + static_cast<int>(style) * 131);
        const auto previous = RiffEngine::generate(settings, stateSeed);

        const int requiredDifference = std::max(6, previous.usedSteps() / 3);
        Phrase bestCandidate{};
        Phrase candidate{};
        double bestJaccard = 2.0;
        int bestDifference = -1;
        int usedAttempts = 0;

        const auto start = std::chrono::steady_clock::now();

        for (int attempt = 0; attempt < 32; ++attempt) {
            ++usedAttempts;
            stateSeed = benchmarkNextSeed(stateSeed);
            candidate = RiffEngine::generate(settings, stateSeed);

            const int difference = countDiffSteps(previous, candidate);
            const double jaccard = onsetJaccard(previous, candidate);

            if (jaccard < bestJaccard ||
                (std::abs(jaccard - bestJaccard) < 1e-9 &&
                 difference > bestDifference)) {
                bestCandidate = candidate;
                bestJaccard = jaccard;
                bestDifference = difference;
            }

            if (difference >= requiredDifference && jaccard <= 0.48)
                break;
        }

        // Mirror the processor's downstream regeneration cascade so the
        // timing represents a complete five-role NEW RIFF operation.
        BassSettings bs{};
        bs.rootPitchClass = settings.rootPitchClass;
        bs.scale = settings.scale;
        bs.style = settings.style;
        const auto bass = BassBrain::generate(
            bestCandidate, bs, stateSeed ^ 0xB4552026u);

        DrumSettings ds{};
        ds.style = settings.style;
        const auto drums = DrumBrain::generate(
            bestCandidate, bass, ds, stateSeed ^ 0xD12A2026u);

        PadSettings ps{};
        ps.rootPitchClass = settings.rootPitchClass;
        ps.scale = settings.scale;
        ps.style = settings.style;
        const auto pads = PadBrain::generate(
            bestCandidate, bass, ps, stateSeed ^ 0x50414426u);

        SynthSettings ss{};
        ss.rootPitchClass = settings.rootPitchClass;
        ss.scale = settings.scale;
        ss.style = settings.style;
        const auto synth = SynthBrain::generate(
            bestCandidate, bass, pads, ss, stateSeed ^ 0x53594E26u);

        // Prevent an optimizing compiler from proving the generated roles dead.
        volatile int sink = bass.usedSteps() + drums.usedSteps() +
                            pads.usedSteps() + synth.usedSteps();
        (void)sink;

        const auto stop = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(stop - start).count();

        attempts.push_back(usedAttempts);
        micros.push_back(us);
        attemptSum += usedAttempts;
        if (usedAttempts == 32)
            ++full32;
    }

    std::sort(attempts.begin(), attempts.end());
    std::sort(micros.begin(), micros.end());

    auto percentileIndex = [samples](double p) {
        const int idx = static_cast<int>(std::ceil(p * samples)) - 1;
        return std::clamp(idx, 0, samples - 1);
    };

    NewRiffBenchResult r{};
    r.avgAttempts = static_cast<double>(attemptSum) / samples;
    r.p95Attempts = attempts[static_cast<size_t>(percentileIndex(0.95))];
    r.p99Attempts = attempts[static_cast<size_t>(percentileIndex(0.99))];
    r.maxAttempts = attempts.back();
    r.full32Share = static_cast<double>(full32) / samples;
    r.p50Us = micros[static_cast<size_t>(percentileIndex(0.50))];
    r.p95Us = micros[static_cast<size_t>(percentileIndex(0.95))];
    r.p99Us = micros[static_cast<size_t>(percentileIndex(0.99))];
    r.maxUs = micros.back();
    return r;
}

static void printNewRiffBenchmarks() {
    static const char* names[] = {
        "NDH / Industrial", "Dark Rock / Gothic", "Heavy Industrial"
    };

    std::cout << "\nNEW RIFF full-path diagnostics (256 operations per style)\n";
    std::cout << "---------------------------------------------------------\n";
    for (int i = 0; i < static_cast<int>(StyleId::Count); ++i) {
        const auto r = benchmarkNewRiffPath(static_cast<StyleId>(i));
        std::cout << names[i]
                  << ": attempts avg=" << r.avgAttempts
                  << " p95=" << r.p95Attempts
                  << " p99=" << r.p99Attempts
                  << " max=" << r.maxAttempts
                  << " full32=" << r.full32Share * 100.0 << "%"
                  << " time_us p50=" << r.p50Us
                  << " p95=" << r.p95Us
                  << " p99=" << r.p99Us
                  << " max=" << r.maxUs
                  << "\n";
    }
}

static uint64_t fnvMix(uint64_t h, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        h ^= (value >> (i * 8)) & 0xffu;
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t hashPhrase(const Phrase& p) {
    uint64_t h = 1469598103934665603ull;
    h = fnvMix(h, static_cast<uint64_t>(p.bars));
    h = fnvMix(h, static_cast<uint64_t>(p.usedSteps()));
    for (int i = 0; i < p.usedSteps(); ++i) {
        const auto& st = p.steps[i];
        h = fnvMix(h, static_cast<uint64_t>(st.noteCount));
        for (int n = 0; n < st.noteCount; ++n) {
            h = fnvMix(h, static_cast<uint64_t>(st.notes[n].pitch));
            h = fnvMix(h, static_cast<uint64_t>(st.notes[n].velocity));
            h = fnvMix(h, static_cast<uint64_t>(st.notes[n].lengthSteps));
        }
    }
    return h;
}

static uint64_t hashDrums(const DrumPhrase& p) {
    uint64_t h = 1469598103934665603ull;
    h = fnvMix(h, static_cast<uint64_t>(p.bars));
    h = fnvMix(h, static_cast<uint64_t>(p.usedSteps()));
    for (int i = 0; i < p.usedSteps(); ++i) {
        const auto& st = p.steps[i];
        h = fnvMix(h, static_cast<uint64_t>(st.hitCount));
        for (int n = 0; n < st.hitCount; ++n) {
            h = fnvMix(h, static_cast<uint64_t>(st.hits[n].voice));
            h = fnvMix(h, static_cast<uint64_t>(st.hits[n].velocity));
        }
    }
    return h;
}

static uint64_t hashPads(const PadPhrase& p) {
    uint64_t h = 1469598103934665603ull;
    h = fnvMix(h, static_cast<uint64_t>(p.bars));
    h = fnvMix(h, static_cast<uint64_t>(p.usedSteps()));
    for (int i = 0; i < p.usedSteps(); ++i) {
        const auto& st = p.steps[i];
        h = fnvMix(h, static_cast<uint64_t>(st.noteCount));
        for (int n = 0; n < st.noteCount; ++n) {
            h = fnvMix(h, static_cast<uint64_t>(st.notes[n].pitch));
            h = fnvMix(h, static_cast<uint64_t>(st.notes[n].velocity));
            h = fnvMix(h, static_cast<uint64_t>(st.notes[n].lengthSteps));
        }
    }
    return h;
}

static uint64_t arrangementHash(uint64_t guitar,
                                uint64_t bass,
                                uint64_t drums,
                                uint64_t pads,
                                uint64_t synth) {
    uint64_t h = 1469598103934665603ull;
    h = fnvMix(h, guitar);
    h = fnvMix(h, bass);
    h = fnvMix(h, drums);
    h = fnvMix(h, pads);
    h = fnvMix(h, synth);
    return h;
}

static void printGoldenArrangementFingerprint() {
    constexpr uint32_t seed = 0x125A5EEDu;

    GeneratorSettings gs{};
    gs.bars = 4;
    gs.rootPitchClass = 9;
    gs.scale = ScaleId::Phrygian;
    gs.style = StyleId::NDHIndustrial;

    const auto guitar = RiffEngine::generate(gs, seed);

    BassSettings bs{};
    bs.rootPitchClass = gs.rootPitchClass;
    bs.scale = gs.scale;
    bs.style = gs.style;
    const auto bass = BassBrain::generate(guitar, bs, seed ^ 0xB4552026u);

    DrumSettings ds{};
    ds.style = gs.style;
    const auto drums = DrumBrain::generate(guitar, bass, ds, seed ^ 0xD12A2026u);

    PadSettings ps{};
    ps.rootPitchClass = gs.rootPitchClass;
    ps.scale = gs.scale;
    ps.style = gs.style;
    const auto pads = PadBrain::generate(guitar, bass, ps, seed ^ 0x50414426u);

    SynthSettings ss{};
    ss.rootPitchClass = gs.rootPitchClass;
    ss.scale = gs.scale;
    ss.style = gs.style;
    const auto synth = SynthBrain::generate(
        guitar, bass, pads, ss, seed ^ 0x53594E26u);

    const auto guitarHash = hashPhrase(guitar);
    const auto bassHash = hashPhrase(bass);
    const auto drumHash = hashDrums(drums);
    const auto padHash = hashPads(pads);
    const auto synthHash = hashPhrase(synth);

    std::cout << "\nGolden arrangement fingerprint\n";
    std::cout << "------------------------------\n";
    std::cout << std::hex << std::showbase;
    std::cout << "Guitar=" << guitarHash << "\n";
    std::cout << "Bass=" << bassHash << "\n";
    std::cout << "Drums=" << drumHash << "\n";
    std::cout << "Pad=" << padHash << "\n";
    std::cout << "Synth=" << synthHash << "\n";
    std::cout << "Arrangement="
              << arrangementHash(guitarHash, bassHash, drumHash, padHash, synthHash)
              << "\n";
    std::cout << std::dec << std::noshowbase;
}

int main() {
    std::cout << "125A Midiator measurement report\n";
    std::cout << "================================\n";

    GeneratorSettings s{};
    s.rootPitchClass = 9;
    s.scale = ScaleId::Phrygian;
    s.bars = 4;

    long long phrases = 0;
    long long hits = 0;
    long long primaryNotes = 0;
    long long rootNotes = 0;
    long long offbeats = 0;
    long long powerChords = 0;
    long long muteLike = 0;
    long long openLike = 0;
    long long vel127 = 0;
    long long repeatedBarPairs = 0;
    long long comparedBarPairs = 0;
    long long completelyIdenticalBarPairs = 0;
    double onsetJaccardSum = 0.0;
    long long sharedOnsets = 0;
    long long sharedPitchMatches = 0;
    long long phrasesWithLongRest = 0;
    long long phrasesWithLongHitRun = 0;
    long long distinctPrimaryPitchClassesTotal = 0;

    for (unsigned seed = 1; seed <= 1000; ++seed) {
        const auto p = RiffEngine::generate(s, seed);
        ++phrases;
        int longestRest = 0;
        int currentRest = 0;
        int longestHitRun = 0;
        int currentHitRun = 0;
        bool pcs[12] = {};

        for (int i = 0; i < p.usedSteps(); ++i) {
            const auto& st = p.steps[i];
            if (st.noteCount <= 0) {
                ++currentRest;
                longestRest = std::max(longestRest, currentRest);
                currentHitRun = 0;
                continue;
            }

            currentRest = 0;
            ++currentHitRun;
            longestHitRun = std::max(longestHitRun, currentHitRun);
            ++hits;
            if ((i % 4) != 0) ++offbeats;
            if (st.noteCount == 2) ++powerChords;

            const auto& n = st.notes[0];
            ++primaryNotes;
            pcs[(n.pitch % 12 + 12) % 12] = true;
            if ((n.pitch % 12 + 12) % 12 == s.rootPitchClass) ++rootNotes;
            if (n.velocity <= 40) ++muteLike;
            if (n.velocity >= 88) ++openLike;
            if (n.velocity == 127) ++vel127;
        }

        if (longestRest >= 4) ++phrasesWithLongRest;
        if (longestHitRun >= 8) ++phrasesWithLongHitRun;

        int distinctPcs = 0;
        for (bool usedPc : pcs) distinctPcs += usedPc ? 1 : 0;
        distinctPrimaryPitchClassesTotal += distinctPcs;

        for (int bar = 1; bar < p.bars; ++bar) {
            int sameSteps = 0;
            bool identical = true;
            int onsetIntersection = 0;
            int onsetUnion = 0;

            for (int sidx = 0; sidx < 16; ++sidx) {
                const auto& a = p.steps[(bar - 1) * 16 + sidx];
                const auto& b = p.steps[bar * 16 + sidx];

                if (a == b) {
                    ++sameSteps;
                } else {
                    identical = false;
                }

                const bool hitA = a.noteCount > 0;
                const bool hitB = b.noteCount > 0;
                if (hitA || hitB)
                    ++onsetUnion;
                if (hitA && hitB) {
                    ++onsetIntersection;
                    ++sharedOnsets;
                    if (a.notes[0].pitch == b.notes[0].pitch)
                        ++sharedPitchMatches;
                }
            }

            ++comparedBarPairs;
            if (sameSteps >= 10) ++repeatedBarPairs;
            if (identical) ++completelyIdenticalBarPairs;

            onsetJaccardSum += onsetUnion > 0
                ? static_cast<double>(onsetIntersection) / static_cast<double>(onsetUnion)
                : 1.0;
        }
    }


    auto pct = [](double x) { return x * 100.0; };

    std::cout << "\nStyle / NEW-RIFF diversity diagnostics\n";
    std::cout << "--------------------------------------\n";
    const char* styleNames[] = {"NDH / Industrial", "Dark Rock / Gothic", "Heavy Industrial"};
    for (int style = 0; style < static_cast<int>(StyleId::Count); ++style) {
        GeneratorSettings ds = s;
        ds.bars = 2;
        ds.style = static_cast<StyleId>(style);

        double rawJaccard = 0.0;
        double rawDiff = 0.0;
        int fast4 = 0;
        int fast6 = 0;
        int chordless = 0;
        constexpr int pairs = 512;

        for (int i = 0; i < pairs; ++i) {
            const auto a = RiffEngine::generate(ds, 200000u + static_cast<unsigned>(i * 2));
            const auto b = RiffEngine::generate(ds, 200001u + static_cast<unsigned>(i * 2));
            rawJaccard += onsetJaccard(a, b);
            rawDiff += countDiffSteps(a, b);

            const int run = longestHitRun(a);
            if (run >= 4) ++fast4;
            if (run >= 6) ++fast6;

            bool hasChord = false;
            for (int step = 0; step < a.usedSteps(); ++step)
                hasChord = hasChord || a.steps[step].noteCount > 1;
            if (!hasChord) ++chordless;
        }

        std::cout << styleNames[style] << ": avg independent-riff onset Jaccard "
                  << pct(rawJaccard / pairs)
                  << "%, avg changed steps "
                  << (rawDiff / pairs) << "/32"
                  << ", >=4x16 run " << pct(static_cast<double>(fast4) / pairs)
                  << "%, >=6x16 run " << pct(static_cast<double>(fast6) / pairs)
                  << "%, chordless " << pct(static_cast<double>(chordless) / pairs)
                  << "%\n";
    }

    std::cout << "\n8-bar macro-development diagnostics (512 phrases)\n";
    std::cout << "------------------------------------------------\n";
    {
        GeneratorSettings macro = s;
        macro.bars = 8;
        macro.repetition = 0.72f;
        macro.complexity = 0.42f;
        macro.density = 0.56f;

        auto barJaccard = [](const Phrase& p, int a, int b) {
            int intersection = 0;
            int unionCount = 0;
            for (int step = 0; step < kStepsPerBar; ++step) {
                const bool ah =
                    p.steps[a * kStepsPerBar + step].noteCount > 0;
                const bool bh =
                    p.steps[b * kStepsPerBar + step].noteCount > 0;
                if (ah || bh) ++unionCount;
                if (ah && bh) ++intersection;
            }
            return unionCount > 0
                ? static_cast<double>(intersection) / unionCount : 1.0;
        };

        double adjacent = 0.0;
        double sectionMirror = 0.0;
        double seedBarSimilarity = 0.0;
        int adjacentPairs = 0;
        int sectionPairs = 0;
        int seedPairs = 0;

        for (unsigned seed = 1; seed <= 512; ++seed) {
            const auto p = RiffEngine::generate(macro, 970000u + seed);
            for (int bar = 1; bar < 8; ++bar) {
                adjacent += barJaccard(p, bar - 1, bar);
                ++adjacentPairs;
                seedBarSimilarity += barJaccard(p, 0, bar);
                ++seedPairs;
            }
            for (int bar = 0; bar < 4; ++bar) {
                sectionMirror += barJaccard(p, bar, bar + 4);
                ++sectionPairs;
            }
        }

        std::cout << "Default 72% Repetition adjacent-bar onset Jaccard: "
                  << pct(adjacent / adjacentPairs) << "%\n";
        std::cout << "Bars 1-4 vs corresponding 5-8 onset Jaccard: "
                  << pct(sectionMirror / sectionPairs) << "%\n";
        std::cout << "Bar 1 vs later bars onset Jaccard: "
                  << pct(seedBarSimilarity / seedPairs) << "%\n";
    }

    std::cout << "\n16-bar macro-development diagnostics (512 phrases)\n";
    std::cout << "-------------------------------------------------\n";
    {
        GeneratorSettings macro = s;
        macro.bars = 16;
        macro.repetition = 0.72f;
        macro.complexity = 0.42f;
        macro.density = 0.56f;

        auto barJaccard = [](const Phrase& p, int a, int b) {
            int intersection = 0;
            int unionCount = 0;
            for (int step = 0; step < kStepsPerBar; ++step) {
                const bool ah =
                    p.steps[a * kStepsPerBar + step].noteCount > 0;
                const bool bh =
                    p.steps[b * kStepsPerBar + step].noteCount > 0;
                if (ah || bh) ++unionCount;
                if (ah && bh) ++intersection;
            }
            return unionCount > 0
                ? static_cast<double>(intersection) / unionCount : 1.0;
        };

        double adjacent = 0.0;
        double halfMirror = 0.0;
        double seedBarSimilarity = 0.0;
        int adjacentPairs = 0;
        int halfPairs = 0;
        int seedPairs = 0;

        for (unsigned seed = 1; seed <= 512; ++seed) {
            const auto p = RiffEngine::generate(macro, 980000u + seed);
            for (int bar = 1; bar < 16; ++bar) {
                adjacent += barJaccard(p, bar - 1, bar);
                ++adjacentPairs;
                seedBarSimilarity += barJaccard(p, 0, bar);
                ++seedPairs;
            }
            for (int bar = 0; bar < 8; ++bar) {
                halfMirror += barJaccard(p, bar, bar + 8);
                ++halfPairs;
            }
        }

        std::cout << "Default 72% Repetition adjacent-bar onset Jaccard (16 bars): "
                  << pct(adjacent / adjacentPairs) << "%\n";
        std::cout << "Bars 1-8 vs corresponding 9-16 onset Jaccard: "
                  << pct(halfMirror / halfPairs) << "%\n";
        std::cout << "Bar 1 vs later bars onset Jaccard (16 bars): "
                  << pct(seedBarSimilarity / seedPairs) << "%\n";
    }

    GeneratorSettings lowVarSettings = s;
    const auto base = RiffEngine::generate(lowVarSettings, 123456u);
    const auto var20 = RiffEngine::vary(base, lowVarSettings, 0.20f, 123457u);
    const auto var50 = RiffEngine::vary(base, lowVarSettings, 0.50f, 123458u);
    const auto var80 = RiffEngine::vary(base, lowVarSettings, 0.80f, 123459u);

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "Phrases sampled: " << phrases << "\n";
    std::cout << "Average hits / 4 bars: " << static_cast<double>(hits) / phrases << "\n";
    std::cout << "Root-note share: " << pct(static_cast<double>(rootNotes) / primaryNotes) << "%\n";
    std::cout << "Offbeat-hit share: " << pct(static_cast<double>(offbeats) / hits) << "%\n";
    std::cout << "Power-chord hit share: " << pct(static_cast<double>(powerChords) / hits) << "%\n";
    std::cout << "Mute-like primary-note share: " << pct(static_cast<double>(muteLike) / primaryNotes) << "%\n";
    std::cout << "Open-like primary-note share: " << pct(static_cast<double>(openLike) / primaryNotes) << "%\n";
    std::cout << "Velocity-127 count: " << vel127 << "\n";
    std::cout << "Average distinct primary pitch classes / phrase: "
              << static_cast<double>(distinctPrimaryPitchClassesTotal) / phrases << "\n";
    std::cout << "Adjacent-bar recognizable similarity (>=10/16 same steps): "
              << pct(static_cast<double>(repeatedBarPairs) / comparedBarPairs) << "%\n";
    std::cout << "Completely identical adjacent bars: "
              << pct(static_cast<double>(completelyIdenticalBarPairs) / comparedBarPairs) << "%\n";
    std::cout << "Adjacent-bar onset Jaccard similarity: "
              << pct(onsetJaccardSum / comparedBarPairs) << "%\n";
    std::cout << "Pitch identity on shared onsets: "
              << (sharedOnsets > 0
                    ? pct(static_cast<double>(sharedPitchMatches) / sharedOnsets)
                    : 0.0)
              << "%\n";
    std::cout << "Phrases containing >= quarter-note rest: "
              << pct(static_cast<double>(phrasesWithLongRest) / phrases) << "%\n";
    std::cout << "Phrases containing >= 8 consecutive hit steps: "
              << pct(static_cast<double>(phrasesWithLongHitRun) / phrases) << "%\n";
    std::cout << "Variation changed steps (20%): " << countDiffSteps(base, var20) << "/" << base.usedSteps() << "\n";
    std::cout << "Variation changed steps (50%): " << countDiffSteps(base, var50) << "/" << base.usedSteps() << "\n";
    std::cout << "Variation changed steps (80%): " << countDiffSteps(base, var80) << "/" << base.usedSteps() << "\n";


    std::cout << "\nVariation diagnostics over 512 phrases\n";
    std::cout << "--------------------------------------\n";
    for (float amount : {0.20f, 0.35f, 0.50f, 0.80f}) {
        double diffSum = 0.0;
        double jacSum = 0.0;
        constexpr int samples = 512;
        for (int i = 0; i < samples; ++i) {
            const auto src = RiffEngine::generate(s, 300000u + static_cast<unsigned>(i));
            const auto dst = RiffEngine::vary(src, s, amount, 400000u + static_cast<unsigned>(i));
            diffSum += countDiffSteps(src, dst);
            jacSum += onsetJaccard(src, dst);
        }
        std::cout << "Variation " << pct(amount) << "%: avg changed steps "
                  << diffSum / samples << "/" << s.bars * 16
                  << ", onset Jaccard " << pct(jacSum / samples) << "%\n";
    }


    std::cout << "\nControl sweep diagnostics (256 phrases per point)\n";
    std::cout << "------------------------------------------------\n";
    const float sweepValues[] = {0.0f, 0.25f, 0.50f, 0.75f, 1.0f};

    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.density = v;
        printSweepLine("Density   ", v, measureSettings(x, 500000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.complexity = v;
        printSweepLine("Complexity", v, measureSettings(x, 510000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.repetition = v;
        printSweepLine("Repetition", v, measureSettings(x, 520000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.palmMuteChance = v;
        printSweepLine("Palm Mute ", v, measureSettings(x, 530000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.powerChordChance = v;
        x.powerChordsEnabled = true;
        printSweepLine("PowerChord", v, measureSettings(x, 540000u + static_cast<unsigned>(v * 1000.0f)));
    }

    std::cout << "\nStyle detail diagnostics (512 phrases each)\n";
    std::cout << "-------------------------------------------\n";
    for (int style = 0; style < static_cast<int>(StyleId::Count); ++style) {
        GeneratorSettings x = s;
        x.style = static_cast<StyleId>(style);
        const auto m = measureSettings(x, 550000u + static_cast<unsigned>(style * 10000), 512);
        printSweepLine(styleNames[style], 0.0, m);
    }


    std::cout << "\nBass Brain control sweep diagnostics (256 phrases per point)\n";
    std::cout << "---------------------------------------------------------\n";

    GeneratorSettings bassGuitarSettings = s;
    bassGuitarSettings.bars = 4;
    bassGuitarSettings.style = StyleId::NDHIndustrial;
    const auto bassGuitarFixture = RiffEngine::generate(bassGuitarSettings, 0xB455F17u);

    for (float v : sweepValues) {
        BassSettings b{};
        b.follow = v;
        printBassSweepLine("Follow    ", v, measureBass(
            bassGuitarFixture, b, 600000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        BassSettings b{};
        b.movement = v;
        printBassSweepLine("Movement  ", v, measureBass(
            bassGuitarFixture, b, 610000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        BassSettings b{};
        b.passing = v;
        printBassSweepLine("Passing   ", v, measureBass(
            bassGuitarFixture, b, 620000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        BassSettings b{};
        b.sustain = v;
        printBassSweepLine("Sustain   ", v, measureBass(
            bassGuitarFixture, b, 630000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        BassSettings b{};
        b.octaveChance = v;
        printBassSweepLine("Octave    ", v, measureBass(
            bassGuitarFixture, b, 640000u + static_cast<unsigned>(v * 1000.0f)));
    }


    std::cout << "\nBass style diagnostics (512 phrases per style)\n";
    std::cout << "----------------------------------------------\n";
    for(int style=0;style<static_cast<int>(StyleId::Count);++style){
        BassSettings bs{}; bs.style=static_cast<StyleId>(style); bs.movement=0.45f; bs.sustain=0.55f;
        printBassSweepLine(styleNames[style],1.0,measureBass(bassGuitarFixture,bs,760000u+style*10000u,512));
    }
    std::cout<<"\n";

    std::cout << "\nDrum Brain control sweep diagnostics (256 phrases per point)\n";
    std::cout << "---------------------------------------------------------\n";
    const auto drumGuitar = RiffEngine::generate(bassGuitarSettings, 0xD12A1001u);
    BassSettings drumBassSettings{};
    const auto drumBass = BassBrain::generate(drumGuitar, drumBassSettings, 0xD12A1002u);

    for (float v : sweepValues) {
        DrumSettings ds{}; ds.follow=v;
        printDrumSweepLine("Follow    ",v,measureDrums(drumGuitar,drumBass,ds,700000u));
    }
    std::cout<<"\n";
    for (float v : sweepValues) {
        DrumSettings ds{}; ds.density=v;
        printDrumSweepLine("Density   ",v,measureDrums(drumGuitar,drumBass,ds,710000u));
    }
    std::cout<<"\n";
    for (float v : sweepValues) {
        DrumSettings ds{}; ds.complexity=v;
        printDrumSweepLine("Complexity",v,measureDrums(drumGuitar,drumBass,ds,720000u));
    }
    std::cout<<"\n";

    GeneratorSettings fillGuitarSettings = bassGuitarSettings;
    fillGuitarSettings.bars = 8;
    const auto fillGuitar =
        RiffEngine::generate(fillGuitarSettings, 0xF1111001u);
    BassSettings fillBassSettings{};
    const auto fillBass =
        BassBrain::generate(fillGuitar, fillBassSettings, 0xF1111002u);

    std::cout << "Fill Intensity focused 8-bar transition activity\n";
    for (float v : sweepValues) {
        DrumSettings ds{};
        ds.fillIntensity = v;
        const double activity = measureSectionFillActivity(
            fillGuitar, fillBass, ds, 7,
            725000u + static_cast<unsigned>(v * 1000.0f));
        std::cout << "FillIntensity " << std::setw(5) << v * 100.0
                  << "%: transitionActivity=" << activity << "\n";
    }

    std::cout<<"\n";
    for (float v : sweepValues) {
        DrumSettings ds{}; ds.humanize=v;
        printDrumSweepLine("Humanize  ",v,measureDrums(drumGuitar,drumBass,ds,730000u));
    }


    std::cout << "\nDrum style diagnostics (512 phrases per style)\n";
    std::cout << "----------------------------------------------\n";
    for (int style = 0; style < static_cast<int>(StyleId::Count); ++style) {
        DrumSettings ds{};
        ds.style = static_cast<StyleId>(style);
        const auto dm = measureDrums(drumGuitar, drumBass, ds,
                                     740000u + static_cast<unsigned>(style) * 10000u, 512);
        printDrumSweepLine(styleNames[style], 1.0, dm);
    }


    std::cout << "\nPad Brain control sweep diagnostics (256 phrases per point)\n";
    std::cout << "--------------------------------------------------------\n";
    const auto padGuitar = RiffEngine::generate(bassGuitarSettings, 0x50414411u);
    BassSettings padBassSettings{};
    const auto padBass = BassBrain::generate(padGuitar, padBassSettings, 0x50414412u);
    for(float v : sweepValues){
        PadSettings ps{}; ps.movement=v;
        printPadSweepLine("Movement",v,measurePads(padGuitar,padBass,ps,810000u));
    }
    std::cout<<"\n";
    for(float v : sweepValues){
        PadSettings ps{}; ps.spread=v;
        printPadSweepLine("Spread  ",v,measurePads(padGuitar,padBass,ps,820000u));
    }
    std::cout<<"\n";
    for(float v : sweepValues){
        PadSettings ps{}; ps.tension=v;
        printPadSweepLine("Tension ",v,measurePads(padGuitar,padBass,ps,830000u));
    }
    std::cout<<"\n";
    for(float v : sweepValues){
        PadSettings ps{}; ps.sustain=v;
        printPadSweepLine("Sustain ",v,measurePads(padGuitar,padBass,ps,840000u));
    }
    std::cout<<"\n";
    for(float v : sweepValues){
        PadSettings ps{}; ps.contextFollow=v;
        printPadSweepLine("Context ",v,measurePads(padGuitar,padBass,ps,845000u));
    }
    std::cout<<"\nPad style diagnostics (512 phrases per style)\n";
    std::cout<<"----------------------------------------------\n";
    for(int style=0;style<static_cast<int>(StyleId::Count);++style){
        PadSettings ps{}; ps.style=static_cast<StyleId>(style);
        const auto pm=measurePads(padGuitar,padBass,ps,850000u+style*10000u,512);
        printPadSweepLine(styleNames[style],1.0,pm);
    }
    std::cout<<"\n";


    std::cout<<"\nSynth Brain control sweep diagnostics (256 phrases per point)\n";
    std::cout<<"----------------------------------------------------------\n";
    const auto synthGuitar=RiffEngine::generate(bassGuitarSettings,0x53594E11u);
    BassSettings synthBassSettings{};
    const auto synthBass=BassBrain::generate(synthGuitar,synthBassSettings,0x53594E12u);
    PadSettings synthPadSettings{};
    const auto synthPads=PadBrain::generate(synthGuitar,synthBass,synthPadSettings,0x53594E13u);
    for(float v:sweepValues){ SynthSettings ss{}; ss.activity=v; printSynthSweepLine("Activity ",v,measureSynth(synthGuitar,synthBass,synthPads,ss,910000u)); }
    std::cout<<"\n";
    for(float v:sweepValues){ SynthSettings ss{}; ss.movement=v; printSynthSweepLine("Movement ",v,measureSynth(synthGuitar,synthBass,synthPads,ss,920000u)); }
    std::cout<<"\n";
    for(float v:sweepValues){ SynthSettings ss{}; ss.repetition=v; printSynthSweepLine("Repetition",v,measureSynth(synthGuitar,synthBass,synthPads,ss,930000u)); }
    std::cout<<"\n";
    for(float v:sweepValues){ SynthSettings ss{}; ss.syncopation=v; printSynthSweepLine("Syncopation",v,measureSynth(synthGuitar,synthBass,synthPads,ss,940000u)); }
    std::cout<<"\n";
    for(float v:sweepValues){ SynthSettings ss{}; ss.sustain=v; printSynthSweepLine("Sustain  ",v,measureSynth(synthGuitar,synthBass,synthPads,ss,950000u)); }
    std::cout<<"\n";
    for(float v:sweepValues){ SynthSettings ss{}; ss.harmonicFollow=v; printSynthSweepLine("Harmonic ",v,measureSynth(synthGuitar,synthBass,synthPads,ss,955000u)); }
    std::cout<<"\nSynth style diagnostics (512 phrases per style)\n";
    for(int style=0;style<static_cast<int>(StyleId::Count);++style){ SynthSettings ss{}; ss.style=static_cast<StyleId>(style); printSynthSweepLine(styleNames[style],1.0,measureSynth(synthGuitar,synthBass,synthPads,ss,960000u+style*10000u,512)); }
    std::cout<<"\n";
    GeneratorSettings exampleSettings = s;
    exampleSettings.bars = 4;
    printPhraseGrid(RiffEngine::generate(exampleSettings, 101u), "Example riff A - A Phrygian");
    printPhraseGrid(RiffEngine::generate(exampleSettings, 202u), "Example riff B - A Phrygian");
    printPhraseGrid(RiffEngine::generate(exampleSettings, 303u), "Example riff C - A Phrygian");

    printNewRiffBenchmarks();
    printGoldenArrangementFingerprint();

    return 0;
}
