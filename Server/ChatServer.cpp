#include "ChatServer.h"
#include "ClientSession.h"

#include <iostream>
#include <algorithm>
#include <vector>
#include <cstring>

#include <sys/socket.h> // socket(), bind(), connect(), listen(), accept()
#include <unistd.h>     // close()
#include <netinet/in.h> // sockaddr_in, htons, htonl
#include <arpa/inet.h>  // inet_ntop()
#include <netdb.h>      // getaddrinfo(), freeaddrinfo()
#include <fcntl.h> // fcntl()

using std::cout;
using std::cerr;

constexpr int MAX_LISTEN = 64; // backlog (Num of clients)
constexpr int MAX_EVENTS = 1024;

//
// === UTILS ===
//

/// <summary>
/// Safely closes a socket and sets it to INVALID_SOCKET.
/// </summary>
/// <param name="s">Reference to a socket handle.</param>
static void SafeCloseSocket(int& socketfd)
{
  if (socketfd != -1) 
  {
    close(socketfd);
    socketfd = -1;
  }
}

/// <summary>
/// Sets file descriptor to non-blocking mode.
/// </summary>
static bool SetNonBlocking(int& sfd)
{
    int flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1)
    {
      return false;
    }

    if (fcntl(sfd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
      return false;
    }

    return true;
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
    {
      port = ntohs(ipv4->sin_port);
    }
  }
  else if (addr->sa_family == AF_INET6) // IPv6
  {
    const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(addr);
    if (inet_ntop(AF_INET6, &(ipv6->sin6_addr), ipStr, sizeof(ipStr)))
    {
      port = ntohs(ipv6->sin6_port);
    }
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

  // Open non-blocking listening sockets for all configured IPs.
  for (const auto& ip : m_ips)
  {
    int sfd = CreateListenSocket(ip);
    if (sfd != -1)
    {
      if (SetNonBlocking(sfd))
      {
        m_listenSockets.insert(sfd);
      }
    }
  }

  if (m_listenSockets.empty())
  {
    cerr << "Failed to create listening sockets\n";
    return;
  }

  // Init epoll
  m_epoll = epoll_create1(EPOLL_CLOEXEC);
  if (m_epoll < 0)
  {
    perror("epoll_create1");
    Stop();
    return;
  }

  // Fill epoll with listeners
  for (const auto& lsfd : m_listenSockets)
  {
    AddListenToEpoll(lsfd);
  }

  m_running = true;
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
  if (!m_running && m_listenSockets.empty() && m_clients.empty())
  {
    return;
  }

  // Close clients
  for (auto& kv : m_clients)
  {
    if (kv.second)
    {
      kv.second->Stop();
    }
  }
  m_clients.clear();

  // Close listeners
  for (auto s : m_listenSockets)
  {
    SafeCloseSocket(s);
  }
  m_listenSockets.clear();

  if (m_epoll != -1) 
  { 
    close(m_epoll); 
    m_epoll = -1; 
  }

  m_running = false;
}

/// <summary>
/// Creates, binds and listens on a socket for the given IP address string.
/// Returns -1 on failure.
/// </summary>
/// <param name="ip">IP address to bind (v4 or v6).</param>
int ChatServer::CreateListenSocket(const std::string& ip)
{
  int retVal = -1; // Socket fd

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* result = nullptr;
  if (getaddrinfo(ip.c_str(), m_port.c_str(), &hints, &result) != 0)
  {
    return -1;
  }

  for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next)
  {
    int sfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (sfd == -1) continue;

    if (bind(sfd, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == -1)
    {
      SafeCloseSocket(sfd);
      continue;
    }

    if (listen(sfd, MAX_LISTEN) == -1)
    {
      SafeCloseSocket(sfd);
      continue;
    }

    cout << "Server listening on: ";
    PrintSockaddr(ptr->ai_addr);

    retVal = sfd;
    break;
  }

  freeaddrinfo(result);
  return retVal;
}

void ChatServer::AddListenToEpoll(const int& lsocket)
{
  epoll_event ev{};
  ev.events = EPOLLIN; // LT is OK for listen sockets
  ev.data.fd = lsocket;
  if (epoll_ctl(m_epoll, EPOLL_CTL_ADD, lsocket, &ev) < 0)
  {
    perror("epoll_ctl ADD listen");
  }
}

