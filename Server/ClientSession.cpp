#include "ClientSession.h"
#include "ChatServer.h"

#include "WS2tcpip.h"

using std::string;


constexpr int RECV_BUF = 4096;
constexpr int RECV_TIMEOUT = 3000;

ClientSession::ClientSession(SOCKET socket, ChatServer* server)
{
  m_socket = socket;
  m_server = server;
}

ClientSession::~ClientSession()
{
  Stop();
}

void ClientSession::Stop()
{
  if (m_socket != INVALID_SOCKET)
  {
    GracefulShutdown();

    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
  }
}

// Shutdown, but allow read all remaining.
void ClientSession::GracefulShutdown()
{
  shutdown(m_socket, SD_SEND);
  setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, 
    reinterpret_cast<const char*>(&RECV_TIMEOUT), sizeof(RECV_TIMEOUT));

  char buf[RECV_BUF];
  while (true)
  {
    int bytes = recv(m_socket, buf, sizeof(buf), 0);

    if (bytes == 0)
      break; // peer closed connection

    if (bytes < 0)
    {
      // e == WSAEWOULDBLOCK could be returned
      //int e = WSAGetLastError();
      break;
    }
  }
}

void ClientSession::SendMsg(const string& msg)
{
  const char* ptr = msg.data();
  size_t size = msg.size();

  while (size > 0)
  {
    int sent = send(m_socket, ptr, static_cast<int>(size), 0);

    if (sent <= 0)
      break;

    ptr += sent;
    size -= static_cast<size_t>(sent);
  }
}

// 0 - peer closed connection
// 1 - no data right now
// -1 - error occured
// int - num of bytes read
int ClientSession::ReceiveMsg()
{
  char buf[RECV_BUF];
  int bytes = recv(m_socket, buf, sizeof(buf), 0);

  if (bytes == 0)
    return 0; // closed connection

  if (bytes < 0)
  {
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK || e == WSAEINTR)
      return 1; // no data right now

    return -1; // Error
  }

  std::string msg(buf, bytes);
  m_server->BroadcastMsg(msg, this);

  return bytes;
}