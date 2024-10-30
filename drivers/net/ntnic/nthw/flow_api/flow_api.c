/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Napatech A/S
 */
#include "ntlog.h"
#include "nt_util.h"

#include "flow_api_engine.h"
#include "flow_api_nic_setup.h"
#include "ntnic_mod_reg.h"

#include "flow_api.h"
#include "flow_filter.h"

#define RSS_TO_STRING(name) \
	{                \
		name, #name   \
	}

const char *dbg_res_descr[] = {
	/* RES_QUEUE */ "RES_QUEUE",
	/* RES_CAT_CFN */ "RES_CAT_CFN",
	/* RES_CAT_COT */ "RES_CAT_COT",
	/* RES_CAT_EXO */ "RES_CAT_EXO",
	/* RES_CAT_LEN */ "RES_CAT_LEN",
	/* RES_KM_FLOW_TYPE */ "RES_KM_FLOW_TYPE",
	/* RES_KM_CATEGORY */ "RES_KM_CATEGORY",
	/* RES_HSH_RCP */ "RES_HSH_RCP",
	/* RES_PDB_RCP */ "RES_PDB_RCP",
	/* RES_QSL_RCP */ "RES_QSL_RCP",
	/* RES_QSL_LTX */ "RES_QSL_LTX",
	/* RES_QSL_QST */ "RES_QSL_QST",
	/* RES_SLC_LR_RCP */ "RES_SLC_LR_RCP",
	/* RES_FLM_FLOW_TYPE */ "RES_FLM_FLOW_TYPE",
	/* RES_FLM_RCP */ "RES_FLM_RCP",
	/* RES_TPE_RCP */ "RES_TPE_RCP",
	/* RES_TPE_EXT */ "RES_TPE_EXT",
	/* RES_TPE_RPL */ "RES_TPE_RPL",
	/* RES_COUNT */ "RES_COUNT",
	/* RES_INVALID */ "RES_INVALID"
};

static struct flow_nic_dev *dev_base;
static pthread_mutex_t base_mtx = PTHREAD_MUTEX_INITIALIZER;

/*
 * Error handling
 */

static const struct {
	const char *message;
} err_msg[] = {
	/* 00 */ { "Operation successfully completed" },
	/* 01 */ { "Operation failed" },
	/* 02 */ { "Memory allocation failed" },
	/* 03 */ { "Too many output destinations" },
	/* 04 */ { "Too many output queues for RSS" },
	/* 05 */ { "The VLAN TPID specified is not supported" },
	/* 06 */ { "The VxLan Push header specified is not accepted" },
	/* 07 */ { "While interpreting VxLan Pop action, could not find a destination port" },
	/* 08 */ { "Failed in creating a HW-internal VTEP port" },
	/* 09 */ { "Too many VLAN tag matches" },
	/* 10 */ { "IPv6 invalid header specified" },
	/* 11 */ { "Too many tunnel ports. HW limit reached" },
	/* 12 */ { "Unknown or unsupported flow match element received" },
	/* 13 */ { "Match failed because of HW limitations" },
	/* 14 */ { "Match failed because of HW resource limitations" },
	/* 15 */ { "Match failed because of too complex element definitions" },
	/* 16 */ { "Action failed. To too many output destinations" },
	/* 17 */ { "Action Output failed, due to HW resource exhaustion" },
	/* 18 */ { "Push Tunnel Header action cannot output to multiple destination queues" },
	/* 19 */ { "Inline action HW resource exhaustion" },
	/* 20 */ { "Action retransmit/recirculate HW resource exhaustion" },
	/* 21 */ { "Flow counter HW resource exhaustion" },
	/* 22 */ { "Internal HW resource exhaustion to handle Actions" },
	/* 23 */ { "Internal HW QSL compare failed" },
	/* 24 */ { "Internal CAT CFN reuse failed" },
	/* 25 */ { "Match variations too complex" },
	/* 26 */ { "Match failed because of CAM/TCAM full" },
	/* 27 */ { "Internal creation of a tunnel end point port failed" },
	/* 28 */ { "Unknown or unsupported flow action received" },
	/* 29 */ { "Removing flow failed" },
};

void flow_nic_set_error(enum flow_nic_err_msg_e msg, struct rte_flow_error *error)
{
	assert(msg < ERR_MSG_NO_MSG);

	if (error) {
		error->message = err_msg[msg].message;
		error->type = (msg == ERR_SUCCESS) ? RTE_FLOW_ERROR_TYPE_NONE :
			RTE_FLOW_ERROR_TYPE_UNSPECIFIED;
	}
}

/*
 * Resources
 */

int flow_nic_alloc_resource(struct flow_nic_dev *ndev, enum res_type_e res_type,
	uint32_t alignment)
{
	for (unsigned int i = 0; i < ndev->res[res_type].resource_count; i += alignment) {
		if (!flow_nic_is_resource_used(ndev, res_type, i)) {
			flow_nic_mark_resource_used(ndev, res_type, i);
			ndev->res[res_type].ref[i] = 1;
			return i;
		}
	}

	return -1;
}

