//
//  Copyright (C) 2023  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "hash.h"
#include "ident.h"
#include "jit/jit.h"
#include "option.h"
#include "phase.h"
#include "rt/shell.h"
#include "server.h"
#include "sha1.h"
#include "thread.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <jansson.h>

#ifdef __MINGW32__
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#define WS_UPGRADE_VALUE     "websocket"
#define WS_WEBSOCKET_VERSION "13"
#define WS_GUID              "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_GUID_LEN          36
#define WS_KEY_LEN           24
#define WS_KEY_GUID_LEN      (WS_KEY_LEN + WS_GUID_LEN)

#define HTTP_SWITCHING_PROTOCOLS   101
#define HTTP_OK                    200
#define HTTP_BAD_REQUEST           400
#define HTTP_NOT_FOUND             404
#define HTTP_METHOD_NOT_ALLOWED    405
#define HTTP_UPGRADE_REQUIRED      426
#define HTTP_INTERNAL_SERVER_ERROR 500

#define WS_OPCODE_TEXT_FRAME   0x1
#define WS_OPCODE_BINARY_FRAME 0x2
#define WS_OPCODE_CLOSE_FRAME  0x8
#define WS_OPCODE_PING_FRAME   0x9
#define WS_OPCODE_PONG_FRAME   0xa

#define MAX_HTTP_REQUEST 1024

#ifndef __MINGW32__
#define closesocket close
#endif

typedef struct _debug_server debug_server_t;

typedef struct _web_socket {
   int           sock;
   bool          mask;
   bool          closing;
   ws_handler_t  handler;
   size_t        tx_size;
   size_t        tx_wptr;
   size_t        tx_rptr;
   uint8_t      *tx_buf;
   size_t        rx_size;
   size_t        rx_wptr;
   size_t        rx_rptr;
   uint8_t      *rx_buf;
} web_socket_t;

typedef struct _packet_buf {
   char  *buf;
   size_t alloc;
   size_t wptr;
   size_t rptr;
} packet_buf_t;

typedef struct {
   debug_server_t *(*new_server)(void);
   void (*free_server)(debug_server_t *);
   void (*new_connection)(debug_server_t *, int);
   int (*fill_fd_set)(debug_server_t *, fd_set *, fd_set *);
   void (*poll_sockets)(debug_server_t *, fd_set *, fd_set *);
   void (*shutdown)(debug_server_t *);
} server_proto_t;

typedef struct _debug_server {
   const server_proto_t *proto;
   tcl_shell_t          *shell;
   bool                  shutdown;
   bool                  banner;
   int                   sock;
   tree_t                top;
   packet_buf_t         *packetbuf;
   const char           *init_cmd;
} debug_server_t;

typedef struct {
   debug_server_t  server;
   web_socket_t   *websocket;
} http_server_t;

typedef struct {
   debug_server_t server;
   int            sock;
   size_t         rx_size;
   size_t         rx_wptr;
   size_t         rx_rptr;
   char          *rx_buf;
   size_t         tx_size;
   size_t         tx_wptr;
   size_t         tx_rptr;
   uint8_t       *tx_buf;
} cxxrtl_server_t;

////////////////////////////////////////////////////////////////////////////////
// WebSocket wrapper

web_socket_t *ws_new(int sock, const ws_handler_t *handler, bool mask)
{
   web_socket_t *ws = xcalloc(sizeof(web_socket_t));
   ws->sock    = sock;
   ws->mask    = mask;
   ws->handler = *handler;

   return ws;
}

void ws_free(web_socket_t *ws)
{
   free(ws->tx_buf);
   free(ws->rx_buf);
   free(ws);
}

static void ws_queue_buf(web_socket_t *ws, const void *data, size_t size)
{
   if (ws->tx_wptr + size > ws->tx_size) {
      ws->tx_size = MAX(ws->tx_size + size, 1024);
      ws->tx_buf = xrealloc(ws->tx_buf, ws->tx_size);
   }

   memcpy(ws->tx_buf + ws->tx_wptr, data, size);
   ws->tx_wptr += size;
}

static void ws_send(web_socket_t *ws, int opcode, const void *data, size_t size)
{
   const uint8_t size0 = (size < 126 ? size : (size <= UINT16_MAX ? 126 : 127));

   const uint8_t header[2] = {
      0x80 | opcode,
      (ws->mask ? 0x80 : 0x00) | (size0 & 0x7f),
   };
   ws_queue_buf(ws, header, sizeof(header));

   if (size0 == 126) {
      const uint8_t extlength[2] = { PACK_BE16(size) };
      ws_queue_buf(ws, extlength, sizeof(extlength));
   }
   else if (size0 == 127) {
      const uint8_t extlength[8] = { PACK_BE64(size) };
      ws_queue_buf(ws, extlength, sizeof(extlength));
   }

   if (ws->mask) {
      const int key = rand();
      const uint8_t masks[4] = { PACK_BE32(key) };
      ws_queue_buf(ws, masks, sizeof(masks));

      char xord[128];
      for (size_t i = 0; i < size; i += sizeof(xord)) {
         const size_t chunksz = MIN(sizeof(xord), size - i);
         for (int j = 0; j < chunksz; j++)
            xord[j] = ((const uint8_t *)data)[i + j] ^ masks[(i + j) % 4];

         ws_queue_buf(ws, xord, chunksz);
      }
   }
   else if (size > 0)
      ws_queue_buf(ws, data, size);
}

void ws_send_binary(web_socket_t *ws, const void *data, size_t size)
{
   ws_send(ws, WS_OPCODE_BINARY_FRAME, data, size);
}

void ws_send_packet(web_socket_t *ws, packet_buf_t *pb)
{
   ws_send_binary(ws, pb->buf, pb->wptr);
}

