#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <regex>
#include <vector>
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
 require(xml.find("title=\"GLOBAL HUMANIZE\"")!=std::string::npos,"Humanize global grouping");
 require(xml.find("title=\"DETAIL CONTROLS\"")!=std::string::npos,"details header must not promise all parameters are live");
 require(xml.find("title=\"PALM MUTE BELOW VELOCITY\"")!=std::string::npos,"palm mute control uses an understandable full label");
 require(xml.find("title=\"SUGGESTED BPM 100-135\"")!=std::string::npos,"default tempo hint wording");
 require(xml.find("midiator-id=\"theoryNotes\" origin=\"250, 145\"")!=std::string::npos,"mode and associated notes must be adjacent");
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
                       "DENSITY","COMPLEXITY","REPETITION","GLOBAL HUMANIZE","AMOUNT"}) {
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

 // Check every functional view against its panel bounds and its neighbours.
 // Decorative divider CView objects are excluded from collision checks.
 struct Element { int x,y,w,h; std::string type; };
 const auto checkPanel = [&](const char* startMarker,const char* endMarker,int width,int height) {
   const auto start=xml.find(startMarker);
   const auto end=xml.find(endMarker,start);
   require(start!=std::string::npos && end!=std::string::npos,"UI panel delimiters");
   const std::string panel=xml.substr(start,end-start);
   const std::regex viewPattern(R"(<view\b[^>]*\/>)");
   const std::regex originPattern(R"rx(origin="([0-9]+),\s*([0-9]+)")rx");
   const std::regex sizePattern(R"rx(size="([0-9]+),\s*([0-9]+)")rx");
   const std::regex classPattern(R"rx(class="([^"]+)")rx");
   std::vector<Element> elements;
   for(std::sregex_iterator it(panel.begin(),panel.end(),viewPattern),last;it!=last;++it) {
     const auto line=it->str();
     std::smatch origin,size,klass;
     require(std::regex_search(line,origin,originPattern) &&
             std::regex_search(line,size,sizePattern) &&
             std::regex_search(line,klass,classPattern),"UI view geometry present");
     Element e{std::stoi(origin[1]),std::stoi(origin[2]),
               std::stoi(size[1]),std::stoi(size[2]),klass[1]};
     require(e.x>=0 && e.y>=0 && e.w>0 && e.h>0 &&
             e.x+e.w<=width && e.y+e.h<=height,"UI element within its panel");
     if(e.type=="CView")continue;
     for(const auto& prev:elements) {
       const bool separated=e.x>=prev.x+prev.w || prev.x>=e.x+e.w ||
                            e.y>=prev.y+prev.h || prev.y>=e.y+e.h;
       require(separated,"UI functional elements must not overlap");
     }
     elements.push_back(e);
   }
   require(elements.size()>5,"UI panel must contain controls");
 };
 checkPanel("origin=\"24, 82\" size=\"1072, 176\"","midiator-id=\"mainGenerate\"",1072,176);
 checkPanel("midiator-id=\"mainGenerate\"","midiator-id=\"mainLocks\"",1072,300);
 checkPanel("midiator-id=\"mainLocks\"","midiator-id=\"detailsPanel\"",1072,122);
 checkPanel("midiator-id=\"detailsPanel\"","origin=\"24, 720\"",1072,434);
 require(xml.find("midiator-id=\"theoryKey\" origin=\"22, 145\" size=\"220, 20\"")!=std::string::npos &&
         xml.find("midiator-id=\"theoryNotes\" origin=\"250, 145\" size=\"790, 20\"")!=std::string::npos,
         "theory labels must remain adjacent at all displayed key lengths");
 require(xml.find("control-tag=\"PalmMuteVelocity\" origin=\"240, 388\"")!=std::string::npos,
         "palm-mute velocity field must sit next to its label");
 std::cout<<"Midiator UI contract test: PASS\n";
 return 0;
}
