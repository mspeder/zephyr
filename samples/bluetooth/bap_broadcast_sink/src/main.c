/*
 * Copyright (c) 2022-2024 Nordic Semiconductor ASA
 * Copyright (c) 2024 Demant A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/sys_clock.h>
#include <zephyr/toolchain.h>

#include "lc3.h"
#include "stream_rx.h"
#include "usb.h"

BUILD_ASSERT(IS_ENABLED(CONFIG_SCAN_SELF) || IS_ENABLED(CONFIG_SCAN_OFFLOAD),
	     "Either SCAN_SELF or SCAN_OFFLOAD must be enabled");

#define SEM_TIMEOUT                 K_SECONDS(60)
#define BROADCAST_ASSISTANT_TIMEOUT K_SECONDS(120) /* 2 minutes */

#define LOG_INTERVAL 1000U

#if defined(CONFIG_SCAN_SELF)
#define ADV_TIMEOUT K_SECONDS(CONFIG_SCAN_DELAY)
#else /* !CONFIG_SCAN_SELF */
#define ADV_TIMEOUT K_FOREVER
#endif /* CONFIG_SCAN_SELF */

#define PA_SYNC_INTERVAL_TO_TIMEOUT_RATIO 5 /* Set the timeout relative to interval */
#define PA_SYNC_SKIP                5
#define NAME_LEN                    sizeof(CONFIG_TARGET_BROADCAST_NAME) + 1
#define BROADCAST_DATA_ELEMENT_SIZE sizeof(int16_t)

static K_SEM_DEFINE(sem_broadcast_sink_stopped, 0U, 1U);
static K_SEM_DEFINE(sem_connected, 0U, 1U);
static K_SEM_DEFINE(sem_disconnected, 0U, 1U);
static K_SEM_DEFINE(sem_broadcaster_found, 0U, 1U);
static K_SEM_DEFINE(sem_pa_synced, 0U, 1U);
static K_SEM_DEFINE(sem_base_received, 0U, 1U);
static K_SEM_DEFINE(sem_syncable, 0U, 1U);
static K_SEM_DEFINE(sem_pa_sync_lost, 0U, 1U);
static K_SEM_DEFINE(sem_broadcast_code_received, 0U, 1U);
static K_SEM_DEFINE(sem_pa_request, 0U, 1U);
static K_SEM_DEFINE(sem_past_request, 0U, 1U);
static K_SEM_DEFINE(sem_bis_sync_requested, 0U, 1U);
static K_SEM_DEFINE(sem_stream_connected, 0U, CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT);
static K_SEM_DEFINE(sem_stream_started, 0U, CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT);
static K_SEM_DEFINE(sem_big_synced, 0U, 1U);

/* Sample assumes that we only have a single Scan Delegator receive state */
static const struct bt_bap_scan_delegator_recv_state *req_recv_state;
static struct bt_bap_broadcast_sink *broadcast_sink;
static struct bt_le_scan_recv_info broadcaster_info;
static bt_addr_le_t broadcaster_addr;
static struct bt_le_per_adv_sync *pa_sync;
static uint32_t broadcaster_broadcast_id;
static struct bt_bap_stream *bap_streams_p[CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT];
static volatile bool big_synced;
static volatile bool base_received;
static struct bt_conn *broadcast_assistant_conn;
static struct bt_le_ext_adv *ext_adv;

static const struct bt_audio_codec_cap codec_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_24KHZ,
	BT_AUDIO_CODEC_CAP_DURATION_10, BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1), 40u, 60u,
	CONFIG_MAX_CODEC_FRAMES_PER_SDU,
	(BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL | BT_AUDIO_CONTEXT_TYPE_MEDIA));

/**
 * The base_recv_cb() function will populate struct bis_audio_allocation with channel allocation
 * information for a BIS.
 *
 * The valid value is false if no valid allocation exists.
 */
struct bis_audio_allocation {
	bool valid;
	enum bt_audio_location value;
};

/**
 * The base_recv_cb() function will populate struct base_subgroup_data with the BIS index and
 * channel allocation information for each BIS in the subgroup.
 *
 * The bis_index_bitfield is a bitfield where each bit represents a BIS index. The
 * first bit (bit 0) represents BIS index 1, the second bit (bit 1) represents BIS index 2,
 * and so on.
 *
 * The audio_allocation array holds the channel allocation information for each BIS in the
 * subgroup. The first element (index 0) is not used (BIS index 0 does not exist), the second
 * element (index 1) corresponds to BIS index 1, and so on.
 */
struct base_subgroup_data {
	uint32_t bis_index_bitfield;
	struct bis_audio_allocation
		audio_allocation[BT_ISO_BIS_INDEX_MAX + 1]; /* First BIS index is 1 */
};

/**
 * The base_recv_cb() function will populate struct base_data with BIS data
 * for each subgroup.
 *
 * The subgroup_cnt is the number of subgroups in the BASE.
 */
struct base_data {
	struct base_subgroup_data subgroup_bis[CONFIG_BT_BAP_BASS_MAX_SUBGROUPS];
	uint8_t subgroup_cnt;
};

static struct base_data base_recv_data; /* holds data from base_recv_cb */
static uint32_t
	requested_bis_sync[CONFIG_BT_BAP_BASS_MAX_SUBGROUPS]; /* holds data from bis_sync_req_cb */
static uint8_t sink_broadcast_code[BT_ISO_BROADCAST_CODE_SIZE];

