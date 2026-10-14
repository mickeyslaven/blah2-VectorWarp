// CPU-only contracts for the exact context-owned production diagnostic helper.
#include "core/GpuFrameDiagnostics.private.h"
#include <iomanip>
#include <sstream>
#include <string>
#include <iostream>
class Sink final:public std::streambuf {
public:
 std::string bytes;size_t writes=0;bool shortWrite=false;
 std::streamsize xsputn(const char* p,std::streamsize n)override{
  ++writes;if(shortWrite)return n? n-1:0;
  bytes.append(p,static_cast<size_t>(n));return n;
 }
};
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main()try{
 const auto flags=std::cerr.flags();const auto precision=std::cerr.precision();
 const auto width=std::cerr.width();const auto fill=std::cerr.fill();
 gpu_diagnostics::FrameLog frame;Sink sink;std::ostream out(&sink);
 std::ostringstream expected;expected.flags(flags);expected.precision(precision);expected.fill(fill);
 frame.begin();frame.stream()<<std::setprecision(12)<<"frame="<<1<<" value="<<1.2345678901234<<'\n';
 expected<<std::setprecision(12)<<"frame="<<1<<" value="<<1.2345678901234<<'\n';
 require(sink.writes==0&&frame.active(),"Diagnostic write escaped before flush");
 frame.flush(out);require(sink.writes==1&&sink.bytes==expected.str()&&!frame.active(),"Buffered bytes/count differ");
 frame.flush(out);require(sink.writes==1,"Idle flush duplicated output");
 std::cerr.flags(out.flags());std::cerr.precision(out.precision());
 frame.begin();frame.stream()<<"second="<<9.8765432101234<<'\n';expected<<"second="<<9.8765432101234<<'\n';frame.flush(out);
 require(sink.writes==2&&sink.bytes==expected.str(),"Second frame persistent precision differs");
 frame.begin();frame.flush(out);require(sink.writes==2,"Empty frame wrote output");
 frame.begin();frame.stream()<<std::hex<<std::showbase<<std::setfill('0')<<std::setw(10)<<42<<'\n';
 expected<<std::hex<<std::showbase<<std::setfill('0')<<std::setw(10)<<42<<'\n';frame.flush(out);
 require(sink.bytes==expected.str()&&out.flags()==expected.flags()&&out.fill()==expected.fill()&&out.width()==expected.width(),"Flags/fill/width differed");
 frame.begin();bool duplicate=false;try{frame.begin();}catch(const std::logic_error&){duplicate=true;}
 require(duplicate,"Nested frame admission accepted");frame.flush(out);
 const std::string exact(gpu_diagnostics::FrameLog::capacity,'x');frame.begin();frame.stream().write(exact.data(),exact.size());
 Sink fullSink;std::ostream fullOut(&fullSink);frame.flush(fullOut);require(fullSink.writes==1&&fullSink.bytes==exact,"Exact capacity was not retained");
 frame.begin();frame.stream().write(exact.data(),exact.size());frame.stream()<<'y';bool overflow=false;
 try{frame.flush(fullOut);}catch(const std::runtime_error&){overflow=true;}
 require(overflow&&!frame.active(),"Overflow was silently accepted");
 frame.begin();frame.stream()<<"recovered\n";frame.flush(out);require(sink.bytes.substr(sink.bytes.size()-10)=="recovered\n","Overflow recovery differs");
 frame.begin();frame.stream()<<"not dropped\n";Sink failing;failing.shortWrite=true;std::ostream bad(&failing);bool writeFailure=false;
 try{frame.flush(bad);}catch(const std::runtime_error&){writeFailure=true;}
 require(writeFailure&&failing.writes==1&&!frame.active(),"Partial sink write was not rejected");
 std::cerr.flags(flags);std::cerr.precision(precision);std::cerr.width(width);std::cerr.fill(fill);
 std::cout<<"FRAME_DIAGNOSTICS_PASS bytes_exact=1 persistent_format=1 deferred_write=1 capacity=65536 overflow_rejected=1 partial_write_rejected=1\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
