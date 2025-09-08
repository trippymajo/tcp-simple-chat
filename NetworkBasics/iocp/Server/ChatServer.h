#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include "winsock2.h"
#include "mswsock.h"

#include "../include/Shared.h"

class ClientSession;

/// <summary>
/// Per-pending-accept context (lives until AcceptEx completes).
/// </summary>
struct AcceptCtx
{
  OvEx ex{ {}, OvEx::Kind::Accept };
  SOCKET listenSock = INVALID_SOCKET;
  SOCKET acceptSock = INVALID_SOCKET;
  LPFN_ACCEPTEX pAcceptEx = nullptr;
  LPFN_GETACCEPTEXSOCKADDRS pGetAddrs = nullptr;
  std::vector<char> buf; // 2*addr + optional first data
};

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
  void RunLoop();

  void RemoveClosedClients();
  SOCKET CreateListeningSocket(const std::string& ip);
  bool InitIocp();
  bool AssociateHandle(HANDLE h, ULONG_PTR key);
  bool CreateAcceptSocket(SOCKET listenSock);

  void OnAcceptComplete(AcceptCtx* ctx, DWORD /*bytes*/, bool ok);
  void OnRecvComplete(ClientSession* sess, DWORD bytes, bool ok);
  void OnSendComplete(ClientSession* sess, DWORD /*bytes*/, bool ok);

private:
  bool m_wsaInit = false;
  HANDLE m_iocp;

  std::string m_port;
  std::vector<std::string> m_ips;

  std::vector<SOCKET> m_listenSockets;

  // Key: SOCKET handle for O(1) lookup/erase.
  std::unordered_map<SOCKET, std::unique_ptr<ClientSession>> m_clients;
};
