/*
 * ============================================================================
 *  Title:    Minimal non-blocking TCP client socket helper
 * ============================================================================
 *  jzIntv has no pre-existing networking code anywhere in the tree.  This
 *  is a small, single-purpose wrapper around a non-blocking TCP client
 *  socket, used by the FujiNet mailbox peripheral (fujinet.c) to connect
 *  to a fujinet-firmware instance's "Bus Over IP" (BoIP) TCP server.  It
 *  intentionally does nothing fancy: one connection, non-blocking I/O,
 *  partial-write-aware send, would-block-aware recv.
 * ============================================================================
 */
#ifndef FN_SOCK_H_
#define FN_SOCK_H_

#if defined(_WIN32) || defined(WIN32)
#  define FN_SOCK_WIN32 1
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET fn_sock_fd_t;
#  define FN_SOCK_INVALID INVALID_SOCKET
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int fn_sock_fd_t;
#  define FN_SOCK_INVALID (-1)
#endif

/* ======================================================================== */
/*  FN_SOCK_T         -- One non-blocking TCP client connection.            */
/* ======================================================================== */
typedef struct fn_sock_t
{
    fn_sock_fd_t fd;
    struct addrinfo *ai_head;   /* Full getaddrinfo() list for this attempt. */
    struct addrinfo *ai_next;   /* Next candidate to try if 'fd' fails.      */
} fn_sock_t;

/* ======================================================================== */
/*  FN_SOCK_START     -- One-time global startup (WSAStartup on Win32).     */
/*                       Safe to call more than once.  Returns 0 on OK.     */
/* ======================================================================== */
int fn_sock_start(void);

/* ======================================================================== */
/*  FN_SOCK_INIT      -- Initializes a fn_sock_t to "no connection."        */
/* ======================================================================== */
void fn_sock_init(fn_sock_t *const sock);

/* ======================================================================== */
/*  FN_SOCK_CONNECT_BEGIN -- Resolves host/port and issues a non-blocking   */
/*                       connect().  Returns 0 on success (connection in    */
/*                       progress or, rarely, already complete), -1 on any  */
/*                       immediate failure (bad host, socket() failure).    */
/*                       Call fn_sock_connect_poll() afterward to find out  */
/*                       when/whether it finishes.                          */
/* ======================================================================== */
int fn_sock_connect_begin(fn_sock_t *const sock, const char *const host,
                           const int port);

/* ======================================================================== */
/*  FN_SOCK_CONNECT_POLL  -- Polls a connect() in progress.                 */
/*                       Returns  1 if connected, 0 if still pending,       */
/*                       -1 on failure (socket has been closed already).    */
/* ======================================================================== */
int fn_sock_connect_poll(fn_sock_t *const sock);

/* ======================================================================== */
/*  FN_SOCK_SEND      -- Non-blocking send.  Returns the number of bytes    */
/*                       actually written (may be 0 if the socket would     */
/*                       block), or -1 on error (connection lost).          */
/* ======================================================================== */
int fn_sock_send(fn_sock_t *const sock, const uint8_t *const buf,
                  const size_t len);

/* ======================================================================== */
/*  FN_SOCK_RECV      -- Non-blocking recv.  Returns the number of bytes    */
/*                       actually read (0 if the socket would block, or if  */
/*                       the peer performed an orderly shutdown -- callers  */
/*                       distinguish those with fn_sock_recv_eof()), or -1  */
/*                       on error (connection lost).                        */
/* ======================================================================== */
int fn_sock_recv(fn_sock_t *const sock, uint8_t *const buf,
                  const size_t len, int *const eof);

/* ======================================================================== */
/*  FN_SOCK_CLOSE     -- Closes the connection, if any, and marks the       */
/*                       fn_sock_t as disconnected.  Safe to call when      */
/*                       already closed.                                    */
/* ======================================================================== */
void fn_sock_close(fn_sock_t *const sock);

/* ======================================================================== */
/*  FN_SOCK_CONNECTED -- Nonzero if the socket currently holds an open fd.  */
/*                       Note this does NOT mean the TCP handshake has      */
/*                       completed -- use fn_sock_connect_poll() for that.  */
/* ======================================================================== */
int fn_sock_connected(const fn_sock_t *const sock);

#endif /* FN_SOCK_H_ */
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
