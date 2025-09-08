#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include "winsock2.h"

class ClientSession;

/// <summary>
/// Chat server that accepts TCP connections and multiplexes client I/O using select().
/// Owns all ClientSession instances and broadcasts incoming messages to other clients.
/// </summary>
class ChatServer
{
public:
  ChatServer(const std::vector<std::string>& ips, const std::string& port);
  ~ChatServer();

  void Start();
  void Stop();
  void BroadcastMsg(const std::string& msg, ClientSession* pSender);

private:
  // Server high-level loop:

  void RunEventLoop();
  void BuildReadSet(fd_set& outReadSet);
  int WaitForEvents(fd_set& inOutReadSet, long timeoutMillis);
  void HandleReadyListeners(const fd_set& readSet);
  void HandleReadyClients(const fd_set& readSet);
  void RemoveClosedClients();

  // LifeCycle helpers:

  SOCKET CreateListeningSocket(const std::string& ip);
  void AddClient(SOCKET s);
  void RemoveClient(SOCKET s, const char* reason);

private:
  bool m_wsaInit = false;
  std::string m_port;
  std::vector<std::string> m_ips;
  std::vector<SOCKET> m_listenSockets;

  // Key: SOCKET handle for O(1) lookup/erase.
  std::unordered_map<SOCKET, std::unique_ptr<ClientSession>> m_clients;

  // stash of sockets scheduled for removal after a loop tick
  std::vector<SOCKET> m_toClose;
};
