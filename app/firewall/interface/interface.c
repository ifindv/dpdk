#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_ring.h>

#include "../config.h"
#include "../json.h"
#include "../module.h"
#include "../packet.h"

#include "interface.h"

MODULE_DECLARE(interface) = {
  .name = "interface",
  .id = MOD_ID_INTERFACE,
  .enabled = true,
  .log = true,
  .init = interface_init,
  .proc = interface_proc,
  .conf = NULL,
  .free = NULL,
  .priv = NULL
};

static int interface_type_str2int(const char *str) {
  if (!strcmp("vwire", str))
    return PORT_TYPE_VWIRE;
  return PORT_TYPE_NONE;
}

static int interface_vwire_init(config_t *config) {
  interface_config_t *itfc = config->itf_cfg;
  vwire_pair_t *vwire_pair = NULL;
  int i, j, pairs = 0;
  int ret = -1;

  itfc->vwire_pairs =
      (vwire_pair_t *)malloc(sizeof(vwire_pair_t) * itfc->vwire_pair_num);
  if (!itfc->vwire_pairs) {
    printf("no mem for vwire pairs\n");
    goto done;
  }

  memset(itfc->vwire_pairs, 0, sizeof(vwire_pair_t) * itfc->vwire_pair_num);

  for (i = 0; i < itfc->port_num; i++) {
    if (itfc->ports[i].type == PORT_TYPE_VWIRE) {
      for (j = 0; j < itfc->vwire_pair_num; j++) {
        vwire_pair = itfc->vwire_pairs + j;
        if (!vwire_pair->vwire_id) {
          vwire_pair->vwire_id = itfc->ports[i].vwire;
          vwire_pair->port1 = itfc->ports[i].id;
          printf("vwire pair %d bind port %d\n", vwire_pair->vwire_id, vwire_pair->port1);
          break;
        } else {
          if (vwire_pair->vwire_id == itfc->ports[i].vwire) {
            if (!vwire_pair->port2) {
              vwire_pair->port2 = itfc->ports[i].id;
              printf("vwire pair %d bind port %d\n", vwire_pair->vwire_id, vwire_pair->port2);
              pairs++;
              break;
            } else {
              printf("vwire pair %u bind more than two port\n",
                     vwire_pair->vwire_id);
              goto done;
            }
          }
        }
      }
    }
  }

  printf("total vwire pair num %d\n", pairs);

  if (pairs != itfc->vwire_pair_num) {
    printf("vwire pair num less than expect, pairs %d expect pairs %d\n", pairs, itfc->vwire_pair_num);
    goto done;
  }

  ret = 0;

done:
  if (ret) {
    if (itfc->vwire_pairs)
      free(itfc->vwire_pairs);
  }
  return ret;
}

static uint16_t interface_vwire_pair(interface_config_t *itfc,
                                     uint16_t port_in) {
  vwire_pair_t *vwire_pair = itfc->vwire_pairs;
  int i;
  for (i = 0; i < itfc->vwire_pair_num; i++) {
    if (vwire_pair[i].port1 == port_in)
      return vwire_pair[i].port2;
    if (vwire_pair[i].port2 == port_in)
      return vwire_pair[i].port1;
  }
  return port_in;
}

static int interface_load(config_t *config) {
  interface_config_t *itfc = config->itf_cfg;
  json_object *jr = NULL, *ja;
  int i, itf_num;
  int vwire_port_num = 0;
  int ret = 0;

  jr = JR(CONFIG_PATH, "interface.json");
  if (!jr) {
    printf("get json string failed\n");
    return -1;
  }

  itf_num = JA(jr, "ports", &ja);
  if (itf_num == -1) {
    printf("no ports found\n");
    ret = -1;
    goto done;
  }

#define INTF_JV(item)                                                          \
  jv = JV(jo, item);                                                           \
  if (!jv) {                                                                   \
    printf("parse %s failed\n", item);                                         \
    ret = -1;                                                                  \
    goto done;                                                                 \
  }

  for (i = 0; i < itf_num; i++) {
    json_object *jo, *jv;
    port_config_t *portc = &itfc->ports[i];
    jo = JO(ja, i);

    INTF_JV("id");
    portc->id = JV_I(jv);

    INTF_JV("type");
    portc->type = interface_type_str2int(JV_S(jv));
    if (portc->type == PORT_TYPE_VWIRE)
      vwire_port_num++;

    INTF_JV("bus");
    sprintf(portc->bus, "%s", JV_S(jv));

    INTF_JV("mac");
    sprintf(portc->mac, "%s", JV_S(jv));

    INTF_JV("vwire");
    portc->vwire = JV_I(jv);

    itfc->port_num++;

    printf("port id %u type %u bus %s mac %s vwire id %u\n",
           portc->id, portc->type, portc->bus, portc->mac, portc->vwire);
  }

  printf("total port num %d\n", itfc->port_num);

#undef INTF_JV

  if (vwire_port_num && (vwire_port_num % 2 == 0)) {
    itfc->vwire_pair_num = vwire_port_num / 2;
    ret = interface_vwire_init(config);
    if (ret)
      printf("interface vwire init error\n");
  }

done:
  if (jr)
    JR_FREE(jr);
  return ret;
}

