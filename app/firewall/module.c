#include "module.h"

// module secetion start and end point, see module_section.lds
module_t __module_start__;
module_t __module_end__;

module_t *modules[MAX_MODULE_NUM] = {0};

mod_id_t hook_ingress[] = {
  MOD_ID_DECODER, 
  MOD_ID_ACL
};

mod_id_t hook_prerouting[] = {
  MOD_ID_INTERFACE
};

mod_id_t hook_forward[] = {
  MOD_ID_NONE
};

mod_id_t hook_postrouting[] = {
  MOD_ID_NONE
};

mod_id_t hook_localin[] = {
  MOD_ID_NONE
};

mod_id_t hook_localout[] = {
  MOD_ID_NONE
};

mod_id_t hook_egress[] = {
  MOD_ID_DECODER, 
  MOD_ID_ACL
};

mod_id_t *hooks[] = {
  hook_ingress,
  hook_prerouting,
  hook_forward,
  hook_postrouting,
  hook_localin,
  hook_localout,
  hook_egress,
};

int hook_size[] = {
  sizeof(hook_ingress) / sizeof(mod_id_t),
  sizeof(hook_prerouting) / sizeof(mod_id_t),
  sizeof(hook_forward) / sizeof(mod_id_t),
  sizeof(hook_postrouting) / sizeof(mod_id_t),
  sizeof(hook_localin) / sizeof(mod_id_t),
  sizeof(hook_localout) / sizeof(mod_id_t),
  sizeof(hook_egress) / sizeof(mod_id_t)
};

#define MODULE_FOREACH(m, id) \
  for (id = MOD_ID_NONE, m = modules[id]; id < MOD_ID_MAX; id++, m = modules[id])

#define MODULE_FOREACH_HOOK(m, id, hook) \
  for (id = 0, m = modules[hooks[hook][id]]; id < hook_size[hook]; id++, m = modules[hooks[hook][id]])

int modules_load(void) {
  module_t *m;

  for (m = &__module_start__; m < &__module_end__; m++) {
    printf("== module load %s \n", m->name);
    MODULE_REGISTER(m);
  }

  return 0;
}

int modules_init(void *config) {
  __rte_unused module_t *m;
  __rte_unused int id;

  MODULE_FOREACH(m, id) {
    if (m && m->init && m->enabled) {
      printf("== module init %s\n", m->name);
      if (m->init(config)) {
        return -1;
      }
    }
  }

  return 0;
}

int modules_conf(void *config) {
  __rte_unused module_t *m;
  __rte_unused int id;

  MODULE_FOREACH(m, id) {
    if (m && m->conf && m->enabled) {
      printf("== module conf %s\n", m->name);
      if (m->conf(config)) {
        return -1;
      }
    }
  }

  return 0;
}

int modules_free(void *config) {
  __rte_unused module_t *m;
  __rte_unused int id;

  MODULE_FOREACH(m, id) {
    if (m && m->free && m->enabled) {
      printf("== module free %s\n", m->name);
      if (m->free(config)) {
        return -1;
      }
    }
  }

  return 0;
}

int modules_proc(void *config, struct rte_mbuf *pkt, mod_hook_t hook) {
  module_t *m;
  int id;

  MODULE_FOREACH_HOOK(m, id, hook) {
    mod_ret_t ret;

    if (m && m->proc && m->enabled) {
      ret = m->proc(config, pkt, hook);

      if (ret == MOD_RET_STOLEN) {
        return ret;
      }

      if (ret == MOD_RET_ACCEPT) {
        continue;
      }
    }
  }

  return MOD_RET_ACCEPT;
}

// file-format: utf-8
// ident using spaces