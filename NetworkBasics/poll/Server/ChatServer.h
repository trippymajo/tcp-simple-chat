#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include <sys/poll.h>   // poll()

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
  int CreateListenSocket(const std::string& ip);
  void RunLoop();

  void RebuildPollSet(std::vector<pollfd>&pollfds);
  void HandleListeners(std::vector<pollfd>&pollfds);
  void HandleClients(std::vector<pollfd>&pollfds);

  void AcceptAll(int& pfd);
  void CloseClient(int& sfd);
  bool ReadClient(int& sfd);
  bool WriteClient(int& sfd);


private:
  bool m_running = false;
  std::string m_port;
  std::vector<std::string> m_ips;

  std::vector<int> m_listenSockets;
  std::unordered_map<int, std::unique_ptr<ClientSession>> m_clients;
};