static int stop_adv(void);
static uint8_t get_stream_count(uint32_t bitfield);

static void stream_connected_cb(struct bt_bap_stream *bap_stream)
{
	printk("Stream %p connected\n", bap_stream);

	k_sem_give(&sem_stream_connected);
}

static void stream_disconnected_cb(struct bt_bap_stream *bap_stream, uint8_t reason)
{
	int err;

	printk("Stream %p disconnected with reason 0x%02X\n", bap_stream, reason);

	err = k_sem_take(&sem_stream_connected, K_NO_WAIT);
	if (err != 0) {
		printk("Failed to take sem_stream_connected: %d\n", err);
	}
}

static void stream_started_cb(struct bt_bap_stream *bap_stream)
{
	int err;

	printk("Stream %p started\n", bap_stream);

	err = stream_rx_started(bap_stream);
	if (err != 0) {
		printk("stream_rx_started returned error: %d\n", err);
	}

	k_sem_give(&sem_stream_started);
}

static void stream_stopped_cb(struct bt_bap_stream *bap_stream, uint8_t reason)
{
	int err;

	printk("Stream %p stopped with reason 0x%02X\n", bap_stream, reason);

	err = stream_rx_stopped(bap_stream);
	if (err != 0) {
		printk("stream_rx_stopped returned error: %d\n", err);
	}

	err = k_sem_take(&sem_stream_started, K_NO_WAIT);
	if (err != 0) {
		printk("Failed to take sem_stream_started: %d\n", err);
	}
}

static void stream_recv_cb(struct bt_bap_stream *bap_stream, const struct bt_iso_recv_info *info,
			   struct net_buf *buf)
{
	stream_rx_recv(bap_stream, info, buf);
}

static struct bt_bap_stream_ops stream_ops = {
	.connected = stream_connected_cb,
	.disconnected = stream_disconnected_cb,
	.started = stream_started_cb,
	.stopped = stream_stopped_cb,
	.recv = stream_recv_cb,
};

/**
 * This is called for each BIS in a subgroup
 *
 * Gets BIS channel allocation (if exists).
 * Always returns `true` to continue to next BIS
 */
static bool bis_get_channel_allocation_cb(const struct bt_bap_base_subgroup_bis *bis,
					  void *user_data)
{
	struct base_subgroup_data *base_subgroup_bis = user_data;
	struct bt_audio_codec_cfg codec_cfg;
	int err;

	err = bt_bap_base_subgroup_bis_codec_to_codec_cfg(bis, &codec_cfg);
	if (err != 0) {
		printk("Could not get codec configuration for BIS: %d\n", err);

		return true; /* continue to next BIS */
	}

	err = bt_audio_codec_cfg_get_chan_allocation(
		&codec_cfg, &base_subgroup_bis->audio_allocation[bis->index].value, true);
	if (err != 0) {
		printk("Could not find channel allocation for BIS: %d\n", err);

		return true; /* continue to next BIS */
	}

	/* Channel allocation data available for this bis */
	base_subgroup_bis->audio_allocation[bis->index].valid = true;

	return true; /* continue to next BIS */
}

/**
 * Called for each subgroup in the BASE. Will populate the struct base_subgroup_data variable with
 * BIS index and channel allocation information.
 *
 * The channel allocation may
 *  - Not exist at all, implicitly meaning BT_AUDIO_LOCATION_MONO_AUDIO
 *  - Exist only in the subgroup codec configuration
 *  - Exist only in the BIS codec configuration
 *  - Exist in both the subgroup and BIS codec configuration, in which case, the BIS codec
 *    configuration overwrites the subgroup values
 */
static bool subgroup_get_valid_bis_indexes_cb(const struct bt_bap_base_subgroup *subgroup,
					      void *user_data)
{
	enum bt_audio_location subgroup_chan_allocation;
	bool subgroup_chan_allocation_available = false;
	struct base_data *data = user_data;
	struct base_subgroup_data *base_subgroup_bis = &data->subgroup_bis[data->subgroup_cnt];
	struct bt_audio_codec_cfg codec_cfg;
	int err;

	err = bt_bap_base_subgroup_codec_to_codec_cfg(subgroup, &codec_cfg);
	if (err != 0) {
		printk("Could not get codec configuration: %d\n", err);
		goto next_subgroup;
	}

	if (codec_cfg.id != BT_HCI_CODING_FORMAT_LC3) {
		printk("Only LC3 codec supported (%u)\n", codec_cfg.id);
		goto next_subgroup;
	}

	/* Get all BIS indexes for subgroup */
	err = bt_bap_base_subgroup_get_bis_indexes(subgroup,
						   &base_subgroup_bis->bis_index_bitfield);
	if (err != 0) {
		printk("Failed to parse all BIS in subgroup: %d\n", err);
		goto next_subgroup;
	}

	/* Get channel allocation at subgroup level */
	err = bt_audio_codec_cfg_get_chan_allocation(&codec_cfg, &subgroup_chan_allocation, true);
	if (err == 0) {
		printk("Channel allocation (subgroup level) 0x%08x\n", subgroup_chan_allocation);
		subgroup_chan_allocation_available = true;
	} else {
		printk("Subgroup error chan allocation error: %d\n", err);
		goto next_subgroup;
	}

	/* Get channel allocation at BIS level */
	err = bt_bap_base_subgroup_foreach_bis(subgroup, bis_get_channel_allocation_cb,
					       base_subgroup_bis);
	if (err != 0) {
		printk("Get channel allocation error (BIS level) %d\n", err);
		goto next_subgroup;
	}

	/* If no BIS channel allocation available use subgroup channel allocation instead if
	 * exists (otherwise mono assumed)
	 */
	for (uint8_t idx = 1U; idx <= BT_ISO_BIS_INDEX_MAX; idx++) {
		if (base_subgroup_bis->bis_index_bitfield & BT_ISO_BIS_INDEX_BIT(idx)) {
			if (!base_subgroup_bis->audio_allocation[idx].valid) {
				base_subgroup_bis->audio_allocation[idx].value =
					subgroup_chan_allocation_available
						? subgroup_chan_allocation
						: BT_AUDIO_LOCATION_MONO_AUDIO;
				base_subgroup_bis->audio_allocation[idx].valid = true;
			}
			printk("BIS index 0x%08x allocation = 0x%08x\n", idx,
			       base_subgroup_bis->audio_allocation[idx].value);
		}
	}

next_subgroup:
	data->subgroup_cnt++;

	return true; /* continue to next subgroup */
}

