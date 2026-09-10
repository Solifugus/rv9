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