void ws_send_text(web_socket_t *ws, const char *text)
{
   ws_send(ws, WS_OPCODE_TEXT_FRAME, text, strlen(text));
}

void ws_send_close(web_socket_t *ws)
{
   ws_send(ws, WS_OPCODE_CLOSE_FRAME, NULL, 0);
}

void ws_send_ping(web_socket_t *ws, const void *data, size_t size)
{
   ws_send(ws, WS_OPCODE_PING_FRAME, data, size);
}

void ws_flush(web_socket_t *ws)
{
   while (ws->tx_wptr != ws->tx_rptr) {
      const size_t chunksz = ws->tx_wptr - ws->tx_rptr;
      const ssize_t nbytes =
         send(ws->sock, (char *)ws->tx_buf + ws->tx_rptr, chunksz, 0);

      if (nbytes == 0)
         break;
      else if (nbytes < 0) {
         ws->closing = true;
         break;
      }

      ws->tx_rptr += nbytes;
   }

   if (ws->tx_wptr == ws->tx_rptr)
      ws->tx_rptr = ws->tx_wptr = 0;
}

void ws_poll(web_socket_t *ws)
{
 read_more:
   if (ws->rx_size - ws->rx_wptr < 1024)
      ws->rx_buf = xrealloc(ws->rx_buf, (ws->rx_size += 1024));

   const ssize_t nbytes = recv(ws->sock, (char *)ws->rx_buf + ws->rx_wptr,
                               ws->rx_size - ws->rx_wptr - 1, 0);
   if (nbytes == -1 && errno == EAGAIN)
      return;
   else if (nbytes <= 0) {
      ws->closing = true;
      return;
   }

   ws->rx_wptr += nbytes;
   assert(ws->rx_wptr <= ws->rx_size);
   assert(ws->rx_rptr < ws->rx_wptr);

   do {
      const size_t rbytes = ws->rx_wptr - ws->rx_rptr;

      if (rbytes < 2)
         goto read_more;   // Not enough for WebSocket header

      uint8_t *frame = ws->rx_buf + ws->rx_rptr;

      // Frame format
      //
      //   0    1     2     3     4 5 6 7  8     9 A B C D E F
      //   FIN  RSV1  RSV2  RSV3  Opcode   Mask  Payload length
      //   Extended payload length (optional)
      //   Masking key (optional)
      //   Payload data

      const bool fin = frame[0] & 0x80;
      const int opcode = frame[0] & 0xf;
      const bool mask = frame[1] & 0x80;
      const int size0 = frame[1] & 0x7f;

      size_t headersz = 2 + (mask ? 4 : 0);
      if (size0 == 126)
         headersz += 2;
      else if (size0 == 127)
         headersz += 8;

      if (rbytes < headersz)
         goto read_more;   // Not enough for extended header

      int flength = size0;
      if (size0 == 127)
         flength = UNPACK_BE64(frame + 2);
      else if (size0 == 126)
         flength = UNPACK_BE16(frame + 2);

      if (rbytes < flength + headersz)
         goto read_more;   // Not enough for full frame

      assert(fin);
      (void)fin;

      if (mask) {
         for (int i = 0; i < flength; i++)
            frame[headersz + i] ^= frame[headersz - 4 + (i % 4)];
      }

      void *payload = frame + headersz;

      switch (opcode) {
      case WS_OPCODE_TEXT_FRAME:
         {
            char *text = payload;
            assert(text + flength < (char *)ws->rx_buf + ws->rx_size);
            text[flength] = '\0';

            if (ws->handler.text_frame != NULL)
               (*ws->handler.text_frame)(ws, text, ws->handler.context);
         }
         break;

      case WS_OPCODE_BINARY_FRAME:
         if (ws->handler.binary_frame != NULL)
            (*ws->handler.binary_frame)(ws, payload, flength,
                                        ws->handler.context);
         break;

      case WS_OPCODE_CLOSE_FRAME:
         ws->closing = true;
         break;

      case WS_OPCODE_PING_FRAME:
         ws_send(ws, WS_OPCODE_PONG_FRAME, payload, flength);
         break;

      case WS_OPCODE_PONG_FRAME:
         if (ws->handler.pong_frame != NULL)
            (*ws->handler.pong_frame)(ws, payload, flength,
                                      ws->handler.context);
         break;

      default:
         DEBUG_ONLY(fatal_trace("unhandled WebSocket opcode %02x", opcode));
         break;
      }

      ws->rx_rptr += flength + headersz;
   } while (ws->rx_rptr < ws->rx_wptr);

   ws->rx_rptr = ws->rx_wptr = 0;
}

bool ws_closing(web_socket_t *ws)
{
   return ws->closing;
}

////////////////////////////////////////////////////////////////////////////////
// Packet buffers

static packet_buf_t *pb_new(void)
{
   packet_buf_t *pb = xcalloc(sizeof(packet_buf_t));
   pb->alloc = 128;
   pb->buf = xmalloc(pb->alloc);

   return pb;
}

static void pb_free(packet_buf_t *pb)
{
   free(pb->buf);
   free(pb);
}

static void pb_grow(packet_buf_t *pb, size_t need)
{
   if (pb->wptr + need > pb->alloc) {
      pb->alloc = MAX(pb->wptr + need, pb->alloc * 2);
      pb->buf = xrealloc(pb->buf, pb->alloc);
   }
}

static void pb_pack_u8(packet_buf_t *pb, uint8_t value)
{
   pb_grow(pb, 1);
   pb->buf[pb->wptr++] = value;
}

static void pb_pack_u16(packet_buf_t *pb, uint16_t value)
{
   pb_grow(pb, 2);
   pb->buf[pb->wptr++] = value >> 8;
   pb->buf[pb->wptr++] = value & 0xff;
}