int flow_nic_alloc_resource_config(struct flow_nic_dev *ndev, enum res_type_e res_type,
	unsigned int num, uint32_t alignment)
{
	unsigned int idx_offs;

	for (unsigned int res_idx = 0; res_idx < ndev->res[res_type].resource_count - (num - 1);
		res_idx += alignment) {
		if (!flow_nic_is_resource_used(ndev, res_type, res_idx)) {
			for (idx_offs = 1; idx_offs < num; idx_offs++)
				if (flow_nic_is_resource_used(ndev, res_type, res_idx + idx_offs))
					break;

			if (idx_offs < num)
				continue;

			/* found a contiguous number of "num" res_type elements - allocate them */
			for (idx_offs = 0; idx_offs < num; idx_offs++) {
				flow_nic_mark_resource_used(ndev, res_type, res_idx + idx_offs);
				ndev->res[res_type].ref[res_idx + idx_offs] = 1;
			}

			return res_idx;
		}
	}

	return -1;
}

void flow_nic_free_resource(struct flow_nic_dev *ndev, enum res_type_e res_type, int idx)
{
	flow_nic_mark_resource_unused(ndev, res_type, idx);
}

int flow_nic_ref_resource(struct flow_nic_dev *ndev, enum res_type_e res_type, int index)
{
	NT_LOG(DBG, FILTER, "Reference resource %s idx %i (before ref cnt %i)",
		dbg_res_descr[res_type], index, ndev->res[res_type].ref[index]);
	assert(flow_nic_is_resource_used(ndev, res_type, index));

	if (ndev->res[res_type].ref[index] == (uint32_t)-1)
		return -1;

	ndev->res[res_type].ref[index]++;
	return 0;
}

int flow_nic_deref_resource(struct flow_nic_dev *ndev, enum res_type_e res_type, int index)
{
	NT_LOG(DBG, FILTER, "De-reference resource %s idx %i (before ref cnt %i)",
		dbg_res_descr[res_type], index, ndev->res[res_type].ref[index]);
	assert(flow_nic_is_resource_used(ndev, res_type, index));
	assert(ndev->res[res_type].ref[index]);
	/* deref */
	ndev->res[res_type].ref[index]--;

	if (!ndev->res[res_type].ref[index])
		flow_nic_free_resource(ndev, res_type, index);

	return !!ndev->res[res_type].ref[index];/* if 0 resource has been freed */
}

/*
 * Nic port/adapter lookup
 */

static struct flow_eth_dev *nic_and_port_to_eth_dev(uint8_t adapter_no, uint8_t port)
{
	struct flow_nic_dev *nic_dev = dev_base;

	while (nic_dev) {
		if (nic_dev->adapter_no == adapter_no)
			break;

		nic_dev = nic_dev->next;
	}

	if (!nic_dev)
		return NULL;

	struct flow_eth_dev *dev = nic_dev->eth_base;

	while (dev) {
		if (port == dev->port)
			return dev;

		dev = dev->next;
	}

	return NULL;
}

static struct flow_nic_dev *get_nic_dev_from_adapter_no(uint8_t adapter_no)
{
	struct flow_nic_dev *ndev = dev_base;

	while (ndev) {
		if (adapter_no == ndev->adapter_no)
			break;

		ndev = ndev->next;
	}

	return ndev;
}
/*
 * Flow API
 */

static struct flow_handle *flow_create(struct flow_eth_dev *dev __rte_unused,
	const struct rte_flow_attr *attr __rte_unused,
	uint16_t forced_vlan_vid __rte_unused,
	uint16_t caller_id __rte_unused,
	const struct rte_flow_item item[] __rte_unused,
	const struct rte_flow_action action[] __rte_unused,
	struct rte_flow_error *error __rte_unused)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG(ERR, FILTER, "%s: profile_inline module uninitialized", __func__);
		return NULL;
	}

	return profile_inline_ops->flow_create_profile_inline(dev, attr,
		forced_vlan_vid, caller_id,  item, action, error);
}

static int flow_destroy(struct flow_eth_dev *dev __rte_unused,
	struct flow_handle *flow __rte_unused,	struct rte_flow_error *error __rte_unused)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG(ERR, FILTER, "%s: profile_inline module uninitialized", __func__);
		return -1;
	}

	return profile_inline_ops->flow_destroy_profile_inline(dev, flow, error);
}

static int flow_flush(struct flow_eth_dev *dev, uint16_t caller_id, struct rte_flow_error *error)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG_DBGX(ERR, FILTER, "profile_inline module uninitialized");
		return -1;
	}

	return profile_inline_ops->flow_flush_profile_inline(dev, caller_id, error);
}

/*
 * Device Management API
 */

static void nic_insert_eth_port_dev(struct flow_nic_dev *ndev, struct flow_eth_dev *dev)
{
	dev->next = ndev->eth_base;
	ndev->eth_base = dev;
}

static int nic_remove_eth_port_dev(struct flow_nic_dev *ndev, struct flow_eth_dev *eth_dev)
{
	struct flow_eth_dev *dev = ndev->eth_base, *prev = NULL;

	while (dev) {
		if (dev == eth_dev) {
			if (prev)
				prev->next = dev->next;

			else
				ndev->eth_base = dev->next;

			return 0;
		}

		prev = dev;
		dev = dev->next;
	}

	return -1;
}

