/*
  Parsing.cpp - HTTP request parsing.

  Copyright (c) 2015 Ivan Grokhotkov. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
  Modified 8 May 2015 by Hristo Gochkov (proper post and file upload handling)
*/

#include <Arduino.h>
#include <esp32-hal-log.h>
#include <new>
#include "NetworkServer.h"
#include "NetworkClient.h"
#include "RadioBoundedWebServer.h"
#include "RadioHttpMultipart.h"
#include "RadioHttpUrlDecode.h"
#include "detail/mimetable.h"

#ifndef WEBSERVER_MAX_POST_ARGS
#define WEBSERVER_MAX_POST_ARGS 32
#endif

#define __STR(a) #a
#define _STR(a)  __STR(a)
static const char *_http_method_str[] = {
#define XX(num, name, string) _STR(name),
  HTTP_METHOD_MAP(XX)
#undef XX
};

static const char Content_Type[] PROGMEM = "Content-Type";
static const char filename[] PROGMEM = "filename";

static char *readBytesWithTimeout(NetworkClient &client, size_t maxLength, size_t &dataLength, int timeout_ms,
                                  uint32_t requestStart) {
  dataLength = 0;
  // The parser validates Content-Length before reaching this function. Retain
  // the same guard here so a future caller cannot bypass allocation bounds.
  if (maxLength > RADIO_HTTP_MAX_BODY_BYTES ||
      radioHttpBudgetExpired(millis(), requestStart, RADIO_HTTP_REQUEST_BUDGET_MS)) return nullptr;
  if (!maxLength) return nullptr;
  char *buf = static_cast<char *>(malloc(maxLength + 1));
  if (!buf) return nullptr;
  uint32_t lastActivity = millis();
  while (dataLength < maxLength) {
    if (radioHttpBudgetExpired(millis(), requestStart, RADIO_HTTP_REQUEST_BUDGET_MS) ||
        radioHttpBudgetExpired(millis(), lastActivity, static_cast<uint32_t>(timeout_ms))) break;
    const int available = client.available();
    if (available <= 0) {
      if (!client.connected()) break;
      delay(1);
      continue;
    }
    const size_t wanted = std::min(static_cast<size_t>(available), maxLength - dataLength);
    const int received = client.read(reinterpret_cast<uint8_t *>(buf + dataLength), wanted);
    if (received <= 0) { delay(1); continue; }
    dataLength += static_cast<size_t>(received);
    lastActivity = millis();
  }
  buf[dataLength] = '\0';
  return buf;
}

