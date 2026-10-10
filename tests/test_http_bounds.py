#!/usr/bin/env python3
"""Fault-inject the shipped SDK-derived parser, body reader and multipart parser."""
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
try:
    from .test_bluetooth_workflow import balanced_block
except ImportError:
    from test_bluetooth_workflow import balanced_block

ROOT=Path(__file__).resolve().parents[1]
LIBRARY=ROOT/'libraries/RadioBoundedWebServer'
SOURCE=LIBRARY/'src/Parsing.cpp'

MOCKS=r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <strings.h>
#include <vector>
#include "RadioHttpBounds.h"
#include "RadioHttpMultipart.h"
#include "RadioHttpUrlDecode.h"
#define PROGMEM
#define F(x) x
#define FPSTR(x) x
#define log_e(...) ((void)0)
#define log_v(...) ((void)0)
#define WEBSERVER_MAX_LINE_WAIT 2000
#define WEBSERVER_MAX_LINE_LEN 1024
#define WEBSERVER_MAX_HEADER_WAIT RADIO_HTTP_HEADER_BUDGET_MS
#define WEBSERVER_MAX_URI_LEN 512
#define WEBSERVER_MAX_QUERY_ARGS 64
#define HTTP_MAX_POST_WAIT 5000
uint32_t tick=100;
uint32_t millis(){return tick;}
void delay(uint32_t n){tick+=n;}
void yield(){delay(1);}
static bool failStringReserve=false;
static bool failStringCopy=false;
static int successfulStringReserves=-1;
class String {
 public:
  std::string s;
  String(const char* value=""):s(value){}
  String(const char* value,unsigned int length):s(value,length){}
  String(std::string value):s(std::move(value)){}
  String(const String& other):s(other.s){}
  String& operator=(const String& other){s=failStringCopy?"":other.s;return *this;}
  size_t length()const{return s.length();}
  const char* c_str()const{return s.c_str();}
  bool reserve(size_t length){if(failStringReserve || successfulStringReserves==0)return false;if(successfulStringReserves>0)--successfulStringReserves;s.reserve(length);return true;}
  bool concat(char value){s+=value;return true;}
  bool concat(const char* value){s+=value;return true;}
  void trim(){auto a=s.find_first_not_of(" \t\r\n");s=a==s.npos?"":s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
  int indexOf(char value,int from=0)const{auto i=s.find(value,from);return i==s.npos?-1:int(i);}
  int indexOf(const char* value,int from=0)const{auto i=s.find(value,from);return i==s.npos?-1:int(i);}
  String substring(size_t a,size_t b=std::string::npos)const{return String(s.substr(std::min(a,s.size()),b==s.npos?s.npos:b-a));}
  bool equalsIgnoreCase(const char* value)const{return strcasecmp(s.c_str(),value)==0;}
  bool startsWith(const char* value)const{return s.rfind(value,0)==0;}
  bool endsWith(const char* value)const{size_t n=strlen(value);return s.size()>=n&&s.compare(s.size()-n,n,value)==0;}
  String& operator+=(char value){s+=value;return *this;}
  friend bool operator==(const String&a,const char*b){return a.s==b;}
  friend bool operator!=(const String&a,const char*b){return !(a==b);}
};
struct Event{uint32_t after;std::string bytes;};
struct NetworkClient {
  std::deque<Event> events;
  std::string buffer,response;
  size_t offset=0,reads=0;
  bool live=true,continuous=false;
  void arrive(){while(!events.empty()&&static_cast<int32_t>(tick-events.front().after)>=0){buffer+=events.front().bytes;events.pop_front();}}
  int available(){arrive();return continuous?64:int(buffer.size()-offset);}
  int read(){arrive();if(continuous){++reads;return 'x';}return offset<buffer.size()?static_cast<unsigned char>(buffer[offset++]):-1;}
  int peek(){arrive();return offset<buffer.size()?static_cast<unsigned char>(buffer[offset]):-1;}
  int read(uint8_t* out,size_t wanted){arrive();size_t n=std::min(wanted,size_t(available()));if(!n)return -1;
    if(continuous)std::memset(out,'x',n);else{std::memcpy(out,buffer.data()+offset,n);offset+=n;}reads+=n;return int(n);}
  bool connected(){return live;}
  uint32_t getTimeout(){return 5000;}
  void clear(){assert(false&&"SDK unbounded trailing-data clear must never run");}
};
template<typename Client>size_t radioHttpClientWrite(Client& client,const char* text,size_t n,uint32_t){client.response.append(text,n);return n;}
static size_t bodyAllocations=0,maxBodyAllocation=0;
static bool failBodyAllocation=false;
void* radioTestMalloc(size_t length){++bodyAllocations;maxBodyAllocation=std::max(maxBodyAllocation,length);return failBodyAllocation?nullptr:std::malloc(length);}
enum HTTPMethod{HTTP_GET,HTTP_POST,HTTP_PUT,HTTP_PATCH,HTTP_DELETE,HTTP_ANY};
static const char* _http_method_str[]={"GET","POST","PUT","PATCH","DELETE","ANY"};
static const char Content_Type[]="Content-Type";
namespace mime{constexpr int txt=0;struct Mime{const char* mimeType;} mimeTable[]={{"text/plain"}};}
class RadioBoundedWebServer;
struct RequestHandler{bool raw=false;RequestHandler* next(){return nullptr;}bool canHandle(RadioBoundedWebServer&,HTTPMethod,const String&){return true;}bool canRaw(RadioBoundedWebServer&,const String&){return raw;}};
class RadioBoundedWebServer {
public:
 struct RequestArgument{String key,value;RequestArgument* next=nullptr;};
 RequestArgument *_postArgs=nullptr,*_currentHeaders=nullptr,*_currentArgs=nullptr;
 int _postArgsLen=0,_currentArgCount=0,_clientContentLength=0;
 bool _collectAllHeaders=false,_chunked=false;
 uint8_t _currentVersion=0;
 HTTPMethod _currentMethod=HTTP_ANY;
 String _currentUri,_hostHeader;
 RequestHandler* _firstHandler=nullptr,*_currentHandler=nullptr;
 std::unique_ptr<int> _currentUpload,_currentRaw;
 ~RadioBoundedWebServer(){delete[] _postArgs;delete[] _currentArgs;}
 void collectAllHeaders(){}
 bool _collectHeader(const char*,const char*){return false;}
 bool _parseArguments(const String&);
 bool checkedArg(const char*,String&)const;
 bool _parseRequest(NetworkClient&);
};
'''

DRIVER=r'''
static std::string headers(const char* method,const char* type,const std::string& length){return std::string(method)+" /api/config HTTP/1.1\r\nHost: esp\r\nContent-Type: "+type+"\r\nContent-Length: "+length+"\r\n\r\n";}
static void reset(){tick=100;bodyAllocations=0;maxBodyAllocation=0;failBodyAllocation=false;failStringReserve=false;failStringCopy=false;successfulStringReserves=-1;}
static void expectRejected(const std::string& request,const char* status){reset();NetworkClient client;client.buffer=request;RadioBoundedWebServer server;
  assert(!server._parseRequest(client));assert(client.response.find(status)!=std::string::npos);assert(!bodyAllocations);assert(tick<6000);}
int main(){
  expectRejected(headers("POST","text/plain","429496729599999999999"),"413");
  expectRejected(headers("POST","text/plain","4097"),"413");
  expectRejected(headers("POST","text/plain","-1"),"400");
  expectRejected(headers("POST","text/plain","64junk"),"400");
  expectRejected("POST /api/config HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx","400");
  expectRejected("POST /api/config HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx","400");
  expectRejected("GET /api/config HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n","400");
  expectRejected("GET /api/config HTTP/1.1\r\nContent-Length: 1\r\n\r\nx","400");
  // Actual reader: the R2 reproduction took313600ms. The same slow64-byte
  // body now returns after10000ms; its partially received value is never used.
  reset();NetworkClient slow;slow.buffer=headers("POST","text/plain","64");
  for(int i=1;i<=64;++i)slow.events.push_back({uint32_t(100+i*4900),"a"});
  RadioBoundedWebServer server;assert(!server._parseRequest(slow));
  assert(slow.response.find("408")!=std::string::npos);assert(tick==10150);
  assert(bodyAllocations==1&&maxBodyAllocation==65&&server._currentArgCount==0);
  reset();NetworkClient combined;combined.buffer="POST /api/config HTTP/1.1\r\n";
  combined.events={{2000,"Content-Length: 64\r\n"},{3900,"Host: esp\r\n"},{4500,"\r\n"},{8100,"a"},{13000,"a"}};
  RadioBoundedWebServer combinedServer;assert(!combinedServer._parseRequest(combined));assert(tick==10150);
  // Header and body use the same total budget, including millis rollover.
  reset();tick=UINT32_MAX-4000;NetworkClient rollover;rollover.buffer=headers("POST","text/plain","64");
  uint32_t begin=tick;for(int i=1;i<=64;++i)rollover.events.push_back({uint32_t(begin+i*4900),"a"});
  RadioBoundedWebServer rolloverServer;assert(!rolloverServer._parseRequest(rollover));assert(uint32_t(tick-begin)==10050);
  // Header line and total-header limits fire before body allocation.
  expectRejected("POST /api/config HTTP/1.1\r\nX-Large: "+std::string(1100,'x')+"\r\n\r\n","431");
  std::string many="POST /api/config HTTP/1.1\r\n";for(int i=0;i<100;++i)many+="X-Extra: "+std::string(50,'x')+"\r\n";
  expectRejected(many+"\r\n","431");
  reset();NetworkClient lineSlow;lineSlow.buffer="POST /api/config HTTP/1.1\r\nX-Test:";
  for(int i=1;i<80;++i)lineSlow.events.push_back({uint32_t(100+i*1000),"a"});
  RadioBoundedWebServer lineServer;assert(!lineServer._parseRequest(lineSlow));assert(tick==2150&&bodyAllocations==0);
  reset();NetworkClient crSlow;crSlow.buffer="POST /api/config HTTP/1.1\r";
  RadioBoundedWebServer crServer;assert(!crServer._parseRequest(crSlow));assert(tick==2150&&bodyAllocations==0);
  // Allocation failure closes admission rather than parsing partial headers.
  reset();failStringReserve=true;NetworkClient noString;noString.buffer=headers("POST","text/plain","1")+"x";
  RadioBoundedWebServer noStringServer;assert(!noStringServer._parseRequest(noString));assert(noString.response.find("503")!=std::string::npos&&bodyAllocations==0);
  reset();failBodyAllocation=true;NetworkClient noMemory;noMemory.buffer=headers("POST","text/plain","1")+"x";
  RadioBoundedWebServer noMemoryServer;assert(!noMemoryServer._parseRequest(noMemory));assert(noMemoryServer._currentArgCount==0&&bodyAllocations==1);
  // Reader respects exact remaining length and actual short read counts.
  reset();NetworkClient exact;exact.buffer=headers("POST","text/plain","4096")+std::string(4096,'x')+std::string(50000,'z');
  RadioBoundedWebServer exactServer;assert(exactServer._parseRequest(exact));assert(exactServer._currentArgs[0].value.length()==4096);
  assert(exact.available()==50000&&maxBodyAllocation==4097&&tick==100);
  // GET trailing data cannot invoke the SDK's unbounded clear loop.
  reset();NetworkClient excess;excess.buffer="GET /api/config HTTP/1.1\r\n\r\n";
  RadioBoundedWebServer excessServer;assert(excessServer._parseRequest(excess));excess.continuous=true;
  assert(tick==100&&excess.reads==0);
  // Existing browser FormData is preserved, including empty fields, UTF-8 and
  // schedule JSON containing line breaks/boundary-like text within a value.
  const std::string multipart="--test-boundary\r\nContent-Disposition: form-data; name=\"ssid\"\r\n\r\n東京é\r\n--test-boundary\r\nContent-Disposition: form-data; name=\"password\"\r\n\r\n\r\n--test-boundary\r\nContent-Disposition: form-data; name=\"schedules\"\r\n\r\n[{\"start\":0,\"end\":60}]\nmore\r\n--test-boundary-prefix\r\n--test-boundary--\r\n";
  reset();NetworkClient form;form.buffer=headers("POST","multipart/form-data; boundary=test-boundary",std::to_string(multipart.size()))+multipart;
  RadioBoundedWebServer formServer;assert(formServer._parseRequest(form));assert(formServer._currentArgCount==3);
  assert(formServer._currentArgs[0].key=="ssid"&&formServer._currentArgs[0].value=="東京é");assert(formServer._currentArgs[1].value.length()==0);
  assert(formServer._currentArgs[2].value.s.find("boundary-prefix")!=std::string::npos);
  RadioHttpParts parts;assert(radioHttpParseMultipart(multipart.data(),multipart.size()-10,"test-boundary",13,parts)==400);
  auto file=multipart;auto position=file.find("name=\"ssid\"");file.insert(position+11,"; filename=\"file.bin\"");
  assert(radioHttpParseMultipart(file.data(),file.size(),"test-boundary",13,parts)==415);
  std::string tooMany;for(int i=0;i<33;++i)tooMany+="--x\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nb\r\n";tooMany+="--x--\r\n";
  assert(radioHttpParseMultipart(tooMany.data(),tooMany.size(),"x",1,parts)==400);
  reset();NetworkClient encoded;encoded.buffer=headers("POST","application/x-www-form-urlencoded","11")+"ssid=a&x=12";
  RadioBoundedWebServer encodedServer;assert(encodedServer._parseRequest(encoded));assert(encodedServer._currentArgCount==2);
  // Actual argument decoder: no partially decoded fields are installed.
  reset();RadioBoundedWebServer argServer;assert(argServer._parseArguments("ssid=previous&password=retained"));
  successfulStringReserves=3;assert(!argServer._parseArguments("ssid=new&password=new-secret"));
  assert(argServer._currentArgCount==2&&argServer._currentArgs[0].value=="previous"&&argServer._currentArgs[1].value=="retained");
  successfulStringReserves=-1;assert(!argServer._parseArguments("ssid=a%00b"));assert(!argServer._parseArguments("ssid=a%zz"));
  assert(argServer._parseArguments("ssid=%E6%9D%B1%E4%BA%AC%C3%A9"));assert(argServer._currentArgs[0].value=="東京é");
  String checked;assert(argServer.checkedArg("ssid",checked)&&checked=="東京é");failStringCopy=true;
  assert(!argServer.checkedArg("ssid",checked)&&argServer._currentArgs[0].value=="東京é");failStringCopy=false;
  std::puts("Actual pinned parser/reader: bounded request, rollover, body allocation, header faults, multipart and excess input PASS");
}
'''

class HttpBoundsTest(unittest.TestCase):
    def test_shipped_parser_faults(self):
        source=SOURCE.read_text()
        reader=balanced_block(source,source.index('static char *readBytesWithTimeout('))
        errors=balanced_block(source,source.index('static void sendErrorResponse('))
        enum=source[source.index('enum class LineStatus {'):source.index('};',source.index('enum class LineStatus {'))+2]
        lines=balanced_block(source,source.index('static LineStatus readLineWithLimit('))
        parser=balanced_block(source,source.index('bool RadioBoundedWebServer::_parseRequest('))
        arguments=balanced_block(source,source.index('bool RadioBoundedWebServer::_parseArguments('))
        server=(LIBRARY/'src/RadioBoundedWebServer.cpp').read_text()
        copied=balanced_block(server,server.index('bool RadioBoundedWebServer::checkedArg('))
        unit=MOCKS+'\n#define malloc radioTestMalloc\n'+reader+'\n#undef malloc\n'+errors+'\n'+enum+'\n'+lines+'\n'+parser+'\n'+arguments+'\n'+copied+'\n'+DRIVER
        with tempfile.TemporaryDirectory(prefix='radioclock-http-bounds-') as tmp:
            src,binary=Path(tmp)/'http.cpp',Path(tmp)/'http'
            src.write_text(unit)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-O1','-I',str(LIBRARY/'src'),str(src),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('multipart and excess input PASS',result.stdout)

    def test_response_partial_send_budget(self):
        driver=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cerrno>
uint32_t tick=100;uint32_t millis(){return tick;}void delay(uint32_t n){tick+=n;}
#define send radioTestSend
#define select radioTestSelect
#include "RadioHttpIo.h"
#undef send
#undef select
static bool failSend=false;
extern "C" int radioTestSelect(int,fd_set*,fd_set*,fd_set*,timeval*){return 1;}
extern "C" ssize_t radioTestSend(int,const void*,size_t,int flags){assert(flags==MSG_DONTWAIT);if(failSend){errno=EPIPE;return -1;}tick+=100;return 1;}
struct Client{bool stopped=false;int fd(){return 1;}void stop(){stopped=true;}};
int main(){Client client;const uint32_t begin=tick;char data[1024]{};
assert(radioHttpClientWrite(client,data,sizeof(data),begin)==50);assert(tick-begin==5000&&client.stopped);
client.stopped=false;assert(radioHttpClientWrite(client,data,10,begin)==0&&client.stopped);
tick=UINT32_MAX-1000;const uint32_t rollover=tick;client.stopped=false;
assert(radioHttpClientWrite(client,data,sizeof(data),rollover)==50&&uint32_t(tick-rollover)==5000&&client.stopped);
failSend=true;assert(radioHttpClientWrite(client,data,1,tick)==0&&client.stopped);
std::puts("Actual response writer: absolute partial-send deadline and rollover PASS");}
'''
        with tempfile.TemporaryDirectory(prefix='radioclock-http-write-') as tmp:
            include=Path(tmp)/'lwip';include.mkdir()
            (include/'sockets.h').write_text('#include <sys/socket.h>\n#include <sys/select.h>\n')
            src,binary=Path(tmp)/'write.cpp',Path(tmp)/'write'
            src.write_text(driver)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-fno-omit-frame-pointer','-no-pie','-O1','-I',tmp,'-I',str(LIBRARY/'src'),str(src),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('partial-send deadline and rollover PASS',result.stdout)