static void flow_ndev_reset(struct flow_nic_dev *ndev)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG(ERR, FILTER, "%s: profile_inline module uninitialized", __func__);
		return;
	}

	/* Delete all eth-port devices created on this NIC device */
	while (ndev->eth_base)
		flow_delete_eth_dev(ndev->eth_base);

	/* Error check */
	while (ndev->flow_base) {
		NT_LOG(ERR, FILTER,
			"ERROR : Flows still defined but all eth-ports deleted. Flow %p",
			ndev->flow_base);

		profile_inline_ops->flow_destroy_profile_inline(ndev->flow_base->dev,
			ndev->flow_base, NULL);
	}

	profile_inline_ops->done_flow_management_of_ndev_profile_inline(ndev);

	km_free_ndev_resource_management(&ndev->km_res_handle);
	kcc_free_ndev_resource_management(&ndev->kcc_res_handle);

	ndev->flow_unique_id_counter = 0;

#ifdef FLOW_DEBUG
	/*
	 * free all resources default allocated, initially for this NIC DEV
	 * Is not really needed since the bitmap will be freed in a sec. Therefore
	 * only in debug mode
	 */

	/* Check if all resources has been released */
	NT_LOG(DBG, FILTER, "Delete NIC DEV Adaptor %i", ndev->adapter_no);

	for (unsigned int i = 0; i < RES_COUNT; i++) {
		int err = 0;
#if defined(FLOW_DEBUG)
		NT_LOG(DBG, FILTER, "RES state for: %s", dbg_res_descr[i]);
#endif

		for (unsigned int ii = 0; ii < ndev->res[i].resource_count; ii++) {
			int ref = ndev->res[i].ref[ii];
			int used = flow_nic_is_resource_used(ndev, i, ii);

			if (ref || used) {
				NT_LOG(DBG, FILTER, "  [%i]: ref cnt %i, used %i", ii, ref,
					used);
				err = 1;
			}
		}

		if (err)
			NT_LOG(DBG, FILTER, "ERROR - some resources not freed");
	}

#endif
}

int flow_delete_eth_dev(struct flow_eth_dev *eth_dev)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG(ERR, FILTER, "%s: profile_inline module uninitialized", __func__);
		return -1;
	}

	struct flow_nic_dev *ndev = eth_dev->ndev;

	if (!ndev) {
		/* Error invalid nic device */
		return -1;
	}

	NT_LOG(DBG, FILTER, "Delete eth-port device %p, port %i", eth_dev, eth_dev->port);

#ifdef FLOW_DEBUG
	ndev->be.iface->set_debug_mode(ndev->be.be_dev, FLOW_BACKEND_DEBUG_MODE_WRITE);
#endif

	/* delete all created flows from this device */
	pthread_mutex_lock(&ndev->mtx);

	struct flow_handle *flow = ndev->flow_base;

	while (flow) {
		if (flow->dev == eth_dev) {
			struct flow_handle *flow_next = flow->next;
			profile_inline_ops->flow_destroy_locked_profile_inline(eth_dev, flow,
				NULL);
			flow = flow_next;

		} else {
			flow = flow->next;
		}
	}

	/*
	 * remove unmatched queue if setup in QSL
	 * remove exception queue setting in QSL UNM
	 */
	hw_mod_qsl_unmq_set(&ndev->be, HW_QSL_UNMQ_DEST_QUEUE, eth_dev->port, 0);
	hw_mod_qsl_unmq_set(&ndev->be, HW_QSL_UNMQ_EN, eth_dev->port, 0);
	hw_mod_qsl_unmq_flush(&ndev->be, eth_dev->port, 1);

	if (ndev->flow_profile == FLOW_ETH_DEV_PROFILE_INLINE) {
		for (int i = 0; i < eth_dev->num_queues; ++i) {
			uint32_t qen_value = 0;
			uint32_t queue_id = (uint32_t)eth_dev->rx_queue[i].hw_id;

			hw_mod_qsl_qen_get(&ndev->be, HW_QSL_QEN_EN, queue_id / 4, &qen_value);
			hw_mod_qsl_qen_set(&ndev->be, HW_QSL_QEN_EN, queue_id / 4,
				qen_value & ~(1U << (queue_id % 4)));
			hw_mod_qsl_qen_flush(&ndev->be, queue_id / 4, 1);
		}
	}

#ifdef FLOW_DEBUG
	ndev->be.iface->set_debug_mode(ndev->be.be_dev, FLOW_BACKEND_DEBUG_MODE_NONE);
#endif

	/* take eth_dev out of ndev list */
	if (nic_remove_eth_port_dev(ndev, eth_dev) != 0)
		NT_LOG(ERR, FILTER, "ERROR : eth_dev %p not found", eth_dev);

	pthread_mutex_unlock(&ndev->mtx);

	/* free eth_dev */
	free(eth_dev);

	return 0;
}

/*
 * Flow API NIC Setup
 * Flow backend creation function - register and initialize common backend API to FPA modules
 */

