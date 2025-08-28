#include "ChatClient.h"

#include <iostream>
#include <cstring>

using std::cout;
using std::cerr;

static void SafeCloseSocket(SOCKET& s)
{
  if (s != INVALID_SOCKET)
  {
    closesocket(s);
    s = INVALID_SOCKET;
  }
}

ChatClient::ChatClient(const char* ipadd, const char* port)
  : m_ipadd(ipadd), m_port(port)
{
  m_recvBufWsa.buf = m_recvStorage;
  m_recvBufWsa.len = static_cast<ULONG>(sizeof(m_recvStorage));
}

ChatClient::~ChatClient()
{
  Stop();
  if (m_sendThread.joinable())
    m_sendThread.join();
}

void ChatClient::Start()
{
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
  {
    cerr << "WSAStartup failed\n";
    return;
  }

  if (!InitIocp() || !CreateConnection() || !AssociateHandle())
  {
    Stop();
    return;
  }

  m_isActive = true;

  if (!PostRecv())
  {
    cerr << "Initial WSARecv failed\n";
    Stop();
    return;
  }

  m_runSend = true;
  m_sendThread = std::thread([this] { SendWorker(); });

  RunEventLoop();
  Stop();
}

void ChatClient::Stop()
{
  m_runSend = false;
  m_sendCv.notify_all();

  if (m_socket != INVALID_SOCKET)
    shutdown(m_socket, SD_BOTH);

  SafeCloseSocket(m_socket);

  if (m_isActive)
  {
    WSACleanup();
    m_isActive = false;
  }

  if (m_sendThread.joinable())
    m_sendThread.join();
}

bool ChatClient::InitIocp()
{
  m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
  return m_iocp != nullptr;
}

bool ChatClient::CreateConnection()
{
  addrinfo hints{}, * result;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  if (getaddrinfo(m_ipadd.c_str(), m_port.c_str(), &hints, &result) != 0)
  {
    cerr << "Cant resolve address for " << m_ipadd << ":" << m_port << "\n";
    return false;
  }

  for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next)
  {
    SOCKET s = WSASocket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol,
      nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET)
      continue;

    if (!ConnectToServer(s, ptr))
    {
      SafeCloseSocket(s);
      continue;
    }

    m_socket = s;
    freeaddrinfo(result);
    return true;
  }

  freeaddrinfo(result);
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

bool ChatClient::AssociateHandle()
{
  return (CreateIoCompletionPort((HANDLE)m_socket, m_iocp, (ULONG_PTR)m_socket, 0) == m_iocp);
}

bool ChatClient::PostSend(const std::string& msg)
{
  if (msg.empty())
    return true;

  std::lock_guard<std::mutex> lg(m_sendMx);
  m_sendQueue.push_back(SendItem{ msg, 0 });

  if (m_sendInFlight)
    return true;

  m_sendInFlight = true;
  return KickSend();
}

bool ChatClient::KickSend()
{
  std::lock_guard<std::mutex> lg(m_sendMx);
  if (m_sendQueue.empty())
  {
    m_sendInFlight = false;
    return true;
  }

  if (m_socket == INVALID_SOCKET)
  {
    m_sendInFlight = false;
    m_sendQueue.clear();
    return false;
  }

  SendItem& it = m_sendQueue.front();

  std::memset(&it.ov.ov, 0, sizeof(it.ov.ov));
  it.ov.kind = OvEx::Kind::Send;

  WSABUF wsa{};
  wsa.buf = const_cast<char*>(it.data.data() + it.off);
  wsa.len = static_cast<ULONG>(it.data.size() - it.off);

  DWORD ignored = 0;
  int rc = WSASend(m_socket, &wsa, 1, &ignored, 0, &it.ov.ov, nullptr);

  if (rc == 0)
    return true;

  if (rc == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING)
    return true;

  m_sendInFlight = false;
  m_sendQueue.pop_front();
  return false;
}

bool ChatClient::PostRecv()
{
  std::memset(&m_ovRecv.ov, 0, sizeof(m_ovRecv.ov));
  m_ovRecv.kind = OvEx::Kind::Recv;

  DWORD flags = 0, ig = 0;
  int rc = WSARecv(m_socket, &m_recvBufWsa, 1, &ig, &flags, &m_ovRecv.ov, nullptr);
  if (rc == 0)
    return true;
  if (rc == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING)
    return true;

  return false;
}

bool ChatClient::Send(const std::string& msg)
{
  if (msg.empty())
    return true;

  {
    std::lock_guard<std::mutex> lg(m_sendMx);
    m_sendQueue.push_back(SendItem{ msg, 0 });
  }
  m_sendCv.notify_one();
  return true;
}

void ChatClient::SendWorker()
{
  while (m_runSend)
  {
    std::unique_lock<std::mutex> lk(m_sendMx);
    m_sendCv.wait(lk, [&] { return !m_sendQueue.empty() || !m_runSend; });

    if (!m_runSend) break;
    if (!m_sendInFlight && !m_sendQueue.empty())
    {
      m_sendInFlight = true;
      lk.unlock();
      KickSend();
    }
  }
}

void ChatClient::RunEventLoop()
{
  while (m_isActive)
  {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED pov = nullptr;

    BOOL ok = GetQueuedCompletionStatus(m_iocp, &bytes, &key, &pov, INFINITE);

    if (!pov)
      break;

    auto* ex = reinterpret_cast<OvEx*>(pov);
    switch (ex->kind)
    {
    case OvEx::Kind::Recv: OnRecvComplete(bytes, ok != FALSE); break;
    case OvEx::Kind::Send: OnSendComplete(bytes, ok != FALSE); break;
    }
  }
}

void ChatClient::OnRecvComplete(DWORD bytes, bool ok)
{
  if (!ok || bytes == 0)
  {
    cout << "Server closed connection\n";
    SafeCloseSocket(m_socket);
    if (m_iocp)
      PostQueuedCompletionStatus(m_iocp, 0, 0, nullptr);
    return;
  }

  std::string msg(m_recvStorage, m_recvStorage + bytes);
  cout << msg;

  PostRecv();
}

void ChatClient::OnSendComplete(DWORD bytes, bool ok)
{
  if (!ok)
  {
    SafeCloseSocket(m_socket);
    if (m_iocp)
      PostQueuedCompletionStatus(m_iocp, 0, 0, nullptr);
    return;
  }

  bool needKick = false;
  {
    std::lock_guard<std::mutex> lg(m_sendMx);
    if (m_sendQueue.empty())
    {
      m_sendInFlight = false;
      return;
    }

    SendItem& it = m_sendQueue.front();
    it.off += bytes;

    if (it.off < it.data.size())
    {
      needKick = true;
    }
    else
    {
      m_sendQueue.pop_front();
      needKick = !m_sendQueue.empty();
      if (!needKick) m_sendInFlight = false;
    }
  }

  if (needKick)
    KickSend();
}
