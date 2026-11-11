/*
 * winsock.h -- WSOCK.DLL: sockets for OS1993 programs, as Winsock 1.1/2
 * has them (TCP, UDP, raw ICMP over IPv4).
 *
 * The VMM runs the TCP/IP stack; these calls reach it through KERNEL.DLL.
 * Errors: SOCKET_ERROR / INVALID_SOCKET, with WSAGetLastError (WSAE*) and
 * errno (POSIX) both set. A call that waits on the program's first thread
 * lets the window system run meanwhile.
 */
#ifndef OS1993_WINSOCK_H
#define OS1993_WINSOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t SOCKET;
typedef uint8_t  u_char;
typedef uint16_t u_short;
typedef uint32_t u_int;
typedef uint32_t u_long;
typedef int      socklen_t;

#define INVALID_SOCKET  ((SOCKET)~0u)
#define SOCKET_ERROR    (-1)

#define AF_UNSPEC       0
#define AF_INET         2
#define PF_INET         AF_INET
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

#define INADDR_ANY       0x00000000u
#define INADDR_LOOPBACK  0x7F000001u
#define INADDR_BROADCAST 0xFFFFFFFFu
#define INADDR_NONE      0xFFFFFFFFu

#define SOL_SOCKET      0xFFFF
#define SO_REUSEADDR    0x0004
#define SO_KEEPALIVE    0x0008
#define SO_BROADCAST    0x0020
#define SO_LINGER       0x0080
#define SO_SNDBUF       0x1001
#define SO_RCVBUF       0x1002
#define SO_SNDTIMEO     0x1005          /* milliseconds, as a DWORD */
#define SO_RCVTIMEO     0x1006
#define SO_ERROR        0x1007
#define SO_TYPE         0x1008
#define TCP_NODELAY     0x0001

#define MSG_PEEK        0x2
#define SD_RECEIVE      0
#define SD_SEND         1
#define SD_BOTH         2
#define FIONBIO         0x8004667Eu
#define FIONREAD        0x4004667Fu
#define SOMAXCONN       8

#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN  128

struct in_addr { uint32_t s_addr; };    /* network byte order */
struct sockaddr { u_short sa_family; char sa_data[14]; };
struct sockaddr_in {
    short          sin_family;
    u_short        sin_port;            /* network byte order */
    struct in_addr sin_addr;
    char           sin_zero[8];
};
struct hostent {
    char  *h_name;
    char **h_aliases;
    short  h_addrtype, h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]
struct addrinfo {
    int              ai_flags, ai_family, ai_socktype, ai_protocol;
    size_t           ai_addrlen;
    char            *ai_canonname;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
};
#define AI_PASSIVE      0x1
#define AI_NUMERICHOST  0x4

#define FD_SETSIZE 64
typedef struct { u_int fd_count; SOCKET fd_array[FD_SETSIZE]; } fd_set;
struct timeval { long tv_sec, tv_usec; };
#define FD_ZERO(set)     ((set)->fd_count = 0)
#define FD_SET(s, set)   do { if (!FD_ISSET(s, set) && (set)->fd_count < FD_SETSIZE) \
                              (set)->fd_array[(set)->fd_count++] = (s); } while (0)
#define FD_ISSET(s, set) __WSAFDIsSet((SOCKET)(s), (fd_set *)(set))
#define FD_CLR(s, set)   __WSAFDClr((SOCKET)(s), (set))

typedef struct { SOCKET fd; short events, revents; } WSAPOLLFD;
#define POLLIN          0x0001
#define POLLRDNORM      POLLIN
#define POLLOUT         0x0004
#define POLLWRNORM      POLLOUT
#define POLLERR         0x0008
#define POLLHUP         0x0010

typedef struct {
    uint16_t wVersion, wHighVersion;
    char     szDescription[WSADESCRIPTION_LEN + 1];
    char     szSystemStatus[WSASYS_STATUS_LEN + 1];
    uint16_t iMaxSockets, iMaxUdpDg;
    char    *lpVendorInfo;
} WSADATA;
#define MAKEWORD(lo, hi) ((uint16_t)((lo) | (hi) << 8))

