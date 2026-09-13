/*
 * Network device codes, shared by the driver and the utilities that
 * configure it. Driver-private, hence above RV9_SS_DRIVER_BASE.
 */
#pragma once

#include <stdint.h>
#include "rv9/module.h"

#define RV9_NET_SS_CONNECT   (RV9_SS_DRIVER_BASE + 0)   /* rv9_net_creds_t */
#define RV9_NET_SS_DISCONNECT (RV9_SS_DRIVER_BASE + 1)
#define RV9_NET_GS_STATUS    (RV9_SS_DRIVER_BASE + 2)   /* rv9_net_status_t */
#define RV9_NET_GS_SCAN      (RV9_SS_DRIVER_BASE + 3)   /* rv9_net_scan_t */
#define RV9_NET_SS_FORGET    (RV9_SS_DRIVER_BASE + 4)   /* clear saved creds */

/*
 * Read without waiting: 0 blocks (the default), 1 returns WOULDBLOCK when
 * nothing has arrived.
 *
 * A blocking read is the right default -- reading means waiting, and that
 * is what every reader here wants. But there is one thing that cannot use
 * it: closing a connection *tidily*. TCP sends a reset instead of a clean
 * finish when a socket is closed with data still unread, and a reset
 * throws away whatever was in flight -- including the last thing the
 * program printed. Draining before closing needs a read that can come back
 * empty.
 */
#define RV9_NET_SS_NOWAIT    (RV9_SS_DRIVER_BASE + 5)

/*
 * Shut the connection down in both directions, without closing the path.
 * setstat, arg ignored.
 *
 * For another thread than the one using the path: a read waiting on a
 * peer that will never send returns at once, as if the peer had closed,
 * and the path stays valid until its owner closes it. Closing it instead
 * would free the socket under the read. This is how a hung-up SSH session
 * gets a background job's read to let go.
 */
#define RV9_NET_SS_SHUTDOWN  (RV9_SS_DRIVER_BASE + 6)

#define RV9_NET_MAX_APS 12

typedef struct __attribute__((packed)) {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    uint8_t band;      /* 1 = 2.4 GHz, 2 = 5 GHz */
    uint8_t authmode;  /* wifi_auth_mode_t as the driver reports it */
} rv9_net_ap_t;

typedef struct __attribute__((packed)) {
    uint8_t      count;
    uint8_t      reserved[3];
    rv9_net_ap_t ap[RV9_NET_MAX_APS];
} rv9_net_scan_t;

typedef struct __attribute__((packed)) {
    char ssid[33];
    char pass[65];
} rv9_net_creds_t;

typedef enum {
    RV9_NET_DOWN = 0,
    RV9_NET_CONNECTING,
    RV9_NET_UP,
    RV9_NET_FAILED,
} rv9_net_state_t;

typedef struct __attribute__((packed)) {
    uint8_t  state;        /* rv9_net_state_t */
    uint8_t  reason;       /* last disconnect reason, 0 if none */
    uint8_t  reserved[2];
    uint32_t ip;           /* host byte order, 0 when down */
    char     ssid[33];
    uint8_t  reserved2[3];
} rv9_net_status_t;
