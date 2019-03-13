/* coap_io_riot.c -- Default network I/O functions for libcoap on RIOT
 *
 * Copyright (C) 2019 Olaf Bergmann <bergmann@tzi.org>
 *
 * This file is part of the CoAP library libcoap. Please see
 * README for terms of use.
 */

#include "coap_config.h"

#ifdef HAVE_STDIO_H
#  include <stdio.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
# define OPTVAL_T(t)         (t)
# define OPTVAL_GT(t)        (t)
#endif
#ifdef HAVE_SYS_IOCTL_H
 #include <sys/ioctl.h>
#endif
#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#ifdef HAVE_SYS_UIO_H
# include <sys/uio.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <errno.h>

#include "net/gnrc.h"
#include "net/gnrc/ipv6.h"
#include "net/gnrc/netreg.h"
#include "net/udp.h"

#include "libcoap.h"
#include "coap_debug.h"
#include "mem.h"
#include "net.h"
#include "coap_io.h"
#include "pdu.h"
#include "utlist.h"
#include "resource.h"

#include "coap_riot.h"

ssize_t
coap_network_send(coap_socket_t *sock,
                  const coap_session_t *session,
                  const uint8_t *data,
                  size_t datalen) {
  ssize_t bytes_written = 0;

  if (!coap_debug_send_packet()) {
    bytes_written = (ssize_t)datalen;
  } else if (sock->flags & COAP_SOCKET_CONNECTED) {
    bytes_written = send(sock->fd, data, datalen, 0);
  } else {
    bytes_written = sendto(sock->fd, data, datalen, 0,
                           &session->remote_addr.addr.sa,
                           session->remote_addr.size);
  }

  if (bytes_written < 0)
    coap_log(LOG_CRIT, "coap_network_send: %s\n", coap_socket_strerror());

  return bytes_written;
}

static udp_hdr_t *
get_udp_header(gnrc_pktsnip_t *pkt) {
  gnrc_pktsnip_t *udp = gnrc_pktsnip_search_type(pkt, GNRC_NETTYPE_UDP);
  return udp ? (udp_hdr_t *)udp->data : NULL;
}

ssize_t
coap_network_read(coap_socket_t *sock, struct coap_packet_t *packet) {
  size_t len;
  ipv6_hdr_t *ipv6_hdr;
  /* The GNRC API currently only supports UDP. */
  gnrc_pktsnip_t *udp;
  udp_hdr_t *udp_hdr;
  const gnrc_nettype_t type = GNRC_NETTYPE_UDP;

  assert(sock);
  assert(packet);

  if ((sock->flags & COAP_SOCKET_CAN_READ) == 0) {
    coap_log(LOG_DEBUG, "coap_network_read: COAP_SOCKET_CAN_READ not set\n");
    return -1;
  } else {
    /* clear has-data flag */
    sock->flags &= ~COAP_SOCKET_CAN_READ;
  }

  /* Search for the transport header in the packet received from the
   * network interface driver. */
  udp = gnrc_pktsnip_search_type(sock->pkt, type);
  ipv6_hdr = gnrc_ipv6_get_header(sock->pkt);

  if (!ipv6_hdr || !udp || !(udp_hdr = (udp_hdr_t *)udp->data)) {
    coap_log(LOG_DEBUG, "no UDP header found in packet\n");
    return -EFAULT;
  }
  udp_hdr_print(udp_hdr);

  len = (size_t)gnrc_pkt_len_upto(sock->pkt, type) - sizeof(udp_hdr_t);
  coap_log(LOG_DEBUG, "coap_network_read: recvfrom got %zd bytes\n", len);
  if (len > COAP_RXBUFFER_SIZE) {
    coap_log(LOG_WARNING, "packet exceeds buffer size, truncated\n");
    len = COAP_RXBUFFER_SIZE;
  }
  packet->ifindex = sock->fd;

  assert(sizeof(struct in6_addr) == sizeof(ipv6_addr_t));
  packet->src.size = sizeof(struct sockaddr_in6);
  memset(&packet->src.addr, 0, sizeof(packet->src.addr));
  packet->src.addr.sin6.sin6_family = AF_INET6;
  memcpy(&packet->src.addr.sin6.sin6_addr, &ipv6_hdr->src, sizeof(ipv6_addr_t));
  memcpy(&packet->src.addr.sin6.sin6_port, &udp_hdr->src_port, sizeof(udp_hdr->src_port));

  packet->dst.size = sizeof(struct sockaddr_in6);
  memset(&packet->dst.addr, 0, sizeof(packet->dst.addr));
  packet->dst.addr.sin6.sin6_family = AF_INET6;
  memcpy(&packet->dst.addr.sin6.sin6_addr, &ipv6_hdr->dst, sizeof(ipv6_addr_t));
  memcpy(&packet->dst.addr.sin6.sin6_port, &udp_hdr->dst_port, sizeof(udp_hdr->src_port));

  packet->ifindex = sock->fd;
  packet->length = (len > 0) ? len : 0;
  memcpy(packet->payload, (uint8_t*)udp_hdr + sizeof(udp_hdr_t), len);
  if (LOG_DEBUG <= coap_get_log_level()) {
    unsigned char addr_str[INET6_ADDRSTRLEN + 8];

    if (coap_print_addr(&packet->src, addr_str, INET6_ADDRSTRLEN + 8)) {
      coap_log(LOG_DEBUG, "received %zd bytes from %s\n", len, addr_str);
    }
  }

  return len;
}