static int init_resource_elements(struct flow_nic_dev *ndev, enum res_type_e res_type,
	uint32_t count)
{
	assert(ndev->res[res_type].alloc_bm == NULL);
	/* allocate bitmap and ref counter */
	ndev->res[res_type].alloc_bm =
		calloc(1, BIT_CONTAINER_8_ALIGN(count) + count * sizeof(uint32_t));

	if (ndev->res[res_type].alloc_bm) {
		ndev->res[res_type].ref =
			(uint32_t *)&ndev->res[res_type].alloc_bm[BIT_CONTAINER_8_ALIGN(count)];
		ndev->res[res_type].resource_count = count;
		return 0;
	}

	return -1;
}

static void done_resource_elements(struct flow_nic_dev *ndev, enum res_type_e res_type)
{
	assert(ndev);

	if (ndev->res[res_type].alloc_bm)
		free(ndev->res[res_type].alloc_bm);
}

static void list_insert_flow_nic(struct flow_nic_dev *ndev)
{
	pthread_mutex_lock(&base_mtx);
	ndev->next = dev_base;
	dev_base = ndev;
	pthread_mutex_unlock(&base_mtx);
}

static int list_remove_flow_nic(struct flow_nic_dev *ndev)
{
	pthread_mutex_lock(&base_mtx);
	struct flow_nic_dev *nic_dev = dev_base, *prev = NULL;

	while (nic_dev) {
		if (nic_dev == ndev) {
			if (prev)
				prev->next = nic_dev->next;

			else
				dev_base = nic_dev->next;

			pthread_mutex_unlock(&base_mtx);
			return 0;
		}

		prev = nic_dev;
		nic_dev = nic_dev->next;
	}

	pthread_mutex_unlock(&base_mtx);
	return -1;
}

/*
 * adapter_no       physical adapter no
 * port_no          local port no
 * alloc_rx_queues  number of rx-queues to allocate for this eth_dev
 */
static struct flow_eth_dev *flow_get_eth_dev(uint8_t adapter_no, uint8_t port_no, uint32_t port_id,
	int alloc_rx_queues, struct flow_queue_id_s queue_ids[],
	int *rss_target_id, enum flow_eth_dev_profile flow_profile,
	uint32_t exception_path)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL)
		NT_LOG(ERR, FILTER, "%s: profile_inline module uninitialized", __func__);

	int i;
	struct flow_eth_dev *eth_dev = NULL;

	NT_LOG(DBG, FILTER,
		"Get eth-port adapter %i, port %i, port_id %u, rx queues %i, profile %i",
		adapter_no, port_no, port_id, alloc_rx_queues, flow_profile);

	if (MAX_OUTPUT_DEST < FLOW_MAX_QUEUES) {
		assert(0);
		NT_LOG(ERR, FILTER,
			"ERROR: Internal array for multiple queues too small for API");
	}

	pthread_mutex_lock(&base_mtx);
	struct flow_nic_dev *ndev = get_nic_dev_from_adapter_no(adapter_no);

	if (!ndev) {
		/* Error - no flow api found on specified adapter */
		NT_LOG(ERR, FILTER, "ERROR: no flow interface registered for adapter %d",
			adapter_no);
		pthread_mutex_unlock(&base_mtx);
		return NULL;
	}

	if (ndev->ports < ((uint16_t)port_no + 1)) {
		NT_LOG(ERR, FILTER, "ERROR: port exceeds supported port range for adapter");
		pthread_mutex_unlock(&base_mtx);
		return NULL;
	}

	if ((alloc_rx_queues - 1) > FLOW_MAX_QUEUES) {	/* 0th is exception so +1 */
		NT_LOG(ERR, FILTER,
			"ERROR: Exceeds supported number of rx queues per eth device");
		pthread_mutex_unlock(&base_mtx);
		return NULL;
	}

	/* don't accept multiple eth_dev's on same NIC and same port */
	eth_dev = nic_and_port_to_eth_dev(adapter_no, port_no);

	if (eth_dev) {
		NT_LOG(DBG, FILTER, "Re-opening existing NIC port device: NIC DEV: %i Port %i",
			adapter_no, port_no);
		pthread_mutex_unlock(&base_mtx);
		flow_delete_eth_dev(eth_dev);
		eth_dev = NULL;
	}

	eth_dev = calloc(1, sizeof(struct flow_eth_dev));

	if (!eth_dev) {
		NT_LOG(ERR, FILTER, "ERROR: calloc failed");
		goto err_exit1;
	}

	pthread_mutex_lock(&ndev->mtx);

	eth_dev->ndev = ndev;
	eth_dev->port = port_no;
	eth_dev->port_id = port_id;

	/* First time then NIC is initialized */
	if (!ndev->flow_mgnt_prepared) {
		ndev->flow_profile = flow_profile;

		/* Initialize modules if needed - recipe 0 is used as no-match and must be setup */
		if (profile_inline_ops != NULL &&
			profile_inline_ops->initialize_flow_management_of_ndev_profile_inline(ndev))
			goto err_exit0;

	} else {
		/* check if same flow type is requested, otherwise fail */
		if (ndev->flow_profile != flow_profile) {
			NT_LOG(ERR, FILTER,
				"ERROR: Different flow types requested on same NIC device. Not supported.");
			goto err_exit0;
		}
	}

	/* Allocate the requested queues in HW for this dev */

	for (i = 0; i < alloc_rx_queues; i++) {
		eth_dev->rx_queue[i] = queue_ids[i];

		if (i == 0 && (flow_profile == FLOW_ETH_DEV_PROFILE_INLINE && exception_path)) {
			/*
			 * Init QSL UNM - unmatched - redirects otherwise discarded
			 * packets in QSL
			 */
			if (hw_mod_qsl_unmq_set(&ndev->be, HW_QSL_UNMQ_DEST_QUEUE, eth_dev->port,
				eth_dev->rx_queue[0].hw_id) < 0)
				goto err_exit0;

			if (hw_mod_qsl_unmq_set(&ndev->be, HW_QSL_UNMQ_EN, eth_dev->port, 1) < 0)
				goto err_exit0;

			if (hw_mod_qsl_unmq_flush(&ndev->be, eth_dev->port, 1) < 0)
				goto err_exit0;
		}

		eth_dev->num_queues++;
	}

	eth_dev->rss_target_id = -1;

	if (flow_profile == FLOW_ETH_DEV_PROFILE_INLINE) {
		for (i = 0; i < eth_dev->num_queues; i++) {
			uint32_t qen_value = 0;
			uint32_t queue_id = (uint32_t)eth_dev->rx_queue[i].hw_id;

			hw_mod_qsl_qen_get(&ndev->be, HW_QSL_QEN_EN, queue_id / 4, &qen_value);
			hw_mod_qsl_qen_set(&ndev->be, HW_QSL_QEN_EN, queue_id / 4,
				qen_value | (1 << (queue_id % 4)));
			hw_mod_qsl_qen_flush(&ndev->be, queue_id / 4, 1);
		}
	}

	*rss_target_id = eth_dev->rss_target_id;

	nic_insert_eth_port_dev(ndev, eth_dev);

	pthread_mutex_unlock(&ndev->mtx);
	pthread_mutex_unlock(&base_mtx);
	return eth_dev;

