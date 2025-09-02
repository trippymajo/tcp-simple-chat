#include "ChatClient.h"

#include <iostream>
#include <cstring>
#include <vector>

#include <sys/socket.h> // socket(), bind(), connect(), listen(), accept()
#include <unistd.h>     // close()
#include <netinet/in.h> // sockaddr_in, htons, htonl
#include <arpa/inet.h>  // inet_ntop()
#include <netdb.h>      // getaddrinfo(), freeaddrinfo()
#include <fcntl.h>      // fcntl()

using std::cout;
using std::cerr;

constexpr int RECV_BUF = 4096;
constexpr int MAX_EVENTS = 16; // more than enough for single fd

//
// === UTILS ===
//

static void SafeCloseSocket(int& sfd)
{
  if (sfd != -1)
  {
    close(sfd);
    sfd = -1;
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

//
// === ChatClient functions ===
//

ChatClient::ChatClient(const char* ipadd, const char* port)
  : m_ip(ipadd), m_port(port) {}

ChatClient::~ChatClient()
{
  Stop();
}

void ChatClient::Start()
{
  if (!CreateConnection())
  {
    cerr << "Failed to establish connection to "
        << m_ip << ":" << m_port << "\n";
    Stop();
    return;
  }

  if (!SetNonBlocking(m_socket))
  {
    cerr << "Failed to make socket non blocking";
    Stop();
    return;
  }

  m_running.store(true, std::memory_order_release);

  CreateEpoll();
  AddSockToEpoll(); 

  RunLoop();
  Stop();
}

void ChatClient::Stop()
{
  cout << "Closing the connection\n";
  cout << "Type /quit to quit";
  m_running.store(false, std::memory_order::memory_order_release);

  if (m_socket == -1)
  {
    if (m_epoll != -1) 
    { 
      close(m_epoll); m_epoll = -1; 
    }
    return;
  }

  shutdown(m_socket, SHUT_WR);
  SafeCloseSocket(m_socket);

  if (m_epoll != -1)
  {
    close(m_epoll);
    m_epoll = -1;
  }
}

bool ChatClient::CreateConnection()
{
  addrinfo hints, *result;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  if (getaddrinfo(m_ip, m_port, &hints, &result) != 0)
  {
    cerr << "Cant resolve address for " << m_ip << ":" << m_port << "\n";
    return false;
  }

  // Try connect
  for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next)
  {
    int sfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (sfd == -1) continue;

    if (connect(sfd, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) != 0)
    {
      SafeCloseSocket(sfd);
      continue;
    }

    m_socket = sfd;
    break;
  }

  freeaddrinfo(result);

  return m_socket != -1;
}

void ChatClient::CreateEpoll()
{
  m_epoll = epoll_create1(EPOLL_CLOEXEC);
  if (m_epoll < 0)
  {
    perror("epoll_create1");
  }
}

void ChatClient::RunLoop()
{
  std::vector<epoll_event> events(MAX_EVENTS);

  while (m_running.load(std::memory_order::memory_order_acquire))
  {
    int n = epoll_wait(m_epoll, events.data(), (int)events.size(), -1);
    if (n < 0)
    {
      if (errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < n; ++i)
    {
      uint32_t ev = events[i].events;
      if (!HandleConnection(ev))
      {
        m_running.store(false, std::memory_order::memory_order_release);
        break;
      }
    }
  }
}

void ChatClient::AddSockToEpoll()
{
  epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.data.fd = m_socket;
  ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;

  if (epoll_ctl(m_epoll, EPOLL_CTL_ADD, m_socket, &ev) < 0)
  {
    perror("epoll_ctl ADD client-sock");
  }
}

bool ChatClient::HandleConnection(uint32_t& ev)
{
  if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
  {
    return false;
  }

  if (ev & EPOLLIN)
  {
    if (!Read())
    {
      return false;
    }
  }

  if (ev & EPOLLOUT)
  {
    if (!Write())
    {
      return false;
    }

    // If queue is empty no need to epollout still be raised
    std::lock_guard<std::mutex> lg(m_sendMutex);
    if (m_sendQueue.empty())
      ModWritable(false);
  }

  return true;
}

bool ChatClient::Read()
{
  char buf[RECV_BUF];

  while (true)
  {
    ssize_t bytes = recv(m_socket, buf, static_cast<size_t>(RECV_BUF), 0);

    // peer closed connection
    if (bytes == 0) return false;

    // Error occured
    if (bytes < 0)
    {
      // Stream of recv fully read
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        return true;
      }

      std::perror("recv");
      return false;
    }

    std::string msg(buf, buf + bytes);
    cout << msg;
  }
}

bool ChatClient::Write()
{
  std::lock_guard<std::mutex>lg(m_sendMutex);

  while (!m_sendQueue.empty())
  {
    std::string& msg = m_sendQueue.front();
    ssize_t bytes = send(m_socket, msg.c_str(), msg.size(), MSG_NOSIGNAL);

    // peer closed connection
    if (bytes == 0) return false;

    // Error occured
    if (bytes < 0)
    {
      // Stream of recv fully read or try again later
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        return true;
      }

      std::perror("send");
      return false;
    }

    // Half part of msg was sent
    if (bytes < static_cast<ssize_t>(msg.size()))
    {
      msg.erase(0, static_cast<size_t>(bytes));
     // Still something to send? 
      ModWritable(true);
      return true;
    }

    m_sendQueue.pop_front();
  }

  // All was read and send queue is free to go
  return true;
}

void ChatClient::Send(const std::string& msg)
{
  if (msg.empty())
    return;

  {
    std::lock_guard<std::mutex> lg(m_sendMutex);
    m_sendQueue.push_back(msg);
  }

  // Trigger sending event
  ModWritable(true);
}

void ChatClient::ModWritable(const bool& enable)
{
  if (m_epoll == -1 || m_socket == -1)
  { 
    return;
  }

  uint32_t event = EPOLLIN | EPOLLRDHUP | EPOLLET;
  {
    std::lock_guard<std::mutex> lg(m_sendMutex);
    if (enable && !m_sendQueue.empty())
    {
      event |= EPOLLOUT;
    }
  }

  epoll_event ev{};
  ev.data.fd = m_socket;
  ev.events  = event;
  if (epoll_ctl(m_epoll, EPOLL_CTL_MOD, m_socket, &ev) < 0)
  {
    perror("epoll_ctl MOD client-sock");
  }
}