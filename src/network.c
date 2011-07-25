/* Copyright 2011 Nick Mathewson, George Kadianakis
   See LICENSE for other credits and copying information
*/

#include "util.h"

#define NETWORK_PRIVATE
#include "network.h"

#include "container.h"
#include "main.h"
#include "socks.h"
#include "protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <event2/util.h>

#ifdef _WIN32
#include <ws2tcpip.h>  /* socklen_t */
#endif

/** All our listeners. */
static smartlist_t *listeners;

struct listener_t {
  struct evconnlistener *listener;
  protocol_params_t *proto_params;
};

/** All active connections.  */
static smartlist_t *connections;

/** Flag toggled when obfsproxy is shutting down. It blocks new
    connections and shutdowns when the last connection is closed. */
static int shutting_down=0;

static void simple_client_listener_cb(struct evconnlistener *evcl,
   evutil_socket_t fd, struct sockaddr *sourceaddr, int socklen, void *arg);
static void socks_client_listener_cb(struct evconnlistener *evcl,
   evutil_socket_t fd, struct sockaddr *sourceaddr, int socklen, void *arg);
static void simple_server_listener_cb(struct evconnlistener *evcl,
   evutil_socket_t fd, struct sockaddr *sourceaddr, int socklen, void *arg);

static void conn_free(conn_t *conn);
static void close_all_connections(void);

static void close_conn_on_flush(struct bufferevent *bev, void *arg);

static void upstream_read_cb(struct bufferevent *bev, void *arg);
static void downstream_read_cb(struct bufferevent *bev, void *arg);
static void socks_read_cb(struct bufferevent *bev, void *arg);

static void input_event_cb(struct bufferevent *bev, short what, void *arg);
static void output_event_cb(struct bufferevent *bev, short what, void *arg);
static void socks_event_cb(struct bufferevent *bev, short what, void *arg);

/**
   Puts obfsproxy's networking subsystem on "closing time" mode. This
   means that we stop accepting new connections and we shutdown when
   the last connection is closed.

   If 'barbaric' is set, we forcefully close all open connections and
   finish shutdown.

   (Only called by signal handlers)
*/
void
start_shutdown(int barbaric)
{
  if (!shutting_down)
    shutting_down=1;

  if (barbaric)
    close_all_connections();

  if (connections && smartlist_len(connections) == 0) {
    smartlist_free(connections);
    connections = NULL;
  }

  if (!connections)
    finish_shutdown();
}

/**
   Closes all open connections.
*/
static void
close_all_connections(void)
{
  if (!connections)
    return;
  SMARTLIST_FOREACH(connections, conn_t *, conn,
                    { conn_free(conn); });
  smartlist_free(connections);
  connections = NULL;
}

/**
   This function spawns a listener configured according to the
   provided 'protocol_params_t' object'.  Returns the listener on
   success, NULL on fail.

   If it succeeds, the new listener object takes ownership of the
   protocol_params_t object provided; if it fails, the protocol_params_t
   object is deallocated.
*/
listener_t *
listener_new(struct event_base *base,
             protocol_params_t *params)
{
  const unsigned flags =
    LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC|LEV_OPT_REUSEABLE;
  evconnlistener_cb callback;
  listener_t *lsn = xzalloc(sizeof(listener_t));

  switch (params->mode) {
  case LSN_SIMPLE_CLIENT: callback = simple_client_listener_cb; break;
  case LSN_SIMPLE_SERVER: callback = simple_server_listener_cb; break;
  case LSN_SOCKS_CLIENT:  callback = socks_client_listener_cb;  break;
  default: obfs_abort();
  }

  lsn->proto_params = params;
  lsn->listener =
    evconnlistener_new_bind(base, callback, lsn, flags, -1,
                            params->listen_addr->ai_addr,
                            params->listen_addr->ai_addrlen);

  if (!lsn->listener) {
    log_warn("Failed to create listener!");
    proto_params_free(params);
    free(lsn);
    return NULL;
  }

  /* If we don't have a listener list, create one now. */
  if (!listeners)
    listeners = smartlist_create();
  smartlist_add(listeners, lsn);

  return lsn;
}