err_exit0:
	pthread_mutex_unlock(&ndev->mtx);
	pthread_mutex_unlock(&base_mtx);

err_exit1:
	if (eth_dev)
		free(eth_dev);

#ifdef FLOW_DEBUG
	ndev->be.iface->set_debug_mode(ndev->be.be_dev, FLOW_BACKEND_DEBUG_MODE_NONE);
#endif

	NT_LOG(DBG, FILTER, "ERR in %s", __func__);
	return NULL;	/* Error exit */
}

struct flow_nic_dev *flow_api_create(uint8_t adapter_no, const struct flow_api_backend_ops *be_if,
	void *be_dev)
{
	(void)adapter_no;

	if (!be_if || be_if->version != 1) {
		NT_LOG(DBG, FILTER, "ERR: %s", __func__);
		return NULL;
	}

	struct flow_nic_dev *ndev = calloc(1, sizeof(struct flow_nic_dev));

	if (!ndev) {
		NT_LOG(ERR, FILTER, "ERROR: calloc failed");
		return NULL;
	}

	/*
	 * To dump module initialization writes use
	 * FLOW_BACKEND_DEBUG_MODE_WRITE
	 * then remember to set it ...NONE afterwards again
	 */
	be_if->set_debug_mode(be_dev, FLOW_BACKEND_DEBUG_MODE_NONE);

	if (flow_api_backend_init(&ndev->be, be_if, be_dev) != 0)
		goto err_exit;

	ndev->adapter_no = adapter_no;

	ndev->ports = (uint16_t)((ndev->be.num_rx_ports > 256) ? 256 : ndev->be.num_rx_ports);

	/*
	 * Free resources in NIC must be managed by this module
	 * Get resource sizes and create resource manager elements
	 */
	if (init_resource_elements(ndev, RES_QUEUE, ndev->be.max_queues))
		goto err_exit;

	if (init_resource_elements(ndev, RES_CAT_CFN, ndev->be.cat.nb_cat_funcs))
		goto err_exit;

	if (init_resource_elements(ndev, RES_CAT_COT, ndev->be.max_categories))
		goto err_exit;

	if (init_resource_elements(ndev, RES_CAT_EXO, ndev->be.cat.nb_pm_ext))
		goto err_exit;

	if (init_resource_elements(ndev, RES_CAT_LEN, ndev->be.cat.nb_len))
		goto err_exit;

	if (init_resource_elements(ndev, RES_KM_FLOW_TYPE, ndev->be.cat.nb_flow_types))
		goto err_exit;

	if (init_resource_elements(ndev, RES_KM_CATEGORY, ndev->be.km.nb_categories))
		goto err_exit;

	if (init_resource_elements(ndev, RES_HSH_RCP, ndev->be.hsh.nb_rcp))
		goto err_exit;

	if (init_resource_elements(ndev, RES_PDB_RCP, ndev->be.pdb.nb_pdb_rcp_categories))
		goto err_exit;

	if (init_resource_elements(ndev, RES_QSL_RCP, ndev->be.qsl.nb_rcp_categories))
		goto err_exit;

	if (init_resource_elements(ndev, RES_QSL_QST, ndev->be.qsl.nb_qst_entries))
		goto err_exit;

	if (init_resource_elements(ndev, RES_SLC_LR_RCP, ndev->be.max_categories))
		goto err_exit;

	if (init_resource_elements(ndev, RES_FLM_FLOW_TYPE, ndev->be.cat.nb_flow_types))
		goto err_exit;

	if (init_resource_elements(ndev, RES_FLM_RCP, ndev->be.flm.nb_categories))
		goto err_exit;

	if (init_resource_elements(ndev, RES_TPE_RCP, ndev->be.tpe.nb_rcp_categories))
		goto err_exit;

	if (init_resource_elements(ndev, RES_TPE_EXT, ndev->be.tpe.nb_rpl_ext_categories))
		goto err_exit;

	if (init_resource_elements(ndev, RES_TPE_RPL, ndev->be.tpe.nb_rpl_depth))
		goto err_exit;

	if (init_resource_elements(ndev, RES_SCRUB_RCP, ndev->be.flm.nb_scrub_profiles))
		goto err_exit;

	/* may need IPF, COR */

	/* check all defined has been initialized */
	for (int i = 0; i < RES_COUNT; i++)
		assert(ndev->res[i].alloc_bm);

	pthread_mutex_init(&ndev->mtx, NULL);
	list_insert_flow_nic(ndev);

	return ndev;

err_exit:

	if (ndev)
		flow_api_done(ndev);

	NT_LOG(DBG, FILTER, "ERR: %s", __func__);
	return NULL;
}

