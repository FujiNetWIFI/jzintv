/*
 * ============================================================================
 *  Title:    Minimal non-blocking TCP client socket helper
 * ============================================================================
 *  See fn_sock.h.
 * ============================================================================
 */
#include "config.h"
#include "fujinet/fn_sock.h"

#ifdef FN_SOCK_WIN32
#  include <errno.h>
#  ifndef EINPROGRESS
#    define EINPROGRESS WSAEWOULDBLOCK
#  endif
#endif

/* ======================================================================== */
/*  FN_SOCK_LAST_ERR  -- Portable "would this call have blocked?" check.    */
/* ======================================================================== */
LOCAL int fn_sock_would_block(void)
{
#ifdef FN_SOCK_WIN32
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

/* ======================================================================== */
/*  FN_SOCK_START     -- One-time global startup (WSAStartup on Win32).     */
/* ======================================================================== */
int fn_sock_start(void)
{
#ifdef FN_SOCK_WIN32
    static int started = 0;
    WSADATA wsa;

    if (started)
        return 0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;

    started = 1;
#endif
    return 0;
}

/* ======================================================================== */
/*  FN_SOCK_INIT      -- Initializes a fn_sock_t to "no connection."        */
/* ======================================================================== */
void fn_sock_init(fn_sock_t *const sock)
{
    sock->fd      = FN_SOCK_INVALID;
    sock->ai_head = NULL;
    sock->ai_next = NULL;
}

/* ======================================================================== */
/*  FN_SOCK_FREE_AI    -- Internal: releases the getaddrinfo() list, if any. */
/* ======================================================================== */
LOCAL void fn_sock_free_ai(fn_sock_t *const sock)
{
    if (sock->ai_head)
        freeaddrinfo(sock->ai_head);
    sock->ai_head = NULL;
    sock->ai_next = NULL;
}

/* ======================================================================== */
/*  FN_SOCK_CLOSE_FD  -- Internal: close a raw fd.                          */
/* ======================================================================== */
LOCAL void fn_sock_close_fd(const fn_sock_fd_t fd)
{
#ifdef FN_SOCK_WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

/* ======================================================================== */
/*  FN_SOCK_CLOSE     -- Closes the connection, if any.                     */
/* ======================================================================== */
void fn_sock_close(fn_sock_t *const sock)
{
    if (sock->fd != FN_SOCK_INVALID)
    {
        fn_sock_close_fd(sock->fd);
        sock->fd = FN_SOCK_INVALID;
    }
    fn_sock_free_ai(sock);
}

/* ======================================================================== */
/*  FN_SOCK_SET_NONBLOCK -- Puts a freshly-created fd into non-blocking     */
/*                          mode.  Returns 0 on success.                    */
/* ======================================================================== */
LOCAL int fn_sock_set_nonblock(const fn_sock_fd_t fd)
{
#ifdef FN_SOCK_WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

/* ======================================================================== */
/*  FN_SOCK_TRY_NEXT   -- Internal: walks sock->ai_next forward, trying to   */
/*                       open a non-blocking connect() to each candidate    */
/*                       address in turn (e.g. localhost's ::1 then          */
/*                       127.0.0.1) until one accepts the connect() call or  */
/*                       the list is exhausted.  Needed because a listener   */
/*                       bound to only one address family makes the other   */
/*                       family's EINPROGRESS connect fail asynchronously,   */
/*                       and we must fall back rather than give up.         */
/* ======================================================================== */
LOCAL int fn_sock_try_next(fn_sock_t *const sock)
{
    struct addrinfo *ai;

    for (ai = sock->ai_next; ai; ai = ai->ai_next)
    {
        int one = 1;
        const fn_sock_fd_t fd =
            socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if (fd == FN_SOCK_INVALID)
            continue;

        if (fn_sock_set_nonblock(fd))
        {
            fn_sock_close_fd(fd);
            continue;
        }

        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&one, sizeof(one));

        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0 ||
            fn_sock_would_block())
        {
            sock->fd      = fd;
            sock->ai_next = ai->ai_next;
            return 1;  /* Connected immediately, or in progress -- either OK. */
        }

        fn_sock_close_fd(fd);
    }

    sock->ai_next = NULL;
    return 0;   /*  No candidate address worked.                            */
}

/* ======================================================================== */
/*  FN_SOCK_CONNECT_BEGIN -- Resolves host/port and issues a non-blocking   */
/*                       connect().                                         */
/* ======================================================================== */
int fn_sock_connect_begin(fn_sock_t *const sock, const char *const host,
                           const int port)
{
    struct addrinfo hints;
    char portstr[16];

    fn_sock_close(sock);

    if (fn_sock_start())
        return -1;

    snprintf(portstr, sizeof(portstr), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host, portstr, &hints, &sock->ai_head) != 0 ||
        !sock->ai_head)
        return -1;

    sock->ai_next = sock->ai_head;

    if (!fn_sock_try_next(sock))
    {
        fn_sock_free_ai(sock);
        return -1;
    }

    return 0;
}