/**
   Deallocates listener_t 'lsn'.
*/
static void
listener_free(listener_t *lsn)
{
  if (lsn->listener)
    evconnlistener_free(lsn->listener);
  if (lsn->proto_params)
    proto_params_free(lsn->proto_params);
  free(lsn);
}

/**
   Frees all active listeners.
*/
void
free_all_listeners(void)
{
  if (!listeners)
    return;
  log_info("Closing all listeners.");

  SMARTLIST_FOREACH(listeners, listener_t *, lsn,
                    { listener_free(lsn); });
  smartlist_free(listeners);
  listeners = NULL;
}

/**
   This function is called when an upstream client connects to us in
   simple client mode.
*/
static void
simple_client_listener_cb(struct evconnlistener *evcl,
                          evutil_socket_t fd, struct sockaddr *sourceaddr,
                          int socklen, void *arg)
{
  listener_t *lsn = arg;
  struct event_base *base;
  conn_t *conn = xzalloc(sizeof(conn_t));

  log_debug("%s: connection attempt.", __func__);

  conn->mode = lsn->proto_params->mode;
  obfs_assert(conn->mode == LSN_SIMPLE_CLIENT);

  conn->proto = proto_create(lsn->proto_params);
  if (!conn->proto) {
    log_warn("Creation of protocol object failed! Closing connection.");
    goto err;
  }

  /* New bufferevent to wrap socket we received. */
  base = evconnlistener_get_base(lsn->listener);
  conn->input = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
  if (!conn->input)
    goto err;
  fd = -1; /* prevent double-close */

  /* New bufferevent to connect to the target address */
  conn->output = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
  if (!conn->output)
    goto err;

  bufferevent_setcb(conn->input, upstream_read_cb, NULL, input_event_cb, conn);
  /* don't enable the input side for reading at this point; wait till we
     have a connection to the target */

  bufferevent_setcb(conn->output,
                    downstream_read_cb, NULL, output_event_cb, conn);

  /* Queue handshake, if any, before connecting. */
  if (proto_handshake(conn->proto, bufferevent_get_output(conn->output)) < 0)
    goto err;

  /* Launch the connect attempt. */
  if (bufferevent_socket_connect(conn->output,
                                 lsn->proto_params->target_addr->ai_addr,
                                 lsn->proto_params->target_addr->ai_addrlen)<0)
    goto err;

  bufferevent_enable(conn->output, EV_READ|EV_WRITE);

  /* add conn to the connection list */
  if (!connections)
    connections = smartlist_create();
  smartlist_add(connections, conn);

  log_debug("%s: setup completed, %d connections",
            __func__, smartlist_len(connections));
  return;

 err:
  if (conn)
    conn_free(conn);
  if (fd >= 0)
    evutil_closesocket(fd);
}

/**
   This function is called when an upstream client connects to us in
   socks mode.
*/
static void
socks_client_listener_cb(struct evconnlistener *evcl,
                         evutil_socket_t fd, struct sockaddr *sourceaddr,
                         int socklen, void *arg)
{
  listener_t *lsn = arg;
  struct event_base *base;
  conn_t *conn = xzalloc(sizeof(conn_t));

  log_debug("%s: connection attempt.", __func__);

  conn->mode = lsn->proto_params->mode;
  obfs_assert(conn->mode == LSN_SOCKS_CLIENT);

  conn->proto = proto_create(lsn->proto_params);
  if (!conn->proto) {
    log_warn("Creation of protocol object failed! Closing connection.");
    goto err;
  }

  /* Construct SOCKS state. */
  conn->socks_state = socks_state_new();

  /* New bufferevent to wrap socket we received. */
  base = evconnlistener_get_base(lsn->listener);
  conn->input = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
  if (!conn->input)
    goto err;
  fd = -1; /* prevent double-close */

  bufferevent_setcb(conn->input, socks_read_cb, NULL, input_event_cb, conn);
  bufferevent_enable(conn->input, EV_READ|EV_WRITE);

  /* Do not create an output bufferevent at this time; the socks
     handler will do it after we know where we're connecting */

  /* add conn to the connection list */
  if (!connections)
    connections = smartlist_create();
  smartlist_add(connections, conn);

  log_debug("%s: setup completed, %d connections",
            __func__, smartlist_len(connections));
  return;

 err:
  if (conn)
    conn_free(conn);
  if (fd >= 0)
    evutil_closesocket(fd);
}