/* Errors (WSAGetLastError) */
#define WSAEINTR            10004
#define WSAEBADF            10009
#define WSAEACCES           10013
#define WSAEFAULT           10014
#define WSAEINVAL           10022
#define WSAEMFILE           10024
#define WSAEWOULDBLOCK      10035
#define WSAEINPROGRESS      10036
#define WSAEALREADY         10037
#define WSAENOTSOCK         10038
#define WSAEDESTADDRREQ     10039
#define WSAEMSGSIZE         10040
#define WSAEPROTOTYPE       10041
#define WSAENOPROTOOPT      10042
#define WSAEPROTONOSUPPORT  10043
#define WSAESOCKTNOSUPPORT  10044
#define WSAEOPNOTSUPP       10045
#define WSAEAFNOSUPPORT     10047
#define WSAEADDRINUSE       10048
#define WSAEADDRNOTAVAIL    10049
#define WSAENETDOWN         10050
#define WSAENETUNREACH      10051
#define WSAECONNABORTED     10053
#define WSAECONNRESET       10054
#define WSAENOBUFS          10055
#define WSAEISCONN          10056
#define WSAENOTCONN         10057
#define WSAESHUTDOWN        10058
#define WSAETIMEDOUT        10060
#define WSAECONNREFUSED     10061
#define WSAEHOSTUNREACH     10065
#define WSANOTINITIALISED   10093
#define WSAHOST_NOT_FOUND   11001
#define WSATRY_AGAIN        11002
#define WSANO_DATA          11004
#define WSATYPE_NOT_FOUND   10109           /* getaddrinfo: not a port number */

int     WSAStartup(uint16_t version, WSADATA *data);
int     WSACleanup(void);
int     WSAGetLastError(void);
void    WSASetLastError(int error);

SOCKET  socket(int af, int type, int protocol);
int     bind(SOCKET s, const struct sockaddr *addr, int len);
int     connect(SOCKET s, const struct sockaddr *addr, int len);
int     listen(SOCKET s, int backlog);
SOCKET  accept(SOCKET s, struct sockaddr *addr, int *len);
int     send(SOCKET s, const char *buf, int len, int flags);
int     recv(SOCKET s, char *buf, int len, int flags);
int     sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen);
int     recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen);
int     shutdown(SOCKET s, int how);
int     closesocket(SOCKET s);
int     setsockopt(SOCKET s, int level, int name, const char *value, int len);
int     getsockopt(SOCKET s, int level, int name, char *value, int *len);
int     ioctlsocket(SOCKET s, long cmd, u_long *arg);
int     getsockname(SOCKET s, struct sockaddr *addr, int *len);
int     getpeername(SOCKET s, struct sockaddr *addr, int *len);
int     select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timeval *timeout);
int     WSAPoll(WSAPOLLFD *fds, u_long n, int timeout_ms);
int     __WSAFDIsSet(SOCKET s, fd_set *set);
void    __WSAFDClr(SOCKET s, fd_set *set);

u_short htons(u_short v);
u_short ntohs(u_short v);
u_long  htonl(u_long v);
u_long  ntohl(u_long v);
uint32_t inet_addr(const char *dotted);                 /* INADDR_NONE if not one */
char   *inet_ntoa(struct in_addr in);                   /* a static buffer */
int     inet_pton(int af, const char *src, void *dst);  /* 1, 0 not an address, -1 af */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

/* Names: numbers, "localhost", or a DNS query to the server DHCP gave. */
struct hostent *gethostbyname(const char *name);        /* a static result */
int     gethostname(char *name, int len);
int     getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                    struct addrinfo **res);
void    freeaddrinfo(struct addrinfo *ai);

/* OS1993: the interface's settings (what DHCP gave), for IPCONFIG */
struct net_config;
bool    GetNetworkConfig(struct net_config *cfg);

#endif