// Answer a request that is being dropped part-way through. The peer is usually
// still sending, and closing a connection with unread input resets it, which
// discards the response we just queued. Drain what already arrived first so the
// status actually reaches the client. The drain is bounded: a peer that keeps
// streaming is cut off.
static void sendErrorResponse(NetworkClient &client, const char *status) {
  char response[128];
  const int length = snprintf(response, sizeof(response), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
  if (length > 0 && static_cast<size_t>(length) < sizeof(response))
    radioHttpClientWrite(client, response, static_cast<size_t>(length), millis());

  const size_t maxDrain = 2 * WEBSERVER_MAX_LINE_LEN;
  uint8_t discard[64];
  size_t drained = 0;
  const uint32_t drainStart = millis();
  while (drained < maxDrain && !radioHttpBudgetExpired(millis(), drainStart, 50)) {
    int available = client.available();
    if (available <= 0) {
      yield();
      continue;
    }
    size_t wanted = (size_t)available < sizeof(discard) ? (size_t)available : sizeof(discard);
    int read = client.read(discard, wanted);
    if (read <= 0) {
      break;
    }
    drained += (size_t)read;
  }
}

enum class LineStatus {
  Ok,
  TooLong,   // reached maxLength before any line terminator
  TimedOut,  // took longer than the per-line or the caller's phase budget
  OutOfMemory,
};

// Read one CRLF-terminated protocol line, consuming the terminator.
//
// Stream::readStringUntil() bounds neither the length of the line nor the total
// time it may take: it only requires that another byte arrive within the stream
// timeout. A peer that never sends a terminator therefore grows the String until
// the heap is gone, and a peer that sends one byte just inside every timeout
// keeps the read alive for as long as it likes. Both keep handleClient() from
// returning.
//
// This bounds all three: the length of the line, the time one line may take, and
// (through phaseBudget) the time the caller's whole parse phase may take. The
// phase budget is what catches a peer that keeps sending complete but tiny lines,
// since each of those restarts the per-line budget.
static LineStatus readLineWithLimit(NetworkClient &client, String &line, size_t maxLength, unsigned long phaseStart = 0, unsigned long phaseBudget = 0) {
  line = "";
  if (!line.reserve(maxLength)) return LineStatus::OutOfMemory;
  const uint32_t idleTimeout = client.getTimeout();
  const uint32_t lineStart = millis();
  uint32_t lastActivity = lineStart;

  while (!radioHttpBudgetExpired(millis(), lastActivity, idleTimeout)) {
#if WEBSERVER_MAX_LINE_WAIT > 0
    if (radioHttpBudgetExpired(millis(), lineStart, WEBSERVER_MAX_LINE_WAIT)) {
      log_e("Protocol line still incomplete after %u ms", (unsigned)WEBSERVER_MAX_LINE_WAIT);
      return LineStatus::TimedOut;
    }
#endif
    if (phaseBudget && radioHttpBudgetExpired(millis(), phaseStart, phaseBudget)) {
      log_e("Request headers still incomplete after %u ms", (unsigned)phaseBudget);
      return LineStatus::TimedOut;
    }

    int c = client.read();
    if (c < 0) {
      yield();
      continue;
    }
    lastActivity = millis();
    if (c == '\r') {
      // Consume the paired LF, waiting for it as readStringUntil('\n') would.
      while (!radioHttpBudgetExpired(millis(), lastActivity, idleTimeout)) {
        if (radioHttpBudgetExpired(millis(), lineStart, WEBSERVER_MAX_LINE_WAIT) ||
            (phaseBudget && radioHttpBudgetExpired(millis(), phaseStart, phaseBudget)))
          return LineStatus::TimedOut;
        int next = client.peek();
        if (next < 0) {
          yield();
          continue;
        }
        if (next == '\n') {
          client.read();
        }
        break;
      }
      return LineStatus::Ok;
    }
    if (line.length() >= maxLength) {
      log_e("Protocol line longer than %u bytes", (unsigned)maxLength);
      return LineStatus::TooLong;
    }
    if (!line.concat(static_cast<char>(c))) return LineStatus::OutOfMemory;
  }
  // The peer stopped sending part-way through a line, so there is no complete
  // request to act on.
  return LineStatus::TimedOut;
}

bool RadioBoundedWebServer::_parseRequest(NetworkClient &client) {
  // Upload, raw and multipart field state all belong to a single request. Drop
  // anything left over from an earlier request on this connection so it cannot
  // poison arg()/hasArg() lookups or be reported as an active upload.
  if (_postArgs) {
    delete[] _postArgs;
    _postArgs = nullptr;
    _postArgsLen = 0;
  }
  _currentUpload.reset();
  _currentRaw.reset();

  // A fixed total deadline includes headers and body. Incoming chunks cannot
  // restart it. RadioClock does not accept multipart/raw upload callbacks.
  const unsigned long headerPhaseStart = millis();
  size_t headerBytes = 0;
  bool contentLengthSeen = false;

  // Read the first line of HTTP request
  String req;
  switch (readLineWithLimit(client, req, WEBSERVER_MAX_LINE_LEN, headerPhaseStart, WEBSERVER_MAX_HEADER_WAIT)) {
    case LineStatus::TooLong:  sendErrorResponse(client, "414 URI Too Long"); return false;
    case LineStatus::TimedOut: sendErrorResponse(client, "408 Request Timeout"); return false;
    case LineStatus::OutOfMemory: sendErrorResponse(client, "503 Service Unavailable"); return false;
    case LineStatus::Ok:       break;
  }
  headerBytes += req.length() + 2;
  //reset header value
  if (_collectAllHeaders) {
    // clear previous headers
    collectAllHeaders();
  } else {
    // clear previous headers
    for (RequestArgument *header = _currentHeaders; header; header = header->next) {
      header->value = String();
    }
  }

  // First line of HTTP request looks like "GET /path HTTP/1.1"
  // Retrieve the "/path" part by finding the spaces
  int addr_start = req.indexOf(' ');
  int addr_end = req.indexOf(' ', addr_start + 1);
  if (addr_start == -1 || addr_end == -1) {
    log_e("Invalid request: %s", req.c_str());
    return false;
  }

  String methodStr = req.substring(0, addr_start);
  String url = req.substring(addr_start + 1, addr_end);
  String versionEnd = req.substring(addr_end + 8);
  _currentVersion = atoi(versionEnd.c_str());
  String searchStr = "";
  int hasSearch = url.indexOf('?');
  if (hasSearch != -1) {
    searchStr = url.substring(hasSearch + 1);
    url = url.substring(0, hasSearch);
  }
  _currentUri = url;
  if (methodStr.length() != static_cast<size_t>(addr_start) ||
      url.length() + searchStr.length() + (hasSearch >= 0 ? 1 : 0) != static_cast<size_t>(addr_end - addr_start - 1)) {
    sendErrorResponse(client, "503 Service Unavailable"); return false;
  }
  _chunked = false;
  _clientContentLength = 0;  // not known yet, or invalid

  // Bound the request-target before it reaches route matching and argument
  // parsing. Answer with 414 so the peer sees why it was refused instead of
  // just having the connection dropped.
#if WEBSERVER_MAX_URI_LEN > 0
  if (url.length() + searchStr.length() > WEBSERVER_MAX_URI_LEN) {
    log_e("Request-target too long (%u bytes, max %u)", (unsigned)(url.length() + searchStr.length()), (unsigned)WEBSERVER_MAX_URI_LEN);
    sendErrorResponse(client, "414 URI Too Long");
    return false;
  }
#endif

  HTTPMethod method = HTTP_ANY;
  size_t num_methods = sizeof(_http_method_str) / sizeof(const char *);
  for (size_t i = 0; i < num_methods; i++) {
    if (methodStr == _http_method_str[i]) {
      method = (HTTPMethod)i;
      break;
    }
  }
  if (method == HTTP_ANY) {
    log_e("Unknown HTTP Method: %s", methodStr.c_str());
    return false;
  }
  _currentMethod = method;

  log_v("method: %s url: %s search: %s", methodStr.c_str(), url.c_str(), searchStr.c_str());

  //attach handler
  RequestHandler *handler;
  for (handler = _firstHandler; handler; handler = handler->next()) {
    if (handler->canHandle(*this, _currentMethod, _currentUri)) {
      break;
    }
  }
  _currentHandler = handler;

  // below is needed only when POST type request
  if (method == HTTP_POST || method == HTTP_PUT || method == HTTP_PATCH || method == HTTP_DELETE
#ifdef HTTP_PARSER_HAS_QUERY
      || method == HTTP_QUERY
#endif
  ) {
    String boundaryStr;
    String headerName;
    String headerValue;
    bool isEncoded = false;
    bool isMultipart = false;
    //parse headers
    while (1) {
      switch (readLineWithLimit(client, req, WEBSERVER_MAX_LINE_LEN, headerPhaseStart, WEBSERVER_MAX_HEADER_WAIT)) {
        case LineStatus::TooLong:  sendErrorResponse(client, "431 Request Header Fields Too Large"); return false;
        case LineStatus::TimedOut: sendErrorResponse(client, "408 Request Timeout"); return false;
        case LineStatus::OutOfMemory: sendErrorResponse(client, "503 Service Unavailable"); return false;
        case LineStatus::Ok:       break;
      }
      headerBytes += req.length() + 2;
      if (headerBytes > RADIO_HTTP_MAX_HEADER_BYTES) {
        sendErrorResponse(client, "431 Request Header Fields Too Large"); return false;
      }
      if (req == "") {
        break;  //no moar headers
      }
      int headerDiv = req.indexOf(':');
      if (headerDiv == -1) {
        sendErrorResponse(client, "400 Bad Request"); return false;
      }
      headerName = req.substring(0, headerDiv);
      headerValue = req.substring(headerDiv + 1);
      if (headerName.length() != static_cast<size_t>(headerDiv) ||
          headerValue.length() != req.length() - static_cast<size_t>(headerDiv) - 1) {
        sendErrorResponse(client, "503 Service Unavailable"); return false;
      }
      headerValue.trim();
      _collectHeader(headerName.c_str(), headerValue.c_str());

      if (headerName.equalsIgnoreCase(FPSTR(Content_Type))) {
        using namespace mime;
        if (headerValue.startsWith(FPSTR(mimeTable[txt].mimeType))) {
        } else if (headerValue.startsWith(F("application/x-www-form-urlencoded"))) {
          isEncoded = true;
        } else if (headerValue.startsWith(F("multipart/form-data"))) {
          const int boundaryStart = headerValue.indexOf("boundary=");
          if (boundaryStart < 0) { sendErrorResponse(client, "400 Bad Request"); return false; }
          boundaryStr = headerValue.substring(boundaryStart + 9);
          boundaryStr.trim();
          if (boundaryStr.startsWith("\"") && boundaryStr.endsWith("\""))
            boundaryStr = boundaryStr.substring(1,boundaryStr.length()-1);
          if (!boundaryStr.length() || boundaryStr.length()>70) {
            sendErrorResponse(client, "400 Bad Request"); return false;
          }
          isMultipart = true;
        } else if (headerValue.startsWith(F("multipart/"))) {
          sendErrorResponse(client, "415 Unsupported Media Type"); return false;
        }
      } else if (headerName.equalsIgnoreCase(F("Content-Length"))) {
        if (contentLengthSeen) { sendErrorResponse(client, "400 Bad Request"); return false; }
        size_t length = 0;
        const auto result = radioHttpContentLength(headerValue.c_str(), headerValue.length(), length);
        if (result != RadioHttpLengthResult::Ok) {
          sendErrorResponse(client, result == RadioHttpLengthResult::TooLarge ? "413 Payload Too Large" : "400 Bad Request");
          return false;
        }
        contentLengthSeen = true;
        _clientContentLength = static_cast<int>(length);
      } else if (headerName.equalsIgnoreCase(F("Transfer-Encoding"))) {
        sendErrorResponse(client, "400 Bad Request"); return false;
      } else if (headerName.equalsIgnoreCase(F("Host"))) {
        _hostHeader = headerValue;
      }
    }

    if (_currentHandler && _currentHandler->canRaw(*this, _currentUri)) {
      sendErrorResponse(client, "415 Unsupported Media Type"); return false;
    }
    {
      size_t plainLength;
      char *plainBuf = readBytesWithTimeout(client, _clientContentLength, plainLength, HTTP_MAX_POST_WAIT, headerPhaseStart);
      if (_clientContentLength > 0 && !plainBuf) {
        sendErrorResponse(client, radioHttpBudgetExpired(millis(), headerPhaseStart, RADIO_HTTP_REQUEST_BUDGET_MS) ?
                          "408 Request Timeout" : "503 Service Unavailable");
        return false;
      }
      if (plainLength < (size_t)_clientContentLength) {
        free(plainBuf);
        sendErrorResponse(client, "408 Request Timeout");
        return false;
      }
      if (_clientContentLength > 0) {
        if (isMultipart) {
          RadioHttpParts parts;
          const int status=radioHttpParseMultipart(plainBuf,plainLength,boundaryStr.c_str(),boundaryStr.length(),parts);
          if (status) {
            free(plainBuf); sendErrorResponse(client, status==415?"415 Unsupported Media Type":"400 Bad Request"); return false;
          }
          if (!_parseArguments(searchStr) || static_cast<size_t>(_currentArgCount)+parts.count>WEBSERVER_MAX_QUERY_ARGS) {
            free(plainBuf); sendErrorResponse(client, "503 Service Unavailable"); return false;
          }
          const size_t total=static_cast<size_t>(_currentArgCount)+parts.count;
          RequestArgument *candidate=new (std::nothrow) RequestArgument[total?total:1];
          if (!candidate) {
            free(plainBuf); sendErrorResponse(client, "503 Service Unavailable"); return false;
          }
          bool complete=true;
          for (size_t i=0;i<parts.count;++i) {
            const auto &part=parts.fields[i];
            candidate[i].key=String(plainBuf+part.nameOffset,part.nameLength);
            candidate[i].value=String(plainBuf+part.valueOffset,part.valueLength);
            complete=complete && candidate[i].key.length()==part.nameLength && candidate[i].value.length()==part.valueLength;
          }
          for (int i=0;i<_currentArgCount;++i) {
            candidate[parts.count+i].key=_currentArgs[i].key;
            candidate[parts.count+i].value=_currentArgs[i].value;
            complete=complete && candidate[parts.count+i].key.length()==_currentArgs[i].key.length() &&
              candidate[parts.count+i].value.length()==_currentArgs[i].value.length();
          }
          if (!complete) {
            delete[] candidate;free(plainBuf);sendErrorResponse(client, "503 Service Unavailable");return false;
          }
          delete[] _currentArgs;_currentArgs=candidate;_currentArgCount=static_cast<int>(total);
        } else if (isEncoded) {
          //url encoded form
          if (strlen(plainBuf) != plainLength) {
            free(plainBuf); sendErrorResponse(client, "400 Bad Request"); return false;
          }
          if (searchStr != "" && !searchStr.concat('&')) {
            free(plainBuf); sendErrorResponse(client, "503 Service Unavailable"); return false;
          }
          if (!searchStr.concat(plainBuf)) {
            free(plainBuf); sendErrorResponse(client, "503 Service Unavailable"); return false;
          }
        }
        if (!isMultipart && !_parseArguments(searchStr)) {
          free(plainBuf); sendErrorResponse(client, "503 Service Unavailable"); return false;
        }
        if (!isEncoded && !isMultipart && _currentArgs) {
          //plain post json or other data
          RequestArgument &arg = _currentArgs[_currentArgCount++];
          arg.key = F("plain");
          arg.value = String(plainBuf);
          if (arg.key.length() != 5 || arg.value.length() != plainLength) {
            free(plainBuf); sendErrorResponse(client, "503 Service Unavailable"); return false;
          }
        }

        log_v("Plain: %s", plainBuf);
        free(plainBuf);
      } else {
        // No content - but we can still have arguments in the URL.
        if (!_parseArguments(searchStr)) {
          sendErrorResponse(client, "503 Service Unavailable"); return false;
        }
      }
    }
  } else {
    String headerName;
    String headerValue;
    //parse headers
    while (1) {
      switch (readLineWithLimit(client, req, WEBSERVER_MAX_LINE_LEN, headerPhaseStart, WEBSERVER_MAX_HEADER_WAIT)) {
        case LineStatus::TooLong:  sendErrorResponse(client, "431 Request Header Fields Too Large"); return false;
        case LineStatus::TimedOut: sendErrorResponse(client, "408 Request Timeout"); return false;
        case LineStatus::OutOfMemory: sendErrorResponse(client, "503 Service Unavailable"); return false;
        case LineStatus::Ok:       break;
      }
      headerBytes += req.length() + 2;
      if (headerBytes > RADIO_HTTP_MAX_HEADER_BYTES) {
        sendErrorResponse(client, "431 Request Header Fields Too Large"); return false;
      }
      if (req == "") {
        break;  //no moar headers
      }
      int headerDiv = req.indexOf(':');
      if (headerDiv == -1) {
        sendErrorResponse(client, "400 Bad Request"); return false;
      }
      headerName = req.substring(0, headerDiv);
      headerValue = req.substring(headerDiv + 1);
      if (headerName.length() != static_cast<size_t>(headerDiv) ||
          headerValue.length() != req.length() - static_cast<size_t>(headerDiv) - 1) {
        sendErrorResponse(client, "503 Service Unavailable"); return false;
      }
      headerValue.trim();
      _collectHeader(headerName.c_str(), headerValue.c_str());

      if (headerName.equalsIgnoreCase("Transfer-Encoding")) {
        sendErrorResponse(client, "400 Bad Request"); return false;
      }
      if (headerName.equalsIgnoreCase("Content-Length")) {
        size_t length = 0;
        const auto result = radioHttpContentLength(headerValue.c_str(), headerValue.length(), length);
        if (contentLengthSeen || result != RadioHttpLengthResult::Ok || length != 0) {
          sendErrorResponse(client, result == RadioHttpLengthResult::TooLarge ? "413 Payload Too Large" : "400 Bad Request");
          return false;
        }
        contentLengthSeen = true;
      }
      if (headerName.equalsIgnoreCase("Host")) {
        _hostHeader = headerValue;
      }
    }
    if (!_parseArguments(searchStr)) {
      sendErrorResponse(client, "503 Service Unavailable"); return false;
    }
  }
  // Every response closes this connection. Do not call NetworkClient::clear():
  // continuously arriving excess bytes can keep its SDK RX drain alive forever.
  // The accepted body was read exactly to Content-Length; trailing input is
  // discarded when handleClient releases the single request's client.

  log_v("Request: %s", url.c_str());
  log_v(" Arguments: %s", searchStr.c_str());

  return true;
}

bool RadioBoundedWebServer::_collectHeader(const char *headerName, const char *headerValue) {
  RequestArgument *last = nullptr;
  for (RequestArgument *header = _currentHeaders; header; header = header->next) {
    if (header->next == nullptr) {
      last = header;
    }
    if (header->key.equalsIgnoreCase(headerName)) {
      header->value = headerValue;
      log_v("header collected: %s: %s", headerName, headerValue);
      return true;
    }
  }
  assert(last);
  if (_collectAllHeaders) {
    last->next = new RequestArgument();
    last->next->key = headerName;
    last->next->value = headerValue;
    _headerKeysCount++;
    log_v("header collected: %s: %s", headerName, headerValue);
    return true;
  }

  log_v("header skipped: %s: %s", headerName, headerValue);

  return false;
}

bool RadioBoundedWebServer::_parseArguments(const String &data) {
  int count=0;
  for (int pos=0;pos<static_cast<int>(data.length());) {
    const int end=data.indexOf('&',pos);
    const int equal=data.indexOf('=',pos);
    if (equal>=0 && (end<0 || equal<end)) {
      if (++count>WEBSERVER_MAX_QUERY_ARGS) return false;
    }
    if (end<0) break;
    pos=end+1;
  }
  // Complete candidate first; failed decoding/allocation never exposes a
  // partial credentials/settings request to any handler.
  RequestArgument *candidate=new (std::nothrow) RequestArgument[count+1];
  if (!candidate) return false;
  int actual=0;
  for (int pos=0;pos<static_cast<int>(data.length());) {
    int end=data.indexOf('&',pos);
    const int equal=data.indexOf('=',pos);
    if (end<0) end=static_cast<int>(data.length());
    if (equal>=0 && equal<end) {
      if (!radioHttpUrlDecode(data.c_str()+pos,equal-pos,candidate[actual].key) ||
          !radioHttpUrlDecode(data.c_str()+equal+1,end-equal-1,candidate[actual].value)) {
        delete[] candidate;return false;
      }
      ++actual;
    }
    pos=end+1;
  }
  delete[] _currentArgs;
  _currentArgs=candidate;_currentArgCount=actual;
  return true;
}

String RadioBoundedWebServer::urlDecode(const String &text) {
  String decoded;
  if (!radioHttpUrlDecode(text.c_str(),text.length(),decoded)) return String();
  return decoded;
}