/**
   This function is called when a remote client connects to us in
   server mode.
*/
static void
simple_server_listener_cb(struct evconnlistener *evcl,
                          evutil_socket_t fd, struct sockaddr *sourceaddr,
                          int socklen, void *arg)
{
  listener_t *lsn = arg;
  struct event_base *base;
  conn_t *conn = xzalloc(sizeof(conn_t));

  log_debug("%s: connection attempt.", __func__);

  conn->mode = lsn->proto_params->mode;
  obfs_assert(conn->mode == LSN_SIMPLE_SERVER);

  conn->proto = proto_create(lsn->proto_params);
  if (!conn->proto) {
    log_warn("Creation of protocol object failed! Closing connection.");
    goto err;
  }

  /* New bufferevent to wrap socket we received. */
  base = evconnlistener_get_base(lsn->listener);
  conn->input = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
  if (!conn->input)
    goto err;
  fd = -1; /* prevent double-close */

  bufferevent_setcb(conn->input, downstream_read_cb, NULL, input_event_cb, conn);

  /* don't enable the input side for reading at this point; wait till we
     have a connection to the target */

  /* New bufferevent to connect to the target address */
  conn->output = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
  if (!conn->output)
    goto err;

  bufferevent_setcb(conn->output, upstream_read_cb, NULL,
                    output_event_cb, conn);

  /* Queue handshake, if any, before connecting. */
  if (proto_handshake(conn->proto,
                      bufferevent_get_output(conn->input))<0)
    goto err;

  if (bufferevent_socket_connect(conn->output,
                                 lsn->proto_params->target_addr->ai_addr,
                                 lsn->proto_params->target_addr->ai_addrlen)<0)
    goto err;

  bufferevent_enable(conn->output, EV_READ|EV_WRITE);

  /* add conn to the connection list */
  if (!connections)
    connections = smartlist_create();
  smartlist_add(connections, conn);

  log_debug("Connection setup completed. "
            "We currently have %d connections!", smartlist_len(connections));
  return;

 err:
  if (conn)
    conn_free(conn);
  if (fd >= 0)
    evutil_closesocket(fd);
}

/**
   Deallocates conn_t 'conn'.
*/
static void
conn_free(conn_t *conn)
{
  if (conn->proto)
    proto_destroy(conn->proto);
  if (conn->socks_state)
    socks_state_free(conn->socks_state);
  if (conn->input)
    bufferevent_free(conn->input);
  if (conn->output)
    bufferevent_free(conn->output);

  memset(conn, 0x99, sizeof(conn_t));
  free(conn);
}

/**
   Closes a fully open connection.
*/
static void
close_conn(conn_t *conn)
{
  obfs_assert(connections);
  smartlist_remove(connections, conn);
  conn_free(conn);
  log_debug("Connection destroyed. "
            "We currently have %d connections!", smartlist_len(connections));

  /** If this was the last connection AND we are shutting down,
      finish shutdown. */
  if (smartlist_len(connections) == 0) {
    smartlist_free(connections);
    connections = NULL;
  }

  if (!connections && shutting_down)
    finish_shutdown();
}

/**
   Closes associated connection if the output evbuffer of 'bev' is
   empty.
*/
static void
close_conn_on_flush(struct bufferevent *bev, void *arg)
{
  conn_t *conn = arg;

  if (evbuffer_get_length(bufferevent_get_output(bev)) == 0)
    close_conn(conn);
}