static void base_recv_cb(struct bt_bap_broadcast_sink *sink, const struct bt_bap_base *base,
			 size_t base_size)
{
	int err;

	if (base_received) {
		return;
	}

	printk("Received BASE with %d subgroups from broadcast sink %p\n",
	       bt_bap_base_get_subgroup_count(base), sink);

	(void)memset(&base_recv_data, 0, sizeof(base_recv_data));

	/* Get BIS index data for each subgroup */
	err = bt_bap_base_foreach_subgroup(base, subgroup_get_valid_bis_indexes_cb,
					   &base_recv_data);
	if (err != 0) {
		printk("Failed to get valid BIS indexes: %d\n", err);

		return;
	}

	if (broadcast_assistant_conn == NULL) {
		/* No broadcast assistant requesting anything */
		for (int i = 0; i < CONFIG_BT_BAP_BASS_MAX_SUBGROUPS; i++) {
			requested_bis_sync[i] = BT_BAP_BIS_SYNC_NO_PREF;
		}
		k_sem_give(&sem_bis_sync_requested);
	}

	base_received = true;
	k_sem_give(&sem_base_received);
}

static void syncable_cb(struct bt_bap_broadcast_sink *sink, const struct bt_iso_biginfo *biginfo)
{
	printk("Broadcast sink (%p) is syncable, BIG %s\n", (void *)sink,
	       biginfo->encryption ? "encrypted" : "not encrypted");

	k_sem_give(&sem_syncable);

	if (!biginfo->encryption) {
		k_sem_give(&sem_broadcast_code_received);
	}
}

static void broadcast_sink_started_cb(struct bt_bap_broadcast_sink *sink)
{
	printk("Broadcast sink %p started\n", sink);

	big_synced = true;
	k_sem_give(&sem_big_synced);
}

static void broadcast_sink_stopped_cb(struct bt_bap_broadcast_sink *sink, uint8_t reason)
{
	printk("Broadcast sink %p stopped with reason 0x%02X\n", sink, reason);

	big_synced = false;
	k_sem_give(&sem_broadcast_sink_stopped);
}

static struct bt_bap_broadcast_sink_cb broadcast_sink_cbs = {
	.base_recv = base_recv_cb,
	.syncable = syncable_cb,
	.started = broadcast_sink_started_cb,
	.stopped = broadcast_sink_stopped_cb,
};

static void pa_timer_handler(struct k_work *work)
{
	if (req_recv_state != NULL) {
		enum bt_bap_pa_state pa_state;

		if (req_recv_state->pa_sync_state == BT_BAP_PA_STATE_INFO_REQ) {
			pa_state = BT_BAP_PA_STATE_NO_PAST;
		} else {
			pa_state = BT_BAP_PA_STATE_FAILED;
		}

		bt_bap_scan_delegator_set_pa_state(req_recv_state->src_id,
						   pa_state);
	}

	printk("PA timeout\n");
}

static K_WORK_DELAYABLE_DEFINE(pa_timer, pa_timer_handler);

static uint16_t interval_to_sync_timeout(uint16_t pa_interval)
{
	uint16_t pa_timeout;

	if (pa_interval == BT_BAP_PA_INTERVAL_UNKNOWN) {
		/* Use maximum value to maximize chance of success */
		pa_timeout = BT_GAP_PER_ADV_MAX_TIMEOUT;
	} else {
		uint32_t interval_us;
		uint32_t timeout;

		/* Add retries and convert to unit in 10's of ms */
		interval_us = BT_GAP_PER_ADV_INTERVAL_TO_US(pa_interval);
		timeout = BT_GAP_US_TO_PER_ADV_SYNC_TIMEOUT(interval_us) *
			  PA_SYNC_INTERVAL_TO_TIMEOUT_RATIO;

		/* Enforce restraints */
		pa_timeout = CLAMP(timeout, BT_GAP_PER_ADV_MIN_TIMEOUT, BT_GAP_PER_ADV_MAX_TIMEOUT);
	}

	return pa_timeout;
}

static int pa_sync_past(struct bt_conn *conn, uint16_t pa_interval)
{
	struct bt_le_per_adv_sync_transfer_param param = { 0 };
	int err;

	param.skip = PA_SYNC_SKIP;
	param.timeout = interval_to_sync_timeout(pa_interval);

	err = bt_le_per_adv_sync_transfer_subscribe(conn, &param);
	if (err != 0) {
		printk("Could not do PAST subscribe: %d\n", err);
	} else {
		printk("Syncing with PAST\n");
		(void)k_work_reschedule(&pa_timer, K_MSEC(param.timeout * 10));
	}

	return err;
}