static void pb_pack_u32(packet_buf_t *pb, uint32_t value)
{
   pb_grow(pb, 4);
   pb->buf[pb->wptr++] = (value >> 24) & 0xff;
   pb->buf[pb->wptr++] = (value >> 16) & 0xff;
   pb->buf[pb->wptr++] = (value >> 8) & 0xff;
   pb->buf[pb->wptr++] = value & 0xff;
}

static void pb_pack_u64(packet_buf_t *pb, uint64_t value)
{
   pb_grow(pb, 8);
   pb->buf[pb->wptr++] = (value >> 56) & 0xff;
   pb->buf[pb->wptr++] = (value >> 48) & 0xff;
   pb->buf[pb->wptr++] = (value >> 40) & 0xff;
   pb->buf[pb->wptr++] = (value >> 32) & 0xff;
   pb->buf[pb->wptr++] = (value >> 24) & 0xff;
   pb->buf[pb->wptr++] = (value >> 16) & 0xff;
   pb->buf[pb->wptr++] = (value >> 8) & 0xff;
   pb->buf[pb->wptr++] = value & 0xff;
}

static void pb_pack_bytes(packet_buf_t *pb, const void *data, size_t len)
{
   pb_grow(pb, len);
   memcpy(pb->buf + pb->wptr, data, len);
   pb->wptr += len;
}

static void pb_pack_str(packet_buf_t *pb, const char *str)
{
   const size_t len = strlen(str);
   assert(len < UINT16_MAX);

   pb_pack_u16(pb, len);
   pb_pack_bytes(pb, str, len);
}

static void pb_pack_ident(packet_buf_t *pb, ident_t ident)
{
   const size_t len = ident_len(ident);
   assert(len < UINT16_MAX);

   pb_pack_u16(pb, len);
   pb_pack_bytes(pb, istr(ident), len);
}

////////////////////////////////////////////////////////////////////////////////
// Generic networking utilities

typedef enum {
   LOG_DEBUG,
   LOG_INFO,
   LOG_WARN,
   LOG_ERROR
} log_level_t;

__attribute__((format(printf, 2, 3)))
static void server_log(log_level_t level, const char *fmt, ...)
{
   if (opt_get_int(OPT_UNIT_TEST))
      return;
   else if (DEBUG_ONLY(false &&) level < LOG_INFO)
      return;

   va_list ap;
   va_start(ap, fmt);

   switch (level) {
   case LOG_DEBUG: color_printf("$#8$D: "); break;
   case LOG_INFO: printf("I: "); break;
   case LOG_WARN: color_printf("$yellow$W: "); break;
   case LOG_ERROR: color_printf("$red$E: "); break;
   }

   vprintf(fmt, ap);
   color_printf("$$\n");
   fflush(stdout);

   va_end(ap);
}

static void send_fully(int fd, const void *data, size_t len)
{
   while (len > 0) {
      ssize_t nbytes = send(fd, data, len, 0);
      if (nbytes <= 0) {
         server_log(LOG_ERROR, "send: %s", strerror(errno));
         return;
      }

      data += nbytes;
      len -= nbytes;
   }
}