/**
    This callback is responsible for handling SOCKS traffic.
*/
static void
socks_read_cb(struct bufferevent *bev, void *arg)
{
  conn_t *conn = arg;
  //struct bufferevent *other;
  enum socks_ret socks_ret;
  obfs_assert(bev == conn->input); /* socks only makes sense on the input side */

  do {
    enum socks_status_t status = socks_state_get_status(conn->socks_state);
    if (status == ST_SENT_REPLY) {
      /* We shouldn't be here. */
      obfs_abort();
    } else if (status == ST_HAVE_ADDR) {
      int af, r, port;
      const char *addr=NULL;
      r = socks_state_get_address(conn->socks_state, &af, &addr, &port);
      obfs_assert(r==0);
      conn->output = bufferevent_socket_new(bufferevent_get_base(conn->input),
                                            -1,
                                            BEV_OPT_CLOSE_ON_FREE);

      bufferevent_setcb(conn->output, downstream_read_cb, NULL,
                        socks_event_cb, conn);

      /* Queue handshake, if any, before connecting. */
      if (proto_handshake(conn->proto,
                          bufferevent_get_output(conn->output))<0) {
        /* XXXX send socks reply */
        close_conn(conn);
        return;
      }

      r = bufferevent_socket_connect_hostname(conn->output,
                                              get_evdns_base(),
                                              af, addr, port);
      bufferevent_enable(conn->output, EV_READ|EV_WRITE);
      log_debug("socket_connect_hostname said %d! (%s,%d)", r, addr, port);

      if (r < 0) {
        /* XXXX send socks reply */
        close_conn(conn);
        return;
      }
      bufferevent_disable(conn->input, EV_READ|EV_WRITE);
      /* ignore data XXX */
      return;
    }

    socks_ret = handle_socks(bufferevent_get_input(bev),
                     bufferevent_get_output(bev), conn->socks_state);
  } while (socks_ret == SOCKS_GOOD);

  if (socks_ret == SOCKS_INCOMPLETE)
    return; /* need to read more data. */
  else if (socks_ret == SOCKS_BROKEN)
    close_conn(conn); /* XXXX send socks reply */
  else if (socks_ret == SOCKS_CMD_NOT_CONNECT) {
    bufferevent_enable(bev, EV_WRITE);
    bufferevent_disable(bev, EV_READ);
    socks5_send_reply(bufferevent_get_output(bev), conn->socks_state,
                      SOCKS5_FAILED_UNSUPPORTED);
    bufferevent_setcb(bev, NULL,
                      close_conn_on_flush, output_event_cb, conn);
    return;
  }
}

/**
   This callback is responsible for handling "upstream" traffic --
   traffic coming in from the higher-level client or server that needs
   to be obfuscated and transmitted.
 */
static void
upstream_read_cb(struct bufferevent *bev, void *arg)
{
  conn_t *conn = arg;
  struct bufferevent *other;
  other = (bev == conn->input) ? conn->output : conn->input;

  log_debug("Got data on upstream side");
  if (proto_send(conn->proto,
                 bufferevent_get_input(bev),
                 bufferevent_get_output(other)) < 0)
    close_conn(conn);
}

/**
   This callback is responsible for handling "downstream" traffic --
   traffic coming in from our remote peer that needs to be deobfuscated
   and passed to the upstream client or server.
 */
static void
downstream_read_cb(struct bufferevent *bev, void *arg)
{
  conn_t *conn = arg;
  struct bufferevent *other;
  other = (bev == conn->input) ? conn->output : conn->input;
  enum recv_ret r;

  log_debug("Got data on downstream side");
  r = proto_recv(conn->proto,
                 bufferevent_get_input(bev),
                 bufferevent_get_output(other));

  if (r == RECV_BAD)
    close_conn(conn);
  else if (r == RECV_SEND_PENDING)
    proto_send(conn->proto,
               bufferevent_get_input(conn->input),
               bufferevent_get_output(conn->output));
}

/**
   Something broke in our connection or we reached EOF.
   We prepare the connection to be closed ASAP.
 */
static void
error_or_eof(conn_t *conn,
             struct bufferevent *bev_err, struct bufferevent *bev_flush)
{
  log_debug("error_or_eof");

  if (conn->flushing || ! conn->is_open ||
      0 == evbuffer_get_length(bufferevent_get_output(bev_flush))) {
    close_conn(conn);
    return;
  }

  conn->flushing = 1;
  /* Stop reading and writing; wait for the other side to flush if it has
   * data. */
  bufferevent_disable(bev_err, EV_READ|EV_WRITE);
  bufferevent_disable(bev_flush, EV_READ);

  bufferevent_setcb(bev_flush, NULL,
                    close_conn_on_flush, output_event_cb, conn);
  bufferevent_enable(bev_flush, EV_WRITE);
}