static void recv_state_updated_cb(struct bt_conn *conn,
				  const struct bt_bap_scan_delegator_recv_state *recv_state)
{
	printk("Receive state updated, pa sync state: %u, encrypt_state %u\n",
	       recv_state->pa_sync_state, recv_state->encrypt_state);

	for (uint8_t i = 0; i < recv_state->num_subgroups; i++) {
		printk("subgroup %d bis_sync: 0x%08x\n", i, recv_state->subgroups[i].bis_sync);
	}

	req_recv_state = recv_state;
}

static int pa_sync_req_cb(struct bt_conn *conn,
			  const struct bt_bap_scan_delegator_recv_state *recv_state,
			  bool past_avail, uint16_t pa_interval)
{

	printk("Received request to sync to PA (PAST %savailble): %u\n", past_avail ? "" : "not ",
	       recv_state->pa_sync_state);

	req_recv_state = recv_state;

	if (recv_state->pa_sync_state == BT_BAP_PA_STATE_SYNCED ||
	    recv_state->pa_sync_state == BT_BAP_PA_STATE_INFO_REQ) {
		/* Already syncing */
		/* TODO: Terminate existing sync and then sync to new?*/
		return -1;
	}

	if (IS_ENABLED(CONFIG_BT_PER_ADV_SYNC_TRANSFER_RECEIVER) && past_avail) {
		int err;

		err = pa_sync_past(conn, pa_interval);
		if (err != 0) {
			printk("Failed to subscribe to PAST: %d\n", err);

			return err;
		}

		k_sem_give(&sem_past_request);

		err = bt_bap_scan_delegator_set_pa_state(recv_state->src_id,
							 BT_BAP_PA_STATE_INFO_REQ);
		if (err != 0) {
			printk("Failed to set PA state to BT_BAP_PA_STATE_INFO_REQ: %d\n", err);

			return err;
		}
	}

	k_sem_give(&sem_pa_request);

	return 0;
}

static int pa_sync_term_req_cb(struct bt_conn *conn,
			       const struct bt_bap_scan_delegator_recv_state *recv_state)
{
	int err;

	printk("PA sync termination req, pa sync state: %u\n", recv_state->pa_sync_state);

	for (uint8_t i = 0; i < recv_state->num_subgroups; i++) {
		printk("subgroup %d bis_sync: 0x%08x\n", i, recv_state->subgroups[i].bis_sync);
	}

	req_recv_state = recv_state;

	printk("Delete periodic advertising sync\n");
	err = bt_le_per_adv_sync_delete(pa_sync);
	if (err != 0) {
		printk("Could not delete per adv sync: %d\n", err);

		return err;
	}

	return 0;
}

static void broadcast_code_cb(struct bt_conn *conn,
			      const struct bt_bap_scan_delegator_recv_state *recv_state,
			      const uint8_t broadcast_code[BT_ISO_BROADCAST_CODE_SIZE])
{
	printk("Broadcast code received for %p\n", recv_state);

	req_recv_state = recv_state;

	(void)memcpy(sink_broadcast_code, broadcast_code, BT_ISO_BROADCAST_CODE_SIZE);

	k_sem_give(&sem_broadcast_code_received);
}

static int bis_sync_req_cb(struct bt_conn *conn,
			   const struct bt_bap_scan_delegator_recv_state *recv_state,
			   const uint32_t bis_sync_req[CONFIG_BT_BAP_BASS_MAX_SUBGROUPS])
{
	bool sync_req = false;
	bool bis_sync_req_no_pref = true;
	uint8_t subgroup_sync_req_cnt = 0;
	uint32_t bis_sync_req_bitfield = 0;

	(void)memset(requested_bis_sync, 0, sizeof(requested_bis_sync));

	for (uint8_t subgroup = 0; subgroup < recv_state->num_subgroups; subgroup++) {

		printk("bis_sync_req[%u] = 0x%0x\n", subgroup, bis_sync_req[subgroup]);
		if (bis_sync_req[subgroup] != 0) {
			requested_bis_sync[subgroup] = bis_sync_req[subgroup];
			if (bis_sync_req[subgroup] != BT_BAP_BIS_SYNC_NO_PREF) {
				bis_sync_req_no_pref = false;
			}
			bis_sync_req_bitfield |= bis_sync_req[subgroup];
			subgroup_sync_req_cnt++;
			sync_req = true;
		}
	}

	if (!bis_sync_req_no_pref) {
		uint8_t stream_count = get_stream_count(bis_sync_req_bitfield);

		/* We only want to sync to a single subgroup. If no preference is given, we will
		 * later set the first possible subgroup as the one to sync to.
		 */
		if (subgroup_sync_req_cnt > 1U) {
			printk("Only request sync to 1 subgroup!\n");

			return -EINVAL;
		}

		if (stream_count > CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT) {
			printk("Too many BIS requested for sync: %u > %d\n", stream_count,
			       CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT);

			return -EINVAL;
		}
	}

	printk("BIS sync req for %p, broadcast id: 0x%06x, (%s)\n", recv_state,
	       recv_state->broadcast_id, big_synced ? "BIG synced" : "BIG not synced");

	if (big_synced) {
		int err;

		if (sync_req) {
			printk("Already synced!\n");

			return -EINVAL;
		}

		/* The stream stopped callback will be called as part of this,
		* and we do not need to wait for any events from the
		* controller. Thus, when this returns, the `big_synced`
		* is back to false.
		*/
		err = bt_bap_broadcast_sink_stop(broadcast_sink);
		if (err != 0) {
			printk("Failed to stop Broadcast Sink: %d\n", err);

			return err;
		}
	}

	broadcaster_broadcast_id = recv_state->broadcast_id;
	if (sync_req) {
		k_sem_give(&sem_bis_sync_requested);
	}

	return 0;
}