static void base64_encode(const void *in, size_t len, text_buf_t *tb)
{
   static const char map[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

   const unsigned char *data = in;

   for (size_t i = 0; i < len; i++) {
      int c0 = (data[i] >> 2) & 0x3F;
      tb_append(tb, map[c0]);
      c0 = (data[i] << 4) & 0x3F;
      if (++i < len)
         c0 |= (data[i] >> 4) & 0x0F;
      tb_append(tb, map[c0]);

      if (i < len) {
         int c1 = (data[i] << 2) & 0x3F;
         if (++i < len)
            c1 |= (data[i] >> 6) & 0x03;
         tb_append(tb, map[c1]);
      }
      else {
         ++i;
         tb_append(tb, '=');
      }

      if (i < len) {
         int c2 = data[i] & 0x3F;
         tb_append(tb, map[c2]);
      }
      else
         tb_append(tb, '=');
   }
}

static packet_buf_t *fresh_packet_buffer(debug_server_t *server)
{
   server->packetbuf->wptr = 0;
   return server->packetbuf;
}

////////////////////////////////////////////////////////////////////////////////
// HTTP and WebSocket server

static void send_http_headers(int fd, int status, const char *type, size_t len,
                              const char *headers)
{
   LOCAL_TEXT_BUF date = tb_new();
   tb_strftime(date, "G%a, %d %b %Y %H:%M:%S %Z", time(NULL));

   char buf[512];
   const int nbytes = checked_sprintf(buf, sizeof(buf),
                                      "HTTP/1.1 %d\r\n"
                                      "Date: %s\r\n"
                                      "Content-Type: %s; charset=UTF-8\r\n"
                                      "Content-Length: %zu\r\n"
                                      "%s\r\n",
                                      status, tb_get(date), type, len, headers);

   send_fully(fd, buf, nbytes);
}

static void send_page(int fd, int status, const char *page)
{
   const size_t len = strlen(page);
   send_http_headers(fd, status, "text/html", len, "");
   send_fully(fd, page, len);
}

#ifdef ENABLE_GUI
static void send_file(int fd, const char *file, const char *mime)
{
   FILE *f = fopen(file, "rb");
   if (f == NULL) {
      send_page(fd, HTTP_NOT_FOUND, "File not found");
      return;
   }

   file_info_t info;
   if (!get_handle_info(fileno(f), &info)) {
      send_page(fd, HTTP_INTERNAL_SERVER_ERROR, "Cannot stat file");
      goto out_close;
   }

   send_http_headers(fd, HTTP_OK, mime, info.size, "");

   char buf[1024];
   for (ssize_t remain = info.size, nbytes; remain > 0; remain -= nbytes) {
      memset(buf, '\0', sizeof(buf));

      if ((nbytes = fread(buf, 1, MIN(remain, sizeof(buf)), f)) == 0) {
         server_log(LOG_ERROR, "fread: %s: %s", file, strerror(errno));
         goto out_close;
      }

      send_fully(fd, buf, nbytes);
   }

 out_close:
   fclose(f);
}
#endif

static void handle_text_frame(web_socket_t *ws, const char *text, void *context)
{
   debug_server_t *server = context;

   const char *result = NULL;
   if (shell_eval(server->shell, text, &result) && *result != '\0')
      ws_send_text(ws, result);
}

static void handle_binary_frame(web_socket_t *ws, const void *data,
                                size_t length, void *context)
{
   debug_server_t *server = context;

   if (length == 0) {
      server_log(LOG_WARN, "ignoring zero-length binary frame");
      return;
   }

   const c2s_opcode_t op = *(const uint8_t *)data;
   switch (op) {
   case C2S_SHUTDOWN:
      server->shutdown = true;
      break;
   default:
      server_log(LOG_ERROR, "unhandled client to server opcode %02x", op);
      break;
   }
}

static void kill_http_connection(http_server_t *http)
{
   diag_set_consumer(NULL, NULL);

   closesocket(http->websocket->sock);

   ws_free(http->websocket);
   http->websocket = NULL;
}

static void tunnel_diag(diag_t *d, void *context)
{
   http_server_t *http = container_of(context, http_server_t, server);

   if (http->websocket != NULL) {
      ws_send_text(http->websocket, diag_get_text(d));
   }
   else
      server_log(LOG_INFO, "%s", diag_get_text(d));
}

static void tunnel_output(const char *buf, size_t nchars, void *user)
{
   http_server_t *http = container_of(user, http_server_t, server);
   ws_send(http->websocket, WS_OPCODE_TEXT_FRAME, buf, nchars);
}

static void tunnel_backchannel(const char *buf, size_t nchars, void *user)
{
   http_server_t *http = container_of(user, http_server_t, server);

   packet_buf_t *pb = fresh_packet_buffer(&(http->server));
   pb_pack_u8(pb, S2C_BACKCHANNEL);
   pb_pack_u32(pb, nchars);
   pb_pack_bytes(pb, buf, nchars);
   ws_send_packet(http->websocket, pb);
}

static void add_wave_handler(ident_t path, const char *enc, void *user)
{
   http_server_t *http = container_of(user, http_server_t, server);

   packet_buf_t *pb = fresh_packet_buffer(&(http->server));
   pb_pack_u8(pb, S2C_ADD_WAVE);
   pb_pack_ident(pb, path);
   pb_pack_str(pb, enc);
   ws_send_packet(http->websocket, pb);
}

static void signal_update_handler(ident_t path, uint64_t now, rt_signal_t *s,
                                  const char *enc, void *user)
{
   http_server_t *http = container_of(user, http_server_t, server);

   packet_buf_t *pb = fresh_packet_buffer(&(http->server));
   pb_pack_u8(pb, S2C_SIGNAL_UPDATE);
   pb_pack_ident(pb, path);
   pb_pack_str(pb, enc);
   ws_send_packet(http->websocket, pb);
}

static void start_sim_handler(ident_t top, void *user)
{
   http_server_t *http = container_of(user, http_server_t, server);

   packet_buf_t *pb = fresh_packet_buffer(&(http->server));
   pb_pack_u8(pb, S2C_START_SIM);
   pb_pack_ident(pb, top);
   ws_send_packet(http->websocket, pb);
}

static void restart_sim_handler(void *user)
{
   http_server_t *http = container_of(user, http_server_t, server);

   packet_buf_t *pb = fresh_packet_buffer(&(http->server));
   pb_pack_u8(pb, S2C_RESTART_SIM);
   ws_send_packet(http->websocket, pb);
}

static void next_time_step_handler(uint64_t now, void *user)
{
   http_server_t *http = container_of(user, http_server_t, server);

   packet_buf_t *pb = fresh_packet_buffer(&(http->server));
   pb_pack_u8(pb, S2C_NEXT_TIME_STEP);
   pb_pack_u64(pb, now);
   ws_send_packet(http->websocket, pb);
}

static void open_websocket(http_server_t *http, int fd)
{
   if (http->websocket != NULL) {
      ws_send_close(http->websocket);
      ws_flush(http->websocket);
      kill_http_connection(http);
   }

   const ws_handler_t handler = {
      .text_frame   = handle_text_frame,
      .binary_frame = handle_binary_frame,
      .context      = &(http->server)
   };

   http->websocket = ws_new(fd, &handler, false);

   diag_set_consumer(tunnel_diag, &(http->server));

   if (http->server.banner)
      shell_print_banner(http->server.shell);

   if (http->server.top != NULL)
      shell_reset(http->server.shell, http->server.top);

   if (http->server.init_cmd != NULL) {
      packet_buf_t *pb = fresh_packet_buffer(&(http->server));
      pb_pack_u8(pb, S2C_INIT_CMD);
      pb_pack_str(pb, http->server.init_cmd);
      ws_send_packet(http->websocket, pb);
   }
}

static bool get_websocket_accept_value(const char *key, text_buf_t *tb)
{
   if (key == NULL || strlen(key) != WS_KEY_LEN)
      return false;

   char *str LOCAL = xmalloc(WS_KEY_LEN + WS_GUID_LEN + 1);
   strncpy(str, key, (WS_KEY_LEN + 1));
   strncpy(str + WS_KEY_LEN, WS_GUID, WS_GUID_LEN + 1);

   SHA1_CTX ctx;
   SHA1Init(&ctx);
   SHA1Update(&ctx, (unsigned char *)str, WS_KEY_GUID_LEN);

   unsigned char hash[SHA1_LEN];
   SHA1Final(hash, &ctx);

   base64_encode(hash, SHA1_LEN, tb);
   return true;
}

static void websocket_upgrade(http_server_t *http, int fd, const char *method,
                              const char *version, shash_t *headers)
{
   LOCAL_TEXT_BUF tb = tb_new();

   if (strcmp(method, "GET") != 0 || strcmp(version, "HTTP/1.1") != 0) {
      send_page(fd, HTTP_BAD_REQUEST, "Bad request");
      goto out_close;
   }

   const char *ws_version_header = shash_get(headers, "sec-websocket-version");

   if (ws_version_header == NULL
       || strcasecmp(ws_version_header, WS_WEBSOCKET_VERSION) != 0) {

      static const char page[] = "Upgrade required";
      static const char header[] =
         "Sec-WebSocket-Version:" WS_WEBSOCKET_VERSION;

      send_http_headers(fd, HTTP_UPGRADE_REQUIRED, "text/html",
                        sizeof(page), header);
      send_fully(fd, page, sizeof(page));

      goto out_close;
   }

   const char *ws_key_header = shash_get(headers, "sec-websocket-key");

   if (ws_key_header == NULL || strlen(ws_key_header) != WS_KEY_LEN) {
      send_page(fd, HTTP_BAD_REQUEST, "Bad request");
      goto out_close;
   }

   tb_cat(tb, "Connection: upgrade\r\n"
          "Upgrade: websocket\r\n"
          "Sec-WebSocket-Accept: ");

   if (!get_websocket_accept_value(ws_key_header, tb))
      goto out_close;

   tb_cat(tb, "\r\n");

   send_http_headers(fd, HTTP_SWITCHING_PROTOCOLS, "text/html", 0, tb_get(tb));

   open_websocket(http, fd);

   return;   // Socket left open

 out_close:
   closesocket(fd);
}

static bool is_websocket_request(shash_t *headers)
{
   const char *upg_header = shash_get(headers, "upgrade");
   const char *con_header = shash_get(headers, "connection");

   return (upg_header != NULL && con_header != NULL)
          && (strcasecmp(upg_header, WS_UPGRADE_VALUE) == 0)
          && (strcasestr(con_header, "Upgrade") != NULL);
}

#ifdef ENABLE_GUI
static void serve_gui_static_files(int fd, const char *url)
{
   LOCAL_TEXT_BUF tb = tb_new();
   get_data_dir(tb);
   tb_cat(tb, "/gui");

   if (strcmp(url, "/") == 0) {
      tb_cat(tb, "/index.html");
      send_file(fd, tb_get(tb), "text/html");
      return;
   }

   const char *mime = "application/octet-stream";
   const char *dot = strrchr(url, '.');
   if (dot != NULL) {
      static const char *mime_map[][2] = {
         { ".js",  "text/javascript" },
         { ".css", "text/css" },
         { ".map", "application/json" },
      };

      for (int i = 0; i < ARRAY_LEN(mime_map); i++) {
         if (strcmp(dot, mime_map[i][0]) == 0) {
            mime = mime_map[i][1];
            break;
         }
      }
   }

   tb_cat(tb, url);
   send_file(fd, tb_get(tb), mime);
}
#endif

static void handle_http_request(http_server_t *http, int fd,
                                const char *method, const char *url,
                                const char *version, shash_t *headers)
{
   server_log(LOG_DEBUG, "%s %s", method, url);

   if (is_websocket_request(headers)) {
      websocket_upgrade(http, fd, method, version, headers);
      return;    // Socket left open
   }
   else if (strcmp(method, "GET") != 0) {
      send_page(fd, HTTP_METHOD_NOT_ALLOWED, "Method not allowed");
      goto out_close;
   }

#ifdef ENABLE_GUI
   serve_gui_static_files(fd, url);
#else
   send_page(fd, HTTP_NOT_FOUND, "Not found");
#endif

 out_close:
   closesocket(fd);
}

static void http_new_connection(debug_server_t *server, int fd)
{
   char buf[MAX_HTTP_REQUEST + 1];
   size_t reqlen = 0;
   do {
      ssize_t n = recv(fd, buf + reqlen, MAX_HTTP_REQUEST - reqlen, 0);

#ifdef __MINGW32__
      const bool would_block =
         (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK);
#else
      const bool would_block = (n == -1 && errno == EWOULDBLOCK);
#endif

      if (would_block) {
         fd_set rfd;
         FD_ZERO(&rfd);
         FD_SET(fd, &rfd);

         struct timeval tv = {
            .tv_sec = 1,
            .tv_usec = 0
         };

         if (select(fd + 1, &rfd, NULL, NULL, &tv) == -1) {
            server_log(LOG_ERROR, "select: %s", last_os_error());
            goto out_close;
         }

         if (FD_ISSET(fd, &rfd))
            continue;

         server_log(LOG_ERROR, "timeout waiting for HTTP request");
         goto out_close;
      }
      else if (n <= 0) {
         server_log(LOG_ERROR, "recv: %s", last_os_error());
         goto out_close;
      }

      reqlen += n;
      assert(reqlen <= MAX_HTTP_REQUEST);

      if (reqlen == MAX_HTTP_REQUEST) {
         server_log(LOG_ERROR, "HTTP request too big");
         goto out_close;
      }

      buf[reqlen] = '\0';
   } while (strstr(buf, "\r\n\r\n") == NULL);

   const char *method = "GET";
   const char *url = "/";
   const char *version = "";

   char *saveptr, *saveptr2;
   char *line = strtok_r(buf, "\r\n", &saveptr);
   if (line == NULL) {
      server_log(LOG_ERROR, "malformed HTTP request");
      goto out_close;
   }

   method = strtok_r(line, " ", &saveptr2);
   if (method == NULL)
      goto malformed;

   url = strtok_r(NULL, " ", &saveptr2);
   if (url == NULL)
      goto malformed;

   version = strtok_r(NULL, " ", &saveptr2);
   if (version == NULL)
      goto malformed;

   shash_t *headers = shash_new(64);

   while ((line = strtok_r(NULL, "\r\n", &saveptr)) && line[0] != '\0') {
      char *colon = strchr(line, ':');
      if (colon != NULL) {
         *colon = '\0';

         char *value = colon + 1;
         while (*value == ' ')
            value++;

         for (char *p = line; *p; p++)
            *p = tolower_iso88591(*p);

         shash_put(headers, line, value);
      }
   }

   http_server_t *http = container_of(server, http_server_t, server);
   handle_http_request(http, fd, method, url, version, headers);
   shash_free(headers);
   return;

 malformed:
   server_log(LOG_ERROR, "malformed HTTP request");

 out_close:
   closesocket(fd);
}

static int http_fill_fd_set(debug_server_t *server, fd_set *rfd, fd_set *wfd)
{
   http_server_t *http = container_of(server, http_server_t, server);

   if (http->websocket == NULL)
      return -1;

   FD_SET(http->websocket->sock, rfd);

   if (http->websocket->tx_wptr != http->websocket->tx_rptr)
      FD_SET(http->websocket->sock, wfd);

   return http->websocket->sock;
}

static void http_poll_sockets(debug_server_t *server, fd_set *rfd, fd_set *wfd)
{
   http_server_t *http = container_of(server, http_server_t, server);

   if (http->websocket == NULL)
      return;

   if (FD_ISSET(http->websocket->sock, rfd))
      ws_poll(http->websocket);

   if (FD_ISSET(http->websocket->sock, wfd))
      ws_flush(http->websocket);

   if (http->websocket->closing)
      kill_http_connection(http);
}

static void http_shutdown(debug_server_t *server)
{
   http_server_t *http = container_of(server, http_server_t, server);

   if (http->websocket != NULL)
      ws_send_close(http->websocket);
}

static debug_server_t *http_server_new(void)
{
   http_server_t *http = xcalloc(sizeof(http_server_t));
   return &(http->server);
}

static void http_server_free(debug_server_t *server)
{
   http_server_t *http = container_of(server, http_server_t, server);
   assert(http->websocket == NULL);
   free(http);
}

static const server_proto_t http_protocol = {
   .new_server = http_server_new,
   .free_server = http_server_free,
   .new_connection = http_new_connection,
   .fill_fd_set = http_fill_fd_set,
   .poll_sockets = http_poll_sockets,
   .shutdown = http_shutdown,
};

////////////////////////////////////////////////////////////////////////////////
// CXXRTL debug protocol over TCP
//
// https://gist.github.com/whitequark/59520e2de0947da8747061bc2ea91639

static void kill_cxxrtl_connection(cxxrtl_server_t *cxxrtl)
{
   diag_set_consumer(NULL, NULL);

   closesocket(cxxrtl->sock);
   cxxrtl->sock = -1;

   cxxrtl->rx_rptr = cxxrtl->rx_wptr = 0;
}

static void cxxrtl_send(cxxrtl_server_t *cxxrtl, json_t *json)
{
   // TODO: use json_dumpb or json_dump_callback
   char *str LOCAL = json_dumps(json, JSON_COMPACT);
   server_log(LOG_DEBUG, "S->C: %s", str);

   const size_t size = strlen(str) + 1;

   if (cxxrtl->tx_wptr + size > cxxrtl->tx_size) {
      cxxrtl->tx_size = MAX(cxxrtl->tx_size + size, 1024);
      cxxrtl->tx_buf = xrealloc(cxxrtl->tx_buf, cxxrtl->tx_size);
   }

   memcpy(cxxrtl->tx_buf + cxxrtl->tx_wptr, str, size);
   cxxrtl->tx_wptr += size;
}

static void cxxrtl_error(cxxrtl_server_t *cxxrtl, json_t *json,
                         const char *error, const char *message)
{
   json_object_set_new(json, "type", json_string("error"));
   json_object_set_new(json, "error", json_string(error));
   json_object_set_new(json, "message", json_string(message));

   cxxrtl_send(cxxrtl, json);
}

static void handle_greeting(cxxrtl_server_t *cxxrtl, json_t *json)
{
   json_t *version = json_object_get(json, "version");
   if (version == NULL)
      return cxxrtl_error(cxxrtl, json, "parse_error", "Missing version");
   else if (json_integer_value(version) != 0)
      return cxxrtl_error(cxxrtl, json, "version_error", "Epected version 0");

   json_t *commands = json_array();
   json_object_set_new(json, "commands", commands);

   static const char *supported_commands[] = {
      "list_scopes",
      "list_items",
      "reference_items",
      "query_interval",
      "get_simulation_status",
      "run_simulation",
      "pause_simulation",
      "nvc.quit_simulation",
   };

   for (size_t i = 0; i < ARRAY_LEN(supported_commands); i++)
      json_array_append_new(commands, json_string(supported_commands[i]));

   json_t *events = json_array();
   json_object_set_new(json, "events", events);

   static const char *supported_events[] = {
      "simulation_paused",
      "simulation_finished"
   };

   for (size_t i = 0; i < ARRAY_LEN(supported_events); i++)
      json_array_append_new(events, json_string(supported_events[i]));

   json_t *features = json_object();
   json_object_set_new(json, "features", features);

   json_t *encoding = json_array();
   json_object_set_new(json, "item_values_encoding", encoding);

   json_array_append_new(encoding, json_string("base64(u32)"));

   json_object_set_new(features, "encoding", encoding);

   cxxrtl_send(cxxrtl, json);
}

static void handle_get_simulation_status(cxxrtl_server_t *cxxrtl, json_t *json)
{
   json_object_set_new(json, "type", json_string("response"));
   json_object_set_new(json, "status", json_string("paused"));
   json_object_set_new(json, "latest_time", json_string("0.0"));
   json_object_set_new(json, "next_sample_time", json_string("0.0"));

   cxxrtl_send(cxxrtl, json);
}

static void handle_quit_simulation(cxxrtl_server_t *cxxrtl, json_t *json)
{
   cxxrtl->server.shutdown = true;

   json_object_set_new(json, "type", json_string("response"));
   cxxrtl_send(cxxrtl, json);
}

static void handle_command(cxxrtl_server_t *cxxrtl, json_t *json)
{
   json_t *command = json_object_get(json, "command");
   if (command == NULL)
      return cxxrtl_error(cxxrtl, json, "parse_error", "Missing command");

   const char *str = json_string_value(command);
   if (strcmp(str, "get_simulation_status") == 0)
      handle_get_simulation_status(cxxrtl, json);
   else if (strcmp(str, "nvc.quit_simulation") == 0)
      handle_quit_simulation(cxxrtl, json);
   else
      cxxrtl_error(cxxrtl, json, "bad_command", "Invalid command");
}

static void cxxrtl_new_connection(debug_server_t *server, int fd)
{
   cxxrtl_server_t *cxxrtl = container_of(server, cxxrtl_server_t, server);

   if (cxxrtl->sock != -1) {
      server_log(LOG_INFO, "closing old connection");
      closesocket(cxxrtl->sock);
   }

   cxxrtl->sock = fd;
}

static int cxxrtl_fill_fd_set(debug_server_t *server, fd_set *rfd, fd_set *wfd)
{
   cxxrtl_server_t *cxxrtl = container_of(server, cxxrtl_server_t, server);

   if (cxxrtl->sock != -1)
      FD_SET(cxxrtl->sock, rfd);

   if (cxxrtl->tx_wptr != cxxrtl->tx_rptr)
      FD_SET(cxxrtl->sock, wfd);

   return cxxrtl->sock;
}

static void cxxrtl_read_message(cxxrtl_server_t *cxxrtl)
{
   if (cxxrtl->rx_size - cxxrtl->rx_wptr < 1024)
      cxxrtl->rx_buf = xrealloc(cxxrtl->rx_buf, (cxxrtl->rx_size += 1024));

   const ssize_t nbytes = recv(cxxrtl->sock, cxxrtl->rx_buf + cxxrtl->rx_wptr,
                               cxxrtl->rx_size - cxxrtl->rx_wptr, 0);
   if (nbytes == -1 && errno == EAGAIN)
      return;
   else if (nbytes == 0) {
      kill_cxxrtl_connection(cxxrtl);
      return;
   }
   else if (nbytes < 0) {
      server_log(LOG_ERROR, "connection closed: %s", last_os_error());
      kill_cxxrtl_connection(cxxrtl);
      return;
   }

   cxxrtl->rx_wptr += nbytes;
   assert(cxxrtl->rx_wptr <= cxxrtl->rx_size);
   assert(cxxrtl->rx_rptr < cxxrtl->rx_wptr);

   do {
      char *endp = memchr(cxxrtl->rx_buf + cxxrtl->rx_rptr, '\0',
                          cxxrtl->rx_wptr - cxxrtl->rx_rptr);
      if (endp == NULL)
         return;

      server_log(LOG_DEBUG, "C->S: %s", cxxrtl->rx_buf + cxxrtl->rx_rptr);

      json_error_t error;
      json_t *root = json_loads(cxxrtl->rx_buf + cxxrtl->rx_rptr, 0, &error);

      if (endp == cxxrtl->rx_buf + cxxrtl->rx_wptr - 1)
         cxxrtl->rx_wptr = cxxrtl->rx_rptr = 0;
      else
         cxxrtl->rx_rptr = endp - cxxrtl->rx_buf + 1;

      if (!json_is_object(root)) {
         cxxrtl_error(cxxrtl, root, "bad_json", "Nnot a JSON object");
         json_decref(root);
         continue;
      }

      json_t *type = json_object_get(root, "type");
      if (!json_is_string(type)) {
         cxxrtl_error(cxxrtl, root, "parse_error", "Missing type field");
         json_decref(root);
         continue;
      }

      const char *typestr = json_string_value(type);
      if (strcmp(typestr, "greeting") == 0)
         handle_greeting(cxxrtl, root);
      else if (strcmp(typestr, "command") == 0)
         handle_command(cxxrtl, root);
      else
         server_log(LOG_ERROR, "unhandled message type '%s'", typestr);
   } while (cxxrtl->rx_rptr != cxxrtl->rx_wptr);
}

static void cxxrtl_flush(cxxrtl_server_t *cxxrtl)
{
   while (cxxrtl->tx_wptr != cxxrtl->tx_rptr) {
      const size_t chunksz = cxxrtl->tx_wptr - cxxrtl->tx_rptr;
      const ssize_t nbytes =
         send(cxxrtl->sock, cxxrtl->tx_buf + cxxrtl->tx_rptr, chunksz, 0);

      if (nbytes == 0)
         break;
      else if (nbytes < 0) {
         kill_cxxrtl_connection(cxxrtl);
         break;
      }

      cxxrtl->tx_rptr += nbytes;
   }

   if (cxxrtl->tx_wptr == cxxrtl->tx_rptr)
      cxxrtl->tx_rptr = cxxrtl->tx_wptr = 0;
}

static void cxxrtl_poll_sockets(debug_server_t *server, fd_set *rfd,
                                fd_set *wfd)
{
   cxxrtl_server_t *cxxrtl = container_of(server, cxxrtl_server_t, server);

   if (cxxrtl->sock == -1)
      return;

   if (FD_ISSET(cxxrtl->sock, rfd))
      cxxrtl_read_message(cxxrtl);

   if (FD_ISSET(cxxrtl->sock, wfd))
      cxxrtl_flush(cxxrtl);

   if (server->shutdown && cxxrtl->tx_wptr == cxxrtl->tx_rptr) {
      closesocket(cxxrtl->sock);
      cxxrtl->sock = -1;
   }
}

static void cxxrtl_shutdown(debug_server_t *server)
{
   // TODO: send an event to the client?
}

static debug_server_t *cxxrtl_server_new(void)
{
   cxxrtl_server_t *cxxrtl = xcalloc(sizeof(cxxrtl_server_t));
   cxxrtl->sock = -1;

   return &(cxxrtl->server);
}

static void cxxrtl_server_free(debug_server_t *server)
{
   cxxrtl_server_t *cxxrtl = container_of(server, cxxrtl_server_t, server);
   assert(cxxrtl->sock == -1);
   free(cxxrtl->rx_buf);
   free(cxxrtl->tx_buf);
   free(cxxrtl);
}

static const server_proto_t cxxrtl_protocol = {
   .new_server = cxxrtl_server_new,
   .free_server = cxxrtl_server_free,
   .new_connection = cxxrtl_new_connection,
   .fill_fd_set = cxxrtl_fill_fd_set,
   .poll_sockets = cxxrtl_poll_sockets,
   .shutdown = cxxrtl_shutdown,
};

////////////////////////////////////////////////////////////////////////////////
// Server event loop

static void handle_new_connection(debug_server_t *server)
{
   int fd = accept(server->sock, NULL, NULL);
   if (fd < 0) {
      server_log(LOG_ERROR, "accept: %s", last_os_error());
      return;
   }

#ifdef __MINGW32__
   if (ioctlsocket(fd, FIONBIO, &(unsigned long){1})) {
      server_log(LOG_ERROR, "ioctlsocket: %s", last_os_error());
      closesocket(fd);
      return;
   }
#else
   const int flags = fcntl(fd, F_GETFL, 0);
   if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      server_log(LOG_ERROR, "fcntl: %s", last_os_error());
      close(fd);
      return;
   }
#endif

   if (server->proto->new_connection != NULL)
      (*server->proto->new_connection)(server, fd);
}

