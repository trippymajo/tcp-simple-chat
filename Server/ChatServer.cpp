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
constexpr int TIMEOUT_MS = 3000; // 3s timeout fo poll

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
        m_listenSockets.push_back(sfd);
      }
    }
  }

  if (m_listenSockets.empty())
  {
    cerr << "Failed to create listening sockets\n";
    return;
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
  for (auto& s : m_listenSockets)
  {
    SafeCloseSocket(s);
  }
  m_listenSockets.clear();

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

    // On Windows, prefer EXCLUSIVEADDRUSE to avoid port hijacking semantics.
    //bool on = TRUE;
    //setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));

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

/// <summary>Top-level select() loop broken into clear steps.</summary>
void ChatServer::RunLoop()
{
  std::vector<pollfd> pfds;
  pfds.reserve(m_listenSockets.size() + 128);

  while (m_running)
  {
    RebuildPollSet(pfds);

    int ready = poll(pfds.data(), pfds.size(), TIMEOUT_MS);

    if (ready == -1) 
    {
      if (errno == EINTR) continue; // Interrupted. Retry.
      perror("poll");
      break;
    }

    // None ready, continue asking for readiness
    if (ready == 0) 
      continue;

    HandleListeners(pfds);

    HandleClients(pfds);
  }
}

void ChatServer::RebuildPollSet(std::vector<pollfd>&pollfds)
{
  pollfds.clear();
  pollfds.reserve(m_listenSockets.size() + m_clients.size());

  // Listeners
  for (const auto& lsfd : m_listenSockets)
  {
    pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = lsfd;
    pfd.events = POLLIN;

    pollfds.push_back(pfd);
  }

  // Clients
  for (const auto& cli : m_clients)
  {
    pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = cli.first;
    pfd.events = POLLIN;

    // Find sockets with not empty buf to send
    if (cli.second && cli.second->IsWantSend())
    {
      pfd.events |= POLLOUT;
    }

    pollfds.push_back(pfd);
  }
}

//
// === Handlers ===
//

void ChatServer::HandleListeners(std::vector<pollfd>&pollfds)
{
  for (size_t i = 0; i < m_listenSockets.size(); i++)
  {
    auto& pPollfd = pollfds[i];

    if (pPollfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
      cerr << "Listener fd = " << pPollfd.fd << "error/hup/nval\n";
      m_running = false;
      return;
    }

    if (pPollfd.revents & POLLIN)
    {
      AcceptAll(pPollfd.fd);
    }
  }
}

void ChatServer::HandleClients(std::vector<pollfd>&pollfds)
{
  for (size_t i = m_listenSockets.size(); i < pollfds.size(); ++i)
  {
    auto& pfd = pollfds[i];

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
      CloseClient(pfd.fd);
      continue;
    }

    auto it = m_clients.find(pfd.fd);
    if (it == m_clients.end())
    {
      continue;
    }

    auto& sess = it->second;
    if (pfd.revents & POLLIN)
    {
      if (!sess->Read())
      {
        CloseClient(pfd.fd);
        continue;
      }
    }

    if (pfd.revents & POLLOUT)
    {
      if (!sess->Write())
      {
        CloseClient(pfd.fd);
        continue;
      }
    }
  }
}


//
// === Handlers' helpers ===
//

void ChatServer::AcceptAll(int& pfd)
{
  while (true)
  {
    int cs = accept(pfd, nullptr, nullptr);

    if (cs == -1)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        // No more clients
        break;
      }

      std::perror("accept");
      break;
    }

    SetNonBlocking(cs);

    // Optional greeting
    static const char* hello = "Welcome to the chat!\n";
    send(cs, hello, strlen(hello), MSG_NOSIGNAL);

    // Save in clients
    m_clients.emplace(cs, std::make_unique<ClientSession>(cs, this));

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

  SafeCloseSocket(sfd);
}

void ChatServer::BroadcastMsg(const std::string& msg, ClientSession* pSender)
{
  for (auto& cli : m_clients)
  {
    const auto& s = cli.second;
    if (s && s.get() != pSender)
      s->PostSend(msg);
  }

  cout << "Message broadcasted:" << msg << "\n";
}