    def test_unique_dependency_manifest(self):
        subprocess.run(['python3',str(ROOT/'scripts/validate-bounded-webserver-source.py')],check=True)
        firmware=(ROOT/'firmware/RadioClock_V4_16/RadioClock_V4_16.ino').read_text()
        self.assertIn('#include <RadioBoundedWebServer.h>',firmware)
        self.assertNotIn('#include <WebServer.h>',firmware)
        spec=importlib.util.spec_from_file_location('bounded_source_validator',ROOT/'scripts/validate-bounded-webserver-source.py')
        validator=importlib.util.module_from_spec(spec);spec.loader.exec_module(validator)
        with tempfile.TemporaryDirectory(prefix='radioclock-http-pin-') as tmp:
            copied=Path(tmp)/'library';shutil.copytree(LIBRARY,copied)
            changed=copied/'src/Parsing.cpp';prior=changed.read_bytes();changed.write_bytes(prior+b'\n// unreviewed\n')
            with self.assertRaises(AssertionError):validator.validate(copied)
            changed.write_bytes(prior);extra=copied/'unexpected.bin';extra.write_bytes(b'not source')
            with self.assertRaises(AssertionError):validator.validate(copied)
            extra.unlink();(copied/'src/RadioHttpBounds.h').unlink()
            with self.assertRaises(AssertionError):validator.validate(copied)

