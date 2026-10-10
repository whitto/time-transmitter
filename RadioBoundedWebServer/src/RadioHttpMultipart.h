#pragma once

#include "RadioHttpBounds.h"
#include <string.h>

// Receive first, parse second: no socket reads or allocation occur here.
struct RadioHttpPart { size_t nameOffset, nameLength, valueOffset, valueLength; };
struct RadioHttpParts { RadioHttpPart fields[32]; size_t count = 0; };

inline bool radioHttpAsciiEqual(const char *text, size_t length, const char *expected) {
  if (length != strlen(expected)) return false;
  for (size_t i=0; i<length; ++i) {
    unsigned char a=text[i], b=expected[i];
    if (a>='A' && a<='Z') a+=32;
    if (b>='A' && b<='Z') b+=32;
    if (a!=b) return false;
  }
  return true;
}

inline size_t radioHttpFind(const char *data, size_t length, size_t start, const char *needle, size_t wanted) {
  if (wanted>length || start>length-wanted) return length;
  for (size_t i=start; i<=length-wanted; ++i)
    if (!memcmp(data+i, needle, wanted)) return i;
  return length;
}

// Returns the HTTP error code, or zero for a complete ordinary form.
inline int radioHttpParseMultipart(const char *body, size_t length, const char *boundary,
                                  size_t boundaryLength, RadioHttpParts &parts) {
  parts.count=0;
  if (!body || !boundary || !boundaryLength || boundaryLength>70 || length>RADIO_HTTP_MAX_BODY_BYTES) return 400;
  for (size_t i=0; i<boundaryLength; ++i)
    if (static_cast<unsigned char>(boundary[i])<32 || static_cast<unsigned char>(boundary[i])>126 || boundary[i]=='"') return 400;
  char marker[74]={'-','-'};
  memcpy(marker+2,boundary,boundaryLength);
  const size_t markerLength=boundaryLength+2;
  if (length<markerLength+2 || memcmp(body,marker,markerLength)) return 400;
  size_t position=markerLength;
  while (position<length) {
    if (position+2>length) return 400;
    if (!memcmp(body+position,"--",2)) {
      position+=2;
      if (position==length || (position+2==length && !memcmp(body+position,"\r\n",2))) return 0;
      return 400;
    }
    if (memcmp(body+position,"\r\n",2) || parts.count>=32) return 400;
    position+=2;
    RadioHttpPart part{};
    bool named=false;
    const size_t headerStart=position;
    while (position<length) {
      const size_t end=radioHttpFind(body,length,position,"\r\n",2);
      if (end==length || end-position>1024 || end-headerStart>1024) return 400;
      if (end==position) {position+=2;break;}
      const size_t colon=radioHttpFind(body,end,position,":",1);
      if (colon==end) return 400;
      if (radioHttpAsciiEqual(body+position,colon-position,"Content-Disposition")) {
        if (named) return 400;
        size_t value=colon+1;
        while (value<end && (body[value]==' ' || body[value]=='\t')) ++value;
        const size_t semi=radioHttpFind(body,end,value,";",1);
        if (semi==end || !radioHttpAsciiEqual(body+value,semi-value,"form-data")) return 400;
        value=semi+1;
        while (value<end && (body[value]==' ' || body[value]=='\t')) ++value;
        if (value+6>end || memcmp(body+value,"name=\"",6)) return 400;
        value+=6;
        const size_t quote=radioHttpFind(body,end,value,"\"",1);
        if (quote==end || quote==value || quote-value>64) return 400;
        for (size_t i=value; i<quote; ++i)
          if (static_cast<unsigned char>(body[i])<32 || body[i]=='\\') return 400;
        size_t remainder=quote+1;
        while (remainder<end && (body[remainder]==' ' || body[remainder]=='\t')) ++remainder;
        // Files have a filename parameter; RadioClock accepts ordinary fields.
        if (remainder!=end) return 415;
        part.nameOffset=value;part.nameLength=quote-value;named=true;
      }
      position=end+2;
    }
    if (!named) return 400;
    part.valueOffset=position;
    size_t end=position;
    for (;;) {
      end=radioHttpFind(body,length,end,"\r\n",2);
      if (end==length) return 400;
      const size_t candidate=end+2;
      if (candidate+markerLength+2<=length && !memcmp(body+candidate,marker,markerLength) &&
          (!memcmp(body+candidate+markerLength,"\r\n",2) || !memcmp(body+candidate+markerLength,"--",2))) break;
      end+=2;
    }
    part.valueLength=end-part.valueOffset;
    for (size_t i=part.valueOffset;i<end;++i) if (!body[i]) return 400;
    parts.fields[parts.count++]=part;
    position=end+2+markerLength;
  }
  return 400;
}
