// SPDX-License-Identifier: GPL-2.0-only

#include <drm/display/drm_dp_mst_helper.h>
#include <drm/display/drm_hdmi_audio_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>

#include "dp_audio.h"
#include "dp_mst_drm.h"
#include "dp_ctrl.h"

#define MSM_DP_MST_MAX_STREAMS 2

struct msm_dp_mst_bridge_state {
	struct drm_bridge_state base;
	struct drm_connector *connector;
	int pbn;
};

struct msm_dp_mst_bridge {
	struct drm_bridge bridge;
	struct msm_dp *dp;
	enum msm_dp_stream_id stream_id;

	/* Runtime state spans the four atomic bridge callbacks. */
	bool pm_ref_held;
	bool payload_part1;
	bool payload_ready;
	bool stream_started;
	bool stream_pre_disabled;
	struct drm_connector *audio_connector;
};

struct msm_dp_mst_connector {
	struct drm_connector connector;
	struct msm_dp_mst *mst;
	struct drm_dp_mst_port *port;
};

struct msm_dp_mst {
	struct msm_dp *dp;
	struct drm_dp_mst_topology_mgr mgr;
	struct msm_dp_mst_bridge *bridges[MSM_DP_MST_MAX_STREAMS];
	int selected_audio_stream;
	bool initialized;
	bool prepared;
	bool active;
	bool disconnecting;
	bool suspended;
	bool resume_failed;
};

#define to_msm_dp_mst_bridge(x) container_of(x, struct msm_dp_mst_bridge, bridge)
#define to_msm_dp_mst_bridge_state(x) \
	container_of(x, struct msm_dp_mst_bridge_state, base)
#define to_msm_dp_mst_connector(x) \
	container_of(x, struct msm_dp_mst_connector, connector)

static void msm_dp_mst_audio_copy_eld(struct msm_dp_mst *mst,
				      struct drm_connector *connector)
{
	struct drm_connector *root = mst->dp->connector;
	u8 eld[MAX_ELD_BYTES] = {};

	if (connector) {
		mutex_lock(&connector->eld_mutex);
		memcpy(eld, connector->eld, sizeof(eld));
		mutex_unlock(&connector->eld_mutex);
	}

	mutex_lock(&root->eld_mutex);
	memcpy(root->eld, eld, sizeof(eld));
	mutex_unlock(&root->eld_mutex);
}

static int msm_dp_mst_find_audio_stream(struct msm_dp_mst *mst)
{
	int i;

	for (i = 0; i < MSM_DP_MST_MAX_STREAMS; i++)
		if (mst->bridges[i] && mst->bridges[i]->audio_connector)
			return i;

	return -1;
}

static void msm_dp_mst_set_audio_stream(struct msm_dp_mst *mst, int stream_id,
					bool stop_engine)
{
	struct drm_connector *root = mst->dp->connector;
	struct msm_dp_mst_bridge *bridge;

	if (mst->selected_audio_stream == stream_id)
		return;

	if (mst->selected_audio_stream >= 0) {
		mst->selected_audio_stream = -1;
		drm_connector_hdmi_audio_plugged_notify(root, false);
		if (stop_engine)
			msm_dp_audio_mst_stop_locked(mst->dp);
		else
			mst->dp->audio_enabled = false;
	}

	if (stream_id < 0) {
		msm_dp_mst_audio_copy_eld(mst, NULL);
		return;
	}

	bridge = mst->bridges[stream_id];
	if (WARN_ON(!bridge || !bridge->audio_connector))
		return;

	msm_dp_mst_audio_copy_eld(mst, bridge->audio_connector);
	mst->selected_audio_stream = stream_id;
	drm_connector_hdmi_audio_plugged_notify(root, true);
}

static void msm_dp_mst_audio_enable(struct msm_dp_mst_bridge *bridge,
				    struct drm_connector *connector)
{
	struct msm_dp_mst *mst = bridge->dp->mst;

	if (!connector->display_info.has_audio)
		return;

	mutex_lock(&bridge->dp->audio_lock);
	if (WARN_ON(bridge->audio_connector))
		goto unlock;

	drm_connector_get(connector);
	bridge->audio_connector = connector;
	if (mst->selected_audio_stream < 0)
		msm_dp_mst_set_audio_stream(mst, bridge->stream_id, false);

unlock:
	mutex_unlock(&bridge->dp->audio_lock);
}

static void msm_dp_mst_audio_disable(struct msm_dp_mst_bridge *bridge)
{
	struct msm_dp_mst *mst = bridge->dp->mst;
	bool selected;

	mutex_lock(&bridge->dp->audio_lock);
	if (!bridge->audio_connector)
		goto unlock;

	selected = mst->selected_audio_stream == bridge->stream_id;
	if (selected)
		msm_dp_mst_set_audio_stream(mst, -1, true);

	drm_connector_put(bridge->audio_connector);
	bridge->audio_connector = NULL;
	if (selected)
		msm_dp_mst_set_audio_stream(mst,
					    msm_dp_mst_find_audio_stream(mst), false);

unlock:
	mutex_unlock(&bridge->dp->audio_lock);
}

static void msm_dp_mst_audio_clear(struct msm_dp_mst *mst, bool stop_engine)
{
	int i;

	mutex_lock(&mst->dp->audio_lock);
	msm_dp_mst_set_audio_stream(mst, -1, stop_engine);
	for (i = 0; i < MSM_DP_MST_MAX_STREAMS; i++) {
		if (!mst->bridges[i] || !mst->bridges[i]->audio_connector)
			continue;

		drm_connector_put(mst->bridges[i]->audio_connector);
		mst->bridges[i]->audio_connector = NULL;
	}
	mutex_unlock(&mst->dp->audio_lock);
}