    def test_browser_ssid_utf8_admission(self):
        source=(ROOT/'ui/radioclock.html').read_text()
        functions='\n'.join(balanced_block(source,source.index(prefix)) for prefix in ['function ssidByteLength(', 'async function saveWifi('])
        driver=r'''
const assert=require('node:assert/strict');let posts=[],messages=[];const elements={ssid:{value:''},password:{value:'new-password'}};
const $=name=>elements[name];function toast(message,success){messages.push([message,success])}async function postConfig(fields){posts.push(fields)}
(async()=>{for(const value of ['x'.repeat(32),'é'.repeat(16),'😀'.repeat(8)]){elements.ssid.value=value;await saveWifi();assert.equal(posts.at(-1).ssid,value);assert.equal(ssidByteLength(value),32)}
const before=posts.length;for(const value of ['x'.repeat(33),'é'.repeat(17),'😀'.repeat(8)+'x','line\nname','nul\0name']){elements.ssid.value=value;await saveWifi()}
assert.equal(posts.length,before);assert.equal(messages.length,5);assert.ok(messages.every(x=>x[1]===false));console.log('UTF-8 SSID boundary PASS')})().catch(e=>{console.error(e);process.exit(1)})
'''
        result=subprocess.run(['node','-e',functions+'\n'+driver],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('UTF-8 SSID boundary PASS',result.stdout)

if __name__=='__main__':unittest.main()
