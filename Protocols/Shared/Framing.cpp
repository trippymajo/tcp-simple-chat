#include "Framing.h"

#include <cstdint>
#include <Ws2tcpip.h>

//
// === Helpers ===
//

static bool recv_all(SOCKET s, char* data, int len, int flags)
{
  int read = 0;
  while (read < len)
  {
    int n = recv(s, data + read, len - read, flags);

    if (n < 0)
    {
      // Needed even for blocking
      int e = WSAGetLastError();
      if (e == WSAEINTR || e == WSAEWOULDBLOCK)
        continue;

      return false;
    }

    if (n == 0)
      return false; // closed connection

    read += n;
  }

  return true;
}


static bool send_all(SOCKET s, const char* data, int len, int flags)
{
  int sent = 0;
  while (sent < len)
  {
    int n = send(s, data + sent, len - sent, flags);

    if (n < 0)
    {
      // Needed even for blocking
      int e = WSAGetLastError();
      if (e == WSAEINTR || e == WSAEWOULDBLOCK)
        continue;

      return false;
    }

    if (n == 0)
      return false; // closed connection

    sent += n;
  }

  return true;
}

//
// === FRAMING API ===
//

static bool send_frame(SOCKET s, const std::string& payload)
{
  if (payload.empty())
    return false;

  // Cast to big-endian
  uint32_t size = static_cast<uint32_t>(payload.size());
  uint32_t nsize = htonl(size);

  // First, send num of bytes;
  // size into 4 bytes
  if (!send_all(s, reinterpret_cast<const char*>(&nsize), static_cast<int>(sizeof(nsize)), 0))
    return false;

  // Second, send payload
  if (!send_all(s, payload.data(), static_cast<int>(payload.size()), 0))
    return false;

  return true;
}

static bool recv_frame(SOCKET s, std::string& out)
{
  out.clear();

  // First read size of payload
  uint32_t nsize = 0;
  if (!recv_all(s, reinterpret_cast<char*>(&nsize), static_cast<int>(sizeof(nsize)), 0))
    return false;
  uint32_t size = ntohl(nsize);

  // DoS protection
  const uint32_t MAX_PAYLOAD = 1024u * 1024u; // 1 MB
  if (size > MAX_PAYLOAD)
    return false;

  out.resize(size);
  if (size == 0)
    return true;

  // Now read the payload
  if (!recv_all(s, out.data(), static_cast<int>(size), 0))
    return false;

  return true;
}