static msg_t _msg_q[LIBCOAP_MSG_QUEUE_SIZE];

void
coap_riot_startup(void) {
  msg_init_queue(_msg_q, LIBCOAP_MSG_QUEUE_SIZE);
}

/**
 * Returns the port of @p addr in network byte order or 0 on error.
 */
static uint16_t
get_port(const coap_address_t *addr) {
  if (addr) {
    switch (addr->addr.sa.sa_family) {
    case AF_INET: return addr->addr.sin.sin_port;
    case AF_INET6: return addr->addr.sin6.sin6_port;
    default:
      ;
    }
  }
  return 0;
}

int
coap_run_once(coap_context_t *ctx, unsigned timeout_ms) {
  coap_tick_t before, now;
  coap_socket_t *sockets[LIBCOAP_MAX_SOCKETS];
  unsigned int num_sockets = 0, timeout;
  gnrc_netreg_entry_t coap_reg =
    GNRC_NETREG_ENTRY_INIT_PID(GNRC_NETREG_DEMUX_CTX_ALL, thread_getpid());
  msg_t msg;

  coap_ticks(&before);

  timeout = coap_write(ctx, sockets, (unsigned int)(sizeof(sockets) / sizeof(sockets[0])), &num_sockets, before);
  if (timeout == 0 || timeout_ms < timeout)
    timeout = timeout_ms;

  if (num_sockets > 0) {
    gnrc_netreg_register(GNRC_NETTYPE_UDP, &coap_reg);
  }

  if (timeout == 0 || timeout_ms < timeout)
    timeout = timeout_ms;

  xtimer_msg_receive_timeout(&msg, timeout_ms * US_PER_SEC);
  switch (msg.type) {
  case GNRC_NETAPI_MSG_TYPE_RCV: {
    coap_endpoint_t *ep;
    udp_hdr_t *udp_hdr = get_udp_header((gnrc_pktsnip_t *)msg.content.ptr);
    if (!udp_hdr)
      break;

    /* set read flag only for the destination of the received packet */
    LL_FOREACH(ctx->endpoint, ep) {
      if (get_port(&ep->bind_addr) == udp_hdr->dst_port.u16) {
        for (unsigned int i = 0; i < num_sockets; i++) {
          if ((ep->sock.fd == sockets[i]->fd) &&
              (sockets[i]->flags & COAP_SOCKET_WANT_READ)) {
            coap_log(LOG_DEBUG, "fd %d on port %u can read\n",
                     ep->sock.fd, ntohs(get_port(&ep->bind_addr)));
            sockets[i]->flags |= COAP_SOCKET_CAN_READ;
            sockets[i]->pkt = msg.content.ptr;
            break;              /* found one, finish loop */
          }
        }
      }
    }
    break;
  }
  case GNRC_NETAPI_MSG_TYPE_SND:
    break;
  case GNRC_NETAPI_MSG_TYPE_SET:
    /* fall through */
  case GNRC_NETAPI_MSG_TYPE_GET:
    break;
  default:
    break;
  }

  coap_ticks(&now);
  coap_read(ctx, now);

  /* cleanup */
  gnrc_netreg_unregister(GNRC_NETTYPE_UDP, &coap_reg);

  return (int)(((now - before) * 1000) / COAP_TICKS_PER_SECOND);
}
