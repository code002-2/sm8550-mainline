/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 */

#ifndef _DP_DISPLAY_H_
#define _DP_DISPLAY_H_

#include "dp_panel.h"
#include "disp/msm_disp_snapshot.h"

#define DP_MAX_PIXEL_CLK_KHZ	675000

struct drm_dp_aux;
struct drm_connector_state;
struct msm_dp_ctrl;
struct msm_dp_mst;

struct msm_dp {
	struct drm_device *drm_dev;
	struct platform_device *pdev;
	struct drm_connector *connector;
	struct drm_bridge *next_bridge;
	bool link_ready;
	bool audio_enabled;
	bool power_on;
	unsigned int connector_type;
	bool is_edp;
	bool internal_hpd;
	struct msm_dp_mst *mst;

	struct msm_dp_audio *msm_dp_audio;
	bool psr_supported;
};

bool msm_dp_display_mst_supported(struct msm_dp *msm_dp_display);
struct drm_dp_aux *msm_dp_display_get_aux(struct msm_dp *msm_dp_display);
const u8 *msm_dp_display_get_dpcd(struct msm_dp *msm_dp_display);
u32 msm_dp_display_get_link_rate(struct msm_dp *msm_dp_display);
u32 msm_dp_display_get_lane_count(struct msm_dp *msm_dp_display);
struct msm_dp_ctrl *msm_dp_display_get_ctrl(struct msm_dp *msm_dp_display);
int msm_dp_display_mst_stream_enable(struct msm_dp *msm_dp_display,
				     enum msm_dp_stream_id stream_id,
				     const struct drm_display_mode *mode,
				     u32 bpp, u32 colorspace, int pbn);
void msm_dp_display_mst_stream_pre_disable(struct msm_dp *msm_dp_display,
					   enum msm_dp_stream_id stream_id);
void msm_dp_display_mst_stream_disable(struct msm_dp *msm_dp_display,
				       enum msm_dp_stream_id stream_id);
void msm_dp_display_mst_stream_config_spd(struct msm_dp *msm_dp_display,
					  enum msm_dp_stream_id stream_id);
int msm_dp_display_mst_stream_config_hdr(struct msm_dp *msm_dp_display,
					 enum msm_dp_stream_id stream_id,
				const struct drm_connector_state *conn_state);
void msm_dp_display_mst_set_channel(struct msm_dp *msm_dp_display,
				    enum msm_dp_stream_id stream_id,
				    u32 start_slot, u32 num_slots);
void msm_dp_display_mst_update_payload(struct msm_dp *msm_dp_display);
int msm_dp_display_mst_stream_get(struct msm_dp *msm_dp_display,
				  enum msm_dp_stream_id stream_id);
void msm_dp_display_mst_stream_put(struct msm_dp *msm_dp_display,
				   enum msm_dp_stream_id stream_id);

int msm_dp_display_get_modes(struct msm_dp *msm_dp_display);
bool msm_dp_display_check_video_test(struct msm_dp *msm_dp_display);
int msm_dp_display_get_test_bpp(struct msm_dp *msm_dp_display);
void msm_dp_display_signal_audio_start(struct msm_dp *msm_dp_display);
void msm_dp_display_signal_audio_complete(struct msm_dp *msm_dp_display);
void msm_dp_display_set_psr(struct msm_dp *dp, bool enter);
void msm_dp_display_debugfs_init(struct msm_dp *msm_dp_display, struct dentry *dentry, bool is_edp);

#endif /* _DP_DISPLAY_H_ */
