#include "ChatServer.h"
#include "ClientSession.h"

#include <iostream>
#include <algorithm>
#include <vector>
#include <cstring>
#include "WS2tcpip.h"

using std::cout;
using std::cerr;

constexpr int MAX_LISTEN = 64; // backlog (Num of clients)
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

/// <summary>
/// prints a sockaddr (IPv4/IPv6) as "ip:port".
/// </summary>
/// <param name="addr">Pointer to a generic sockaddr.</param>
static void PrintSockaddr(const sockaddr* addr)
{
  char ipStr[INET6_ADDRSTRLEN] = {};
  int port = 0;

  if (addr->sa_family == AF_INET) // IPv4
  {
    const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(addr);
    if (inet_ntop(AF_INET, &(ipv4->sin_addr), ipStr, sizeof(ipStr)))
      port = ntohs(ipv4->sin_port);
  }
  else if (addr->sa_family == AF_INET6) // IPv6
  {
    const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(addr);
    if (inet_ntop(AF_INET6, &(ipv6->sin6_addr), ipStr, sizeof(ipStr)))
      port = ntohs(ipv6->sin6_port);
  }
  else
  {
    cerr << "Unknown address family\n";
  }

  cout << ipStr << ":" << port << "\n";
}

//
// === ChatServer functions ===
//

ChatServer::ChatServer(const std::vector<std::string>& ips, const std::string& port)
  : m_port(port), m_ips(ips) {}

ChatServer::~ChatServer()
{
  Stop();
}

/// <summary>
/// Starts Winsock, creates listening sockets for all IPs, switches them to non-blocking mode,
/// and enters the select() loop. This function blocks until Stop() is invoked or an error occurs.
/// </summary>
void ChatServer::Start()
{
  cout << "Strarting server...\n";

  WSADATA wsaData{};
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    cerr << "WSAStartup failed\n";
    return;
  }

  // Open non-blocking listening sockets for all configured IPs.
  for (const auto& ip : m_ips)
  {
    SOCKET s = CreateListeningSocket(ip);
    if (s != INVALID_SOCKET) 
    {
      SetNonBlocking(s);
      m_listenSockets.push_back(s);
    }
  }

  if (m_listenSockets.empty())
  {
    cerr << "Failed to create listening sockets\n";
    WSACleanup();
    return;
  }

  m_wsaInit = true;
  RunEventLoop();

  // Ensure cleanup when the loop exits.
  Stop();
}

/// <summary>
/// Stops the server, closes all sockets and client sessions, and calls WSACleanup().
/// The method is idempotent and safe to call multiple times.
/// </summary>
void ChatServer::Stop()
{
  if (!m_wsaInit && m_listenSockets.empty() && m_clients.empty())
    return;

  for (auto& s : m_listenSockets)
    SafeCloseSocket(s);
  m_listenSockets.clear();

  for (auto& kv : m_clients) 
  {
    if (kv.second) kv.second->Stop();
  }
  m_clients.clear();

  WSACleanup();
  m_wsaInit = false;
}

/// <summary>
/// Creates, binds and listens on a socket for the given IP address string.
/// Returns INVALID_SOCKET on failure.
/// </summary>
/// <param name="ip">IP address to bind (v4 or v6).</param>
SOCKET ChatServer::CreateListeningSocket(const std::string& ip)
{
  SOCKET retSocket = INVALID_SOCKET;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* result = nullptr;
  if (getaddrinfo(ip.c_str(), m_port.c_str(), &hints, &result) != 0)
    return retSocket;

  for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next)
  {
    SOCKET s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (s == INVALID_SOCKET)
      continue;

    // On Windows, prefer EXCLUSIVEADDRUSE to avoid port hijacking semantics.
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));

    if (bind(s, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == SOCKET_ERROR)
    {
      SafeCloseSocket(s);
      continue;
    }

    if (listen(s, MAX_LISTEN) == SOCKET_ERROR)
    {
      SafeCloseSocket(s);
      continue;
    }

    cout << "Server listening on: ";
    PrintSockaddr(ptr->ai_addr);
    retSocket = s;
    break;
  }

  freeaddrinfo(result);
  return retSocket;
}

/// <summary>Top-level select() loop broken into clear steps.</summary>
void ChatServer::RunEventLoop()
{
  while (m_wsaInit) 
  {
    fd_set readSet;
    BuildReadSet(readSet);

    int ready = WaitForEvents(readSet, DEFAULT_TICK_MS);
    if (!m_wsaInit) 
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

    if (ready == 0) 
      continue;

    HandleReadyListeners(readSet);
    HandleReadyClients(readSet);
    RemoveClosedClients();
  }
}

