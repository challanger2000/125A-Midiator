#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#ifndef MIDIATOR_UIDESC_PATH
#error MIDIATOR_UIDESC_PATH is not defined
#endif
static void require(bool condition,const char* message) {
 if(!condition){std::cerr<<"FAIL: "<<message<<"\n";std::exit(1);}
}
int main() {
 std::ifstream in(MIDIATOR_UIDESC_PATH,std::ios::binary);
 require(static_cast<bool>(in),"UIDesc readable");
 std::ostringstream ss;ss<<in.rdbuf();const auto xml=ss.str();
 require(xml.find("size=\"1120, 1000\"")!=std::string::npos,"spacious base size");
 require(xml.find("minSize=\"896, 800\"")!=std::string::npos,"80% size support");
 for(int i=100;i<=135;++i) {
   auto needle="tag=\""+std::to_string(i)+"\"";
   require(xml.find(needle)!=std::string::npos,"all parameter IDs remain bound");
 }
 for(const char* tag:{"Root","Scale","SectionLength","RootSource","Trigger","SectionType",
    "Style","DrumMap","Density","Complexity","Repetition","PowerChords",
    "PowerChordsEnabled","PalmMute","PalmMuteVelocity","VariationAmount",
    "NewRiff","Variation","GuitarLock","BassLock","DrumsLock",
    "PadLock","SynthLock","BassFollow","BassMovement","DrumDensity",
    "DrumComplexity","FillIntensity","PadSpread","PadTension",
    "SynthActivity","SynthMovement","Humanize"}) {
   const auto needle=std::string("control-tag=\"")+tag+"\"";
   require(xml.find(needle)!=std::string::npos,"required control visible");
 }
 for(const char* tag:{"GuitarLock","BassLock","DrumsLock","PadLock",
                      "SynthLock","PowerChordsEnabled"}) {
   const auto needle=std::string("control-tag=\"")+tag+"\"";
   const auto p=xml.find(needle);
   require(p!=std::string::npos,"toggle control present");
   const auto a=xml.rfind("<view",p);
   require(a!=std::string::npos &&
           xml.substr(a,p-a).find("class=\"CTextButton\"")!=std::string::npos,
           "binary toggles must be direct buttons");
 }
 for(const char* heading:{"01  SET THE MUSIC","02  GENERATE A RIFF",
                            "03  KEEP WHAT WORKS","04  SHAPE THE BAND"}) {
   require(xml.find(heading)!=std::string::npos,"workflow hierarchy present");
 }
 for(const char* id:{"theoryKey","theoryNotes","styleBpm"}) {
   require(xml.find(std::string("midiator-id=\"")+id+"\"")!=std::string::npos,
           "dynamic helper visible");
 }
 std::cout<<"Midiator UI contract test: PASS\n";
 return 0;
}
