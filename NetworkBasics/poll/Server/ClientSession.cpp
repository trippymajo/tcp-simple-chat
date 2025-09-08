#include "ClientSession.h"
#include "ChatServer.h"

#include <sys/socket.h> // socket(), bind(), connect(), listen(), accept()
#include <unistd.h>     // close()

constexpr int RECV_BUF = 4096;

//
// === ClientSession functions ===
//

ClientSession::ClientSession(int& sfd, ChatServer* server)
  : m_socket(sfd), m_server(server) {}

ClientSession::~ClientSession()
{
  Stop();
}

void ClientSession::Stop()
{
  if (m_socket != -1)
  {
    GracefulShutdown();
    close(m_socket);
    m_socket = -1;
  }
}

void ClientSession::GracefulShutdown()
{
  shutdown(m_socket, SHUT_WR);

  char buf[RECV_BUF];
  while (true)
  {
    ssize_t bytes = recv(m_socket, buf, static_cast<size_t>(RECV_BUF), 0);

    // peer closed connection
    if (bytes == 0)
      break;

    // Error occured
    if (bytes < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        break;
      }

      std::perror("recv");
      break;
    }
  }
}

bool ClientSession::Read()
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
    m_server->BroadcastMsg(msg, this);
  }
}

bool ClientSession::Write()
{
  while (!m_sendQueue.empty())
  {
    std::string& msg = m_sendQueue.front();
    ssize_t bytes = send(m_socket, msg.c_str(), msg.size(), 0);

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

void ClientSession::PostSend(const std::string& msg)
{
  if (!msg.empty()) 
  {
    m_sendQueue.push_back(msg);
  }
}
