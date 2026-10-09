#ifndef MIDDLEWARES_H
#define MIDDLEWARES_H

#include <RadioBoundedWebServer.h>
#include <Stream.h>

#include <assert.h>

// curl-like logging middleware
class LoggingMiddleware : public Middleware {
public:
  void setOutput(Print &output);

  bool run(RadioBoundedWebServer &server, Middleware::Callback next) override;

private:
  Print *_out = nullptr;
};

class CorsMiddleware : public Middleware {
public:
  CorsMiddleware &setOrigin(const char *origin);
  CorsMiddleware &setMethods(const char *methods);
  CorsMiddleware &setHeaders(const char *headers);
  CorsMiddleware &setAllowCredentials(bool credentials);
  CorsMiddleware &setMaxAge(uint32_t seconds);

  void addCORSHeaders(RadioBoundedWebServer &server);

  bool run(RadioBoundedWebServer &server, Middleware::Callback next) override;

private:
  String _origin = F("*");
  String _methods = F("*");
  String _headers = F("*");
  bool _credentials = true;
  uint32_t _maxAge = 86400;
};

class AuthenticationMiddleware : public Middleware {
public:
  AuthenticationMiddleware &setUsername(const char *username);
  AuthenticationMiddleware &setPassword(const char *password);
  AuthenticationMiddleware &setPasswordHash(const char *sha1AsBase64orHex);
  AuthenticationMiddleware &setCallback(RadioBoundedWebServer::THandlerFunctionAuthCheck fn);

  AuthenticationMiddleware &setRealm(const char *realm);
  AuthenticationMiddleware &setAuthMethod(HTTPAuthMethod method);
  AuthenticationMiddleware &setAuthFailureMessage(const char *message);

  bool isAllowed(RadioBoundedWebServer &server) const;

  bool run(RadioBoundedWebServer &server, Middleware::Callback next) override;

private:
  String _username;
  String _password;
  bool _hash = false;
  RadioBoundedWebServer::THandlerFunctionAuthCheck _callback;

  const char *_realm = nullptr;
  HTTPAuthMethod _method = BASIC_AUTH;
  String _authFailMsg;
};

#endif
