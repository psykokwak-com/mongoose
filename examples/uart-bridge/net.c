// Copyright (c) 2022 Cesanta Software Limited
// All rights reserved

#include "mongoose.h"

#define DEFAULT_TCP "tcp://0.0.0.0:4001"
#define DEFAULT_WEBSOCKET "ws://0.0.0.0:4002"
#define DEFAULT_MQTT "mqtt://broker.hivemq.com:1883?tx=b/tx&rx=b/rx"

struct endpoint {
  char *url;
  bool enable;
  struct mg_connection *c;
};

struct state {
  struct endpoint tcp, websocket, mqtt;
  int tx, rx, baud;
} s_state = {.tcp = {.enable = true},
             .websocket = {.enable = true},
             .mqtt = {.enable = true},
             .tx = 5,
             .rx = 4,
             .baud = 115200};

void uart_init(int tx, int rx, int baud);
int uart_read(char *buf, size_t len);
void uart_write(const void *buf, size_t len);

// Let users define their own UART API. If they don't, use a dummy one
#if defined(UART_API_IMPLEMENTED)
#else
void uart_init(int tx, int rx, int baud) {
  // We use stdin/stdout as UART. Make stdin non-blocking
  fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
  (void) tx, (void) rx, (void) baud;
}

void uart_write(const void *buf, size_t len) {
  fwrite(buf, 1, len, stdout);  // Write to stdout
  fflush(stdout);
}

int uart_read(char *buf, size_t len) {
  return read(0, buf, len);  // Read from stdin
}
#endif

// Event handler for a connected Websocket client
static void ws_fn(struct mg_connection *c, int ev, void *evd, void *fnd) {
  if (ev == MG_EV_HTTP_MSG) {
    mg_ws_upgrade(c, evd, NULL);
  } else if (ev == MG_EV_WS_OPEN) {
    // c->is_hexdumping = 1;
    c->label[0] = 'W';  // When WS handhake is done, mark us as WS client
  } else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *) evd;
    uart_write(wm->data.ptr, wm->data.len);  // Send to UART
    c->recv.len = 0;                         // Discard received data
  } else if (ev == MG_EV_CLOSE) {
    if (c->is_listening) s_state.websocket.c = NULL;
  }
  (void) fnd;
}

// Event handler for a connected TCP client
static void tcp_fn(struct mg_connection *c, int ev, void *evd, void *fnd) {
  if (ev == MG_EV_OPEN) {
    // c->is_hexdumping = 1;
    c->label[0] = 'T';  // When client is connected, mark us as TCP client
  } else if (ev == MG_EV_READ) {
    uart_write(c->recv.buf, c->recv.len);  // Send to UART
    c->recv.len = 0;                       // Discard received data
  } else if (ev == MG_EV_CLOSE) {
    if (c->is_listening) s_state.tcp.c = NULL;
  }
  (void) fnd, (void) evd;
}

// Extract RX topic name from the MQTT address
static struct mg_str mqtt_rx_topic(void) {
  char *url = s_state.mqtt.url, *p = strrchr(url, ',');
  return mg_str(p ? p : "b/rx");
}

// Extract TX topic name from the MQTT address
static struct mg_str mqtt_tx_topic(void) {
  char *url = s_state.mqtt.url, *p1 = strchr(url, ','), *p2 = strrchr(url, ',');
  return mg_str_n(p1 && p2 ? p1 : "b/tx", p1 && p2 ? p2 - p1 + 1 : 4);
}

// Event handler for MQTT connection
static void mq_fn(struct mg_connection *c, int ev, void *evd, void *fnd) {
  if (ev == MG_EV_OPEN) {
    // c->is_hexdumping = 1;
    c->label[0] = 'M';
  } else if (ev == MG_EV_MQTT_OPEN) {
    mg_mqtt_sub(c, mqtt_rx_topic(), 1);  // Subscribe to RX topic
  } else if (ev == MG_EV_MQTT_MSG) {
    struct mg_mqtt_message *mm = evd;        // MQTT message
    uart_write(mm->data.ptr, mm->data.len);  // Send to UART
  } else if (ev == MG_EV_CLOSE) {
    s_state.mqtt.c = NULL;
  }
  (void) fnd, (void) evd;
}

// Software timer with a frequency close to the scheduling time slot
static void timer_fn(void *param) {
  // Start listeners if they're stopped for any reason
  struct mg_mgr *mgr = (struct mg_mgr *) param;
  if (s_state.tcp.c == NULL && s_state.tcp.enable) {
    s_state.tcp.c = mg_listen(mgr, s_state.tcp.url, tcp_fn, 0);
  }
  if (s_state.websocket.c == NULL && s_state.websocket.enable) {
    s_state.websocket.c = mg_http_listen(mgr, s_state.websocket.url, ws_fn, 0);
  }
  if (s_state.mqtt.c == NULL && s_state.mqtt.enable) {
    struct mg_mqtt_opts opts = {.clean = true, .will_qos = 1};
    s_state.mqtt.c = mg_mqtt_connect(mgr, s_state.mqtt.url, &opts, mq_fn, 0);
  }

  // Read UART
  char buf[512];
  int len = uart_read(buf, sizeof(buf));
  if (len > 0) {
    // Iterate over all connections. Send data to WS and TCP clients
    for (struct mg_connection *c = mgr->conns; c != NULL; c = c->next) {
      if (c->label[0] == 'W') mg_ws_send(c, buf, len, WEBSOCKET_OP_TEXT);
      if (c->label[0] == 'T') mg_send(c, buf, len);
      if (c->label[0] == 'M')
        mg_mqtt_pub(c, mqtt_tx_topic(), mg_str_n(buf, len), 1, false);
    }
  }
}

// HTTP request handler function
void uart_bridge_fn(struct mg_connection *c, int ev, void *ev_data,
                    void *fn_data) {
  if (ev == MG_EV_OPEN && c->is_listening) {
    s_state.tcp.url = strdup(DEFAULT_TCP);
    s_state.websocket.url = strdup(DEFAULT_WEBSOCKET);
    s_state.mqtt.url = strdup(DEFAULT_MQTT);
    mg_timer_add(c->mgr, 20, MG_TIMER_REPEAT, timer_fn, c->mgr);
    uart_init(s_state.tx, s_state.rx, s_state.baud);
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_http_match_uri(hm, "/api/hi")) {
      mg_http_reply(c, 200, "", "hi\n");  // Testing endpoint
    } else if (mg_http_match_uri(hm, "/api/config/get")) {
      mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                    "{%Q:{%Q:%Q,%Q:%s},%Q:{%Q:%Q,%Q:%s},%Q:{%Q:%Q,%Q:%s},"
                    "%Q:%d,%Q:%d,%Q:%d}\n",
                    "tcp", "url", s_state.tcp.url, "enable",
                    s_state.tcp.enable ? "true" : "false", "ws", "url",
                    s_state.websocket.url, "enable",
                    s_state.websocket.enable ? "true" : "false", "mqtt", "url",
                    s_state.mqtt.url, "enable",
                    s_state.mqtt.enable ? "true" : "false", "rx", s_state.rx,
                    "tx", s_state.tx, "baud", s_state.baud);
    } else {
      struct mg_http_serve_opts opts = {0};
#if 0
      opts.root_dir = "/web_root";
      opts.fs = &mg_fs_packed;
#else
      opts.root_dir = "web_root";
#endif
      mg_http_serve_dir(c, ev_data, &opts);
    }
  }
  (void) fn_data;
}