/* ======================================================================== */
/*  FN_SOCK_CONNECT_POLL  -- Polls a connect() in progress.                 */
/* ======================================================================== */
int fn_sock_connect_poll(fn_sock_t *const sock)
{
    fd_set wfds, efds;
    struct timeval tv;
    int so_err = 0;
    socklen_t so_err_len = sizeof(so_err);

    if (sock->fd == FN_SOCK_INVALID)
        return -1;

    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(sock->fd, &wfds);
    FD_SET(sock->fd, &efds);
    tv.tv_sec  = 0;
    tv.tv_usec = 0;

    if (select((int)sock->fd + 1, NULL, &wfds, &efds, &tv) < 0)
        return -1;

    if (!FD_ISSET(sock->fd, &wfds) && !FD_ISSET(sock->fd, &efds))
        return 0;   /*  Still pending.                                     */

    if (getsockopt(sock->fd, SOL_SOCKET, SO_ERROR,
                   (char *)&so_err, &so_err_len) < 0 || so_err != 0)
    {
        /*  This candidate address failed (e.g. "localhost" resolved to     */
        /*  ::1 first, but the peer is only listening on 127.0.0.1) -- try  */
        /*  the next address in the list, if any, rather than giving up.    */
        fn_sock_close_fd(sock->fd);
        sock->fd = FN_SOCK_INVALID;

        if (fn_sock_try_next(sock))
            return 0;   /*  New candidate is now pending.                  */

        fn_sock_free_ai(sock);
        return -1;
    }

    fn_sock_free_ai(sock);
    return 1;   /*  Connected.                                             */
}

/* ======================================================================== */
/*  FN_SOCK_SEND      -- Non-blocking send.                                 */
/* ======================================================================== */
int fn_sock_send(fn_sock_t *const sock, const uint8_t *const buf,
                  const size_t len)
{
    int n;

    if (sock->fd == FN_SOCK_INVALID)
        return -1;

    if (len == 0)
        return 0;

#ifdef FN_SOCK_WIN32
    n = send(sock->fd, (const char *)buf, (int)len, 0);
#else
    n = (int)send(sock->fd, buf, len, MSG_NOSIGNAL);
#endif

    if (n >= 0)
        return n;

    if (fn_sock_would_block())
        return 0;

    return -1;
}

/* ======================================================================== */
/*  FN_SOCK_RECV      -- Non-blocking recv.                                 */
/* ======================================================================== */
int fn_sock_recv(fn_sock_t *const sock, uint8_t *const buf,
                  const size_t len, int *const eof)
{
    int n;

    if (eof)
        *eof = 0;

    if (sock->fd == FN_SOCK_INVALID)
        return -1;

    if (len == 0)
        return 0;

#ifdef FN_SOCK_WIN32
    n = recv(sock->fd, (char *)buf, (int)len, 0);
#else
    n = (int)recv(sock->fd, buf, len, 0);
#endif

    if (n > 0)
        return n;

    if (n == 0)
    {
        /*  Orderly shutdown by the peer.                                  */
        if (eof)
            *eof = 1;
        return 0;
    }

    if (fn_sock_would_block())
        return 0;

    return -1;
}

/* ======================================================================== */
/*  FN_SOCK_CONNECTED -- Nonzero if the socket currently holds an open fd.  */
/* ======================================================================== */
int fn_sock_connected(const fn_sock_t *const sock)
{
    return sock->fd != FN_SOCK_INVALID;
}
/* ======================================================================== */
/*  This program is free software; you can redistribute it and/or modify    */
/*  it under the terms of the GNU General Public License as published by    */
/*  the Free Software Foundation; either version 2 of the License, or       */
/*  (at your option) any later version.                                     */
/*                                                                          */
/*  This program is distributed in the hope that it will be useful,         */
/*  but WITHOUT ANY WARRANTY; without even the implied warranty of          */
/*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU       */
/*  General Public License for more details.                                */
/*                                                                          */
/*  You should have received a copy of the GNU General Public License along */
/*  with this program; if not, write to the Free Software Foundation, Inc., */
/*  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.             */
/* ======================================================================== */
