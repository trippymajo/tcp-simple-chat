# WinSocket

## Common Steps

1. **Initialize Winsock**
Startup: `WSAStartup(...), WSACleanup()`
> [!IMPORTANT]
> Always WSACleanup() at process exit.
  
2. **Resolve addresses**
```
getaddrinfo(host, port, &hints, &res)
hints.ai_family = AF_INET / AF_INET6 / AF_UNSPEC
hints.ai_socktype = SOCK_STREAM (TCP) or SOCK_DGRAM (UDP)
For servers: hints.ai_flags = AI_PASSIVE
```
  
3. **Create socket**  
`socket(family, type, protocol)`  
For IOCP: `WSASocket(..., WSA_FLAG_OVERLAPPED)`  
Server listen sockets: `setsockopt(..., SO_EXCLUSIVEADDRUSE, ...)`  
  
4. **Cleanup**  
Close: `shutdown(sock, SD_SEND/SD_BOTH) -> closesocket(sock)`  
Error handling: `WSAGetLastError()`  
  
## Blocking sockets

**Client:**
1. `WSAStartup`
2. `getaddrinfo(server, port, …)`
3. `socket(...)`
4. `connect(sock, addr, addrlen)`  
5. Exchange: `send(sock, buf, len, 0), recv(sock, buf, len, 0)`
6. `shutdown(...)` -> `closesocket(...)` -> `WSACleanup`

**Server:**
1. `WSAStartup`
2. `getaddrinfo(NULL, port, AI_PASSIVE, ...)`
3. `socket(...)`
4. `bind(listenSock, addr, addrlen)`
5. `listen(listenSock, SOMAXCONN)`
6. Loop: `accept(listenSock) -> blocking send/recv`
7. closesocket clients, then listen socket -> `WSACleanup`  
  
**Pros:** very simple.
**Cons:** poor scalability — one thread per client.

## select (I/O multiplexing)
  
**Idea:**  
Monitor multiple sockets for read/write/exception readiness.  
  
**Steps:**
1. Initialize sockets as usual.
2. Make non-blocking: `u_long on=1; ioctlsocket(sock, FIONBIO, &on)`
3. Loop:  
Build sets: `fd_set readSet, writeSet, exceptSet; FD_ZERO/FD_SET`  
Optional timeout: `timeval tv{sec, usec}` (or NULL for infinite)  
`select(0, &readSet, &writeSet, &exceptSet, &tv)`  
`FD_ISSET(sock, &readSet)` -> accept or recv  
`FD_ISSET(sock, &writeSet)` -> send  
`FD_ISSET(sock, &exceptSet)` -> handle errors  
  
**Pros:** works in one thread.  
**Cons:** limited (FD_SETSIZE ~64 on Windows by default), O(n) copy cost.  
  
## WSAPoll (Windows poll)
**Idea:**  
Poll an array of sockets (WSAPOLLFD[]) for events.  
  
**Steps:**
1. Non-blocking sockets: `ioctlsocket(FIONBIO)`
2. Fill array of `WSAPOLLFD`:  
`.fd = sock`  
`.events = POLLRDNORM | POLLWRNORM | POLLERR | POLLHUP`  
3. Loop: `WSAPoll(fds, count, timeout_ms)`  
For each fd:  
`POLLRDNORM` -> accept/recv  
`POLLWRNORM` -> send  
`POLLERR/POLLHUP` -> close  
  
**Pros:** easier than select, no FD_SETSIZE limit.  
**Cons:** still O(n) scan; not as scalable as IOCP.

