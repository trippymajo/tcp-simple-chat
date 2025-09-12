#include "ClientSession.h"
#include "ChatServer.h"
#include "../Shared/Framing.h"

#include "WS2tcpip.h"

using std::string;


constexpr int RECV_BUF = 4096;
constexpr int RECV_TIMEOUT = 3000;

ClientSession::ClientSession(SOCKET socket, ChatServer* server)
{
  m_socket = socket;
  m_server = server;
  m_active.store(false, std::memory_order_release);
}

ClientSession::~ClientSession()
{
  Stop();
}

void ClientSession::Start()
{
  m_active.store(true, std::memory_order_release);

  m_thread = std::thread(&ClientSession::ReceiveMsg, this);
}

void ClientSession::Stop()
{
  m_active.store(false, std::memory_order_release);

  if (m_socket != INVALID_SOCKET)
  {
    GracefulShutdown();

    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
  }

  if (m_thread.joinable())
    m_thread.join();
}

void ClientSession::GracefulShutdown()
{
  shutdown(m_socket, SD_SEND);
  setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, 
    reinterpret_cast<const char*>(&RECV_TIMEOUT), sizeof(RECV_TIMEOUT));

  string msg;
  recv_frame(m_socket, msg);
}

void ClientSession::SendMsg(const string& msg)
{
  std::lock_guard<std::mutex> lock(m_sendMutex);

  send_frame(m_socket, msg);
}

void ClientSession::ReceiveMsg()
{
  string msg;

  while (m_active.load(std::memory_order_acquire))
  {
    if (!recv_frame(m_socket, msg))
      break;

    m_server->BroadcastMsg(msg, this);
  }
}