static struct bt_bap_scan_delegator_cb scan_delegator_cbs = {
	.recv_state_updated = recv_state_updated_cb,
	.pa_sync_req = pa_sync_req_cb,
	.pa_sync_term_req = pa_sync_term_req_cb,
	.broadcast_code = broadcast_code_cb,
	.bis_sync_req = bis_sync_req_cb,
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0U) {
		printk("Failed to connect to %s %u %s\n", addr, err, bt_hci_err_to_str(err));

		broadcast_assistant_conn = NULL;
		return;
	}

	printk("Connected: %s\n", addr);
	broadcast_assistant_conn = bt_conn_ref(conn);

	k_sem_give(&sem_connected);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (conn != broadcast_assistant_conn) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected: %s, reason 0x%02x %s\n", addr, reason, bt_hci_err_to_str(reason));

	bt_conn_unref(broadcast_assistant_conn);
	broadcast_assistant_conn = NULL;

	k_sem_give(&sem_disconnected);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static struct bt_pacs_cap cap = {
	.codec_cap = &codec_cap,
};

static bool scan_check_and_sync_broadcast(struct bt_data *data, void *user_data)
{
	const struct bt_le_scan_recv_info *info = user_data;
	char le_addr[BT_ADDR_LE_STR_LEN];
	struct bt_uuid_16 adv_uuid;
	uint32_t broadcast_id;

	if (data->type != BT_DATA_SVC_DATA16) {
		return true;
	}

	if (data->data_len < BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE) {
		return true;
	}

	if (!bt_uuid_create(&adv_uuid.uuid, data->data, BT_UUID_SIZE_16)) {
		return true;
	}

	if (bt_uuid_cmp(&adv_uuid.uuid, BT_UUID_BROADCAST_AUDIO)) {
		return true;
	}

	broadcast_id = sys_get_le24(data->data + BT_UUID_SIZE_16);

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));

	printk("Found broadcaster with ID 0x%06X and addr %s and sid 0x%02X\n", broadcast_id,
	       le_addr, info->sid);

	if (broadcast_assistant_conn == NULL /* Not requested by Broadcast Assistant */ ||
	    (req_recv_state != NULL && bt_addr_le_eq(info->addr, &req_recv_state->addr) &&
	     info->sid == req_recv_state->adv_sid &&
	     broadcast_id == req_recv_state->broadcast_id)) {

		/* Store info for PA sync parameters */
		memcpy(&broadcaster_info, info, sizeof(broadcaster_info));
		bt_addr_le_copy(&broadcaster_addr, info->addr);
		broadcaster_broadcast_id = broadcast_id;
		printk("broadcaster_broadcast_id = 0x%06X\n", broadcaster_broadcast_id);
		k_sem_give(&sem_broadcaster_found);
	}

	/* Stop parsing */
	return false;
}

static bool is_substring(const char *substr, const char *str)
{
	const size_t str_len = strlen(str);
	const size_t sub_str_len = strlen(substr);

	if (sub_str_len > str_len) {
		return false;
	}

	for (size_t pos = 0; pos < str_len; pos++) {
		if (pos + sub_str_len > str_len) {
			return false;
		}

		if (strncasecmp(substr, &str[pos], sub_str_len) == 0) {
			return true;
		}
	}

	return false;
}

static bool data_cb(struct bt_data *data, void *user_data)
{
	bool *device_found = user_data;
	char name[NAME_LEN] = {0};

	switch (data->type) {
	case BT_DATA_NAME_SHORTENED:
	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_BROADCAST_NAME:
		memcpy(name, data->data, MIN(data->data_len, NAME_LEN - 1));

		if (is_substring(CONFIG_TARGET_BROADCAST_NAME, name)) {
			/* Device found */
			*device_found = true;
			return false;
		}
		return true;
	default:
		return true;
	}
}

static void broadcast_scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
	if (info->interval != 0U) {
		/* call to bt_data_parse consumes netbufs so shallow clone for verbose output */

		/* If req_recv_state is not NULL then we have been requested by a broadcast
		 * assistant to sync to a specific broadcast source. In that case we do not apply
		 * our own broadcast name filter.
		 */
		if (req_recv_state == NULL && strlen(CONFIG_TARGET_BROADCAST_NAME) > 0U) {
			bool device_found = false;
			struct net_buf_simple buf_copy;

			net_buf_simple_clone(ad, &buf_copy);
			bt_data_parse(&buf_copy, data_cb, &device_found);

			if (!device_found) {
				return;
			}
		}
		bt_data_parse(ad, scan_check_and_sync_broadcast, (void *)info);
	}
}

static struct bt_le_scan_cb bap_scan_cb = {
	.recv = broadcast_scan_recv,
};

