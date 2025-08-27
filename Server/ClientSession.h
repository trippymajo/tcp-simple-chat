#pragma once

#include <string>
#include <deque>
#include <winsock2.h>

#include "Shared.h"

class ChatServer;

/// <summary>
/// Client session with overlapped recv/send and a send queue.
/// </summary>
class ClientSession
{
public:
  ClientSession(SOCKET socket, ChatServer* server);
  ~ClientSession();

  void Stop();
  bool PostRecv();
  bool PostSend(const std::string& msg);

  SOCKET GetSocket() const { return m_socket; }

  // Exposed to server's completion loop:
  OvEx   m_ovRecv;
  OvEx   m_ovSend;
  WSABUF m_recvBufWsa{};
  char   m_recvBuf[64 * 1024]{};

  std::deque<std::string> m_sendQueue;
  bool m_sendInFlight = false;

private:
  SOCKET      m_socket = INVALID_SOCKET;
  ChatServer* m_server = nullptr;
};