/**
   Called when an "event" happens on conn->input.
   On the input side, all such events are error conditions.
 */
static void
input_event_cb(struct bufferevent *bev, short what, void *arg)
{
  conn_t *conn = arg;
  obfs_assert(bev == conn->input);

  /* It should be impossible to get BEV_EVENT_CONNECTED on this side. */
  obfs_assert(what & (BEV_EVENT_EOF|BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT));
  obfs_assert(!(what & BEV_EVENT_CONNECTED));

  log_warn("Got error: %s",
           evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
  error_or_eof(conn, bev, conn->output);
}

/**
   Called when an "event" happens on conn->output.
   In addition to the error cases dealt with above, this side can see
   BEV_EVENT_CONNECTED which indicates that the output connection is
   now open.
 */
static void
output_event_cb(struct bufferevent *bev, short what, void *arg)
{
  conn_t *conn = arg;
  obfs_assert(bev == conn->output);

  /* If the connection is terminating *OR* if we got one of the error
     events, close this connection soon. */
  if (conn->flushing ||
      (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT))) {
    log_warn("Got error: %s",
             evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    error_or_eof(conn, bev, conn->input);
    return;
  }

  /* Upon successful connection, go ahead and enable traffic on the
     input side. */
  if (what & BEV_EVENT_CONNECTED) {
    conn->is_open = 1;
    log_debug("Connection done") ;
    bufferevent_enable(conn->input, EV_READ|EV_WRITE);
    return;
  }

  /* unrecognized event */
  obfs_abort();
}

/**
   Called when an "event" happens on conn->output in socks mode.
   Handles the same cases as output_event_cb but must also generate
   appropriate socks messages back on the input side.
 */
static void
socks_event_cb(struct bufferevent *bev, short what, void *arg)
{
  conn_t *conn = arg;
  obfs_assert(bev == conn->output);

  /* If we got an error while in the ST_HAVE_ADDR state, chances are
     that we failed connecting to the host requested by the CONNECT
     call. This means that we should send a negative SOCKS reply back
     to the client and terminate the connection. */
  if ((what & BEV_EVENT_ERROR) &&
      socks_state_get_status(conn->socks_state) == ST_HAVE_ADDR) {
    log_debug("Connection failed");
    /* Enable EV_WRITE so that we can send the response.
       Disable EV_READ so that we don't get more stuff from the client. */
    bufferevent_enable(conn->input, EV_WRITE);
    bufferevent_disable(conn->input, EV_READ);
    socks_send_reply(conn->socks_state, bufferevent_get_output(conn->input),
                     evutil_socket_geterror(bufferevent_getfd(bev)));
    bufferevent_setcb(conn->input, NULL,
                      close_conn_on_flush, output_event_cb, conn);
    return;
  }

  /* Additional work to do for BEV_EVENT_CONNECTED: send a happy
     response to the client and switch to the actual obfuscated
     protocol handlers. */
  if (what & BEV_EVENT_CONNECTED) {
    struct sockaddr_storage ss;
    struct sockaddr *sa = (struct sockaddr*)&ss;
    socklen_t slen = sizeof(&ss);
    obfs_assert(conn->socks_state);
    if (getpeername(bufferevent_getfd(bev), sa, &slen) == 0) {
      /* Figure out where we actually connected to so that we can tell the
       * socks client */
      socks_state_set_address(conn->socks_state, sa);
    }
    socks_send_reply(conn->socks_state,
                     bufferevent_get_output(conn->input), 0);
    /* we sent a socks reply.  We can finally move over to being a regular
       input bufferevent. */
    socks_state_free(conn->socks_state);
    conn->socks_state = NULL;
    bufferevent_setcb(conn->input,
                      upstream_read_cb, NULL, input_event_cb, conn);
    bufferevent_setcb(conn->output,
                      downstream_read_cb, NULL, output_event_cb, conn);
    if (evbuffer_get_length(bufferevent_get_input(conn->input)) != 0)
      downstream_read_cb(bev, conn->input);
  }

  /* also do everything that's done on a normal connection */
  output_event_cb(bev, what, arg);
}
