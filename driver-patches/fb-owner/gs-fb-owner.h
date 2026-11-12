/* SPDX-License-Identifier: MIT */
/*
 * geforce-sriov: the owner of the framebuffer slots of Ada VFs, inside nvidia.ko.
 * See gs-fb-owner.c. These are the six places NVIDIA's open layer calls it from; the lines that
 * call them are added by fb-owner.patch.
 */
#ifndef GS_FB_OWNER_H
#define GS_FB_OWNER_H

/* Module load, after NVIDIA's /proc directory exists; module unload, before the devices go. */
void gs_fb_init(void);
void gs_fb_exit(void);
/*
 * nvidia_ioctl(), default arm. pre: 1 when the call was answered with a duplicate of a held slot
 * and must not reach rm_ioctl(); 0 when it goes on unchanged. post: after rm_ioctl() returned.
 * seen carries what pre read of the caller's parameters to post, for the one call they belong to.
 */
int gs_fb_ioctl_pre(nv_linux_file_private_t *nvlfp, int cmd, void *arg, size_t size, NvU32 *seen);
void gs_fb_ioctl_post(nv_linux_file_private_t *nvlfp, int cmd, void *arg, size_t size, NV_STATUS rm_status, NvU32 seen);
/* A control file is closed: what was recorded for it goes. */
void gs_fb_file_closed(nv_linux_file_private_t *nvlfp);
/* A GPU is about to be removed from the driver: whatever is held on it is freed first. */
void gs_fb_gpu_remove(NvU32 gpu_id);

#endif
