#include "ChatServer.h"
#include "ClientSession.h"

#include <iostream>
#include <algorithm>
#include <vector>
#include <cstring>
#include "WS2tcpip.h"

using std::cout;
using std::cerr;

// Micosoft guids
GUID guidAcceptEx = WSAID_ACCEPTEX;
GUID guidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;

constexpr int ACCEPT_DEPTH = 4;
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
      m_listenSockets.push_back(s);
  }

  if (m_listenSockets.empty())
  {
    cerr << "Failed to create listening sockets\n";
    WSACleanup();
    return;
  }

  if (!InitIocp()) 
  {
    cerr << "CreateIoCompletionPort failed\n";
    Stop();
    return;
  }

  for (SOCKET ls : m_listenSockets)
    AssociateHandle((HANDLE)ls, (ULONG_PTR)ls);

  // Post several AcceptEx per listener
  for (SOCKET ls : m_listenSockets)
  {
    for (int i = 0; i < ACCEPT_DEPTH; ++i)
      CreateAcceptSocket(ls);
  }

  m_wsaInit = true;
  RunLoop();

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

  // Close clients
  for (auto& kv : m_clients)
  {
    if (kv.second) 
      kv.second->Stop();
  }
  m_clients.clear();

  // Close listeners
  for (auto& s : m_listenSockets)
    SafeCloseSocket(s);
  m_listenSockets.clear();

  if (m_iocp)
  {
    CloseHandle(m_iocp);
    m_iocp = nullptr;
  }

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
    SOCKET s = WSASocket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol,
                         nullptr, 0, WSA_FLAG_OVERLAPPED);
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

bool ChatServer::InitIocp()
{
  m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
  return m_iocp != nullptr;
}

/// <summary>Associates a handle with IOCP using a completion key.</summary>
bool ChatServer::AssociateHandle(HANDLE h, ULONG_PTR key)
{
  return (CreateIoCompletionPort(h, m_iocp, key, 0) == m_iocp);
}

bool ChatServer::CreateAcceptSocket(SOCKET listenSock)
{
  // Load AcceptEx
  DWORD bytes = 0;
  LPFN_ACCEPTEX pAcceptEx = nullptr;
  if (WSAIoctl(listenSock, SIO_GET_EXTENSION_FUNCTION_POINTER,
    &guidAcceptEx, sizeof(guidAcceptEx),
    &pAcceptEx, sizeof(pAcceptEx),
    &bytes, nullptr, nullptr) == SOCKET_ERROR)
  {
    return false;
  }

  // Load GetAcceptExSockaddrs
  LPFN_GETACCEPTEXSOCKADDRS pGetAddrs = nullptr;
  if (WSAIoctl(listenSock, SIO_GET_EXTENSION_FUNCTION_POINTER,
    &guidGetAcceptExSockaddrs, sizeof(guidGetAcceptExSockaddrs),
    &pGetAddrs, sizeof(pGetAddrs),
    &bytes, nullptr, nullptr) == SOCKET_ERROR)
  {
    return false;
  }

  // Create accept socket with the same provider (clone WSAPROTOCOL_INFO)
  WSAPROTOCOL_INFOW pi{}; int len = sizeof(pi);
  if (getsockopt(listenSock, SOL_SOCKET, SO_PROTOCOL_INFOW,
    reinterpret_cast<char*>(&pi), &len) == SOCKET_ERROR)
  {
    return false;
  }

  SOCKET a = WSASocket(pi.iAddressFamily, pi.iSocketType, pi.iProtocol, &pi, 0, WSA_FLAG_OVERLAPPED);
  if (a == INVALID_SOCKET) 
    return false;

  // Prepare context (lives until completion)
  auto* ctx = new AcceptCtx{};
  ctx->listenSock = listenSock;
  ctx->acceptSock = a;
  ctx->pAcceptEx = pAcceptEx;
  ctx->pGetAddrs = pGetAddrs;
  const DWORD addrLen = sizeof(SOCKADDR_STORAGE) + 16;
  ctx->buf.resize(2 * addrLen + 1024);

  DWORD recvBytes = 0;
  BOOL ok = pAcceptEx(listenSock, a,
    ctx->buf.data(), 0,
    addrLen, addrLen,
    &recvBytes, &ctx->ex.ov);

  if (!ok && WSAGetLastError() != WSA_IO_PENDING)
  {
    closesocket(a);
    delete ctx;
    return false;
  }

  return true;
}

