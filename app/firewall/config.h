#ifndef _M_CONFIG_H_
#define _M_CONFIG_H_

#include <stdbool.h>

#define MAX_FILE_PATH 256
#define MAX_WORKER_NUM 8
#define MAX_PORT_NUM  32
#define MAX_QUEUE_NUM 16
#define MAX_PKT_BURST 32

#define CONFIG_PATH "/opt/firewall/config"
#define BINARY_PATH "/opt/firewall/bin"
#define SCRIPT_PATH "/opt/firewall/script"

typedef struct {
  // memory pool
  struct rte_mempool *pktmbuf_pool;
  
  // command line
  void *cli_def;
  void *cli_show;
  int cli_sockfd;

  // worker
  void *workers;
  int worker_num;
  int worker_map[MAX_WORKER_NUM];
  void *rx_queues[MAX_WORKER_NUM];
  void *tx_queues[MAX_PORT_NUM][MAX_QUEUE_NUM];
  int rxq_num;
  int txq_num;
  
  // interface
  void *itf_cfg;
  int promiscuous;
  int port_num;
  int queue_num;

  // acl
  void *acl_ctx;

  // configuration
  int reload_mark;
  int switch_mark;
} config_t;

int config_reload(config_t *c);
config_t *config_switch(config_t *c, int lcore_id);

#endif

// file format utf-8
// ident using space