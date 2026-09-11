#include "Usrp.h"
#include "UsrpSettings.h"
#include "UsrpStream.h"

#include <string.h>
#include <iostream>
#include <vector>
#include <complex>
#include <uhd/usrp/multi_usrp.hpp>
#include <cmath>
#include <stdexcept>

// constructor
Usrp::Usrp(std::string _type, uint32_t _fc, uint32_t _fs, 
  std::string _path, bool *_saveIq, std::string _address, 
  std::string _subdev, std::vector<std::string> _antenna, 
  std::vector<double> _gain)
    : Source(_type, _fc, _fs, _path, _saveIq)
{
  address = _address;
  subdev = _subdev;
  antenna = _antenna;
  gain = _gain;
  if (antenna.size() != 2 || gain.size() != 2)
    throw std::invalid_argument("[USRP] Two antenna ports and two gain values are required.");
}

void Usrp::start()
{
}

void Usrp::stop()
{
  stopRequested = true;
}

void Usrp::process(IqData *buffer1, IqData *buffer2)
{
    // create a USRP object
    uhd::usrp::multi_usrp::sptr usrp = 
      uhd::usrp::multi_usrp::make(address);

    // Set and independently read back both channels before opening the stream.
    apply_usrp_settings(*usrp, uhd::usrp::subdev_spec_t(subdev), fc, fs, gain, antenna);

    // create a receive streamer
    uhd::stream_args_t streamArgs("fc32", "sc16");
    streamArgs.channels = {0, 1};
    uhd::rx_streamer::sptr rxStreamer = usrp->get_rx_stream(streamArgs);

    // allocate buffers to receive with samples (one buffer per channel)
    const size_t samps_per_buff = rxStreamer->get_max_num_samps();
    verify_usrp_receive_capacity(samps_per_buff);
    std::vector<std::complex<float>> usrpBuffer1(samps_per_buff);
    std::vector<std::complex<float>> usrpBuffer2(samps_per_buff);

    // create a vector of pointers to point to each of the channel buffers
    std::vector<std::complex<float>*> buff_ptrs;
    buff_ptrs.push_back(&usrpBuffer1.front());
    buff_ptrs.push_back(&usrpBuffer2.front());

    // setup stream
    uhd::rx_metadata_t metadata;
    uhd::stream_cmd_t streamCmd = uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS;
    streamCmd.stream_now = false;
    streamCmd.time_spec  = usrp->get_time_now() + uhd::time_spec_t(0.05);
    rxStreamer->issue_stream_cmd(streamCmd);
    struct StopStream {
      uhd::rx_streamer::sptr stream;
      ~StopStream() {
        try { stream->issue_stream_cmd(uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS)); }
        catch (...) {} // Do not replace the original receive/processing error.
      }
    } stopStream{rxStreamer};

    while(!stopRequested)
    {
      // receive samples
      size_t nReceived = rxStreamer->recv(buff_ptrs, samps_per_buff, metadata);
      if (stopRequested) break;
      // Do not silently splice discontinuous IQ into a coherent CPI. This is
      // fail-closed error handling, not a claimed fix for unqualified B210
      // hardware/USB endurance failures reported by the upstream project.
      try { verify_usrp_receive(metadata, nReceived, samps_per_buff); }
      catch (const std::exception& error) {
        recording_discontinuity(error.what());
        throw;
      }

      buffer1->lock();
      buffer2->lock();
      for (size_t i = 0; i < nReceived; i++)
      {
        buffer1->push_back({(double)buff_ptrs[0][i].real(), (double)buff_ptrs[0][i].imag()});
        buffer2->push_back({(double)buff_ptrs[1][i].real(), (double)buff_ptrs[1][i].imag()});
      }
      buffer1->unlock();
      buffer2->unlock();

      // save IQ data to file
      if (is_recording() && nReceived) {
        blah2::IqBlock block(2);
        for (unsigned ch=0; ch<2; ++ch)
          block[ch].assign(buff_ptrs[ch], buff_ptrs[ch]+nReceived);
        record_block(block);
      }
    }
}
