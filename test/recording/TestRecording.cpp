#include "capture/Recording.h"
#include "capture/Replay.h"
#include "capture/Source.h"
#include "capture/rspduo/SampleSequence.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

using namespace blah2;
class MockSource final : public Source {
public:
  explicit MockSource(const std::string& path, bool* save)
    : Source("Mock", 100000000, 2000000, path, save) {}
  void process(IqData*, IqData*) override {}
  void start() override {}
  void stop() override {}
};
class BackendFixtureSource final : public Source {
  std::string details;
public:
  BackendFixtureSource(const std::string& receiver,const std::string& path,
      bool* save,std::string provenance)
    : Source(receiver,527000000,2400000,path,save),details(std::move(provenance)) {}
  void process(IqData*, IqData*) override {}
  void start() override {}
  void stop() override {}
  SampleTimePosition sample_time_position() const override {
    return {true,7,1000,40000000,2400000,"external","internal","B210","A:A A:B"};
  }
  std::string recording_provenance_json() const override { return details; }
};
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void rejects(const std::function<void()>& work,const char* message) {
  bool rejected=false; try { work(); } catch(const std::runtime_error&) { rejected=true; }
  require(rejected,message);
}
void write(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
  std::ofstream stream(path,std::ios::binary); stream.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
}

int main() try {
  char temporary[]="/tmp/blah2-recording-tests.XXXXXX";
  require(mkdtemp(temporary),"Cannot create test directory");
  const auto directory=std::filesystem::path(temporary), path=directory/"fixture.iq";
  auto cleanup=[&] { std::filesystem::remove_all(directory); };
  try {
    const uint32_t maxSample=std::numeric_limits<uint32_t>::max();
    require(rspduo_sequence::continues(maxSample,maxSample) &&
      rspduo_sequence::next(maxSample,1)==0 &&
      rspduo_sequence::next(maxSample-2,3)==0,
      "RSPduo sample counter wrap");
    for(unsigned channels=2;channels<=8;++channels) {
      IqBlock original(channels,std::vector<std::complex<float>>(11));
      for(unsigned ch=0;ch<channels;++ch) for(unsigned i=0;i<11;++i) original[ch][i]={float(ch*11+i)-40,float(i)-9};
      {
        RecordingWriter writer(path,{channels,2400000,527000000,0,"blah2"});
        writer.append(original); writer.append(original);
        require(writer.samples()==22,"Writer sample count"); writer.close(); writer.close();
        rejects([&] { writer.append(original); },"Accepted append to closed recording");
      }
      rejects([&] { RecordingWriter writer(path,{channels,2400000,527000000,0,"blah2"}); },"Overwrote an existing recording");
      ReplayOptions options{"auto","Kraken",channels,2400000,527000000,0};
      RecordingReader reader(path,options); IqBlock block;
      require(reader.metadata().samples==22,"Reader sample count");
      require(reader.provenance().legacy && !reader.provenance().exactTime,
        "Missing sidecar was not exposed as legacy/unqualified");
      auto decisionGrade=options; decisionGrade.provenanceMode="decision-grade";
      rejects([&] { RecordingReader invalid(path,decisionGrade); },
        "Decision-grade replay accepted a recording without provenance");
      for(unsigned repeat=0;repeat<3;++repeat) {
        require(reader.read(block) && block==original,"First recording block differs");
        require(reader.read(block) && block==original,"Second recording block differs");
        require(!reader.read(block),"Reader did not reach EOF"); reader.rewind();
      }
      reader.seek(5);
      require(reader.read(block) && block.front().size()==6 &&
        block.front().front()==original.front()[5],"Indexed replay seek changed the selected sample");
      require(reader.read(block) && block==original,"Indexed replay seek lost the following block");
      reader.seek(22); require(!reader.read(block),"Replay seek to EOF returned samples");
      options.channels=channels==2 ? 3 : 2;
      rejects([&] { RecordingReader invalid(path,options); },"Accepted wrong channel count");
      options.channels=channels; options.frequency++;
      rejects([&] { RecordingReader invalid(path,options); },"Accepted wrong frequency");
      options.frequency--; options.sampleRate++;
      rejects([&] { RecordingReader invalid(path,options); },"Accepted wrong sample rate");
      options.sampleRate--;
      std::filesystem::resize_file(path,std::filesystem::file_size(path)-1);
      rejects([&] { RecordingReader invalid(path,options); },"Accepted truncated recording");
      std::filesystem::remove(path);
    }
    {
      RecordingWriter writer(path,{2,2000000,100000000,0,"blah2"});
      writer.append({{{1,2}},{{3,4}}});
      rejects([&] { writer.append({{{std::numeric_limits<float>::quiet_NaN(),0}},{{0,0}}}); },"Accepted NaN IQ");
    }
    rejects([&] { RecordingReader invalid(path,{"auto","RspDuo",2,2000000,100000000,0}); },"Accepted unfinished recording");
    std::filesystem::remove(path);

    rejects([&] { RecordingWriter invalid(path,{1,2000000,100000000,0,"blah2"}); },"Accepted invalid writer metadata");
    require(!std::filesystem::exists(path),"Invalid writer left a recording artifact");

    bool save=false;
    MockSource empty(directory.string(),&save);
    const auto emptyPath=empty.open_file();
    rejects([&] { empty.close_file(); },"Accepted a clean close with no samples");
    require(empty.recording_status().error=="No samples were recorded",
      "Empty recording did not retain its error");
    rejects([&] { RecordingReader invalid(emptyPath,{"auto","Mock",2,2000000,100000000,0}); },
      "Empty recording was marked complete");

    MockSource source(directory.string(),&save);
    const auto sourcePath=source.open_file();
    source.record_channel(0,0,{{1,2},{3,4},{5,6}});
    source.record_channel(1,0,{{7,8},{9,10},{11,12}});
    source.close_file();
    const auto sourceStatus=source.recording_status();
    require(!sourceStatus.active && sourceStatus.error.empty() && sourceStatus.samples==3,
      "Source lost completed recording status");
    RecordingReader sourceReader(sourcePath,{"auto","Mock",2,2000000,100000000,0});
    IqBlock sourceBlock;
    require(sourceReader.read(sourceBlock) && sourceBlock[0].size()==3 && sourceBlock[1][2]==std::complex<float>(11,12),
      "Source paired recording differs");
    require(sourceReader.provenance().contractValid && !sourceReader.provenance().decisionGrade &&
      sourceReader.provenance().schema=="vectorwarp-recording-provenance-v2",
      "Source recording provenance was not consumed as explicitly inexact v2");
    std::filesystem::remove(sourcePath);
    std::filesystem::remove(sourcePath+".provenance.json");

    // Deterministic backend-shaped live -> portable record -> replay contracts.
    // The backend details deliberately cover distinct identity/gain/topology
    // shapes while the common reader consumes the same fail-closed envelope.
    const std::vector<std::pair<std::string,std::string>> backendDetails{
      {"Kraken",R"({"device":{"identityAvailable":false,"reason":"Heimdall protocol"},"channels":[{"gainDb":19.7},{"gainDb":20.7}],"rf":{"frequencyHz":527000000},"topology":{"transport":"heimdall-v2"}})"},
      {"RspDuo",R"({"device":{"serial":"RSP-FIXTURE"},"channels":[{"tuner":"A","gainReductionDb":50,"lnaState":1},{"tuner":"B","gainReductionDb":45,"lnaState":1}],"rf":{"frequencyHz":527000000,"bandwidth":5},"topology":{"mode":"Dual_Tuner"}})"},
      {"Usrp",R"({"device":{"address":"type=b200"},"channels":[{"antenna":"RX2","gainDb":10},{"antenna":"RX2","gainDb":20}],"rf":{"frequencyHz":527000000},"topology":{"subdevices":"A:A A:B","clockSource":"external","timeSource":"internal"}})"},
      {"HackRF",R"({"device":{"serials":["a1","b2"]},"channels":[{"lnaGainDb":16,"vgaGainDb":20,"amp":false},{"lnaGainDb":32,"vgaGainDb":40,"amp":true}],"rf":{"frequencyHz":527000000},"topology":{"clock":"shared","trigger":"hardware"}})"}
    };
    for(const auto& fixture:backendDetails) {
      BackendFixtureSource live(fixture.first,directory.string(),&save,fixture.second);
      const auto recorded=live.open_file();
      live.record_block({{{1,2},{3,4}},{{5,6},{7,8}}});
      live.close_file();
      ReplayOptions qualified{"auto",fixture.first,2,2400000,527000000,0};
      RecordingReader replay(recorded,qualified);
      IqBlock replayed;
      require(replay.read(replayed) && replayed[0][1]==std::complex<float>(3,4),
        "Backend v2 round trip changed samples");
      require(replay.provenance().contractValid && replay.provenance().receiver==fixture.first &&
        !replay.provenance().exactTime && !replay.provenance().uncertaintyBounded &&
        replay.provenance().configurationFingerprintValid &&
        replay.provenance().codeFingerprintMatches && replay.provenance().sourceSampleAvailable &&
        replay.provenance().sourceFirstSample==1000,
        "Backend v2 round trip lost receiver/sample-time provenance");
      qualified.provenanceMode="decision-grade";
      rejects([&] { RecordingReader invalid(recorded,qualified); },
        "Decision-grade replay accepted inexact backend time");
      if(fixture.first=="Kraken") {
        qualified.provenanceMode="legacy-unqualified";
        qualified.receiver="Usrp";
        RecordingReader crossReceiver(recorded,qualified);
        require(crossReceiver.provenance().receiver=="Kraken" &&
          crossReceiver.provenance().contractValid,
          "Cross-receiver replay lost the recording receiver provenance");
        qualified.receiver="Kraken";
        const auto sidecar=recorded+".provenance.json";
        std::ifstream input(sidecar);
        std::string tampered(std::istreambuf_iterator<char>(input),{});
        const auto gain=tampered.find("\"gainDb\":19.7");
        require(gain!=std::string::npos,"Fixture sidecar omitted Kraken gain");
        tampered.replace(gain,std::strlen("\"gainDb\":19.7"),"\"gainDb\":19.8");
        std::ofstream output(sidecar,std::ios::trunc); output << tampered; output.close();
        rejects([&] { RecordingReader invalid(recorded,qualified); },
          "Replay accepted tampered receiver settings provenance");
      }
      std::filesystem::remove(recorded);
      std::filesystem::remove(recorded+".provenance.json");
    }

    MockSource unpaired(directory.string(),&save);
    unpaired.open_file();
    unpaired.record_channel(0,0,{{1,1},{2,2}});
    unpaired.record_channel(1,0,{{3,3}});
    rejects([&] { unpaired.close_file(); },"Accepted unpaired Source close");
    require(!unpaired.recording_status().error.empty(),"Unpaired close did not retain an error");

    MockSource gap(directory.string(),&save);
    gap.open_file();
    gap.record_channel(0,0,{{1,1}});
    gap.record_channel(0,2,{{2,2}});
    require(!gap.recording_status().error.empty(),"Source accepted a callback sample gap");

    MockSource discontinuity(directory.string(),&save);
    require(discontinuity.processing_epoch()==0,"New source did not start in processing epoch zero");
    const auto discontinuityPath=discontinuity.open_file();
    discontinuity.record_block({{{1,2}},{{3,4}}});
    discontinuity.recording_discontinuity("mock capture gap");
    require(discontinuity.processing_epoch()==1,
      "Capture gap did not advance the adaptive-processing epoch");
    const auto discontinuityStatus=discontinuity.recording_status();
    require(!discontinuityStatus.active && discontinuityStatus.samples==1 &&
      discontinuityStatus.error=="mock capture gap","Discontinuity status was not retained");
    rejects([&] { RecordingReader invalid(discontinuityPath,{"auto","Mock",2,2000000,100000000,0}); },
      "Discontinuous recording was marked complete");

    MockSource segmented(directory.string(),&save);
    const auto ambiguousEpoch=segmented.open_file();
    segmented.record_block({{{1,2}},{{3,4}}});
    segmented.recording_epoch_discontinuity("mock setting epoch changed");
    require(segmented.processing_epoch()==1,
      "RF setting change did not advance the adaptive-processing epoch");
    const auto transition=segmented.recording_status();
    require(!transition.active && transition.resumePending && transition.samples==1,
      "Qualified epoch rollover did not retain a bounded resume request");
    segmented.record_block({{{5,6}},{{7,8}}});
    const auto resumed=segmented.recording_status();
    require(resumed.active && !resumed.resumePending && resumed.samples==1 &&
      resumed.file!=ambiguousEpoch,
      "First qualified post-boundary block did not lazily resume recording");
    segmented.close_file();
    rejects([&] { RecordingReader invalid(ambiguousEpoch,
      {"auto","Mock",2,2000000,100000000,0}); },
      "Ambiguous pre-boundary epoch was marked complete");
    RecordingReader resumedReader(resumed.file,
      {"auto","Mock",2,2000000,100000000,0});
    IqBlock resumedBlock;
    require(resumedReader.read(resumedBlock) &&
      resumedBlock[0][0]==std::complex<float>(5,6),
      "Post-boundary recording retained pre-boundary samples");
    std::filesystem::remove(ambiguousEpoch);
    std::filesystem::remove(ambiguousEpoch+".provenance.json");
    std::filesystem::remove(resumed.file);
    std::filesystem::remove(resumed.file+".provenance.json");

    MockSource unrecordedEpoch(directory.string(),&save);
    unrecordedEpoch.recording_epoch_discontinuity("unrecorded setting epoch changed");
    require(unrecordedEpoch.processing_epoch()==1,
      "An unrecorded live discontinuity did not reset adaptive processing");

    MockSource coordinatedEpoch(directory.string(),&save);
    coordinatedEpoch.recording_epoch_discontinuity(
      "buffer clearing owns this boundary", false);
    require(coordinatedEpoch.processing_epoch()==0,
      "Deferred discontinuity published before coordinated buffer clearing");
    coordinatedEpoch.advance_processing_epoch();
    require(coordinatedEpoch.processing_epoch()==1,
      "Coordinated buffer boundary did not publish its processing epoch");

    {
      RecordingWriter writer(path,{2,1000000,100000000,0,"blah2"});
      IqBlock replayBlock(2,std::vector<std::complex<float>>(6));
      for (unsigned ch=0;ch<2;++ch) for (unsigned i=0;i<6;++i) replayBlock[ch][i]={float(ch),float(i)};
      writer.append(replayBlock); writer.close();
      IqData first(4), second(4); std::vector<IqData*> queues{&first,&second};
      std::atomic<bool> stopped{false}, consume{true};
      std::vector<std::complex<double>> observed;
      std::thread consumer([&] {
        while (consume.load()) {
          first.lock(); second.lock();
          if (first.get_length()>=4 && second.get_length()>=4) {
            auto frame=first.drain_front(4); second.drain_front(4);
            observed.insert(observed.end(),frame.begin(),frame.end());
          }
          second.unlock(); first.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
      ReplayPlayer player; ReplayProgress last; unsigned updates=0;
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,false,stopped,
        [&](const ReplayProgress& progress) { last=progress; ++updates; }, [] { return false; });
      consume=false; consumer.join();
      require(last.state=="complete" && last.samples==6 && last.trailingSamples==2 && updates>2,
        "Replay did not reach bounded EOF completion");
      require(observed.size()==4 && observed[0]==std::complex<double>(0,0) &&
        observed[3]==std::complex<double>(0,3),"Replay changed frame ordering under backpressure");

      stopped=false; consume=true;
      std::thread loopConsumer([&] {
        while (consume.load()) {
          first.lock(); second.lock();
          if (first.get_length()>=4 && second.get_length()>=4) { first.drain_front(4); second.drain_front(4); }
          second.unlock(); first.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,true,stopped,
        [&](const ReplayProgress& progress) { if (progress.loops==1) stopped=true; }, [] { return false; });
      consume=false; loopConsumer.join();
      require(stopped.load(),"Replay loop was not cancellable");

      // Cancellation must also win while every playback queue is full.
      stopped=false;
      std::thread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); stopped=true; });
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,false,stopped,
        [](const ReplayProgress&) {}, [] { return false; });
      cancel.join();
      require(stopped.load(),"Replay did not cancel while backpressured");

      // A seek is an indexed processor-generation boundary, and pause must not
      // consume samples while the user is scrubbing.
      first.clear(); second.clear(); stopped=false; consume=true;
      std::atomic<bool> paused{true}; std::atomic<uint64_t> seek{0},revision{0};
      bool sawPaused=false; ReplayProgress controlled;
      std::thread seekConsumer([&] {
        while(consume.load()) {
          first.lock(); second.lock();
          if(first.get_length()>=4&&second.get_length()>=4){first.drain_front(4);second.drain_front(4);}
          second.unlock();first.unlock();std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
      std::thread controller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        seek=2;paused=false;revision=1;
      });
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,false,stopped,
        [&](const ReplayProgress& progress){controlled=progress;sawPaused=sawPaused||progress.state=="paused";},
        []{return false;},[&]{return ReplayCommand{paused.load(),seek.load(),revision.load()};});
      controller.join();consume=false;seekConsumer.join();
      require(sawPaused&&controlled.state=="complete"&&controlled.samples==6&&controlled.generation==1,
        "Replay pause/seek did not publish a clean processor generation");
      std::filesystem::remove(path);
    }

    // The common player is not a two-channel adapter: all supported coherent
    // channel counts retain the same first frame under a full-queue cancel.
    for (unsigned channels=2; channels<=8; ++channels) {
      const auto multiPath=directory/("replay-"+std::to_string(channels)+".iq");
      RecordingWriter writer(multiPath,{channels,1000000,100000000,0,"blah2"});
      IqBlock multi(channels,std::vector<std::complex<float>>(4));
      for (unsigned ch=0;ch<channels;++ch) for (unsigned i=0;i<4;++i) multi[ch][i]={float(ch),float(i)};
      writer.append(multi); writer.close();
      std::vector<std::unique_ptr<IqData>> storage;
      std::vector<IqData*> multiQueues;
      for (unsigned ch=0;ch<channels;++ch) { storage.push_back(std::make_unique<IqData>(4)); multiQueues.push_back(storage.back().get()); }
      std::atomic<bool> stopMulti{false}, fullFrameQueued{false};
      ReplayPlayer player;
      // The cancellation must occur while the grouped queues are actually
      // full. A fixed delay races CI scheduling and can stop playback before
      // it has written any channel, which tests scheduling rather than
      // multi-channel alignment.
      std::thread cancel([&] {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        while (!fullFrameQueued.load() && std::chrono::steady_clock::now()<deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        stopMulti=true;
      });
      player.run(multiPath,{"auto","Mock",channels,1000000,100000000,0},multiQueues,4,false,stopMulti,
        [&](const ReplayProgress& progress) { if (progress.samples==4) fullFrameQueued=true; }, [] { return false; });
      cancel.join();
      require(fullFrameQueued.load(),"Replay did not fill the coherent queues before cancellation");
      for (unsigned ch=0;ch<channels;++ch) {
        storage[ch]->lock(); const auto values=storage[ch]->get_data(); storage[ch]->unlock();
        require(values.size()==4 && values[0]==std::complex<double>(ch,0) && values[3]==std::complex<double>(ch,3),
          "Replay lost multi-channel alignment under backpressure");
      }
      std::filesystem::remove(multiPath);
    }

    std::vector<uint8_t> raw;
    for(int value : {-32768,32767,-1,0,1,-2,300,-400}) { const auto bits=static_cast<uint16_t>(value); raw.push_back(bits); raw.push_back(bits>>8); }
    write(path,raw);
    RecordingReader rsp(path,{"auto","RspDuo",2,2000000,100000000,0}); IqBlock block;
    require(rsp.read(block) && block[0][0]==std::complex<float>(-32768,32767) && block[1][1]==std::complex<float>(300,-400),"Legacy RSPduo values");
    require(!rsp.read(block),"Legacy RSPduo EOF");
    raw.pop_back(); write(path,raw);
    rejects([&] { RecordingReader invalid(path,{"auto","RspDuo",2,2000000,100000000,0}); },"Accepted partial legacy sample");

    write(path,{0x80,0x7f,0xff,0,1,2,3,4});
    RecordingReader hackrf(path,{"s8-interleaved","HackRF",2,2000000,100000000,0});
    require(hackrf.read(block) && block[0][0]==std::complex<float>(-128,127) && block[1][0]==std::complex<float>(-1,0),"Legacy signed int8 values");

    raw.clear();
    for(float v:{1.f,2.f,3.f,4.f,5.f,6.f,7.f,8.f}) {
      uint32_t bits; std::memcpy(&bits,&v,4); for(unsigned b=0;b<4;++b) raw.push_back(bits>>(b*8));
    }
    write(path,raw);
    rejects([&] { RecordingReader invalid(path,{"auto","Usrp",2,2400000,527000000,0}); },"Guessed old USRP block size");
    RecordingReader usrp(path,{"usrp-blocks","Usrp",2,2400000,527000000,2});
    require(usrp.read(block) && block[0][1]==std::complex<float>(3,4) && block[1][0]==std::complex<float>(5,6),"Legacy USRP block layout");

    raw.clear();
    for(uint32_t v:{0x4d434851U,2U,4U,4U,0U,0U,0U,0U}) for(int b=3;b>=0;--b) raw.push_back(v>>(b*8));
    for(float v:{527000000.f,49.6f,527000000.f,49.6f}) {
      uint32_t bits; std::memcpy(&bits,&v,4); for(unsigned b=0;b<4;++b) raw.push_back(bits>>(b*8));
    }
    for(unsigned ch=0;ch<2;++ch) for(unsigned i=0;i<4;++i) { raw.push_back(125+i); raw.push_back(130-i); }
    write(path,raw);
    RecordingReader kraken(path,{"auto","Kraken",2,2400000,527000000,0});
    require(kraken.metadata().samples==4 && kraken.read(block),"MCHQ recording read");
    require(std::abs(block[0][0]-std::complex<float>(-1.5/127.5,1.5/127.5))<1e-7,"MCHQ normalization");
    raw.back()=128; raw[15]=3; write(path,raw); // Phase no longer calibrated.
    rejects([&] { RecordingReader invalid(path,{"auto","Kraken",2,2400000,527000000,0}); },"Accepted calibration frame");
    cleanup();
    std::cout << "Recording: 2–8 channels, exact samples, EOF/rewind, metadata, malformed files, legacy RSPduo/USRP/HackRF and MCHQ passed\n";
  } catch(...) { cleanup(); throw; }
} catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
