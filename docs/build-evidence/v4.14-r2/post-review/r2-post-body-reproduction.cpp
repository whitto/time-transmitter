#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
uint64_t tick=0;
void delay(uint32_t elapsed) { tick+=elapsed; }
struct NetworkClient {
  size_t remain=64;
  uint64_t next=4900;
  size_t available() const { return remain && tick>=next ? 1 : 0; }
  size_t readBytes(char* out,size_t size) {
    assert(size==1 && available()); *out='a'; --remain; next=tick+4900; return 1;
  }
};
static char *readBytesWithTimeout(NetworkClient &client, size_t maxLength, size_t &dataLength, int timeout_ms) {
  char *buf = nullptr;
  dataLength = 0;
  while (dataLength < maxLength) {
    int tries = timeout_ms;
    size_t newLength;
    while (!(newLength = client.available()) && tries--) {
      delay(1);
    }
    if (!newLength) {
      break;
    }
    if (!buf) {
      buf = (char *)malloc(newLength + 1);
      if (!buf) {
        return nullptr;
      }
    } else {
      char *newBuf = (char *)realloc(buf, dataLength + newLength + 1);
      if (!newBuf) {
        free(buf);
        return nullptr;
      }
      buf = newBuf;
    }
    client.readBytes(buf + dataLength, newLength);
    dataLength += newLength;
    buf[dataLength] = '\0';
  }
  return buf;
}
int main() {
NetworkClient client; size_t length=0;
char* result=readBytesWithTimeout(client,64,length,5000);
assert(result && length==64 && tick==313600); free(result);
printf("CONFIRMED actual pinned WebServer body reader: 64-byte form body takes %llu ms with one byte every 4900ms; exceeds R2 70000ms loop restart budget\n",(unsigned long long)tick);
}
