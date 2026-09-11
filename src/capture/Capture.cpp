#include "Capture.h"
#ifdef BLAH2_ENABLE_RSPDUO
#include "rspduo/RspDuo.h"
#endif
#ifdef BLAH2_ENABLE_USRP
#include "usrp/Usrp.h"
#endif
#ifdef BLAH2_ENABLE_HACKRF
#include "hackrf/HackRf.h"
#endif
#include "kraken/Kraken.h"
#include <iostream>
#include <thread>
#include <csignal>
#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

extern volatile sig_atomic_t STOP_SIGNAL;

// constants
const std::string Capture::VALID_TYPE[4] = {"RspDuo", "Usrp", "HackRF", "Kraken"};

// constructor
Capture::Capture(std::string _type, uint32_t _fs, uint32_t _fc, std::string _path)
{
  type = _type;
  fs = _fs;
  fc = _fc;
  path = _path;
  replay = false;
  saveIq = false;
}

void Capture::process(IqData *buffer1, IqData *buffer2, c4::yml::NodeRef config, 
  std::string ip_capture, uint16_t port_capture)
{
  process(std::vector<IqData *>{buffer1, buffer2}, config, ip_capture,
    port_capture);
}

void Capture::process(const std::vector<IqData *>& buffers,
  c4::yml::NodeRef config, std::string ip_capture, uint16_t port_capture,
  uint32_t frameSamples)
{
  std::cout << (replay ? "Opening recording" : "Setting up device " + type) << std::endl;
  // Playback never constructs or initializes a physical receiver.
  if (!replay) {
    try {
      device = factory_source(type, config, buffers.size());
      if (!device) throw std::runtime_error("Capture backend is unavailable in this build");
    } catch (const std::exception& error) { processing_error(error.what()); }
  }
  std::thread control([&] {
    httplib::Client cli("http://" + ip_capture + ":" + std::to_string(port_capture));
    cli.set_connection_timeout(0, 250000);
    cli.set_read_timeout(0, 500000);
    cli.set_write_timeout(0, 500000);
    bool deviceStopped = false;
    bool haveRecordingRevision = false;
    uint64_t recordingRevision = 0;
    auto applyRecordingRequest = [&](bool requested, uint64_t revision,
                                     bool revisioned) {
      const bool changed = revisioned ?
        (!haveRecordingRevision || revision != recordingRevision) : requested != saveIq;
      if (!changed) return;
      try {
        // A distinct revision is a distinct capture request, even when its
        // boolean value is unchanged: acknowledge it only after re-opening.
        if (saveIq) device->close_file();
        saveIq = requested;
        if (saveIq) device->open_file();
      } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
      if (revisioned) {
        haveRecordingRevision = true;
        recordingRevision = revision;
        std::lock_guard<std::mutex> lock(statusMutex);
        recordingRequestId = revision;
      }
    };
    while (!stopping.load()) {
      if (STOP_SIGNAL) request_stop();
      if (device && inputStopped.load() && !deviceStopped) {
        try { device->stop(); }
        catch (const std::exception& error) { processing_error(error.what()); }
        try { device->close_file(); }
        catch (const std::exception& error) { processing_error(error.what()); }
        deviceStopped = true;
      }
      if (device && !inputStopped.load()) {
        const auto request = cli.Get("/capture/request");
        if (request && request->status == 200) {
          rapidjson::Document payload;
          payload.Parse(request->body.c_str());
          if (!payload.HasParseError() && payload.IsObject() &&
              payload.HasMember("recording") && payload["recording"].IsBool() &&
              payload.HasMember("revision") && payload["revision"].IsUint64()) {
            applyRecordingRequest(payload["recording"].GetBool(),
              payload["revision"].GetUint64(), true);
          }
        } else if (request && request->status == 404) {
          const auto legacy = cli.Get("/capture");
          if (legacy && legacy->status == 200 &&
              (legacy->body == "true" || legacy->body == "false"))
            applyRecordingRequest(legacy->body == "true", 0, false);
        }
      }
      cli.Post("/api/processor/status", status_json(), "application/json");
      for (unsigned i=0; i<25 && !stopping.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (device && !deviceStopped) {
      try { device->stop(); }
      catch (const std::exception& error) { processing_error(error.what()); }
      try { device->close_file(); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    }
  });
  try {
    if (replay) {
      replayOptions.receiver = type; replayOptions.channels = buffers.size();
      replayOptions.sampleRate = fs; replayOptions.frequency = fc;
      blah2::ReplayPlayer player;
      player.run(file, replayOptions, buffers, frameSamples, loop, inputStopped,
        [&](const blah2::ReplayProgress& value) {
          std::lock_guard<std::mutex> lock(statusMutex);
          replayProgress = value; replayGeneration.store(value.loops);
        }, [&] { return processingBusy.load(); });
    } else if (device) { device->start(); device->process(buffers); }
  } catch (const std::exception& error) {
    processing_error(error.what()); request_input_stop();
  }
  // Keep status available at EOF or after an input error. The browser can show
  // what happened and restart with corrected settings instead of losing the API.
  control.join();
}

void Capture::processing_error(const std::string& error) {
  std::lock_guard<std::mutex> lock(statusMutex);
  captureError = error;
  std::cerr << "Radar input: " << error << '\n';
}

std::string Capture::status_json() const {
  std::lock_guard<std::mutex> lock(statusMutex);
  rapidjson::Document document(rapidjson::kObjectType);
  auto& a = document.GetAllocator();
  auto text = [&](const char* name,const std::string& value) {
    document.AddMember(rapidjson::Value(name,a),rapidjson::Value(value.c_str(),a),a);
  };
  text("input",replay ? "replay" : "live"); text("receiver",type);
  text("state",captureError.empty() ? (replay ? replayProgress.state : "live") : "error");
  text("error",captureError); text("file",replay ? file : "");
  document.AddMember("sampleRate",fs,a); document.AddMember("frequency",fc,a);
  document.AddMember("positionSamples",replayProgress.samples,a);
  document.AddMember("totalSamples",replayProgress.total,a);
  document.AddMember("loops",replayProgress.loops,a);
  document.AddMember("trailingSamples",replayProgress.trailingSamples,a);
  const auto recording = device ? device->recording_status() : Source::RecordingStatus{};
  document.AddMember("recording",recording.active,a);
  text("recordingFile",recording.file); text("recordingError",recording.error);
  document.AddMember("recordedSamples",recording.samples,a);
  document.AddMember("recordingRequestId",recordingRequestId,a);
  rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer); return buffer.GetString();
}

std::unique_ptr<Source> Capture::factory_source(const std::string& type,
  c4::yml::NodeRef config, std::size_t channelCount)
{
    if (type == VALID_TYPE[3])
    {
      std::string heimdallHost = "127.0.0.1";
      uint16_t heimdallPort = 8091;
      if (config.has_child("heimdall"))
      {
        config["heimdall"]["host"] >> heimdallHost;
        config["heimdall"]["port"] >> heimdallPort;
      }
      return std::make_unique<Kraken>(type, fc, fs, path, &saveIq,
        channelCount, heimdallHost, heimdallPort);
    }

    // SDRplay RSPduo
#ifdef BLAH2_ENABLE_RSPDUO
    if (type == VALID_TYPE[0])
    {
        int agcSetPoint, bandwidthNumber, gainReductionA, gainReductionB, lnaState;
        bool dabNotch, rfNotch;
        config["agcSetPoint"] >> agcSetPoint;
        config["bandwidthNumber"] >> bandwidthNumber;
        config["gainReduction"][0] >> gainReductionA;
        config["gainReduction"][1] >> gainReductionB;
        config["lnaState"] >> lnaState;
        config["dabNotch"] >> dabNotch;
        config["rfNotch"] >> rfNotch;
        return std::make_unique<RspDuo>(type, fc, fs, path, &saveIq,
          agcSetPoint, bandwidthNumber, gainReductionA, gainReductionB, lnaState,
          dabNotch, rfNotch);
    }
#endif
    // Usrp
#ifdef BLAH2_ENABLE_USRP
    if (type == VALID_TYPE[1])
    {
        std::string address, subdev;
        std::vector<std::string> antenna;
        std::vector<double> gain;
        std::string _antenna;
        double _gain;
        config["address"] >> address;
        config["subdev"] >> subdev;
        config["antenna"][0] >> _antenna;
        antenna.push_back(_antenna);
        config["antenna"][1] >> _antenna;
        antenna.push_back(_antenna);
        config["gain"][0] >> _gain;
        gain.push_back(_gain);
        config["gain"][1] >> _gain;
        gain.push_back(_gain);
        return std::make_unique<Usrp>(type, fc, fs, path, &saveIq, 
          address, subdev, antenna, gain);
    }
#endif
    // HackRF
#ifdef BLAH2_ENABLE_HACKRF
    if (type == VALID_TYPE[2])
    {
      std::vector<std::string> serial;
      std::vector<uint32_t> gainLna, gainVga;
      std::vector<bool> ampEnable;
      std::string _serial;
      uint32_t gain;
      int _gain;
      bool _ampEnable;
      config["serial"][0] >> _serial;
      serial.push_back(_serial);
      config["serial"][1] >> _serial;
      serial.push_back(_serial);
      config["gain_lna"][0] >> _gain;
      gain = static_cast<uint32_t> (_gain);
      gainLna.push_back(gain);
      config["gain_lna"][1] >> _gain;
      gain = static_cast<uint32_t>(_gain);
      gainLna.push_back(gain);
      config["gain_vga"][0] >> _gain;
      gain = static_cast<uint32_t>(_gain);
      gainVga.push_back(gain);
      config["gain_vga"][1] >> _gain;
      gain = static_cast<uint32_t>(_gain);
      gainVga.push_back(gain);
      config["amp_enable"][0] >> _ampEnable;
      ampEnable.push_back(_ampEnable);
      config["amp_enable"][1] >> _ampEnable;
      ampEnable.push_back(_ampEnable);
      return std::make_unique<HackRf>(type, fc, fs, path, &saveIq,
        serial, gainLna, gainVga, ampEnable);
    }
#endif
    // handle unknown type
    std::cerr << "Error: Source type does not exist." << std::endl;
    return nullptr;
}

void Capture::set_replay(bool _loop, std::string _file, std::string format,
    uint32_t legacyBlockSamples)
{
  replay = true;
  loop = _loop;
  file = _file;
  replayOptions.format = std::move(format);
  replayOptions.legacyBlockSamples = legacyBlockSamples;
}