int flow_api_done(struct flow_nic_dev *ndev)
{
	NT_LOG(DBG, FILTER, "FLOW API DONE");

	if (ndev) {
		flow_ndev_reset(ndev);

		/* delete resource management allocations for this ndev */
		for (int i = 0; i < RES_COUNT; i++)
			done_resource_elements(ndev, i);

		flow_api_backend_done(&ndev->be);
		list_remove_flow_nic(ndev);
		free(ndev);
	}

	return 0;
}

void *flow_api_get_be_dev(struct flow_nic_dev *ndev)
{
	if (!ndev) {
		NT_LOG(DBG, FILTER, "ERR: %s", __func__);
		return NULL;
	}

	return ndev->be.be_dev;
}

/* Information for a given RSS type. */
struct rss_type_info {
	uint64_t rss_type;
	const char *str;
};

static struct rss_type_info rss_to_string[] = {
	/* RTE_BIT64(2)   IPv4 dst + IPv4 src */
	RSS_TO_STRING(RTE_ETH_RSS_IPV4),
	/* RTE_BIT64(3)   IPv4 dst + IPv4 src + Identification of group of fragments  */
	RSS_TO_STRING(RTE_ETH_RSS_FRAG_IPV4),
	/* RTE_BIT64(4)   IPv4 dst + IPv4 src + L4 protocol */
	RSS_TO_STRING(RTE_ETH_RSS_NONFRAG_IPV4_TCP),
	/* RTE_BIT64(5)   IPv4 dst + IPv4 src + L4 protocol */
	RSS_TO_STRING(RTE_ETH_RSS_NONFRAG_IPV4_UDP),
	/* RTE_BIT64(6)   IPv4 dst + IPv4 src + L4 protocol */
	RSS_TO_STRING(RTE_ETH_RSS_NONFRAG_IPV4_SCTP),
	/* RTE_BIT64(7)   IPv4 dst + IPv4 src + L4 protocol */
	RSS_TO_STRING(RTE_ETH_RSS_NONFRAG_IPV4_OTHER),
	/*
	 * RTE_BIT64(14)  128-bits of L2 payload starting after src MAC, i.e. including optional
	 * VLAN tag and ethertype. Overrides all L3 and L4 flags at the same level, but inner
	 * L2 payload can be combined with outer S-VLAN and GTPU TEID flags.
	 */
	RSS_TO_STRING(RTE_ETH_RSS_L2_PAYLOAD),
	/* RTE_BIT64(18)  L4 dst + L4 src + L4 protocol - see comment of RTE_ETH_RSS_L4_CHKSUM */
	RSS_TO_STRING(RTE_ETH_RSS_PORT),
	/* RTE_BIT64(19)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_VXLAN),
	/* RTE_BIT64(20)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_GENEVE),
	/* RTE_BIT64(21)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_NVGRE),
	/* RTE_BIT64(23)  GTP TEID - always from outer GTPU header */
	RSS_TO_STRING(RTE_ETH_RSS_GTPU),
	/* RTE_BIT64(24)  MAC dst + MAC src */
	RSS_TO_STRING(RTE_ETH_RSS_ETH),
	/* RTE_BIT64(25)  outermost VLAN ID + L4 protocol */
	RSS_TO_STRING(RTE_ETH_RSS_S_VLAN),
	/* RTE_BIT64(26)  innermost VLAN ID + L4 protocol */
	RSS_TO_STRING(RTE_ETH_RSS_C_VLAN),
	/* RTE_BIT64(27)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_ESP),
	/* RTE_BIT64(28)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_AH),
	/* RTE_BIT64(29)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_L2TPV3),
	/* RTE_BIT64(30)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_PFCP),
	/* RTE_BIT64(31)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_PPPOE),
	/* RTE_BIT64(32)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_ECPRI),
	/* RTE_BIT64(33)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_MPLS),
	/* RTE_BIT64(34)  IPv4 Header checksum + L4 protocol */
	RSS_TO_STRING(RTE_ETH_RSS_IPV4_CHKSUM),

