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
 require(xml.find("size=\"1120, 744\"")!=std::string::npos,"spacious base size");
 require(xml.find("minSize=\"896, 595\"")!=std::string::npos,"80% size support");
 require(xml.find("control-tag=\"UiZoom\"")!=std::string::npos &&
         xml.find("title=\"UI ZOOM\"")!=std::string::npos,
         "explicit and visible zoom button required");
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
 // Placement contract: instruments and global controls have distinct groups.
 require(xml.find("origin=\"282, 216\" size=\"237, 20\" title=\"DRUM FILL\"")!=std::string::npos,"Drum Fill grouped with drums");
 require(xml.find("title=\"GLOBAL PERFORMANCE\"")!=std::string::npos,"Humanize global grouping");
 for(const char* tag:{"GuitarLock","BassLock","DrumsLock","PadLock","SynthLock","PowerChordsEnabled"}) {
   auto p=xml.find(std::string("control-tag=\"")+tag+"\"");
   require(p!=std::string::npos,"toggle control exists");
   auto a=xml.rfind("<view",p);auto b=xml.find("/>",p);
   require(a!=std::string::npos && b!=std::string::npos &&
           xml.substr(a,b-a).find("kick-style=\"false\"")!=std::string::npos,
           "OPEN/LOCK and ON/OFF must be persistent, not kick switches");
 }
 require(xml.find("origin=\"375, 10\" size=\"370, 33\" title=\"MIDIATOR\"")!=std::string::npos,
         "MIDIATOR header must be centered independently of utility controls");
 require(xml.find("MIDIATOR V1.0.0")!=std::string::npos,
         "visible version 1.0.0 must appear in GUI");
 require(xml.find("control-tag=\"UiDetails\"")!=std::string::npos &&
         xml.find("midiator-id=\"detailsPanel\"")!=std::string::npos &&
         xml.find("midiator-id=\"mainGenerate\"")!=std::string::npos &&
         xml.find("midiator-id=\"mainLocks\"")!=std::string::npos,
         "main and details views must be switchable");
 for(const char* panel:{"PanelTonal","PanelGenerate","PanelLocks","PanelLive"}) {
   const auto declaration=std::string("<color name=\"")+panel+"\"";
   const auto usage=std::string("background-color=\"")+panel+"\"";
   require(xml.find(declaration)!=std::string::npos &&
           xml.find(usage)!=std::string::npos,
           "each function group needs a distinct eye-friendly background");
 }
 for(const char* label:{"ROOT SOURCE","TRIGGER MODE","METAL STYLE",
                       "DENSITY","COMPLEXITY","REPETITION","HUMANIZE"}) {
   require(xml.find(std::string("title=\"")+label+"\"")!=std::string::npos,
           "key controls must keep legible labels");
 }
 // Keep advanced guitar controls and mapping out of the main workflow.
 {
   const auto main=xml.find("midiator-id=\"mainGenerate\"");
   const auto locks=xml.find("midiator-id=\"mainLocks\"");
   const auto detail=xml.find("midiator-id=\"detailsPanel\"");
   require(main!=std::string::npos && locks>main && detail>locks,
           "main and details panel hierarchy");
   const auto mainBlock=xml.substr(main,locks-main);
   const auto detailBlock=xml.substr(detail);
   for(const char* tag:{"PowerChords","PalmMute","PowerChordsEnabled",
                        "PalmMuteVelocity","DrumMap"}) {
     const auto needle=std::string("control-tag=\"")+tag+"\"";
     require(mainBlock.find(needle)==std::string::npos &&
             detailBlock.find(needle)!=std::string::npos,
             "advanced controls belong to details, not main");
   }
 }
 for(const char* heading:{"01  SET THE MUSIC","02  RIFF GENERATOR",
                            "03  KEEP WHAT WORKS","DETAIL CONTROLS"}) {
   require(xml.find(heading)!=std::string::npos,"workflow hierarchy present");
 }
 for(const char* id:{"theoryKey","theoryNotes","styleBpm"}) {
   require(xml.find(std::string("midiator-id=\"")+id+"\"")!=std::string::npos,
           "dynamic helper visible");
 }
 std::cout<<"Midiator UI contract test: PASS\n";
 return 0;
}
