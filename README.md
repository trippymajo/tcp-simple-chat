# Sockets mini guide

**Table of contents:**  
- [Common Steps](#common-steps)
- [Blocking sockets](#blocking-sockets)
- [select](#select)
- [Windows.WSAPoll](#wsapoll)
- [Windows.IOCP](#iocp)
- [POSIX.poll](#poll)

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

## select
  
**Idea:**  
Monitor multiple sockets for read/write/exception readiness. I/O multiplexing)  
  
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
  
## WSAPoll
**Idea:**  
Poll an array of sockets (WSAPOLLFD[]) for events. Windows poll  
  
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
  
## IOCP
**Idea:**  
Asynchronous I/O with completion notifications. Threads wait on the completion port;
operations are posted and completed asynchronously. I/O Completion Ports.  
  
**Steps:**
1. Create completion port `HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);`
2. Create sockets (overlapped): `WSASocket(..., WSA_FLAG_OVERLAPPED)`
3. Associate sockets with IOCP: `CreateIoCompletionPort((HANDLE)sock, iocp, (ULONG_PTR)key, 0);`
4. Load extension functions `WSAIoctl(..., SIO_GET_EXTENSION_FUNCTION_POINTER, &WSAID_ACCEPTEX, ...)` -> `AcceptEx`
5. Worker Threads: Usually create ~2 x CPU threads.
6. Post operations:  
Pre-post `AcceptEx` with a new socket and `OVERLAPPED` structure  
For active connections: `WSARecv` and `WSASend` with `OVERLAPPED`  
7. Worker loop:  
`GetQueuedCompletionStatus(iocp, &bytes, &key, &pOv, INFINITE)`  
Handle `AcceptEx` completion:  
Call `setsockopt(..., SO_UPDATE_ACCEPT_CONTEXT, ...)`  
Associate new client socket with IOCP  
Start `WSARecv` on it  
Post another AcceptEx on listen socket  
8. Shutdown  
Post exit signals: `PostQueuedCompletionStatus(iocp, 0, 0, NULL)`  
Close sockets, free buffers, `CloseHandle(iocp)`  
  
**Pros:** highest scalability, suited for tens of thousands of clients.
**Cons:** most complex — requires careful memory and lifecycle management.  
  
# poll
**Idea:**  
I/O multiplexing with an array of struct `pollfd`. More scalable than select, no `FD_SETSIZE` limit.

**Steps:**  
1. Create sockets and make them non-blocking `fcntl(fd, F_SETFL, O_NONBLOCK)`
2. Fill an array/vector of `pollfd`:  
`pollfd.fd = sock`  
`pollfd.events = POLLIN | POLLOUT | POLLERR | POLLHUP`
3. Loop:  
`poll(fds, count, timeout_ms)`  
`POLLIN` -> accept/recv  
`POLLOUT` -> send pending data, then disable `POLLOUT` if queue is empty  
`POLLERR/POLLHUP` -> close the socket  
  
**Pros:** Works on all POSIX systems, No `FD_SETSIZE` limitation like `select`  
**Cons:** Still O(n) scan each iteration. Rebuild array after removing/closing sockets  
  
# epoll
**Idea:**  
Efficient Linux-specific API. The kernel tracks socket interests and returns only ready fds.  
  
**Level-Triggered (LT) vs Edge-Triggered (ET):**  
**LT:** simpler, events keep firing until you consume all data  
**ET:** more efficient, but you must drain accept/recv/send fully to `EAGAIN`  
  
**Steps:**  
1. Non-blocking sockets: `fcntl(fd, F_SETFL, O_NONBLOCK)`
2. Create epoll instance: `epoll_create1(EPOLL_CLOEXEC)`
3. Add sockets:  
`epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev  
`ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET`
4. Loop:  
`epoll_wait(ep, events, maxevents, timeout)`  
For each event:  
On listen socket: `accept4()` in a loop until `EAGAIN`  
On client socket:  
`EPOLLIN` -> recv in loop until `EAGAIN`  
`EPOLLOUT` -> flush pending sends, disable `EPOLLOUT` when empty  
`EPOLLRDHUP/EPOLLHUP/EPOLLERR` -> close/remove  
  
**Pros:** Much better scalability for thousands of socket, Only ready sockets are returned, no full scan.  
**Cons:** Linux only, Edge-triggered mode requires careful loops and buffering