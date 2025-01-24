#include <stdio.h>
#include <unistd.h>

#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_ethdev.h>

#include "config.h"
#include "module.h"
#include "packet.h"
#include "worker.h"
#include "json.h"

static int worker_split_port_by_comma(const char *str, uint16_t ports[], int max) {
  char _str[512] = {0};
  char *p = NULL, *q = NULL;
  int i = 0;

  if (strlen(str) >= sizeof(_str)) {
    printf("split by comma: string is too long\n");
    return 0;
  }

  if (str) {
    snprintf(_str, sizeof(_str), "%s", str);
  }

  p = _str;
  while (p && i < max) {
    q = strchr(p, ',');
    if (q) {
      *q = '\0';
    }
    ports[i++] = atoi(p);
    if (q) {
      p = q + 1;
    } else {
      break;
    }
  }
  return i;
}

static int worker_split_queue_by_comma(const char *str, uint16_t queues[], int max) {
  return worker_split_port_by_comma(str, queues, max);
}

static int worker_load(config_t *config) {
  worker_t *workers = NULL;
  json_object *jr = NULL, *ja;
  int i, worker_num;
  int ret = -1;

  if (config->workers) {
    goto done;
  }

  jr = JR(CONFIG_PATH, "worker.json");
  if (!jr) {
    goto done;
  }

  worker_num = JA(jr, "lcores", &ja);
  if (worker_num == -1) {
    goto done;
  }

  workers = (worker_t *)malloc(sizeof(worker_t) * worker_num);
  if (!workers) {
    goto done;
  }

  memset(workers, 0, sizeof(worker_t) * worker_num);

#define WORKER_JV(item)                                                        \
  jv = JV(jo, item);                                                           \
  if (!jv) {                                                                   \
    ret = -1;                                                                  \
    goto done;                                                                 \
  }

  for (i = 0; i < worker_num; i++) {
    json_object *jo, *jv;

    jo = JO(ja, i);

    WORKER_JV("lcore_id");
    workers[i].lcore_id = JV_I(jv);

    WORKER_JV("role");
    if (strcmp(JV_S(jv), "RX") == 0) {
      workers[i].role = ROLE_RX;
    } else if (strcmp(JV_S(jv), "TX") == 0) {
      workers[i].role = ROLE_TX;
    } else if (strcmp(JV_S(jv), "RTX") == 0) {
      workers[i].role = ROLE_RTX;
    } else if (strcmp(JV_S(jv), "WORKER") == 0) {
      workers[i].role = ROLE_WORKER;
    } else if (strcmp(JV_S(jv), "RTX_WORKER") == 0) {
      workers[i].role = ROLE_RTX_WORKER;
    } else if (strcmp(JV_S(jv), "MGMT") == 0) {
      workers[i].role = ROLE_MGMT;
    } else {
      workers[i].role = ROLE_NONE;
    }

    if ((workers[i].role == ROLE_RX)
    || (workers[i].role == ROLE_TX)
    ||  (workers[i].role == ROLE_RTX)
    ||  (workers[i].role == ROLE_RTX_WORKER)) {
      WORKER_JV("ports");
      workers[i].port_num = worker_split_port_by_comma(JV_S(jv), workers[i].ports, MAX_PORT_NUM);
      if (!workers[i].port_num) {
        printf("worker %d port num is 0\n", i);
        goto done;
      }

      WORKER_JV("queues");
      workers[i].queue_num = worker_split_queue_by_comma(JV_S(jv), workers[i].queues, MAX_QUEUE_NUM);
      if (!workers[i].queue_num) {
        printf("worker %d queue num is 0\n", i);
        goto done;
      }
    }

    config->worker_map[workers[i].lcore_id] = i;

    printf("worker %d lcore_id %d role %d port_num %d queue_num %d\n", 
           i, workers[i].lcore_id, workers[i].role, workers[i].port_num, workers[i].queue_num);
  }

#undef WORKER_JV

  config->workers = workers;
  config->worker_num = worker_num;

  ret = 0;

done:
  if (jr) {
    JR_FREE(jr);
  }
  
  if (ret) {
    if (workers) {
      free(workers);
    }
  }

  return ret;
}