static u32 msm_dp_mst_connector_bpp(const struct drm_connector *connector)
{
	u32 bpp = connector->display_info.bpc * 3;

	if (!bpp)
		return 24;

	/* The SM8550 DP source and the inherited panel code support up to 10 bpc. */
	return min(bpp, 30U);
}

static struct drm_bridge_state *
msm_dp_mst_bridge_atomic_duplicate_state(struct drm_bridge *bridge)
{
	struct msm_dp_mst_bridge_state *state;

	state = kmemdup(bridge->base.state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_bridge_duplicate_state(bridge, &state->base);
	if (state->connector)
		drm_connector_get(state->connector);

	return &state->base;
}

static void msm_dp_mst_bridge_atomic_destroy_state(struct drm_bridge *bridge,
						   struct drm_bridge_state *state)
{
	struct msm_dp_mst_bridge_state *mst_state =
		to_msm_dp_mst_bridge_state(state);

	if (mst_state->connector)
		drm_connector_put(mst_state->connector);
	kfree(mst_state);
}

static struct drm_bridge_state *
msm_dp_mst_bridge_atomic_reset(struct drm_bridge *bridge)
{
	struct msm_dp_mst_bridge_state *state;

	state = kzalloc_obj(*state);
	if (!state)
		return NULL;

	__drm_atomic_helper_bridge_reset(bridge, &state->base);

	return &state->base;
}

static void
msm_dp_mst_bridge_set_connector(struct msm_dp_mst_bridge_state *state,
				struct drm_connector *connector)
{
	if (state->connector == connector)
		return;

	if (connector)
		drm_connector_get(connector);
	if (state->connector)
		drm_connector_put(state->connector);

	state->connector = connector;
}

static void msm_dp_mst_bridge_put_pm(struct msm_dp_mst_bridge *bridge)
{
	if (!bridge->pm_ref_held)
		return;

	msm_dp_display_mst_stream_put(bridge->dp, bridge->stream_id);
	bridge->pm_ref_held = false;
}

static bool msm_dp_mst_root_in_use(struct msm_dp *dp,
				   struct drm_atomic_state *state)
{
	const struct drm_connector_state *root_state;

	if (!dp->connector)
		return false;

	root_state = drm_atomic_get_new_connector_state(state, dp->connector);
	if (!root_state)
		root_state = dp->connector->state;

	return root_state->crtc;
}

static int msm_dp_mst_bridge_atomic_check(struct drm_bridge *bridge,
					  struct drm_bridge_state *bridge_state,
					  struct drm_crtc_state *crtc_state,
					  struct drm_connector_state *conn_state)
{
	struct drm_connector_state *old_conn_state;
	struct msm_dp_mst_bridge *mst_bridge = to_msm_dp_mst_bridge(bridge);
	struct msm_dp_mst_bridge_state *mst_state =
		to_msm_dp_mst_bridge_state(bridge_state);
	struct msm_dp_mst_connector *mst_conn =
		to_msm_dp_mst_connector(conn_state->connector);
	struct drm_dp_mst_topology_state *topology_state;
	u32 link_rate, lane_count;
	int bpp, slots;

	old_conn_state = drm_atomic_get_old_connector_state(conn_state->state,
							    conn_state->connector);
	if (old_conn_state && crtc_state && conn_state->crtc &&
	    (!drm_connector_atomic_hdr_metadata_equal(old_conn_state, conn_state) ||
	     old_conn_state->colorspace != conn_state->colorspace))
		crtc_state->mode_changed = true;

	/* Plane-only updates do not change the VC payload. */
	if (!conn_state->crtc || !crtc_state->active ||
	    !drm_atomic_crtc_needs_modeset(crtc_state))
		return 0;

	/* A removed topology may still be present while its old stream tears down. */
	if (!mst_conn->mst->active &&
	    (!mst_conn->mst->disconnecting || conn_state->crtc))
		return -ENOTCONN;

	if (WARN_ON(mst_bridge->stream_id >= MSM_DP_MST_MAX_STREAMS))
		return -EINVAL;

	/* SST and MST cannot use the shared source and stream-0 interface together. */
	if (msm_dp_mst_root_in_use(mst_bridge->dp, crtc_state->state))
		return -EBUSY;

	bpp = msm_dp_mst_connector_bpp(conn_state->connector);

	mst_state->pbn = drm_dp_calc_pbn_mode(crtc_state->adjusted_mode.clock,
					      bpp << 4);
	topology_state = drm_atomic_get_mst_topology_state(crtc_state->state,
							   &mst_conn->mst->mgr);
	if (IS_ERR(topology_state))
		return PTR_ERR(topology_state);

	if (!topology_state->pbn_div.full) {
		link_rate = msm_dp_display_get_link_rate(mst_conn->mst->dp);
		lane_count = msm_dp_display_get_lane_count(mst_conn->mst->dp);
		topology_state->pbn_div = drm_dp_get_vc_payload_bw(link_rate,
								   lane_count);
	}

	slots = drm_dp_atomic_find_time_slots(crtc_state->state,
					      &mst_conn->mst->mgr,
					      mst_conn->port, mst_state->pbn);
	if (slots < 0)
		return slots;

	return 0;
}

static void msm_dp_mst_update_channels(struct msm_dp_mst *mst,
				       struct drm_atomic_state *atomic_state,
				       struct drm_dp_mst_topology_state *topology_state,
				       const struct drm_dp_mst_atomic_payload *removed_payload)
{
	int i;

	for (i = 0; i < MSM_DP_MST_MAX_STREAMS; i++) {
		struct msm_dp_mst_bridge *bridge = mst->bridges[i];
		struct msm_dp_mst_bridge_state *bridge_state;
		struct msm_dp_mst_connector *connector;
		struct drm_dp_mst_atomic_payload *payload;
		struct drm_bridge_state *bridge_atomic_state;

		if (!bridge) {
			msm_dp_display_mst_set_channel(mst->dp, i, 0, 0);
			continue;
		}

		bridge_atomic_state =
			drm_atomic_get_new_bridge_state(atomic_state, &bridge->bridge);
		if (!bridge_atomic_state) {
			WARN_ON_ONCE(1);
			msm_dp_display_mst_set_channel(mst->dp, i, 0, 0);
			continue;
		}
		bridge_state = to_msm_dp_mst_bridge_state(bridge_atomic_state);
		if (!bridge_state->connector) {
			msm_dp_display_mst_set_channel(mst->dp, i, 0, 0);
			continue;
		}

		connector = to_msm_dp_mst_connector(bridge_state->connector);
		payload = drm_atomic_get_mst_payload_state(topology_state,
							   connector->port);
		if (!payload || payload->delete || payload->vc_start_slot < 0 ||
		    payload->payload_allocation_status <
				DRM_DP_MST_PAYLOAD_ALLOCATION_DFP ||
		    (removed_payload && payload->port == removed_payload->port)) {
			msm_dp_display_mst_set_channel(mst->dp, i, 0, 0);
		} else {
			s8 start_slot = payload->vc_start_slot;

			if (removed_payload &&
			    start_slot > removed_payload->vc_start_slot)
				start_slot -= removed_payload->time_slots;
			msm_dp_display_mst_set_channel(mst->dp, i,
						       start_slot,
						       payload->time_slots);
		}
	}
}

static void msm_dp_mst_bridge_atomic_pre_enable(struct drm_bridge *drm_bridge,
						struct drm_atomic_state *state)
{
	struct msm_dp_mst_bridge *bridge = to_msm_dp_mst_bridge(drm_bridge);
	struct msm_dp_mst *mst = bridge->dp->mst;
	struct msm_dp_mst_bridge_state *bridge_state;
	struct msm_dp_mst_connector *connector;
	struct drm_dp_mst_topology_state *topology_state;
	struct drm_dp_mst_atomic_payload *payload;
	struct drm_bridge_state *bridge_atomic_state;
	int ret;

	bridge_atomic_state = drm_atomic_get_new_bridge_state(state, drm_bridge);
	if (WARN_ON(!bridge_atomic_state))
		return;
	bridge_state = to_msm_dp_mst_bridge_state(bridge_atomic_state);
	if (WARN_ON(!bridge_state->connector))
		return;
	connector = to_msm_dp_mst_connector(bridge_state->connector);
	topology_state = drm_atomic_get_new_mst_topology_state(state, &mst->mgr);
	if (IS_ERR(topology_state) || !topology_state) {
		drm_err(mst->mgr.dev, "missing MST topology state in pre-enable: %ld\n",
			IS_ERR(topology_state) ? PTR_ERR(topology_state) : -ENOENT);
		return;
	}
	payload = drm_atomic_get_mst_payload_state(topology_state, connector->port);
	if (WARN_ON(!payload))
		return;

	if (!bridge->pm_ref_held) {
		ret = msm_dp_display_mst_stream_get(bridge->dp, bridge->stream_id);
		if (ret) {
			drm_err(mst->mgr.dev, "failed to hold MST stream PM ref: %d\n",
				ret);
			return;
		}
		bridge->pm_ref_held = true;
	}

	ret = drm_dp_add_payload_part1(&mst->mgr, topology_state, payload);
	/* Even a failed part 1 owns local slot accounting until disable. */
	bridge->payload_part1 = true;
	bridge->payload_ready = !ret;
	bridge->stream_pre_disabled = false;
	if (ret) {
		drm_err(mst->mgr.dev, "failed to add MST payload part 1: %d\n", ret);
		return;
	}

	msm_dp_mst_update_channels(mst, state, topology_state, NULL);
}

static void msm_dp_mst_bridge_atomic_enable(struct drm_bridge *drm_bridge,
					    struct drm_atomic_state *state)
{
	struct msm_dp_mst_bridge *bridge = to_msm_dp_mst_bridge(drm_bridge);
	struct msm_dp_mst *mst = bridge->dp->mst;
	struct msm_dp_mst_bridge_state *bridge_state;
	struct msm_dp_mst_connector *connector;
	struct drm_dp_mst_topology_state *topology_state;
	struct drm_dp_mst_atomic_payload *payload;
	struct drm_bridge_state *bridge_atomic_state;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	u32 bpp;
	int ret;

	if (!bridge->pm_ref_held || !bridge->payload_ready)
		return;
	if (!mst->active) {
		drm_dbg_dp(mst->mgr.dev,
			   "MST topology disappeared before stream enable\n");
		return;
	}

	bridge_atomic_state = drm_atomic_get_new_bridge_state(state, drm_bridge);
	if (WARN_ON(!bridge_atomic_state))
		return;
	bridge_state = to_msm_dp_mst_bridge_state(bridge_atomic_state);
	if (WARN_ON(!bridge_state->connector))
		return;
	connector = to_msm_dp_mst_connector(bridge_state->connector);
	topology_state = drm_atomic_get_new_mst_topology_state(state, &mst->mgr);
	if (IS_ERR(topology_state) || !topology_state) {
		drm_err(mst->mgr.dev, "missing MST topology state in enable: %ld\n",
			IS_ERR(topology_state) ? PTR_ERR(topology_state) : -ENOENT);
		return;
	}
	payload = drm_atomic_get_mst_payload_state(topology_state, connector->port);
	if (WARN_ON(!payload))
		return;
	crtc = drm_atomic_get_new_crtc_for_encoder(state, drm_bridge->encoder);
	crtc_state = crtc ? drm_atomic_get_new_crtc_state(state, crtc) : NULL;
	if (WARN_ON(!crtc_state))
		return;
	conn_state = drm_atomic_get_new_connector_state(state,
							&connector->connector);
	if (WARN_ON(!conn_state))
		return;
	bpp = msm_dp_mst_connector_bpp(&connector->connector);

	ret = msm_dp_display_mst_stream_enable(bridge->dp, bridge->stream_id,
					       &crtc_state->adjusted_mode, bpp,
					       conn_state->colorspace,
					       bridge_state->pbn);
	if (ret) {
		drm_err(mst->mgr.dev, "failed to enable MST stream %u: %d\n",
			bridge->stream_id, ret);
		return;
	}

	bridge->stream_started = true;

	/* Sink ACT and remote allocation are intentionally best effort here. */
	if (mst->active) {
		ret = drm_dp_check_act_status(&mst->mgr);
		if (ret)
			drm_dbg_dp(mst->mgr.dev, "MST sink ACT check returned %d\n",
				   ret);
	} else {
		/*
		 * A physical unplug can race the commit; leave part 2 for the
		 * normal disable path rather than issuing sideband traffic now.
		 */
		return;
	}
	ret = drm_dp_add_payload_part2(&mst->mgr, payload);
	if (ret) {
		drm_err(mst->mgr.dev, "failed to add MST payload part 2: %d\n", ret);
		return;
	}

	msm_dp_display_mst_stream_config_spd(bridge->dp, bridge->stream_id);
	ret = msm_dp_display_mst_stream_config_hdr(bridge->dp,
						   bridge->stream_id, conn_state);
	if (ret)
		drm_dbg_dp(mst->mgr.dev, "failed to configure MST HDR: %d\n", ret);

	msm_dp_mst_audio_enable(bridge, &connector->connector);
}

static void msm_dp_mst_bridge_atomic_disable(struct drm_bridge *drm_bridge,
					     struct drm_atomic_state *state)
{
	struct msm_dp_mst_bridge *bridge = to_msm_dp_mst_bridge(drm_bridge);
	struct msm_dp_mst *mst = bridge->dp->mst;
	struct msm_dp_mst_bridge_state *old_bridge_state;
	struct msm_dp_mst_connector *connector;
	struct drm_dp_mst_topology_state *old_topology_state;
	struct drm_dp_mst_topology_state *new_topology_state;
	struct drm_dp_mst_atomic_payload *old_payload, *new_payload;
	struct drm_dp_mst_atomic_payload old_payload_copy;
	struct drm_bridge_state *old_drm_bridge_state;
	bool payload_removed = false;
	int ret;

	if (!bridge->payload_part1 && !bridge->stream_started)
		return;

	old_drm_bridge_state = drm_atomic_get_old_bridge_state(state, drm_bridge);
	if (WARN_ON(!old_drm_bridge_state))
		goto stop_stream;
	old_bridge_state = to_msm_dp_mst_bridge_state(old_drm_bridge_state);
	if (WARN_ON(!old_bridge_state->connector))
		goto stop_stream;
	connector = to_msm_dp_mst_connector(old_bridge_state->connector);

	old_topology_state =
		drm_atomic_get_old_mst_topology_state(state, &mst->mgr);
	new_topology_state =
		drm_atomic_get_new_mst_topology_state(state, &mst->mgr);
	if (IS_ERR(old_topology_state) || IS_ERR(new_topology_state) ||
	    !old_topology_state || !new_topology_state) {
		drm_err(mst->mgr.dev, "missing MST topology state in disable\n");
		goto stop_stream;
	}
	old_payload = drm_atomic_get_mst_payload_state(old_topology_state,
						       connector->port);
	new_payload = drm_atomic_get_mst_payload_state(new_topology_state,
						       connector->port);
	if (!old_payload || !new_payload) {
		drm_err(mst->mgr.dev, "missing MST payload state in disable\n");
		goto stop_stream;
	}

	old_payload_copy = *old_payload;
	if (bridge->payload_part1) {
		drm_dp_remove_payload_part1(&mst->mgr, new_topology_state,
					    new_payload);
		bridge->payload_part1 = false;
		bridge->payload_ready = false;
		payload_removed = true;
	}

	msm_dp_mst_update_channels(mst, state, new_topology_state,
				   &old_payload_copy);

stop_stream:
	/*
	 * The atomic helpers normally guarantee the payload state above. If a
	 * damaged state reaches commit, still stop the source stream and release
	 * its PM reference in post-disable rather than leaving hardware running.
	 */
	msm_dp_mst_audio_disable(bridge);
	msm_dp_display_mst_stream_config_hdr(bridge->dp, bridge->stream_id, NULL);
	if (!payload_removed)
		msm_dp_display_mst_set_channel(bridge->dp, bridge->stream_id, 0, 0);
	if (bridge->stream_started && !bridge->stream_pre_disabled) {
		msm_dp_display_mst_stream_pre_disable(bridge->dp,
						      bridge->stream_id);
		bridge->stream_pre_disabled = true;
	} else if (bridge->payload_part1 || payload_removed) {
		msm_dp_display_mst_update_payload(bridge->dp);
	}

	if (payload_removed && mst->active) {
		ret = drm_dp_check_act_status(&mst->mgr);
		if (ret)
			drm_dbg_dp(mst->mgr.dev, "MST sink ACT check returned %d\n",
				   ret);
	}
	if (payload_removed)
		drm_dp_remove_payload_part2(&mst->mgr, new_topology_state,
					    &old_payload_copy, new_payload);
}

static void msm_dp_mst_bridge_atomic_post_disable(struct drm_bridge *drm_bridge,
						  struct drm_atomic_state *state)
{
	struct msm_dp_mst_bridge *bridge = to_msm_dp_mst_bridge(drm_bridge);

	if (bridge->stream_started) {
		msm_dp_display_mst_stream_disable(bridge->dp, bridge->stream_id);
		bridge->stream_started = false;
	}

	bridge->stream_pre_disabled = false;
	bridge->payload_part1 = false;
	bridge->payload_ready = false;
	msm_dp_mst_bridge_put_pm(bridge);
}

static const struct drm_bridge_funcs msm_dp_mst_bridge_funcs = {
	.atomic_duplicate_state = msm_dp_mst_bridge_atomic_duplicate_state,
	.atomic_destroy_state = msm_dp_mst_bridge_atomic_destroy_state,
	.atomic_reset = msm_dp_mst_bridge_atomic_reset,
	.atomic_check = msm_dp_mst_bridge_atomic_check,
	.atomic_pre_enable = msm_dp_mst_bridge_atomic_pre_enable,
	.atomic_enable = msm_dp_mst_bridge_atomic_enable,
	.atomic_disable = msm_dp_mst_bridge_atomic_disable,
	.atomic_post_disable = msm_dp_mst_bridge_atomic_post_disable,
};

static int msm_dp_mst_connector_get_modes(struct drm_connector *connector)
{
	struct msm_dp_mst_connector *mst_conn = to_msm_dp_mst_connector(connector);
	const struct drm_edid *drm_edid;
	int ret;

	drm_edid = drm_dp_mst_edid_read(connector, &mst_conn->mst->mgr,
					mst_conn->port);
	drm_edid_connector_update(connector, drm_edid);
	ret = drm_edid_connector_add_modes(connector);
	drm_edid_free(drm_edid);

	return ret;
}

static int msm_dp_mst_connector_detect(struct drm_connector *connector,
				       struct drm_modeset_acquire_ctx *ctx,
				       bool force)
{
	struct msm_dp_mst_connector *mst_conn = to_msm_dp_mst_connector(connector);

	if (!mst_conn->mst->active || drm_connector_is_unregistered(connector))
		return connector_status_disconnected;

	return drm_dp_mst_detect_port(connector, ctx, &mst_conn->mst->mgr,
				      mst_conn->port);
}

static enum drm_mode_status
msm_dp_mst_connector_mode_valid(struct drm_connector *connector,
				const struct drm_display_mode *mode)
{
	struct msm_dp_mst_connector *mst_conn = to_msm_dp_mst_connector(connector);
	int mode_pclk_khz = mode->clock;

	if (!mode_pclk_khz)
		return MODE_CLOCK_LOW;
	if (drm_mode_is_420_only(&connector->display_info, mode))
		return MODE_NO_420;

	if (msm_dp_wide_bus_available(mst_conn->mst->dp))
		mode_pclk_khz /= 2;

	if (mode_pclk_khz > DP_MAX_PIXEL_CLK_KHZ)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static struct drm_encoder *
msm_dp_mst_connector_atomic_best_encoder(struct drm_connector *connector,
					 struct drm_atomic_state *state)
{
	struct msm_dp_mst_connector *mst_conn = to_msm_dp_mst_connector(connector);
	int i;

	for (i = 0; i < MSM_DP_MST_MAX_STREAMS; i++) {
		struct msm_dp_mst_bridge *mst_bridge = mst_conn->mst->bridges[i];
		struct drm_bridge *bridge;
		struct msm_dp_mst_bridge_state *bridge_state;
		struct drm_bridge_state *bridge_atomic_state;

		if (!mst_bridge)
			continue;
		bridge = &mst_bridge->bridge;
		bridge_atomic_state = drm_atomic_get_bridge_state(state, bridge);
		if (IS_ERR(bridge_atomic_state))
			return NULL;
		bridge_state = to_msm_dp_mst_bridge_state(bridge_atomic_state);
		if (bridge_state->connector == connector)
			return bridge->encoder;
	}

	for (i = 0; i < MSM_DP_MST_MAX_STREAMS; i++) {
		struct msm_dp_mst_bridge *mst_bridge = mst_conn->mst->bridges[i];
		struct drm_bridge *bridge;
		struct msm_dp_mst_bridge_state *bridge_state;
		struct drm_bridge_state *bridge_atomic_state;
		struct drm_connector_state *reserved_state;
		struct drm_connector *conn;

		if (!mst_bridge)
			continue;
		bridge = &mst_bridge->bridge;
		bridge_atomic_state = drm_atomic_get_bridge_state(state, bridge);
		if (IS_ERR(bridge_atomic_state))
			return NULL;
		bridge_state = to_msm_dp_mst_bridge_state(bridge_atomic_state);
		if (bridge_state->connector) {
			conn = bridge_state->connector;
			reserved_state = drm_atomic_get_new_connector_state(state, conn);
			if (reserved_state && !reserved_state->crtc)
				msm_dp_mst_bridge_set_connector(bridge_state, NULL);
		}
		if (!bridge_state->connector) {
			msm_dp_mst_bridge_set_connector(bridge_state, connector);
			return bridge->encoder;
		}
	}

	return NULL;
}

static int msm_dp_mst_connector_atomic_check(struct drm_connector *connector,
					     struct drm_atomic_state *state)
{
	struct msm_dp_mst_connector *mst_conn = to_msm_dp_mst_connector(connector);
	struct drm_connector_state *old_conn_state;
	struct drm_connector_state *new_conn_state;
	struct msm_dp_mst_bridge_state *mst_bridge_state;
	struct drm_bridge_state *bridge_state;
	struct drm_bridge *first_bridge;
	struct drm_bridge *bridge;
	struct drm_encoder *old_encoder;
	int i, ret;

	new_conn_state = drm_atomic_get_new_connector_state(state, connector);
	old_conn_state = drm_atomic_get_old_connector_state(state, connector);
	for (i = 0; i < MSM_DP_MST_MAX_STREAMS; i++) {
		if (!mst_conn->mst->bridges[i])
			continue;
		bridge = &mst_conn->mst->bridges[i]->bridge;
		bridge_state = drm_atomic_get_bridge_state(state, bridge);
		if (IS_ERR(bridge_state))
			return PTR_ERR(bridge_state);
	}

	/* The Linux 7.1 MST contract requires this call for every atomic check. */
	ret = drm_dp_atomic_release_time_slots(state, &mst_conn->mst->mgr,
					       mst_conn->port);
	if (ret)
		return ret;

	if (old_conn_state->crtc && !new_conn_state->crtc &&
	    old_conn_state->best_encoder) {
		old_encoder = old_conn_state->best_encoder;
		first_bridge = drm_bridge_chain_get_first_bridge(old_encoder);
		if (!first_bridge)
			return -EINVAL;
		bridge_state = drm_atomic_get_bridge_state(state, first_bridge);
		drm_bridge_put(first_bridge);
		if (!IS_ERR(bridge_state)) {
			mst_bridge_state = to_msm_dp_mst_bridge_state(bridge_state);
			if (mst_bridge_state->connector == connector)
				msm_dp_mst_bridge_set_connector(mst_bridge_state, NULL);
		}
	}

	return 0;
}

static const struct drm_connector_helper_funcs msm_dp_mst_connector_helper_funcs = {
	.get_modes = msm_dp_mst_connector_get_modes,
	.mode_valid = msm_dp_mst_connector_mode_valid,
	.atomic_best_encoder = msm_dp_mst_connector_atomic_best_encoder,
	.atomic_check = msm_dp_mst_connector_atomic_check,
	.detect_ctx = msm_dp_mst_connector_detect,
};

static int msm_dp_mst_connector_late_register(struct drm_connector *connector)
{
	struct msm_dp_mst_connector *mst_conn = to_msm_dp_mst_connector(connector);

	return drm_dp_mst_connector_late_register(connector, mst_conn->port);
}

static void msm_dp_mst_connector_early_unregister(struct drm_connector *connector)
{
	struct msm_dp_mst_connector *mst_conn = to_msm_dp_mst_connector(connector);

	drm_dp_mst_connector_early_unregister(connector, mst_conn->port);
}

static void msm_dp_mst_connector_destroy(struct drm_connector *connector)
{
	struct msm_dp_mst_connector *mst_conn = to_msm_dp_mst_connector(connector);

	drm_connector_cleanup(connector);
	drm_dp_mst_put_port_malloc(mst_conn->port);
	kfree(mst_conn);
}

static const struct drm_connector_funcs msm_dp_mst_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = msm_dp_mst_connector_destroy,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.late_register = msm_dp_mst_connector_late_register,
	.early_unregister = msm_dp_mst_connector_early_unregister,
};

static struct drm_connector *
msm_dp_mst_add_connector(struct drm_dp_mst_topology_mgr *mgr,
			 struct drm_dp_mst_port *port, const char *path)
{
	struct msm_dp_mst *mst = container_of(mgr, struct msm_dp_mst, mgr);
	struct msm_dp_mst_connector *mst_conn;
	int i, ret;

	mst_conn = kzalloc_obj(*mst_conn);
	if (!mst_conn)
		return NULL;

	mst_conn->mst = mst;
	mst_conn->port = port;
	ret = drm_connector_dynamic_init(mgr->dev, &mst_conn->connector,
					 &msm_dp_mst_connector_funcs,
					 DRM_MODE_CONNECTOR_DisplayPort, NULL);
	if (ret)
		goto err_free;

	drm_connector_helper_add(&mst_conn->connector,
				 &msm_dp_mst_connector_helper_funcs);
	for (i = 0; i < MSM_DP_MST_MAX_STREAMS; i++) {
		if (!mst->bridges[i]) {
			ret = -ENODEV;
			goto err_cleanup;
		}
		ret = drm_connector_attach_encoder(&mst_conn->connector,
						   mst->bridges[i]->bridge.encoder);
		if (ret)
			goto err_cleanup;
	}

	drm_object_attach_property(&mst_conn->connector.base,
				   mgr->dev->mode_config.path_property, 0);
	drm_object_attach_property(&mst_conn->connector.base,
				   mgr->dev->mode_config.tile_property, 0);
	drm_connector_set_path_property(&mst_conn->connector, path);
	msm_dp_mst_connector_funcs.reset(&mst_conn->connector);
	msm_dp_drm_attach_colorspace_property(&mst_conn->connector,
					      mst->dp->connector);
	drm_connector_attach_hdr_output_metadata_property(&mst_conn->connector);
	drm_dp_mst_get_port_malloc(port);

	return &mst_conn->connector;

err_cleanup:
	drm_connector_cleanup(&mst_conn->connector);
err_free:
	kfree(mst_conn);
	return NULL;
}

static const struct drm_dp_mst_topology_cbs msm_dp_mst_topology_cbs = {
	.add_connector = msm_dp_mst_add_connector,
};

int msm_dp_mst_init(struct msm_dp *dp)
{
	struct msm_dp_mst *mst;
	int ret;

	if (!msm_dp_display_mst_supported(dp))
		return 0;

	mst = devm_kzalloc(&dp->pdev->dev, sizeof(*mst), GFP_KERNEL);
	if (!mst)
		return -ENOMEM;

	mst->dp = dp;
	mst->mgr.cbs = &msm_dp_mst_topology_cbs;
	mst->selected_audio_stream = -1;
	ret = drm_dp_mst_topology_mgr_init(&mst->mgr, dp->drm_dev,
					   msm_dp_display_get_aux(dp), 16,
					   MSM_DP_MST_MAX_STREAMS,
					   dp->connector->base.id);
	if (ret)
		return ret;

	mst->initialized = true;
	dp->mst = mst;

	return 0;
}

void msm_dp_mst_destroy(struct msm_dp *dp)
{
	if (!dp->mst || !dp->mst->initialized)
		return;

	msm_dp_mst_audio_clear(dp->mst, false);
	if (dp->mst->mgr.mst_state)
		drm_dp_mst_topology_mgr_set_mst(&dp->mst->mgr, false);
	else if (dp->mst->prepared)
		drm_dp_dpcd_writeb(msm_dp_display_get_aux(dp), DP_MSTM_CTRL, 0);
	msm_dp_ctrl_set_mst(msm_dp_display_get_ctrl(dp), false);
	dp->mst->active = false;
	dp->mst->prepared = false;
	dp->mst->disconnecting = false;
	dp->mst->suspended = false;
	dp->mst->resume_failed = false;
	drm_dp_mst_topology_mgr_destroy(&dp->mst->mgr);
	dp->mst->initialized = false;
}

int msm_dp_mst_bridge_init(struct msm_dp *dp, struct drm_encoder *encoder,
			   enum msm_dp_stream_id stream_id)
{
	struct msm_dp_mst_bridge *mst_bridge;
	struct drm_bridge *bridge;
	int ret;

	if (!dp->mst || stream_id >= MSM_DP_MST_MAX_STREAMS)
		return -EINVAL;

	mst_bridge = devm_drm_bridge_alloc(&dp->pdev->dev,
					   struct msm_dp_mst_bridge, bridge,
					   &msm_dp_mst_bridge_funcs);
	if (IS_ERR(mst_bridge))
		return PTR_ERR(mst_bridge);

	mst_bridge->dp = dp;
	mst_bridge->stream_id = stream_id;
	bridge = &mst_bridge->bridge;
	bridge->type = DRM_MODE_CONNECTOR_DisplayPort;

	ret = devm_drm_bridge_add(&dp->pdev->dev, bridge);
	if (ret)
		return ret;

	ret = drm_bridge_attach(encoder, bridge, NULL,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		return ret;

	dp->mst->bridges[stream_id] = mst_bridge;

	return 0;
}

bool msm_dp_mst_active(struct msm_dp *dp)
{
	return dp->mst && (dp->mst->active || dp->mst->disconnecting);
}

bool msm_dp_mst_audio_stream(struct msm_dp *dp,
			     enum msm_dp_stream_id *stream_id)
{
	struct msm_dp_mst *mst = dp->mst;
	int selected;

	lockdep_assert_held(&dp->audio_lock);

	if (!mst || !mst->initialized || !mst->active || mst->suspended)
		return false;

	selected = mst->selected_audio_stream;
	if (selected < 0 || selected >= MSM_DP_MST_MAX_STREAMS)
		return false;

	*stream_id = selected;
	return true;
}

void msm_dp_mst_audio_disconnect(struct msm_dp *dp)
{
	if (dp->mst && dp->mst->initialized)
		msm_dp_mst_audio_clear(dp->mst, true);
}

void msm_dp_mst_audio_link_maintenance(struct msm_dp *dp, bool enable)
{
	struct msm_dp_mst *mst = dp->mst;

	if (!mst || !mst->initialized)
		return;

	mutex_lock(&dp->audio_lock);
	if (enable) {
		if (mst->active && !mst->suspended &&
		    mst->selected_audio_stream < 0)
			msm_dp_mst_set_audio_stream(mst,
						    msm_dp_mst_find_audio_stream(mst),
						    false);
	} else {
		msm_dp_mst_set_audio_stream(mst, -1, true);
	}
	mutex_unlock(&dp->audio_lock);
}

bool msm_dp_mst_disconnecting(struct msm_dp *dp)
{
	return dp->mst && dp->mst->disconnecting;
}

bool msm_dp_mst_suspend(struct msm_dp *dp)
{
	struct msm_dp_mst *mst = dp->mst;

	if (!mst || !mst->initialized || !mst->active || mst->disconnecting ||
	    mst->suspended)
		return false;

	drm_dp_mst_topology_mgr_suspend(&mst->mgr);
	mst->suspended = true;

	return true;
}

bool msm_dp_mst_suspended(struct msm_dp *dp)
{
	return dp->mst && dp->mst->suspended;
}

int msm_dp_mst_resume(struct msm_dp *dp)
{
	struct msm_dp_mst *mst = dp->mst;
	int ret;

	if (!mst || !mst->initialized || !mst->suspended)
		return 0;

	ret = drm_dp_mst_topology_mgr_resume(&mst->mgr, true);
	mst->suspended = false;
	if (!ret)
		return 0;

	msm_dp_mst_audio_clear(mst, true);
	if (mst->mgr.mst_state)
		drm_dp_mst_topology_mgr_set_mst(&mst->mgr, false);
	msm_dp_ctrl_set_mst(msm_dp_display_get_ctrl(dp), false);
	mst->active = false;
	mst->prepared = false;
	mst->disconnecting = false;
	mst->resume_failed = false;

	return ret;
}

/*
 * The runtime-PM resume callback may fail before the DP host is powered.
 * Keep topology teardown pending until a later, powered prepare path.
 */
void msm_dp_mst_resume_failed(struct msm_dp *dp)
{
	struct msm_dp_mst *mst = dp->mst;

	if (!mst || !mst->initialized)
		return;

	/* The failed runtime resume does not permit audio register accesses. */
	msm_dp_mst_audio_clear(mst, false);
	mst->active = false;
	mst->prepared = false;
	mst->disconnecting = true;
	mst->suspended = false;
	mst->resume_failed = true;
}

int msm_dp_mst_root_atomic_check(struct msm_dp *dp,
				 struct drm_connector_state *conn_state)
{
	if (!dp->mst)
		return 0;

	return drm_dp_mst_root_conn_atomic_check(conn_state, &dp->mst->mgr);
}

int msm_dp_mst_hpd(struct msm_dp *dp, bool connected)
{
	struct msm_dp_mst *mst = dp->mst;
	int ret;

	if (!mst || !mst->initialized)
		return 0;

	if (connected) {
		if (!mst->prepared || mst->active || mst->disconnecting)
			return 0;

		msm_dp_ctrl_set_mst(msm_dp_display_get_ctrl(dp), true);
		ret = drm_dp_mst_topology_mgr_set_mst(&mst->mgr, true);
		if (!ret) {
			mst->active = true;
			mst->disconnecting = false;
			mst->suspended = false;
		} else {
			/* set_mst(true) sets mgr->mst_state before later AUX steps. */
			drm_dp_mst_topology_mgr_set_mst(&mst->mgr, false);
			drm_dp_dpcd_writeb(msm_dp_display_get_aux(dp), DP_MSTM_CTRL, 0);
			msm_dp_ctrl_set_mst(msm_dp_display_get_ctrl(dp), false);
			mst->prepared = false;
		}
	} else {
		if (mst->mgr.mst_state)
			drm_dp_mst_topology_mgr_set_mst(&mst->mgr, false);
		else if (mst->prepared)
			drm_dp_dpcd_writeb(msm_dp_display_get_aux(dp), DP_MSTM_CTRL, 0);

		msm_dp_ctrl_set_mst(msm_dp_display_get_ctrl(dp), false);
		mst->active = false;
		mst->prepared = false;
		mst->disconnecting = false;
		mst->suspended = false;
		ret = 0;
	}

	return ret;
}

void msm_dp_mst_start_disconnect(struct msm_dp *dp)
{
	struct msm_dp_mst *mst = dp->mst;

	if (!mst || !mst->initialized)
		return;

	mst->disconnecting = true;
	mst->active = false;
	mst->prepared = false;
	mst->suspended = false;
}

/*
 * Drop the dynamic topology as soon as userspace has seen the base-connector
 * unplug, but keep the source controller alive until the last stream bridge
 * reaches atomic_post_disable().
 */
void msm_dp_mst_hpd_unplug(struct msm_dp *dp)
{
	struct msm_dp_mst *mst = dp->mst;
	int ret;

	if (!mst || !mst->initialized)
		return;

	if (mst->mgr.mst_state) {
		ret = drm_dp_mst_topology_mgr_set_mst(&mst->mgr, false);
		if (ret)
			drm_dbg_dp(mst->mgr.dev,
				   "MST topology unplug cleanup returned %d\n", ret);
	}

	/* Do not clear ctrl->mst_mode here; active bridges still use source ACT. */
}

static int msm_dp_mst_dpcd_writeb(struct msm_dp *dp, unsigned int reg, u8 value)
{
	int ret;

	ret = drm_dp_dpcd_writeb(msm_dp_display_get_aux(dp), reg, value);
	if (ret == 1)
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int msm_dp_mst_dpcd_readb(struct msm_dp *dp, unsigned int reg, u8 *value)
{
	int ret;

	ret = drm_dp_dpcd_readb(msm_dp_display_get_aux(dp), reg, value);
	if (ret == 1)
		return 0;

	return ret < 0 ? ret : -EIO;
}

int msm_dp_mst_prepare(struct msm_dp *dp)
{
	struct msm_dp_mst *mst = dp->mst;
	u8 old_mstm_ctrl = 0;
	int ret;

	if (!mst || !mst->initialized)
		return 0;

	if (mst->resume_failed) {
		if (mst->mgr.mst_state)
			drm_dp_mst_topology_mgr_set_mst(&mst->mgr, false);
		mst->resume_failed = false;
		mst->disconnecting = false;
	}

	if (drm_dp_read_mst_cap(msm_dp_display_get_aux(dp),
				msm_dp_display_get_dpcd(dp)) != DRM_DP_MST)
		return 0;

	ret = msm_dp_mst_dpcd_readb(dp, DP_MSTM_CTRL, &old_mstm_ctrl);
	if (ret)
		goto err_disable;

	ret = msm_dp_mst_dpcd_writeb(dp, DP_MSTM_CTRL, 0);
	if (ret)
		goto err_disable;
	if (old_mstm_ctrl)
		usleep_range(100000, 101000);

	ret = msm_dp_mst_dpcd_writeb(dp, DP_MSTM_CTRL,
				     DP_MST_EN | DP_UP_REQ_EN | DP_UPSTREAM_IS_SRC);
	if (ret)
		goto err_disable;

	msm_dp_ctrl_set_mst(msm_dp_display_get_ctrl(dp), true);
	mst->prepared = true;
	mst->disconnecting = false;
	mst->suspended = false;

	return 0;

err_disable:
	msm_dp_ctrl_set_mst(msm_dp_display_get_ctrl(dp), false);
	mst->prepared = false;
	mst->disconnecting = false;
	mst->suspended = false;
	return ret;
}

bool msm_dp_mst_hpd_irq(struct msm_dp *dp)
{
	struct msm_dp_mst *mst = dp->mst;
	u8 esi[8] = {}, ack[8] = {};
	bool consumed = false, handled = true;
	int ret;

	if (!mst || !mst->active)
		return false;

	while (handled) {
		memset(ack, 0, sizeof(ack));
		ret = drm_dp_dpcd_read(msm_dp_display_get_aux(dp),
				       DP_SINK_COUNT_ESI, esi, sizeof(esi));
		if (ret != sizeof(esi))
			break;

		drm_dp_mst_hpd_irq_handle_event(&mst->mgr, esi, ack, &handled);
		if (!handled)
			break;

		consumed = true;
		ret = drm_dp_dpcd_write(msm_dp_display_get_aux(dp),
					DP_SINK_COUNT_ESI + 1, &ack[1], 3);
		if (ret != 3)
			break;

		drm_dp_mst_hpd_irq_send_new_request(&mst->mgr);
	}

	return consumed;
}