/// <summary>Top-level select() loop broken into clear steps.</summary>
void ChatServer::RunLoop()
{
  while (m_wsaInit)
  {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED pov = nullptr;

    BOOL ok = GetQueuedCompletionStatus(m_iocp, &bytes, &key, &pov, INFINITE);

    if (!pov)
    {
      //if handle closed - quit
      if (!ok) 
        break;
      continue;
    }

    auto* ex = reinterpret_cast<OvEx*>(pov);
    switch (ex->kind)
    {
      case OvEx::Kind::Accept:
      {
        auto* actx = reinterpret_cast<AcceptCtx*>(pov); // ex ? ?????? AcceptCtx
        OnAcceptComplete(actx, bytes, ok != FALSE);
        break;
      }
      case OvEx::Kind::Recv:
      {
        auto* sess = reinterpret_cast<ClientSession*>(key); // key = session*
        OnRecvComplete(sess, bytes, ok != FALSE);
        break;
      }
      case OvEx::Kind::Send:
      {
        auto* sess = reinterpret_cast<ClientSession*>(key);
        OnSendComplete(sess, bytes, ok != FALSE);
        break;
      }
    }
  }
}

/// <summary>Handles completed AcceptEx: attach socket to IOCP, start recv, repost accept.</summary>
void ChatServer::OnAcceptComplete(AcceptCtx* ctx, DWORD /*bytes*/, bool ok)
{
  SOCKET as = ctx->acceptSock;
  SOCKET ls = ctx->listenSock;

  if (!ok)
  {
    closesocket(as);
    delete ctx;
    return;
  }

  // Required for correct getsockname/getpeername/shutdown semantics
  setsockopt(as, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
    reinterpret_cast<const char*>(&ls), sizeof(ls));

  // (Optional) log remote endpoint
  SOCKADDR* local = nullptr, * remote = nullptr; int lsz = 0, rsz = 0;
  const DWORD addrLen = sizeof(SOCKADDR_STORAGE) + 16;
  ctx->pGetAddrs(ctx->buf.data(), 0, addrLen, addrLen, &local, &lsz, &remote, &rsz);
  cout << "Client connected: "; PrintSockaddr(remote); cout << "\n";

  // Create session, associate with IOCP (key = session ptr), post first recv
  auto sess = std::make_unique<ClientSession>(as, this);
  bool bAssociated = AssociateHandle((HANDLE)as, reinterpret_cast<ULONG_PTR>(sess.get()));
  if (!bAssociated || !sess->PostRecv())
  {
    sess->Stop();
    delete ctx;
    return;
  }

  m_clients.emplace(as, std::move(sess));

  // Repost a new AcceptEx on the same listener to keep backlog full
  CreateAcceptSocket(ls);

  // Free completed accept context
  delete ctx;
}

/// <summary>Handles completed WSARecv on a session: broadcast or close.</summary>
void ChatServer::OnRecvComplete(ClientSession* sess, DWORD bytes, bool ok)
{
  if (!ok || bytes == 0)
  {
    SOCKET s = sess->GetSocket();
    cout << "Client disconnected\n";
    sess->Stop();
    m_clients.erase(s);
    return;
  }

  // Broadcast to others (skip sender)
  std::string msg(sess->m_recvBuf, sess->m_recvBuf + bytes);
  BroadcastMsg(msg, sess);

  // Keep receiving
  sess->PostRecv();
}

/// <summary>Handles completed WSASend on a session: pop queue and post next if any.</summary>
void ChatServer::OnSendComplete(ClientSession* sess, DWORD /*bytes*/, bool ok)
{
  if (!ok)
  {
    SOCKET s = sess->GetSocket();
    sess->Stop();
    m_clients.erase(s);
    return;
  }
  if (!sess->m_sendQueue.empty())
    sess->m_sendQueue.pop_front();
  sess->m_sendInFlight = false;

  if (!sess->m_sendQueue.empty())
    sess->PostSend(sess->m_sendQueue.front());
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
      up->PostSend(msg);
  }

  cout << "Message broadcated:" << msg << "\n";
}
