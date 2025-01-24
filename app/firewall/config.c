#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "module.h"

config_t config_a = {
  .pktmbuf_pool = NULL,
  .cli_def = NULL,
  .cli_show = NULL,
  .cli_sockfd = 0,
  .itf_cfg = NULL,
  .acl_ctx = NULL,
  .promiscuous = 1,
  .worker_num = 0,
  .port_num = 0,
  .queue_num = 0,
  .rx_queues = {0},
  .tx_queues = {{0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}},
  .rxq_num = 0,
  .txq_num = 0,
  .reload_mark = 0,
  .switch_mark = 0,
}, config_b;

int config_index = 0;
int _config_index[MAX_WORKER_NUM] = {-1, -1, -1, -1, -1, -1, -1, -1};

int config_reload(config_t *c) {
  config_t *new = (c == &config_a) ? &config_b : &config_a;

  memcpy(new, c, sizeof(config_t));
  modules_conf(new);
  new->reload_mark = 0;
  new->switch_mark = 0;
  config_index =
      (config_index <= 0) ? 1 : config_index + 1;
  return 1;
}

config_t *config_switch(config_t *c, int lcore_id) {
  if (lcore_id == -1) {
    int i;
    while (1) {
      usleep(50000);

      for (i = 0; i < MAX_WORKER_NUM; i++) {
        if (_config_index[i] == -1)
          continue;
        if (_config_index[i] != config_index)
          break;
      }

      if (i == MAX_WORKER_NUM) {
        c->switch_mark = 0;
        break;
      }
    }

    modules_free(c);
  } else {
    _config_index[lcore_id] =
        (_config_index[lcore_id] <= 0)
            ? 1
            : _config_index[lcore_id] + 1;
  }

  return (c == &config_a) ? &config_b : &config_a;
}

// file format utf-8
// ident using space