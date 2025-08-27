#include "ClientSession.h"
#include "ChatServer.h"

#include <stdexcept>
#include <cstring>


ClientSession::ClientSession(SOCKET socket, ChatServer* server)
  : m_socket(socket), m_server(server)
{
  std::memset(&m_ovRecv, 0, sizeof(m_ovRecv));
  std::memset(&m_ovSend, 0, sizeof(m_ovSend));
  m_ovRecv.kind = OvEx::Kind::Recv;
  m_ovSend.kind = OvEx::Kind::Send;

  m_recvBufWsa.buf = m_recvBuf;
  m_recvBufWsa.len = sizeof(m_recvBuf);
}

ClientSession::~ClientSession()
{
  Stop();
}

void ClientSession::Stop()
{
  if (m_socket != INVALID_SOCKET)
  {
    shutdown(m_socket, SD_BOTH);
    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
  }
}

bool ClientSession::PostRecv()
{
  DWORD flags = 0;
  DWORD ignored = 0;
  std::memset(&m_ovRecv.ov, 0, sizeof(m_ovRecv.ov));
  m_ovRecv.kind = OvEx::Kind::Recv;

  int rc = WSARecv(m_socket, &m_recvBufWsa, 1, &ignored, &flags, &m_ovRecv.ov, nullptr);
  if (rc == 0) return true;
  if (rc == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING) return true;
  return false;
}

bool ClientSession::PostSend(const std::string& msg)
{
  if (msg.empty()) return true;

  m_sendQueue.push_back(msg);
  if (m_sendInFlight) return true;

  m_sendInFlight = true;

  std::memset(&m_ovSend.ov, 0, sizeof(m_ovSend.ov));
  m_ovSend.kind = OvEx::Kind::Send;

  WSABUF wsa{};
  wsa.buf = const_cast<char*>(m_sendQueue.front().data());
  wsa.len = static_cast<ULONG>(m_sendQueue.front().size());

  DWORD ignored = 0;
  int rc = WSASend(m_socket, &wsa, 1, &ignored, 0, &m_ovSend.ov, nullptr);
  if (rc == 0) return true;
  if (rc == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING) return true;

  // Immediate failure
  m_sendInFlight = false;
  m_sendQueue.pop_front();
  return false;
}
