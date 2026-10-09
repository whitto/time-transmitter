#pragma once

#include <stddef.h>

inline int radioHttpHex(char value) {
  if (value>='0' && value<='9') return value-'0';
  if (value>='a' && value<='f') return value-'a'+10;
  if (value>='A' && value<='F') return value-'A'+10;
  return -1;
}

template<typename Output>
bool radioHttpUrlDecode(const char *data, size_t length, Output &result) {
  result="";
  if (!result.reserve(length)) return false;
  for (size_t i=0;i<length;++i) {
    char byte=data[i];
    if (byte=='+') byte=' ';
    else if (byte=='%') {
      if (i+2>=length) return false;
      const int a=radioHttpHex(data[i+1]),b=radioHttpHex(data[i+2]);
      if (a<0 || b<0) return false;
      byte=static_cast<char>(a*16+b);i+=2;
    }
    if (!byte || !result.concat(byte)) return false;
  }
  return true;
}