/// <summary>Builds read fd_set from listeners + clients.</summary>
void ChatServer::BuildReadSet(fd_set& outReadSet)
{
  FD_ZERO(&outReadSet);
  for (SOCKET ls : m_listenSockets) {
    if (ls != INVALID_SOCKET) FD_SET(ls, &outReadSet);
  }
  for (const auto& kv : m_clients) {
    SOCKET s = kv.first;
    if (s != INVALID_SOCKET) FD_SET(s, &outReadSet);
  }
}

/// <summary>Waits on select() with a given timeout (ms). Returns ready count or SOCKET_ERROR.</summary>
int ChatServer::WaitForEvents(fd_set& inOutReadSet, long timeoutMillis)
{
  timeval tv{};
  tv.tv_sec = timeoutMillis / 1000;
  tv.tv_usec = (timeoutMillis % 1000) * 1000;
  // nfds is ignored on Windows; pass 0
  return select(0, &inOutReadSet, nullptr, nullptr, &tv);
}

/// <summary>Accepts all pending clients from all ready listeners</summary>
void ChatServer::HandleReadyListeners(const fd_set& readSet)
{
  for (SOCKET ls : m_listenSockets) 
  {
    if (ls == INVALID_SOCKET)
      continue;
    if (!FD_ISSET(ls, const_cast<fd_set*>(&readSet)))
     continue;

    while (true)
    {
      SOCKET cs = accept(ls, nullptr, nullptr);

      if (cs == INVALID_SOCKET)
      {
        if (IsWouldBlock())
          break; // no more to accept right now

        // hard error: stop accepting on this listener for this tick
        break;
      }

      AddClient(cs);
    }
  }
}

/// <summary>Handles readable clients; schedules closures into m_toClose.</summary>
void ChatServer::HandleReadyClients(const fd_set& readSet)
{
  m_toClose.clear();

  for (auto& kv : m_clients) 
  {
    SOCKET s = kv.first;
    auto& client = kv.second;

    if (s == INVALID_SOCKET)
      continue;
    if (!FD_ISSET(s, const_cast<fd_set*>(&readSet)))
      continue;

    int n = client->ReceiveMsg();
    if (n <= 0) 
      m_toClose.push_back(s);
  }
}

/// <summary>Closes and erases clients collected in m_toClose.</summary>
void ChatServer::RemoveClosedClients()
{
  for (SOCKET s : m_toClose)
    RemoveClient(s, "recv <= 0");

  m_toClose.clear();
}

/// <summary>Adds a newly accepted client socket; sets non-blocking, logs peer, sends greeting.</summary>
void ChatServer::AddClient(SOCKET s)
{
  SetNonBlocking(s);

  // Optional greeting
  static const char* hello = "Welcome to the chat!\n";
  send(s, hello, static_cast<int>(strlen(hello)), 0);

  // Log peer address
  sockaddr addr{}; int alen = sizeof(addr);
  if (getpeername(s, &addr, &alen) == 0) 
  {
    cout << "Client connected: ";
    PrintSockaddr(&addr);
    cout << "\n";
  }

  m_clients.emplace(s, std::make_unique<ClientSession>(s, this));
}

/// <summary>Stops and erases a client; logs disconnect.</summary>
void ChatServer::RemoveClient(SOCKET s, const char* reason)
{
  auto it = m_clients.find(s);
  if (it == m_clients.end())
    return;

  sockaddr addr{}; int alen = sizeof(addr);
  if (getpeername(s, &addr, &alen) == 0) 
  {
    cout << "Client disconnected (" << (reason ? reason : "n/a") << "): ";
    PrintSockaddr(&addr);
    cout << "\n";
  }

  it->second->Stop();
  m_clients.erase(it);
}

/// <summary>
/// Broadcasts a message to all connected clients except the sender.
/// </summary>
/// <param name="msg">Message bytes to send.</param>
/// <param name="pSender">Pointer to the session that originated the message (may be null).</param>
void ChatServer::BroadcastMsg(const std::string& msg, ClientSession* pSender)
{
  for (auto& kv : m_clients)
  {
    const auto& up = kv.second;
    if (up && up.get() != pSender)
      up->SendMsg(msg);
  }

  cout << "Message broadcated:" << msg << "\n";
}