static int interface_setup(config_t *config) {
  struct rte_eth_dev_info dev_info;
  struct rte_eth_conf port_conf;
  uint16_t port_id;
  int i, ret;

  config_t *c = config;

  memset(&port_conf, 0, sizeof(struct rte_eth_conf));  
  port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
  port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

  c->queue_num = 0;

  RTE_ETH_FOREACH_DEV(port_id) {
    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret) {
      printf("rte eth dev info get failed\n");
      return -1;
    }

    if ((c->txq_num > dev_info.max_tx_queues)
    || (c->txq_num > dev_info.max_rx_queues)) {
      printf("worker tx queue num out of range\n");
      return -1;
    }
    c->queue_num = c->txq_num;

    ret = rte_eth_dev_configure(port_id, c->queue_num, c->queue_num, &port_conf);
    if (ret < 0) {
      printf("rte eth dev configure failed\n");
      return -1;
    }

    for (i = 0; i < c->queue_num; i++) {
      ret = rte_eth_rx_queue_setup(
        port_id, 
        i, 
        DEF_RX_DESC_NUM, 
        rte_eth_dev_socket_id(port_id),
        &dev_info.default_rxconf, 
        c->pktmbuf_pool
      );
      if (ret < 0) {
        printf("rx queue setup failed\n");
        return -1;
      }
    }

    for (i = 0; i < c->queue_num; i++) {
      ret = rte_eth_tx_queue_setup(
        port_id, 
        i, 
        DEF_TX_DESC_NUM,
        rte_eth_dev_socket_id(port_id),
        &dev_info.default_txconf
      );
      if (ret < 0) {
        printf("tx queue setup failed\n");
        return -1;
      }
    }

    ret = rte_eth_dev_set_ptypes(port_id, RTE_PTYPE_UNKNOWN, NULL, 0);
    if (ret < 0) {
      printf("setup ptypes failed\n");
      return -1;
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
      printf("port startup failed\n");
      return -1;
    }

    if (c->promiscuous) {
      ret = rte_eth_promiscuous_enable(port_id);
      if (ret != 0) {
        printf("promiscuous enable failed\n");
        return -1;
      }
    }
  }

  return 0;
}

int interface_init(void *config) {
  config_t *c = config;
  int ret = -1;

  if (c->itf_cfg) {
    printf("interface config exist\n");
    return ret;
  }

  c->itf_cfg = malloc(sizeof(interface_config_t));
  if (!c->itf_cfg) {
    printf("alloc interface config failed\n");
    goto done;
  }

  memset(c->itf_cfg, 0, sizeof(interface_config_t));

  ret = interface_load(c);
  if (ret) {
    printf("interface load config failed\n");
    goto done;
  }

  ret = interface_setup(c);
  if (ret) {
    printf("interface setup failed\n");
    goto done;
  }

  ret = 0;

done:
  if (ret) {
    if (c->itf_cfg) {
      free(c->itf_cfg);
      c->itf_cfg = NULL;
    }
  }
  return ret;
}

static int interface_proc_prerouting(config_t *config, struct rte_mbuf *mbuf) {
  packet_t *p = rte_mbuf_to_priv(mbuf);
  interface_config_t *itfc = config->itf_cfg;
  uint16_t port_in;

  if (!p) {
    rte_pktmbuf_free(mbuf);
    return -1;
  }

  port_in = p->port_in;
  switch (itfc->ports[port_in].type) {
  case PORT_TYPE_VWIRE:
    p->port_out = interface_vwire_pair(itfc, port_in);
    break;
  default:
    break;
  }

  return 0;
}

mod_ret_t interface_proc(void *config, struct rte_mbuf *mbuf, mod_hook_t hook) {
  if (hook == MOD_HOOK_PREROUTING)
    interface_proc_prerouting(config, mbuf);
  return MOD_RET_ACCEPT;
}

// file-format: utf-8
// ident using spaces