/// <summary>Top-level select() loop broken into clear steps.</summary>
void ChatServer::RunLoop()
{
   std::vector<epoll_event> events(MAX_EVENTS);

  while (m_running)
  {
    int n = epoll_wait(m_epoll, events.data(), static_cast<int>(events.size()), -1);
    if (n < 0)
    { // dont need to continue on 0, cos it is impossible to return such.
      if (errno == EINTR) continue; // Interrupted. Retry.
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < n; ++i)
    {
      int fd = events[i].data.fd; // why copy?
      uint32_t ev = events[i].events; // why copy?
      
        // is event Listener?
      if (m_listenSockets.count(fd) > 0)
      {
        HandleListeners(fd, ev);
        continue;
      }

      HandleClients(fd, ev);
    }
  }
}

//
// === Handlers ===
//

void ChatServer::HandleListeners(int& sfd, uint32_t& event)
{
  if (event & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
  {
    cerr << "Listener fd = " << sfd << "error/hup/nval\n";
    m_running = false;
    return;
  }

  if (event & EPOLLIN) 
  {
    AcceptAll(sfd);  // drain accept() to EAGAIN
  }
}

void ChatServer::HandleClients(int& sfd, uint32_t& event)
{
  // Errors / hangups first
  if (event & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
  {
    CloseClient(sfd);
    return;
  }

  auto it = m_clients.find(sfd);
  if (it == m_clients.end()) return;
  auto& sess = it->second;

  // Readable
  if (event & EPOLLIN)
  {
    if (!sess->Read())
    {
      CloseClient(sfd);
      return;
    }
  }

  // Writable
  if (event & EPOLLOUT)
  {
    if (!sess->Write())
    {
      CloseClient(sfd);
      return;
    }

    // If queue is empty now — drop EPOLLOUT to avoid busy wakeups
    if (!sess->IsWantSend())
    {
      ModClientWritable(sfd, false);
    }
  }
}

//
// === Handlers' helpers ===
//

void ChatServer::AddClientToEpoll(const int& clsocket)
{
  epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.data.fd = clsocket;
  ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET; // ET for clients

  auto it = m_clients.find(clsocket);
  if (it == m_clients.end())
  {
    return;
  }

  if (it->second->IsWantSend())
  {
   ev.events |= EPOLLOUT;
  }

  if (epoll_ctl(m_epoll, EPOLL_CTL_ADD, clsocket, &ev) < 0)
  {
    perror("epoll_ctl ADD client");
  }
}

void ChatServer::AcceptAll(int& fd)
{
  while (true)
  {
    int cs = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK);

    if (cs == -1)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        // No more clients
        break;
      }

      std::perror("accept4");
      break;
    }

    // Optional greeting
    static const char* hello = "Welcome to the chat!\n";
    send(cs, hello, strlen(hello), MSG_NOSIGNAL);

    // Save in clients, add to epoll
    m_clients.emplace(cs, std::make_unique<ClientSession>(cs, this));
    AddClientToEpoll(cs);

    // Log peer address
    sockaddr addr;
    size_t addLen = sizeof(addr);
    memset(&addr, 0, addLen);
    
    if (getpeername(cs, &addr, reinterpret_cast<socklen_t*>(&addLen)) == 0) 
    {
      cout << "Client connected: ";
      PrintSockaddr(&addr);
    }
  }
}

void ChatServer::ModClientWritable(int fd, const bool& enable)
{
  auto it = m_clients.find(fd);
  if (it == m_clients.end()) return;

  // Base mask for clients under ET
  uint32_t mask = EPOLLIN | EPOLLRDHUP | EPOLLET;
  if (enable)
  {
    mask |= EPOLLOUT;
  }

  epoll_event ev{};
  ev.data.fd = fd;
  ev.events  = mask;

  if (epoll_ctl(m_epoll, EPOLL_CTL_MOD, fd, &ev) < 0)
  {
    perror("epoll_ctl MOD client");
  }
}

void ChatServer::CloseClient(int& sfd)
{
  auto it = m_clients.find(sfd);
  if (it != m_clients.end())
  {
    if (it->second)
    {
      it->second->Stop();
    }
    m_clients.erase(it);
  }

  // remove socket from epoll just incase
  if (m_epoll != -1)
    epoll_ctl(m_epoll, EPOLL_CTL_DEL, sfd, nullptr);

  SafeCloseSocket(sfd);
}

void ChatServer::BroadcastMsg(const std::string& msg, ClientSession* pSender)
{
  for (auto& cli : m_clients)
  {
    const auto& s = cli.second;
    if (s && s.get() != pSender)
    {
      s->PostSend(msg);
      // Ensure we get notified to flush
      ModClientWritable(cli.first, true);
    }
  }

  cout << "Message broadcasted:" << msg << "\n";
}