	/*
	 * if combined with RTE_ETH_RSS_NONFRAG_IPV4_[TCP|UDP|SCTP] then
	 *   L4 protocol + chosen protocol header Checksum
	 * else
	 *   error
	 */
	/* RTE_BIT64(35) */
	RSS_TO_STRING(RTE_ETH_RSS_L4_CHKSUM),
#ifndef ANDROMEDA_DPDK_21_11
	/* RTE_BIT64(36)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_L2TPV2),
#endif

	{ RTE_BIT64(37), "unknown_RTE_BIT64(37)" },
	{ RTE_BIT64(38), "unknown_RTE_BIT64(38)" },
	{ RTE_BIT64(39), "unknown_RTE_BIT64(39)" },
	{ RTE_BIT64(40), "unknown_RTE_BIT64(40)" },
	{ RTE_BIT64(41), "unknown_RTE_BIT64(41)" },
	{ RTE_BIT64(42), "unknown_RTE_BIT64(42)" },
	{ RTE_BIT64(43), "unknown_RTE_BIT64(43)" },
	{ RTE_BIT64(44), "unknown_RTE_BIT64(44)" },
	{ RTE_BIT64(45), "unknown_RTE_BIT64(45)" },
	{ RTE_BIT64(46), "unknown_RTE_BIT64(46)" },
	{ RTE_BIT64(47), "unknown_RTE_BIT64(47)" },
	{ RTE_BIT64(48), "unknown_RTE_BIT64(48)" },
	{ RTE_BIT64(49), "unknown_RTE_BIT64(49)" },

	/* RTE_BIT64(50)  outermost encapsulation */
	RSS_TO_STRING(RTE_ETH_RSS_LEVEL_OUTERMOST),
	/* RTE_BIT64(51)  innermost encapsulation */
	RSS_TO_STRING(RTE_ETH_RSS_LEVEL_INNERMOST),

	/* RTE_BIT64(52)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_L3_PRE96),
	/* RTE_BIT64(53)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_L3_PRE64),
	/* RTE_BIT64(54)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_L3_PRE56),
	/* RTE_BIT64(55)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_L3_PRE48),
	/* RTE_BIT64(56)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_L3_PRE40),
	/* RTE_BIT64(57)  Not supported */
	RSS_TO_STRING(RTE_ETH_RSS_L3_PRE32),

	/* RTE_BIT64(58) */
	RSS_TO_STRING(RTE_ETH_RSS_L2_DST_ONLY),
	/* RTE_BIT64(59) */
	RSS_TO_STRING(RTE_ETH_RSS_L2_SRC_ONLY),
	/* RTE_BIT64(60) */
	RSS_TO_STRING(RTE_ETH_RSS_L4_DST_ONLY),
	/* RTE_BIT64(61) */
	RSS_TO_STRING(RTE_ETH_RSS_L4_SRC_ONLY),
	/* RTE_BIT64(62) */
	RSS_TO_STRING(RTE_ETH_RSS_L3_DST_ONLY),
	/* RTE_BIT64(63) */
	RSS_TO_STRING(RTE_ETH_RSS_L3_SRC_ONLY),
};

int sprint_nt_rss_mask(char *str, uint16_t str_len, const char *prefix, uint64_t hash_mask)
{
	if (str == NULL || str_len == 0)
		return -1;

	memset(str, 0x0, str_len);
	uint16_t str_end = 0;
	const struct rss_type_info *start = rss_to_string;

	for (const struct rss_type_info *p = start; p != start + ARRAY_SIZE(rss_to_string); ++p) {
		if (p->rss_type & hash_mask) {
			if (strlen(prefix) + strlen(p->str) < (size_t)(str_len - str_end)) {
				snprintf(str + str_end, str_len - str_end, "%s", prefix);
				str_end += strlen(prefix);
				snprintf(str + str_end, str_len - str_end, "%s", p->str);
				str_end += strlen(p->str);

			} else {
				return -1;
			}
		}
	}

	return 0;
}

/*
 * Hash
 */