static int open_server_socket(void)
{
#ifdef __MINGW32__
   WSADATA wsaData;
   if (WSAStartup(MAKEWORD(2, 2), &wsaData))
      fatal_errno("WSAStartup failed");
#endif

   int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (sock < 0)
      fatal_errno("socket");

   if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                  (char *)&(int){1}, sizeof(int)) < 0)
      fatal_errno("setsockopt");

   const uint16_t port = opt_get_int(OPT_SERVER_PORT);

   struct sockaddr_in addr;
   addr.sin_family = AF_INET;
   addr.sin_port = htons(port);
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

   if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
      fatal_errno("bind");

   if (listen(sock, SOMAXCONN) < 0)
      fatal_errno("listen");

   server_log(LOG_INFO, "listening on 127.0.0.1:%u", port);

   return sock;
}

void start_server(server_kind_t kind, jit_factory_t make_jit,
                  unit_registry_t *registry, tree_t top,
                  server_ready_fn_t cb, void *arg, const char *init_cmd)
{
   static const server_proto_t *map[] = {
      [SERVER_HTTP] = &http_protocol,
      [SERVER_CXXRTL] = &cxxrtl_protocol
   };

   debug_server_t *server = (*map[kind]->new_server)();
   server->shell     = shell_new(make_jit, registry);
   server->top       = top;
   server->packetbuf = pb_new();
   server->init_cmd  = init_cmd;
   server->banner    = !opt_get_int(OPT_UNIT_TEST);
   server->proto     = map[kind];

   shell_handler_t handler = {
      .add_wave = add_wave_handler,
      .signal_update = signal_update_handler,
      .stderr_write = tunnel_output,
      .stdout_write = tunnel_output,
      .backchannel_write = tunnel_backchannel,
      .start_sim = start_sim_handler,
      .restart_sim = restart_sim_handler,
      .next_time_step = next_time_step_handler,
      .context = server
   };
   shell_set_handler(server->shell, &handler);

   server->sock = open_server_socket();

   if (cb != NULL)
      (*cb)(arg);

   for (;;) {
      fd_set rfd, wfd, efd;
      FD_ZERO(&rfd);
      FD_ZERO(&wfd);
      FD_ZERO(&efd);

      int max_fd = -1;

      if (server->sock != -1) {
         FD_SET(server->sock, &rfd);
         max_fd = MAX(max_fd, server->sock);
      }

      const int proto_max = (*server->proto->fill_fd_set)(server, &rfd, &wfd);
      max_fd = MAX(max_fd, proto_max);

      if (max_fd == -1)
         break;

      struct timeval tv = {
         .tv_sec = 1,
         .tv_usec = 0
      };

      if (select(max_fd + 1, &rfd, &wfd, &efd, &tv) == -1)
         fatal_errno("select");

      if (server->sock != -1 && FD_ISSET(server->sock, &rfd))
         handle_new_connection(server);

      (*server->proto->poll_sockets)(server, &rfd, &wfd);

      if (server->shutdown && server->sock != -1) {
         server_log(LOG_INFO, "stopping server");

         closesocket(server->sock);
         server->sock = -1;

         (*server->proto->shutdown)(server);
      }
   }

   assert(server->sock == -1);

   pb_free(server->packetbuf);
   (*server->proto->free_server)(server);
}