static int worker_setup(config_t *config) {
  worker_t *worker;
  int i, j, rxq = 0, txq = 0;
  char queue[128];
  int ret = -1;

  for (i = 0; i < config->worker_num; i++) {
    worker = (worker_t *)config->workers + i;
    
    // setup rx queues for each worker (as work queue)
    if ((worker->role == ROLE_WORKER)
    || (worker->role == ROLE_RTX_WORKER)) {
      if (!config->rx_queues[rxq]) {
        memset(queue, 0, 128);
        sprintf(queue, "%s-%d", "worker-rx-queue", rxq);
        config->rx_queues[rxq] = rte_ring_create(queue, 1024, rte_socket_id(), 0);
        if (!config->rx_queues[rxq]) {
          printf("create rx queue failed\n");
          goto done;
        }
        worker->work_queue = config->rx_queues[rxq];
        rxq++;
      }
    }

    // setup tx queues for each port-queue pair (as output buffer)
    if ((worker->role == ROLE_RX)
    || (worker->role == ROLE_TX)
    || (worker->role == ROLE_RTX)
    || (worker->role == ROLE_RTX_WORKER)) {
      int k, p, q;
      for (j = 0; j < worker->port_num; j++) {
        for (k = 0; k < worker->queue_num; k++) {
          p = worker->ports[j];
          q = worker->queues[k];

          if (!config->tx_queues[p][q]) {
            memset(queue, 0, 128);
            sprintf(queue, "%s-%d-%d", "worker-tx-queue", p, q);
            config->tx_queues[p][q] = rte_ring_create(queue, 1024, rte_socket_id(), 0);
            if (!config->tx_queues[p][q]) {
              printf("create tx queue failed\n");
              goto done;
            }
            txq = txq < q ? q : txq;
          }
        }
      }
    }
  }

  config->rxq_num = rxq;
  config->txq_num = txq + 1;
  ret = 0;

done:
  if (ret) {
    for (i = 0; i < config->rxq_num; i++) {
      if (config->rx_queues[i]) {
        rte_ring_free(config->rx_queues[i]);
        config->rx_queues[i] = NULL;
      }
    }

    for (i = 0; i < config->port_num; i++) {
      for (j = 0; j < config->txq_num; j++) {
        if (config->tx_queues[i][j]) {
          rte_ring_free(config->tx_queues[i][j]);
          config->tx_queues[i][j] = NULL;
        }
      }
    }
  }

  return ret;
}

int worker_init(config_t *config) {
  int ret;

  if (config->workers) {
    printf("worker config already exist\n");
    return -1;
  }

  ret = worker_load(config);
  if (ret) {
    printf("worker load config failed\n");
    goto done;
  }

  ret = worker_setup(config);
  if (ret) {
    printf("worker queue setup failed\n");
    goto done;
  }

done:
  if (ret) {
    if (config->workers) {
      free(config->workers);
      config->workers = NULL;
    }
  }
  return ret;
}

int RX(__rte_unused config_t *config) {
  struct rte_mbuf *pkts_burst[MAX_PKT_BURST] = {0};
  worker_t *worker;
  int i, j, port_id, queue_id, nb_rx;
  packet_t *p;

  worker = (worker_t *)config->workers + config->worker_map[rte_lcore_id()];

  for (i = 0; i < worker->port_num; i++) {
    for (j = 0; j < worker->queue_num; j++) {
      port_id = worker->ports[i];
      queue_id = worker->queues[j];
      
      nb_rx = rte_eth_rx_burst(port_id, queue_id, pkts_burst, MAX_PKT_BURST);
      if (nb_rx) {
        for (i = 0; i < nb_rx; i++) {
          p = rte_mbuf_to_priv(pkts_burst[i]);
          if (p) {
            p->port_in = port_id;
            p->queue_id = queue_id;
          }
        }

        queue_id = queue_id % config->rxq_num;
        while (!rte_ring_enqueue_bulk(config->rx_queues[queue_id], (void *const *)pkts_burst, nb_rx, NULL))
          ; // must success
      }
    }
  }
  return 0;
}

int TX(__rte_unused config_t *config) {
  struct rte_mbuf *pkts_burst[MAX_PKT_BURST] = {0};
  worker_t *worker;
  int i, j, port_id, queue_id, nb_tx;

  worker = (worker_t *)config->workers + config->worker_map[rte_lcore_id()];

  for (i = 0; i < worker->port_num; i++) {
    for (j = 0; j < worker->queue_num; j++) {
      port_id = worker->ports[i];
      queue_id = worker->queues[j];

      if (!config->tx_queues[port_id][queue_id])
        continue;

      nb_tx = rte_ring_count(config->tx_queues[port_id][queue_id]);
      if (!nb_tx) {
        continue;
      }
      nb_tx = nb_tx > MAX_PKT_BURST ? MAX_PKT_BURST : nb_tx;
      
      nb_tx = rte_ring_dequeue_bulk(config->tx_queues[port_id][queue_id], (void **)pkts_burst, nb_tx, NULL);
      if (nb_tx) {
        int tx = rte_eth_tx_burst(port_id, queue_id, pkts_burst, nb_tx);
        if (tx < nb_tx) {
          printf("port %d queue %d tx %d failed\n", port_id, queue_id, nb_tx - tx);
        }
      }
    }
  }
  return 0;
}

int RTX(config_t *config) {
  RX(config);
  TX(config);
  return 0;
}

int WORKER(config_t *config) {
  struct rte_mbuf *mbuf;
  worker_t *worker;
  packet_t *p;
  int ret, hook, port_id, queue_id;

  worker = (worker_t *)config->workers + config->worker_map[rte_lcore_id()];
  ret = rte_ring_dequeue(worker->work_queue, (void **)&mbuf);
  if (ret || !mbuf) {
    return 0;
  }

  for (hook = MOD_HOOK_INGRESS; hook <= MOD_HOOK_EGRESS; hook++) {
    if (modules_proc(config, mbuf, hook)) {
      return 0;
    }
  }

  p = rte_mbuf_to_priv(mbuf);
  if (!p) {
    rte_pktmbuf_free(mbuf);
    return -1;
  }
  port_id = p->port_out;
  queue_id = p->queue_id;

  ret = rte_ring_enqueue(config->tx_queues[port_id][queue_id], mbuf);
  if (ret) {
    rte_pktmbuf_free(mbuf);
    return -1;
  }

  return 0;
}

int RTX_WORKER(config_t *config) {
  RX(config);
  WORKER(config);
  TX(config);
  return 0;
}

// file-format utf-8
// ident using space