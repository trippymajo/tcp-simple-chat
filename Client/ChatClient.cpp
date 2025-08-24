#include "ChatClient.h"

#include <iostream>
#include "WS2tcpip.h"

using std::cout;
using std::cerr;

constexpr int RECV_BUF = 4096;
constexpr int MAX_READS_PER_TICK = 20;
constexpr long DEFAULT_TICK_MS = 1000; // 1s tick to check stop flag

//
// === UTILS ===
//

/// <summary>
/// Safely closes a socket and sets it to INVALID_SOCKET.
/// </summary>
/// <param name="s">Reference to a socket handle.</param>
static void SafeCloseSocket(SOCKET& s)
{
  if (s != INVALID_SOCKET)
  {
    closesocket(s);
    s = INVALID_SOCKET;
  }
}

/// <summary>
/// Puts a socket into non-blocking mode (FIONBIO = 1). Best effort.
/// </summary>
/// <param name="s">Socket handle.</param>
static void SetNonBlocking(SOCKET s)
{
  u_long on = 1;
  ioctlsocket(s, FIONBIO, &on);
}

/// <summary>Returns true if the last WSA error is WSAEWOULDBLOCK or WSAEINTR.</summary>
static bool IsWouldBlock()
{
  int e = WSAGetLastError();
  return (e == WSAEWOULDBLOCK || e == WSAEINTR);
}

//
// === ChatServer functions ===
//

ChatClient::ChatClient(const char* ipadd, const char* port)
  : m_ipadd(ipadd), m_port(port) {};

void ChatClient::Start()
{
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) 
  { 
    cerr << "WSAStartup failed\n"; return; 
  }
  m_isActive = true;

  if (!CreateConnection())
  {
    cerr << "Failed to connect to " << m_ipadd << m_port << "\n";
    return;
  }

  RunEventLoop();

  Stop();
}

void ChatClient::Stop()
{
  if (m_socket != INVALID_SOCKET)
    shutdown(m_socket, SD_BOTH);

  SafeCloseSocket(m_socket);

  if (m_isActive)
  { 
    WSACleanup();
    m_isActive = false;
  }
}

bool ChatClient::CreateConnection()
{
  addrinfo hints{}, *result;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  if (getaddrinfo(m_ipadd.c_str(), m_port.c_str(), &hints, &result) != 0)
  {
    cerr << "Cant resolve address for " << m_ipadd << m_port << "\n";
    return false;
  }

  for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next)
  {
    SOCKET s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (s == INVALID_SOCKET)
      continue;

    if (!ConnectToServer(s, ptr))
    {
      SafeCloseSocket(s);
      continue;
    }

    SetNonBlocking(s);

    m_socket = s;
    return true;
  }

  return false;
}

bool ChatClient::ConnectToServer(SOCKET socket, addrinfo* pInfo)
{
  if (socket == INVALID_SOCKET)
    return false;

  if (connect(socket, pInfo->ai_addr, static_cast<int>(pInfo->ai_addrlen)) != 0)
    return false;

  return true;
}

void ChatClient::RunEventLoop()
{
  while (m_isActive)
  {
    fd_set readSet, writeSet;
    BuildFdSets(readSet, writeSet);

    int ready = WaitForEvents(readSet, writeSet);

    if (!m_isActive)
      break;

    if (ready == -1)
    {
      if (!IsWouldBlock())
      {
        cerr << "select() failed, err=" << WSAGetLastError() << "\n";
        break;
      }
      continue;
    }

    if (ready == 0) // Timeout
      continue;

    if (!HandleReadyReceivers(readSet))
      break;

    if (!HandleReadyWriters(writeSet))
      break;
  }
}

void ChatClient::BuildFdSets(fd_set& rset, fd_set& wset)
{
  FD_ZERO(&rset); FD_ZERO(&wset);

  if (m_socket != INVALID_SOCKET)
  {
    FD_SET(m_socket, &rset);

    if (!m_sendQ.empty())
      FD_SET(m_socket, &wset);
  }
}

int ChatClient::WaitForEvents(fd_set& rset, fd_set& wset)
{
  timeval tv{};
  tv.tv_sec = DEFAULT_TICK_MS / 1000;
  tv.tv_usec = (DEFAULT_TICK_MS % 1000) * 1000;
  // nfds is ignored on Windows; pass 0
  return select(0, &rset, &wset, nullptr, &tv);
}

bool ChatClient::HandleReadyReceivers(const fd_set& readSet)
{
  if (!FD_ISSET(m_socket, const_cast<fd_set*>(&readSet)))
    return true;

  return ReceiveMsg() != -1;
}

bool ChatClient::HandleReadyWriters(const fd_set& writeSet)
{
  if (!FD_ISSET(m_socket, const_cast<fd_set*>(&writeSet)))
    return true;

  return SendMsg() != -1;
}

// 0 - peer closed connection
// 1 - no data right now
// -1 - error occured
int ChatClient::ReceiveMsg()
{
  char buf[RECV_BUF];

  for (int reads = 0; reads < MAX_READS_PER_TICK; ++reads)
  {
    int bytes = ::recv(m_socket, buf, sizeof(buf), 0);

    if (bytes == 0)
    {
      cout << "Server closed the connection.\n";
      return 0;
    }

    if (bytes < 0)
    {
      if (IsWouldBlock())
        return 1;

      cerr << "An error occured on reading message from Server\n";
      return -1; // Error
    }

    std::string msg(buf, bytes);
    cout << msg;
  }

  return 1;
}

void ChatClient::EnqueueSend(std::string line)
{
  if (line.empty() || line.back() != '\n')
    line.push_back('\n');

  m_sendQ.push_back({ std::move(line), 0 });
}

int ChatClient::SendMsg()
{
  if (m_socket == INVALID_SOCKET)
    return -1;

  if (m_sendQ.empty())
    return 1; // nothing to send

  Pending& front = m_sendQ.front();
  const char* p = front.data.data() + front.ofs;
  int remaining = static_cast<int>(front.data.size() - front.ofs);

  int n = send(m_socket, p, remaining, 0);
  if (n > 0)
  {
    front.ofs += n;

    if (front.ofs == front.data.size())
      m_sendQ.pop_front(); // fully sended

    return 1;
  }

  if (n == 0)
    return 1; // transient, try later

  // n < 0
  if (IsWouldBlock())
    return 1; // not ready now; will retry when writable again

  cerr << "send() failed, err=" << WSAGetLastError() << "\n";
  return -1;
}