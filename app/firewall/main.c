#include <signal.h>
#include <stdio.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_log.h>
#include <rte_per_lcore.h>

#include "cli.h"
#include "config.h"
#include "interface/interface.h"
#include "module.h"
#include "packet.h"
#include "worker.h"

extern config_t config_a, config_b;
extern int _config_index[MAX_WORKER_NUM], config_index;

config_t *config = &config_a;
__thread config_t *_config;

volatile bool force_quit;

static void signal_handler(int signum) {
  if (signum == SIGINT || signum == SIGTERM) {
    printf("\n\nSignal %d received, preparing to exit...\n", signum);
    force_quit = true;
  }
}

static int cli_show_conf(struct cli_def *cli, const char *command, char *argv[],
                         int argc) {
  CLI_PRINT(cli, "command %s argv[0] %s argc %d", command, argv[0], argc);

  config_t *c = cli_get_context(cli);
  CLI_PRINT(cli, "working copy config-%s", (c == &config_a) ? "A" : "B");
  CLI_PRINT(cli, "indicator [%d %d %d %d %d %d %d %d] %d",
            _config_index[0], _config_index[1],
            _config_index[2], _config_index[3],
            _config_index[4], _config_index[5],
            _config_index[6], _config_index[7],
            config_index);

  CLI_PRINT(cli, "pktmbuf pool %p", c->pktmbuf_pool);
  CLI_PRINT(cli, "promiscuous %d", c->promiscuous);
  CLI_PRINT(cli, "worker num %d", c->worker_num);
  CLI_PRINT(cli, "port num %d", c->port_num);
  CLI_PRINT(cli, "queue num %d", c->queue_num);
  CLI_PRINT(cli, "cli def %p", c->cli_def);
  CLI_PRINT(cli, "cli show %p", c->cli_show);
  CLI_PRINT(cli, "cli socket id %d", c->cli_sockfd);
  CLI_PRINT(cli, "rx queues %p", c->rx_queues);
  CLI_PRINT(cli, "tx queues %p", c->tx_queues);
  CLI_PRINT(cli, "rx queue num %d", c->rxq_num);
  CLI_PRINT(cli, "tx queue num %d", c->txq_num);
  CLI_PRINT(cli, "interface config %p", c->itf_cfg);
  CLI_PRINT(cli, "acl context %p", c->acl_ctx);
  CLI_PRINT(cli, "reload mark %d", c->reload_mark);
  CLI_PRINT(cli, "switch mark %d", c->switch_mark);
  return 0;
}

static int main_loop(__rte_unused void *arg) {
  int lcore_id = rte_lcore_id();
  worker_t *worker = (worker_t *)config->workers + config->worker_map[lcore_id];
  role_t role = worker->role;

  printf("lcore %d start, role %d\n", lcore_id, role);

  _config = (config_t *)arg;
  _config_index[lcore_id] = 0;

  while (!force_quit) {
    if (_config->switch_mark) {
      _config = config_switch(_config, lcore_id);
    }

    if (role == ROLE_RX)
      RX(_config);
    else if (role == ROLE_TX)
      TX(_config);
    else if (role == ROLE_RTX)
      RTX(_config);
    else if (role == ROLE_RTX_WORKER)
      RTX_WORKER(_config);
    else if (role == ROLE_WORKER)
      WORKER(_config);
  }

  return 0;
}

static void mgmt_loop(__rte_unused config_t *c) {
  config_t *_c = c;

  while (!force_quit) {
    /** When a reload mark set, a config switch process started, included steps
     * below:
     * 1. reload config
     * 2. tell worker to switch config and wait all worker switch done
     * 3. switch config
     * */
    if (_c->reload_mark) {
      if (config_reload(_c)) {
        _c->reload_mark = 0;
        _c->switch_mark = 1;
        _c = config_switch(_c, -1);
        cli_set_context(_c->cli_def, _c);
        config = _c;
      }
    }
    _cli_run(_c);
  }
}

int main(int argc, char **argv) {
  int ret = 0;

  ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "rte eal init failed\n");
  }
  argc -= ret;
  argv += ret;

  force_quit = false;
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  config->pktmbuf_pool = rte_pktmbuf_pool_create(
    "mbuf_pool", 
    81920, 
    256, 
    sizeof(packet_t), 
    128 + 2048, 
    rte_socket_id()
  );
  if (!config->pktmbuf_pool) {
    rte_exit(EXIT_FAILURE, "create pktmbuf pool failed\n");
  }

  config->port_num = rte_eth_dev_count_avail();

  ret = _cli_init(config);
  if (ret) {
    rte_exit(EXIT_FAILURE, "cli init erorr\n");
  }

  CLI_CMD_C(config->cli_def, config->cli_show, "config",
            cli_show_conf, "global configuration");

  ret = worker_init(config);
  if (ret) {
    rte_exit(EXIT_FAILURE, "worker init erorr\n");
  }

  modules_load();
  ret = modules_init(config);
  if (ret) {
    rte_exit(EXIT_FAILURE, "module init erorr\n");
  }

  rte_eal_mp_remote_launch(main_loop, (void *)config, SKIP_MAIN);
  mgmt_loop(config);

  ret = 0;
  rte_eal_mp_wait_lcore();
  modules_free(config);
  rte_eal_cleanup();

  return ret;
}

// file-format utf-8
// ident using space