static void bap_pa_sync_synced_cb(struct bt_le_per_adv_sync *sync,
				  struct bt_le_per_adv_sync_synced_info *info)
{
	if (sync == pa_sync ||
	    (req_recv_state != NULL && bt_addr_le_eq(info->addr, &req_recv_state->addr) &&
	     info->sid == req_recv_state->adv_sid)) {
		printk("PA sync %p synced for broadcast sink with broadcast ID 0x%06X\n", sync,
		       broadcaster_broadcast_id);

		if (pa_sync == NULL) {
			pa_sync = sync;
		}

		k_work_cancel_delayable(&pa_timer);
		k_sem_give(&sem_pa_synced);
	}
}

static void bap_pa_sync_terminated_cb(struct bt_le_per_adv_sync *sync,
				      const struct bt_le_per_adv_sync_term_info *info)
{
	if (sync == pa_sync) {
		printk("PA sync %p lost with reason 0x%02X\n", sync, info->reason);
		pa_sync = NULL;

		k_sem_give(&sem_pa_sync_lost);

		if (info->reason != BT_HCI_ERR_LOCALHOST_TERM_CONN && req_recv_state != NULL) {
			int err;

			if (big_synced) {
				err = bt_bap_broadcast_sink_stop(broadcast_sink);
				if (err != 0) {
					printk("Failed to stop Broadcast Sink: %d\n", err);

					return;
				}
			}

			err = bt_bap_scan_delegator_rem_src(req_recv_state->src_id);
			if (err != 0) {
				printk("Failed to remove source: %d\n", err);

				return;
			}
		}
	}
}

static struct bt_le_per_adv_sync_cb bap_pa_sync_cb = {
	.synced = bap_pa_sync_synced_cb,
	.term = bap_pa_sync_terminated_cb,
};


static int init(void)
{
	const struct bt_pacs_register_param pacs_param = {
		.snk_pac = true,
		.snk_loc = true,
	};
	int err;

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth enable failed (err %d)\n", err);
		return err;
	}

	printk("Bluetooth initialized\n");

	err = bt_pacs_register(&pacs_param);
	if (err) {
		printk("Could not register PACS (err %d)\n", err);
		return err;
	}

	err = bt_pacs_cap_register(BT_AUDIO_DIR_SINK, &cap);
	if (err) {
		printk("Capability register failed (err %d)\n", err);
		return err;
	}

	err = bt_bap_scan_delegator_register(&scan_delegator_cbs);
	if (err) {
		printk("Scan delegator register failed (err %d)\n", err);
		return err;
	}

	bt_bap_broadcast_sink_register_cb(&broadcast_sink_cbs);
	bt_le_per_adv_sync_cb_register(&bap_pa_sync_cb);
	bt_le_scan_cb_register(&bap_scan_cb);

	stream_rx_get_streams(bap_streams_p);

	for (size_t i = 0U; i < CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT; i++) {
		bt_bap_stream_cb_register(bap_streams_p[i], &stream_ops);
	}

	if (IS_ENABLED(CONFIG_LIBLC3)) {
		lc3_init();
	}

	if (IS_ENABLED(CONFIG_USE_USB_AUDIO_OUTPUT)) {
		usb_init();
	}

	return 0;
}

static int reset(void)
{
	int err;

	printk("Reset\n");

	req_recv_state = NULL;
	big_synced = false;
	base_received = false;
	(void)memset(&base_recv_data, 0, sizeof(base_recv_data));
	(void)memset(&requested_bis_sync, 0, sizeof(requested_bis_sync));
	(void)memset(sink_broadcast_code, 0, sizeof(sink_broadcast_code));
	(void)memset(&broadcaster_info, 0, sizeof(broadcaster_info));
	(void)memset(&broadcaster_addr, 0, sizeof(broadcaster_addr));
	broadcaster_broadcast_id = BT_BAP_INVALID_BROADCAST_ID;

	if (broadcast_sink != NULL) {
		err = bt_bap_broadcast_sink_delete(broadcast_sink);
		if (err) {
			printk("Deleting broadcast sink failed (err %d)\n", err);

			return err;
		}

		broadcast_sink = NULL;
	}

	if (pa_sync != NULL) {
		bt_le_per_adv_sync_delete(pa_sync);
		if (err) {
			printk("Deleting PA sync failed (err %d)\n", err);

			return err;
		}

		pa_sync = NULL;
	}

	k_sem_reset(&sem_broadcaster_found);
	k_sem_reset(&sem_pa_synced);
	k_sem_reset(&sem_base_received);
	k_sem_reset(&sem_syncable);
	k_sem_reset(&sem_pa_sync_lost);
	k_sem_reset(&sem_broadcast_code_received);
	k_sem_reset(&sem_bis_sync_requested);
	k_sem_reset(&sem_stream_connected);
	k_sem_reset(&sem_stream_started);
	k_sem_reset(&sem_broadcast_sink_stopped);

	return 0;
}

static int start_adv(void)
{
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
		BT_DATA_BYTES(BT_DATA_UUID16_ALL,
			      BT_UUID_16_ENCODE(BT_UUID_BASS_VAL),
			      BT_UUID_16_ENCODE(BT_UUID_PACS_VAL)),
		BT_DATA_BYTES(BT_DATA_SVC_DATA16, BT_UUID_16_ENCODE(BT_UUID_BASS_VAL)),
		BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
			sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	};
	int err;

	/* Create a connectable advertising set */
	err = bt_le_ext_adv_create(BT_BAP_ADV_PARAM_CONN_REDUCED, NULL, &ext_adv);
	if (err != 0) {
		printk("Failed to create advertising set (err %d)\n", err);

		return err;
	}

	err = bt_le_ext_adv_set_data(ext_adv, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err != 0) {
		printk("Failed to set advertising data (err %d)\n", err);

		return err;
	}

	err = bt_le_ext_adv_start(ext_adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err != 0) {
		printk("Failed to start advertising set (err %d)\n", err);

		return err;
	}

	return 0;
}