int flow_nic_set_hasher(struct flow_nic_dev *ndev, int hsh_idx, enum flow_nic_hash_e algorithm)
{
	hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_PRESET_ALL, hsh_idx, 0, 0);

	switch (algorithm) {
	case HASH_ALGO_5TUPLE:
		/* need to create an IPv6 hashing and enable the adaptive ip mask bit */
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_LOAD_DIST_TYPE, hsh_idx, 0, 2);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_QW0_PE, hsh_idx, 0, DYN_FINAL_IP_DST);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_QW0_OFS, hsh_idx, 0, -16);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_QW4_PE, hsh_idx, 0, DYN_FINAL_IP_DST);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_QW4_OFS, hsh_idx, 0, 0);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_W8_PE, hsh_idx, 0, DYN_L4);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_W8_OFS, hsh_idx, 0, 0);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_W9_PE, hsh_idx, 0, 0);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_W9_OFS, hsh_idx, 0, 0);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_W9_P, hsh_idx, 0, 0);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_P_MASK, hsh_idx, 0, 1);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 0, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 1, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 2, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 3, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 4, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 5, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 6, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 7, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 8, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_WORD_MASK, hsh_idx, 9, 0);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_SEED, hsh_idx, 0, 0xffffffff);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_HSH_VALID, hsh_idx, 0, 1);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_HSH_TYPE, hsh_idx, 0, HASH_5TUPLE);
		hw_mod_hsh_rcp_set(&ndev->be, HW_HSH_RCP_AUTO_IPV4_MASK, hsh_idx, 0, 1);

		NT_LOG(DBG, FILTER, "Set IPv6 5-tuple hasher with adaptive IPv4 hashing");
		break;

	default:
	case HASH_ALGO_ROUND_ROBIN:
		/* zero is round-robin */
		break;
	}

	return 0;
}

static int flow_dev_dump(struct flow_eth_dev *dev,
	struct flow_handle *flow,
	uint16_t caller_id,
	FILE *file,
	struct rte_flow_error *error)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG(ERR, FILTER, "%s: profile_inline module uninitialized", __func__);
		return -1;
	}

	return profile_inline_ops->flow_dev_dump_profile_inline(dev, flow, caller_id, file, error);
}

int flow_nic_set_hasher_fields(struct flow_nic_dev *ndev, int hsh_idx,
	struct nt_eth_rss_conf rss_conf)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG(ERR, FILTER, "%s: profile_inline module uninitialized", __func__);
		return -1;
	}

	return profile_inline_ops->flow_nic_set_hasher_fields_inline(ndev, hsh_idx, rss_conf);
}

static int flow_get_aged_flows(struct flow_eth_dev *dev,
	uint16_t caller_id,
	void **context,
	uint32_t nb_contexts,
	struct rte_flow_error *error)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG_DBGX(ERR, FILTER, "profile_inline_ops uninitialized");
		return -1;
	}

	if (nb_contexts > 0 && !context) {
		error->type = RTE_FLOW_ERROR_TYPE_UNSPECIFIED;
		error->message = "rte_flow_get_aged_flows - empty context";
		return -1;
	}

	return profile_inline_ops->flow_get_aged_flows_profile_inline(dev, caller_id, context,
			nb_contexts, error);
}

static int flow_info_get(struct flow_eth_dev *dev, uint8_t caller_id,
	struct rte_flow_port_info *port_info, struct rte_flow_queue_info *queue_info,
	struct rte_flow_error *error)
{
	(void)dev;
	(void)caller_id;
	(void)port_info;
	(void)queue_info;
	(void)error;

	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG_DBGX(ERR, FILTER, "profile_inline module uninitialized");
		return -1;
	}

	return 0;
}

static int flow_configure(struct flow_eth_dev *dev, uint8_t caller_id,
	const struct rte_flow_port_attr *port_attr, uint16_t nb_queue,
	const struct rte_flow_queue_attr *queue_attr[], struct rte_flow_error *error)
{
	(void)dev;
	(void)caller_id;
	(void)port_attr;
	(void)queue_attr;
	(void)nb_queue;
	(void)error;

	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL) {
		NT_LOG_DBGX(ERR, FILTER, "profile_inline module uninitialized");
		return -1;
	}

	return 0;
}

int flow_get_flm_stats(struct flow_nic_dev *ndev, uint64_t *data, uint64_t size)
{
	const struct profile_inline_ops *profile_inline_ops = get_profile_inline_ops();

	if (profile_inline_ops == NULL)
		return -1;

	if (ndev->flow_profile == FLOW_ETH_DEV_PROFILE_INLINE)
		return profile_inline_ops->flow_get_flm_stats_profile_inline(ndev, data, size);

	return -1;
}

static const struct flow_filter_ops ops = {
	.flow_filter_init = flow_filter_init,
	.flow_filter_done = flow_filter_done,
	/*
	 * Device Management API
	 */
	.flow_get_eth_dev = flow_get_eth_dev,
	/*
	 * NT Flow API
	 */
	.flow_create = flow_create,
	.flow_destroy = flow_destroy,
	.flow_flush = flow_flush,
	.flow_dev_dump = flow_dev_dump,
	.flow_get_flm_stats = flow_get_flm_stats,
	.flow_get_aged_flows = flow_get_aged_flows,

	/*
	 * NT Flow asynchronous operations API
	 */
	.flow_info_get = flow_info_get,
	.flow_configure = flow_configure,

	/*
	 * Other
	 */
	 .hw_mod_hsh_rcp_flush = hw_mod_hsh_rcp_flush,
	 .flow_nic_set_hasher_fields = flow_nic_set_hasher_fields,
};

void init_flow_filter(void)
{
	register_flow_filter_ops(&ops);
}
