/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _DP_MST_DRM_H_
#define _DP_MST_DRM_H_

#include "dp_drm.h"

int msm_dp_mst_init(struct msm_dp *msm_dp_display);
void msm_dp_mst_destroy(struct msm_dp *msm_dp_display);
int msm_dp_mst_bridge_init(struct msm_dp *msm_dp_display,
			   struct drm_encoder *encoder,
			   enum msm_dp_stream_id stream_id);
bool msm_dp_mst_active(struct msm_dp *msm_dp_display);
bool msm_dp_mst_disconnecting(struct msm_dp *msm_dp_display);
bool msm_dp_mst_suspend(struct msm_dp *msm_dp_display);
bool msm_dp_mst_suspended(struct msm_dp *msm_dp_display);
int msm_dp_mst_resume(struct msm_dp *msm_dp_display);
void msm_dp_mst_resume_failed(struct msm_dp *msm_dp_display);
int msm_dp_mst_root_atomic_check(struct msm_dp *msm_dp_display,
				 struct drm_connector_state *conn_state);
int msm_dp_mst_prepare(struct msm_dp *msm_dp_display);
int msm_dp_mst_hpd(struct msm_dp *msm_dp_display, bool connected);
void msm_dp_mst_start_disconnect(struct msm_dp *msm_dp_display);
void msm_dp_mst_hpd_unplug(struct msm_dp *msm_dp_display);
bool msm_dp_mst_hpd_irq(struct msm_dp *msm_dp_display);

#endif /* _DP_MST_DRM_H_ */