static int stop_adv(void)
{
	int err;

	err = bt_le_ext_adv_stop(ext_adv);
	if (err != 0) {
		printk("Failed to stop advertising set (err %d)\n", err);

		return err;
	}

	err = bt_le_ext_adv_delete(ext_adv);
	if (err != 0) {
		printk("Failed to delete advertising set (err %d)\n", err);

		return err;
	}

	ext_adv = NULL;

	return 0;
}

static int pa_sync_create(void)
{
	struct bt_le_per_adv_sync_param create_params = {0};

	bt_addr_le_copy(&create_params.addr, &broadcaster_addr);
	create_params.options = BT_LE_PER_ADV_SYNC_OPT_FILTER_DUPLICATE;
	create_params.sid = broadcaster_info.sid;
	create_params.skip = PA_SYNC_SKIP;
	create_params.timeout = interval_to_sync_timeout(broadcaster_info.interval);

	return bt_le_per_adv_sync_create(&create_params, &pa_sync);
}

#if !defined(CONFIG_TARGET_BROADCAST_CHANNEL)
static uint32_t keep_n_least_significant_ones(uint32_t bitfield, uint8_t n)
{
	uint32_t result = 0U;

	for (uint8_t i = 0; i < n && bitfield != 0; i++) {
		uint32_t lsb = bitfield & -bitfield; /* extract lsb */

		result |= lsb;
		bitfield &= ~lsb; /* clear the extracted bit */
	}

	return result;
}
#endif

static uint8_t get_stream_count(uint32_t bitfield)
{
	uint8_t count = 0U;

	for (uint8_t i = 0U; i < BT_ISO_MAX_GROUP_ISO_COUNT; i++) {
		if ((bitfield & BIT(i)) != 0) {
			count++;
		}
	}

	return count;
}

static uint32_t select_bis_sync_bitfield(struct base_data *base_sg_data,
					 uint32_t bis_sync_req[CONFIG_BT_BAP_BASS_MAX_SUBGROUPS])

{
	uint32_t result = 0U;

#if defined(CONFIG_TARGET_BROADCAST_CHANNEL)
	for (int i = 0; i < CONFIG_BT_BAP_BASS_MAX_SUBGROUPS; i++) {
		enum bt_audio_location combine_alloc = BT_AUDIO_LOCATION_MONO_AUDIO;
		uint32_t combine_bis_sync = 0U;

		if (bis_sync_req[i] == 0) {
			continue;
		}
		/* BIS sync requested in this subgroup. Look for allocation match.
		 * BIS index 0 is not a valid index, so start from 1.
		 */
		for (int idx = 1; idx <= BT_ISO_BIS_INDEX_MAX; idx++) {
			const struct bis_audio_allocation *bis_alloc =
				&base_sg_data->subgroup_bis[i].audio_allocation[idx];

			if (!base_sg_data->subgroup_bis[i].audio_allocation[idx].valid) {
				/* BIS not present or channel allocation not valid for this BIS */
				continue;
			}
			if ((bis_sync_req[i] & BT_ISO_BIS_INDEX_BIT(idx)) == 0) {
				/* No request to sync to this BIS */
				continue;
			}
			if (bis_alloc->value == CONFIG_TARGET_BROADCAST_CHANNEL) {
				/* Exact match */
				result = BT_ISO_BIS_INDEX_BIT(idx);
				break;
			} else if ((bis_alloc->value & CONFIG_TARGET_BROADCAST_CHANNEL) != 0) {
				combine_alloc |= bis_alloc->value;
				combine_bis_sync |= BT_ISO_BIS_INDEX_BIT(idx);
				if (combine_alloc == CONFIG_TARGET_BROADCAST_CHANNEL) {
					/* Combined match */
					result = combine_bis_sync;
					break;
				}
				/* Partial match */
				printk("Channel allocation match, partial %d\n", combine_alloc);
			} else {
				 /* No action required */
			}
		}

		if (result != 0U) {
			printk("Channel allocation match, result = 0x%08x\n", result);
			break;
		}
	}
#else  /* !CONFIG_TARGET_BROADCAST_CHANNEL */
	bool bis_sync_req_no_pref = false;

	for (uint8_t i = 0; i < CONFIG_BT_BAP_BASS_MAX_SUBGROUPS; i++) {
		if (bis_sync_req[i] != 0) {
			if (bis_sync_req[i] == BT_BAP_BIS_SYNC_NO_PREF) {
				bis_sync_req_no_pref = true;
			}
			result |=
				bis_sync_req[i] & base_sg_data->subgroup_bis[i].bis_index_bitfield;
		}
	}

	if (bis_sync_req_no_pref) {
		/** Keep the CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT least significant bits
		 * of bitfield, as that is the maximum number of BISes we can sync to
		 */
		result = keep_n_least_significant_ones(result,
						       CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT);
	}
#endif /* CONFIG_TARGET_BROADCAST_CHANNEL */

	return result;
}

