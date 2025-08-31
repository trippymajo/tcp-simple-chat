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
constexpr int TIMEOUT_MS = 1000; // 1s timeout fo poll

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
  : m_ip(ipadd), m_port(port) 
{
  // may be this
  // memset(&m_poll, 0, sizeof(m_poll));
}

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

  m_running.store(true, std::memory_order::memory_order_release);

  RunLoop();
  Stop();
}

void ChatClient::Stop()
{
  cout << "Closing the connection\n";
  cout << "Type /quit to quit";
  m_running.store(false, std::memory_order::memory_order_release);

  if (m_socket == -1)
    return;

  shutdown(m_socket, SHUT_WR);
  SafeCloseSocket(m_socket);
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

void ChatClient::RunLoop()
{
  CreatePoll();

  while (m_running.load(std::memory_order::memory_order_acquire))
  {
    SetPollEvents();

    int ready = poll(&m_poll, 1, TIMEOUT_MS);

    if (ready == -1) 
    {
      if (errno == EINTR) continue; // Interrupted. Retry.
      perror("poll");
      break;
    }

    // None ready, continue asking for readiness
    if (ready == 0) continue;

    if (!HandleConnection())
      break;
  }
}

void ChatClient::CreatePoll()
{
  memset(&m_poll, 0, sizeof(m_poll));
  m_poll.fd = m_socket;
  m_poll.events = POLLIN;
}

void ChatClient::SetPollEvents()
{
  bool isEmpty = false;
  { // If something in send make it signal about it
    std::lock_guard<std::mutex>lg(m_sendMutex);
    isEmpty = m_sendQueue.empty();
  }

  isEmpty 
    ? m_poll.events &= ~POLLOUT
    : m_poll.events |= POLLOUT;
}

bool ChatClient::HandleConnection()
{
  if (m_poll.revents & (POLLERR | POLLHUP | POLLNVAL))
  {
    return false;
  }

  if (m_poll.revents & POLLIN)
  {
    if (!Read())
    {
      return false;
    }
  }

  if (m_poll.revents & POLLOUT)
  {
    if (!Write())
    {
      return false;
    }
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
      return true; // Return for Server to call Write again later
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
}
