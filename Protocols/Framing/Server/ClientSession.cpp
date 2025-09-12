#include "ClientSession.h"
#include "ChatServer.h"
#include "../Shared/Framing.h"

#include "WS2tcpip.h"

using std::string;

constexpr size_t TX_HWM_BYTES = 256 * 1024;
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

  m_rxThread = std::thread(&ClientSession::RecvLoop, this);
  m_txThread = std::thread(&ClientSession::SendLoop, this);
}

void ClientSession::Stop()
{
  m_active.store(false, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lg(m_txMutex);
    m_txNotEmpty.notify_all();
    m_txNotFull.notify_all();
  }

  if (m_socket != INVALID_SOCKET)
  {
    GracefulShutdown();
    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
  }

  if (m_rxThread.joinable())
    m_rxThread.join();

  if (m_txThread.joinable())
    m_txThread.join();

  {
    std::lock_guard<std::mutex> lg(m_txMutex);
    m_txQueue.clear();
    m_txBytesQueued = 0;
  }
}

void ClientSession::GracefulShutdown()
{
  shutdown(m_socket, SD_SEND);
  setsockopt(
    m_socket, SOL_SOCKET, SO_RCVTIMEO,
    reinterpret_cast<const char*>(&RECV_TIMEOUT),
    sizeof(RECV_TIMEOUT));

  string msg;
  recv_frame(m_socket, msg);
}

void ClientSession::SendMsg(const string& msg)
{
  if (!m_active.load(std::memory_order_acquire))
    return;

  std::unique_lock<std::mutex> lock(m_txMutex);
  m_txNotFull.wait(lock, [&]
  {
    return !m_active.load(std::memory_order_acquire) ||
            (m_txBytesQueued + msg.size()) <= TX_HWM_BYTES;
  });

  if (!m_active.load(std::memory_order_acquire))
    return;

  m_txQueue.push_back(msg);
  m_txBytesQueued += msg.size();

  lock.unlock();
  m_txNotEmpty.notify_one();
}

void ClientSession::SendMsg(string&& msg)
{
  if (!m_active.load(std::memory_order_acquire))
    return;

  const size_t add = msg.size();
  std::unique_lock<std::mutex> lock(m_txMutex);

  m_txNotFull.wait(lock, [&]
  {
    return !m_active.load(std::memory_order_acquire) ||
      (m_txBytesQueued + add) <= TX_HWM_BYTES;
  });

  if (!m_active.load(std::memory_order_acquire))
    return;

  m_txBytesQueued += add;
  m_txQueue.push_back(std::move(msg));

  lock.unlock();
  m_txNotEmpty.notify_one();
}

void ClientSession::SendLoop()
{
  while (true)
  {
    string msg;

    {
      std::unique_lock<std::mutex> lock(m_txMutex);

      m_txNotEmpty.wait(lock, [&]
      {
        return !m_active.load(std::memory_order_acquire) || !m_txQueue.empty();
      });

      if (!m_active.load(std::memory_order_acquire) && m_txQueue.empty())
        break;

      msg = std::move(m_txQueue.front());
      m_txQueue.pop_front();
      if (m_txBytesQueued >= msg.size())
        m_txBytesQueued -= msg.size();
      else
        m_txBytesQueued = 0;

      m_txNotFull.notify_all();
    }

    if (!send_frame(m_socket, msg))
    {
      m_active.store(false, std::memory_order_release);
      m_txNotEmpty.notify_all();
      m_txNotFull.notify_all();
      break;
    }
  }
}

void ClientSession::RecvLoop()
{
  string msg;

  while (m_active.load(std::memory_order_acquire))
  {
    if (!recv_frame(m_socket, msg))
      break;

    m_server->BroadcastMsg(msg, this);
  }

  m_active.store(false, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lg(m_txMutex);
    m_txNotEmpty.notify_all();
    m_txNotFull.notify_all();
  }
}