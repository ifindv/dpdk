#ifndef _M_WORKER__H_
#define _M_WORKER__H_

#include "config.h"

typedef enum {
  ROLE_NONE,
  ROLE_MGMT,
  ROLE_RX,
  ROLE_TX,
  ROLE_RTX,
  ROLE_WORKER,
  ROLE_RTX_WORKER
} role_t;

typedef struct {
  int lcore_id;
  role_t role;
  uint16_t ports[MAX_PORT_NUM];
  uint16_t queues[MAX_QUEUE_NUM];
  uint16_t port_num;
  uint16_t queue_num;
  void *work_queue;
} worker_t;

int worker_init(config_t *config);

int RX(__rte_unused config_t *config);
int TX(__rte_unused config_t *config);
int RTX(config_t *config);
int WORKER(config_t *config);
int RTX_WORKER(config_t *config);

#endif

// file-format utf-8
// ident using space