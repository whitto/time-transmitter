#pragma once

#include "RadioHttpBounds.h"
#include <errno.h>
#include <lwip/sockets.h>

// NetworkClient::write resets its retry count after each partial send. Bound
// the entire response instead; a slow reader cannot keep loop() inside write.
static constexpr uint32_t RADIO_HTTP_RESPONSE_BUDGET_MS = 5000;

template <typename Client>
size_t radioHttpClientWrite(Client &client, const char *data, size_t length, uint32_t responseStart) {
  const int socket = client.fd();
  if (socket < 0 || socket >= FD_SETSIZE || !data) return 0;
  size_t sent = 0;
  while (sent < length && !radioHttpBudgetExpired(millis(), responseStart, RADIO_HTTP_RESPONSE_BUDGET_MS)) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket, &writable);
    timeval timeout = {0, 20000};
    const int ready = select(socket + 1, nullptr, &writable, nullptr, &timeout);
    if (ready < 0) break;
    if (!ready) { delay(1); continue; }
    const int written = ::send(socket, data + sent, length - sent, MSG_DONTWAIT);
    if (written > 0) sent += static_cast<size_t>(written);
    else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
    else delay(1);
  }
  if (sent != length) client.stop();
  return sent;
}