int main(void)
{
	int err;

	err = init();
	if (err) {
		printk("Init failed (err %d)\n", err);
		return 0;
	}

	while (true) {
		uint8_t stream_count;
		uint32_t sync_bitfield;

		err = reset();
		if (err != 0) {
			printk("Resetting failed: %d - Aborting\n", err);

			return 0;
		}

		if (IS_ENABLED(CONFIG_SCAN_OFFLOAD)) {
			if (broadcast_assistant_conn == NULL) {
				k_sem_reset(&sem_connected);

				printk("Starting advertising\n");
				/* Stop advertising before starting if needed */
				if (ext_adv != NULL) {
					err = stop_adv();
					if (err != 0) {
						printk("Unable to stop advertising: %d\n", err);

						return 0;
					}
				}
				err = start_adv();
				if (err != 0) {
					printk("Unable to start advertising connectable: %d\n",
					       err);

					return 0;
				}

				printk("Waiting for Broadcast Assistant\n");
				err = k_sem_take(&sem_connected, ADV_TIMEOUT);
				if (err != 0) {
					printk("No Broadcast Assistant connected\n");

					err = stop_adv();
					if (err != 0) {
						printk("Unable to stop advertising: %d\n", err);

						return 0;
					}
				}
			}

			if (broadcast_assistant_conn != NULL) {
				k_sem_reset(&sem_pa_request);
				k_sem_reset(&sem_past_request);
				k_sem_reset(&sem_disconnected);

				/* Wait for the PA request to determine if we
				 * should start scanning, or wait for PAST
				 */
				printk("Waiting for PA sync request\n");
				err = k_sem_take(&sem_pa_request,
						 BROADCAST_ASSISTANT_TIMEOUT);
				if (err != 0) {
					printk("sem_pa_request timed out, resetting\n");
					continue;
				}

				if (k_sem_take(&sem_past_request, K_NO_WAIT) == 0) {
					goto wait_for_pa_sync;
				} /* else continue with scanning below */
			}
		}

		if (strlen(CONFIG_TARGET_BROADCAST_NAME) > 0U) {
			printk("Scanning for broadcast sources containing "
			       "`" CONFIG_TARGET_BROADCAST_NAME "`\n");
		} else {
			printk("Scanning for broadcast sources\n");
		}

		err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
		if (err != 0 && err != -EALREADY) {
			printk("Unable to start scan for broadcast sources: %d\n",
			       err);
			return 0;
		}

		printk("Waiting for Broadcaster\n");
		err = k_sem_take(&sem_broadcaster_found, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_broadcaster_found timed out, resetting\n");
			continue;
		}

		err = bt_le_scan_stop();
		if (err != 0) {
			printk("bt_le_scan_stop failed with %d, resetting\n", err);
			continue;
		}

		printk("Attempting to PA sync to the broadcaster with id 0x%06X\n",
		       broadcaster_broadcast_id);
		err = pa_sync_create();
		if (err != 0) {
			printk("Could not create Broadcast PA sync: %d, resetting\n", err);
			continue;
		}

wait_for_pa_sync:
		printk("Waiting for PA synced\n");
		err = k_sem_take(&sem_pa_synced, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_pa_synced timed out, resetting\n");
			continue;
		}

		printk("Broadcast source PA synced, creating Broadcast Sink\n");
		err = bt_bap_broadcast_sink_create(pa_sync, broadcaster_broadcast_id,
						   &broadcast_sink);
		if (err != 0) {
			printk("Failed to create broadcast sink: %d\n", err);
			continue;
		}

		printk("Broadcast Sink created, waiting for BASE\n");
		err = k_sem_take(&sem_base_received, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_base_received timed out, resetting\n");
			continue;
		}

		printk("BASE received, waiting for syncable\n");
		err = k_sem_take(&sem_syncable, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_syncable timed out, resetting\n");
			continue;
		}

		/* sem_broadcast_code_received is also given if the
		 * broadcast is not encrypted
		 */
		printk("Waiting for broadcast code\n");
		err = k_sem_take(&sem_broadcast_code_received, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_broadcast_code_received timed out, resetting\n");
			continue;
		}

		printk("Waiting for BIS sync request\n");
		err = k_sem_take(&sem_bis_sync_requested, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_bis_sync_requested timed out, resetting\n");
			continue;
		}

		/* Select BIS'es to sync to */
		sync_bitfield = select_bis_sync_bitfield(&base_recv_data, requested_bis_sync);
		if (sync_bitfield == 0U) {
			printk("No valid BIS sync found, resetting\n");
			continue;
		}

		stream_count = get_stream_count(sync_bitfield);

		printk("Syncing to broadcast with bitfield: 0x%08x, stream_count = %u\n",
		       sync_bitfield, stream_count);

		err = bt_bap_broadcast_sink_sync(broadcast_sink, sync_bitfield, bap_streams_p,
						 sink_broadcast_code);
		if (err != 0) {
			printk("Unable to sync to broadcast source: %d\n", err);
			return 0;
		}

		printk("Waiting for stream(s) started\n");
		err = k_sem_take(&sem_big_synced, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_big_synced timed out, resetting\n");
			continue;
		}

		printk("Waiting for PA disconnected\n");
		k_sem_take(&sem_pa_sync_lost, K_FOREVER);

		printk("Waiting for sink to stop\n");
		err = k_sem_take(&sem_broadcast_sink_stopped, SEM_TIMEOUT);
		if (err != 0) {
			printk("sem_broadcast_sink_stopped timed out, resetting\n");
			continue;
		}
	}

	return 0;
}
