#include "osal.h"
#include "obicall/wire.h"
#include "obicall/digest.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define CLOSESOCK closesocket
static int would_block_err(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define CLOSESOCK close
static int would_block_err(void) { return errno == EINPROGRESS; }
#endif

struct osal_socket {
    sock_t fd;
};

int osal_net_init(void) {
#if defined(_WIN32)
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void osal_net_shutdown(void) {
#if defined(_WIN32)
    WSACleanup();
#endif
}

static void set_nonblocking(sock_t fd, int nonblocking) {
#if defined(_WIN32)
    u_long mode = nonblocking ? 1 : 0;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (nonblocking) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    else fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

static void make_timeval(int timeout_ms, struct timeval* tv) {
    tv->tv_sec = timeout_ms / 1000;
    tv->tv_usec = (timeout_ms % 1000) * 1000;
}

int osal_listen_loopback(uint16_t* inout_port, osal_socket_t** out_listener) {
    sock_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == SOCK_INVALID) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(*inout_port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        CLOSESOCK(fd);
        return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &alen) != 0) {
        CLOSESOCK(fd);
        return -1;
    }
    *inout_port = ntohs(addr.sin_port);
    if (listen(fd, 16) != 0) {
        CLOSESOCK(fd);
        return -1;
    }
    osal_socket_t* s = (osal_socket_t*)malloc(sizeof(*s));
    if (!s) { CLOSESOCK(fd); return -1; }
    s->fd = fd;
    *out_listener = s;
    return 0;
}

int osal_accept(osal_socket_t* listener, int timeout_ms, osal_socket_t** out_conn) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listener->fd, &rfds);
    struct timeval tv;
    make_timeval(timeout_ms, &tv);
    int r = select((int)listener->fd + 1, &rfds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    if (r == 0) return 0;
    if (r < 0) return -1;
    sock_t fd = accept(listener->fd, NULL, NULL);
    if (fd == SOCK_INVALID) return -1;
    osal_socket_t* s = (osal_socket_t*)malloc(sizeof(*s));
    if (!s) { CLOSESOCK(fd); return -1; }
    s->fd = fd;
    *out_conn = s;
    return 1;
}

int osal_connect_loopback(uint16_t port, int timeout_ms, osal_socket_t** out_conn) {
    sock_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == SOCK_INVALID) return -1;
    set_nonblocking(fd, 1);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    int cr = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (cr != 0) {
        if (!would_block_err()) {
            CLOSESOCK(fd);
            return -1;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        make_timeval(timeout_ms, &tv);
        int r = select((int)fd + 1, NULL, &wfds, NULL, timeout_ms < 0 ? NULL : &tv);
        if (r <= 0) {
            CLOSESOCK(fd);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
        if (err != 0) {
            CLOSESOCK(fd);
            return -1;
        }
    }
    set_nonblocking(fd, 0);
    osal_socket_t* s = (osal_socket_t*)malloc(sizeof(*s));
    if (!s) { CLOSESOCK(fd); return -1; }
    s->fd = fd;
    *out_conn = s;
    return 0;
}

static int send_all(sock_t fd, const uint8_t* buf, uint32_t len, int timeout_ms) {
    uint32_t sent = 0;
    while (sent < len) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        make_timeval(timeout_ms, &tv);
        int r = select((int)fd + 1, NULL, &wfds, NULL, timeout_ms < 0 ? NULL : &tv);
        if (r <= 0) return -1;
        int n = send(fd, (const char*)buf + sent, (int)(len - sent), 0);
        if (n <= 0) return -1;
        sent += (uint32_t)n;
    }
    return 0;
}

/* Returns 0 (all len bytes read), 1 (select() timed out before any byte of
 * this call was read - safe for the caller to retry against the same
 * stream position), or -1 (hard error: peer closed, socket error, or a
 * timeout after some-but-not-all bytes were already consumed, which
 * leaves the frame stream desynced and is therefore never retryable). */
static int recv_all(sock_t fd, uint8_t* buf, uint32_t len, int timeout_ms) {
    uint32_t got = 0;
    while (got < len) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        make_timeval(timeout_ms, &tv);
        int r = select((int)fd + 1, &rfds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
        if (r == 0) return (got == 0) ? 1 : -1;
        if (r < 0) return -1;
        int n = recv(fd, (char*)buf + got, (int)(len - got), 0);
        if (n <= 0) return -1;
        got += (uint32_t)n;
    }
    return 0;
}

int osal_send_frame(osal_socket_t* s, uint8_t msg_type, const uint8_t* payload, uint32_t payload_len,
                     int timeout_ms) {
    if (payload_len > OBICALL_WIRE_MAX_PAYLOAD) return -1;
    obicall_wire_header_t hdr;
    hdr.wire_version = OBICALL_WIRE_VERSION;
    hdr.msg_type = msg_type;
    hdr.flags = 0;
    hdr.payload_len = payload_len;
    hdr.crc32 = obicall_crc32(payload, payload_len);
    uint8_t hdr_buf[OBICALL_WIRE_HEADER_LEN];
    if (obicall_wire_encode_header(&hdr, hdr_buf) != OBICALL_OK) return -1;
    if (send_all(s->fd, hdr_buf, sizeof(hdr_buf), timeout_ms) != 0) return -1;
    if (payload_len > 0 && send_all(s->fd, payload, payload_len, timeout_ms) != 0) return -1;
    return 0;
}

/* Return contract matches recv_all: 0 = a full frame was read, 1 = no
 * frame was waiting within timeout_ms (nothing consumed - safe to call
 * again), -1 = hard error (peer gone, malformed frame, or a timeout after
 * the header was already consumed - the stream position is no longer
 * trustworthy so the caller must close the connection, never retry). */
int osal_recv_frame(osal_socket_t* s, uint8_t* out_msg_type, uint8_t* payload_buf,
                     uint32_t payload_cap, uint32_t* out_payload_len, int timeout_ms) {
    uint8_t hdr_buf[OBICALL_WIRE_HEADER_LEN];
    int hr = recv_all(s->fd, hdr_buf, sizeof(hdr_buf), timeout_ms);
    if (hr != 0) return hr; /* 1 (clean timeout) or -1 (error) both pass straight through */
    obicall_wire_header_t hdr;
    if (obicall_wire_decode_header(hdr_buf, &hdr) != OBICALL_OK) return -1;
    if (hdr.payload_len > payload_cap) return -1;
    if (hdr.payload_len > 0 && recv_all(s->fd, payload_buf, hdr.payload_len, timeout_ms) != 0) {
        return -1; /* mid-frame timeout or error: stream desynced either way */
    }
    if (obicall_crc32(payload_buf, hdr.payload_len) != hdr.crc32) return -1;
    *out_msg_type = hdr.msg_type;
    *out_payload_len = hdr.payload_len;
    return 0;
}

void osal_close(osal_socket_t* s) {
    if (!s) return;
    CLOSESOCK(s->fd);
    free(s);
}
