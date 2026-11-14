/*
 * What Mesa's freedreno and panfrost take from libdrm's xf86drm.h, for a system without Linux's
 * DRM: the declarations here, the functions in hakios.drm.c, which carry the requests to the
 * kernel's GPU driver as calls of its user client -- the service HakiosGPU (the POCO's Adreno, as
 * Linux's msm; drivers/gpu/hakios.gpu.cpp) or HakiosMali (the Galaxy S9's Mali, as Linux's
 * panfrost; drivers/gpu/mali/hakios.mali.cpp). tools/mk-mesa.sh puts both into Mesa's tree, where
 * they are the "libdrm" the two drivers are built against. The request numbers and their
 * structures are Linux's (Mesa's include/drm-uapi).
 */
#ifndef HAKIOS_XF86DRM_H
#define HAKIOS_XF86DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_RENDER         2
#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmCloseBufferHandle(int fd, uint32_t handle);

/*
 * Buffers as file descriptors: there are none here. But memory that somebody else has made does
 * come in, as Linux's comes in by file descriptor: an IOSurface, by its ID, where the driver
 * expects the descriptor (see hakios_drm_surface() below).
 */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle);

/*
 * Fences ("sync objects"). For panfrost they are the kernel's job numbers (hakios.drm.c says how);
 * for freedreno, which waits by its own requests, there are none.
 */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
/* (syncobjs kept in this process, over the kernel's fences: hakios.drm.c) */
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags);
int drmGetNodeTypeFromFd(int fd);

int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
/* (timelines: only for the Malis with a command stream frontend, panthor's, which are not here) */
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags);

/*
 * Not libdrm's: a buffer's memory in this process, in place of mmap() on the device
 * (freedreno_bo.c, and panfrost's pan_kmod.h and pan_bo.c, are patched to call these).
 */
void *hakios_drm_map(int fd, uint32_t handle, uint64_t size);
void hakios_drm_unmap(int fd, uint32_t handle, void *address);

/*
 * Not libdrm's either: what the memory of an IOSurface would be like as a buffer (the kernel's
 * request and its answer: struct hakios_surface in drivers/gpu/hakios.gpu.cpp, the same as struct
 * mali_surface in drivers/gpu/mali/hakios.mali.cpp); 0, or -1 with errno. The buffer itself is made by
 * drmPrimeFDToHandle(), which the driver calls with its table of buffers locked, so that a buffer
 * on its way out and the same surface on its way in cannot pass each other.
 */
struct hakios_drm_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* (the kernel's: which of the two is asked) */
	uint64_t size;                          /* back: how big the buffer is, */
	uint32_t offset, row_bytes;             /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixel_format, planes;   /* and what they are */
};
int hakios_drm_surface(int fd, struct hakios_drm_surface *surface);
/* Not libdrm's: which IOSurface's memory is mapped at BASE in this process, told by whoever maps it (AdrenoMetal's frontend,
 * tools/m2v), for Turnip's VK_EXT_external_memory_host, which is given only the address: its ID, 0 if nobody said. */
void hakios_drm_host_surface(const void *base, uint64_t size, uint32_t surface);
uint32_t hakios_drm_host_surface_at(const void *base, uint64_t size);
/* Not libdrm's: pages of this process (whole ones) as a buffer, Linux's userptr (HakiosGPU's request 0x7d, HakiosMali's 0x7b) */
int hakios_drm_user_memory(int fd, const void *address, uint64_t size, uint32_t *handle);
/* Not libdrm's: a range of a buffer's lines cleaned and invalidated by the kernel (HakiosMali's request 0x7a), for such a
 * buffer of the process's own cached pages; -1 with errno ENOSYS where the kernel has no such request */
int hakios_drm_sync(int fd, uint32_t handle, uint64_t offset, uint64_t size);
/*
 * Not libdrm's: work this process submitted writes the pixels of IOSurface SURFACE, whose buffer the kernel made
 * (drmPrimeFDToHandle): its seed changes, as at a device's write (HakiosMali's request 0x79), for readers in other processes
 * that keep what they made of the pixels. Called once the work is queued (the Metal frontend, for metal2vulkan's
 * surface_watch.rs). 0, or -1 with errno: ENOENT where no buffer of this process is the surface's, ENOSYS where the kernel
 * has no such request.
 */
int hakios_drm_surface_written(uint32_t surface);
/*
 * Not libdrm's: a handle of a submission's bo_handles that a job chain writes may carry this bit (Mesa's panfrost knows
 * which, tools/mesa/panfrost-written-bos.patch): the kernel then changes a surface's seed once the chain is queued. Taken
 * off here where the kernel does not take it (its parameter 0x7900).
 */
#define HAKIOS_DRM_BO_WRITTEN   (1u << 31)

#ifdef __cplusplus
}
#endif

/*
 * (Declared only where Mesa is built with this file as its libdrm, HAVE_LIBDRM -- the PanVK build,
 * tools/mesa/panvk/panvk-darwin.patch -- and in hakios.drm.c itself: elsewhere Mesa's util/libdrm.h
 * declares its own stand-ins for these types, and the two must not meet in one file.)
 *
 * The devices there are, for a driver that looks for its GPU among them rather than opening it by
 * name: Mesa's Vulkan drivers (PanVK: vk_instance.c's enumeration, panvk_physical_device.c). There is
 * at most one here, the Mali, and it is a "platform" device (not on a bus) with a render node only,
 * whose path is a file that means nothing (/dev/null), as drmOpenWithType()'s descriptor is.
 * drmGetDevices2() opens the connection to the service on the way, so that drmGetVersion() on the
 * descriptor the driver then open()s answers as it does after drmOpenWithType().
 */
#if defined(HAVE_LIBDRM) || defined(HAKIOS_DRM_DEVICES)
#define DRM_NODE_CONTROL        1
#define DRM_NODE_MAX            3
#define DRM_BUS_PCI             0
#define DRM_BUS_USB             1
#define DRM_BUS_PLATFORM        2
#define DRM_BUS_HOST1X          3
#define DRM_PLATFORM_DEVICE_NAME_LEN 512

typedef struct _drmPlatformBusInfo {
	char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
	char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

/* (PCI's, for the code that compares a device's bus address; never filled in here) */
typedef struct _drmPciBusInfo {
	uint16_t domain;
	uint8_t bus, dev, func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
	uint16_t vendor_id, device_id, subvendor_id, subdevice_id;
	uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

typedef struct _drmDevice {
	char **nodes;                   /* DRM_NODE_MAX of them; only DRM_NODE_RENDER is there */
	int available_nodes;            /* 1 << DRM_NODE_RENDER */
	int bustype;                    /* DRM_BUS_PLATFORM */
	union {
		drmPciBusInfoPtr pci;
		drmPlatformBusInfoPtr platform;
	} businfo;
	union {
		drmPciDeviceInfoPtr pci;
		drmPlatformDeviceInfoPtr platform;
	} deviceinfo;
} drmDevice, *drmDevicePtr;

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
int drmGetDevices(drmDevicePtr devices[], int max_devices);
void drmFreeDevices(drmDevicePtr devices[], int count);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
#endif

/*
 * Not libdrm's: with HAKIOS_DRM_NOOP=1 in the environment, no kernel at all. The service is not
 * looked for; a Mali-G72 is made up in this process (its parameters as the S9's, buffers in this
 * process's memory, every job "done" the moment it is given, none run). For running a driver on a
 * machine without the GPU, as Mesa's drm-shim does on Linux: everything up to the GPU itself (device
 * creation, the shader compiler, command building, submission) is gone through, and nothing a job
 * would write is ever written. 1 when this process has it on.
 */
int hakios_drm_noop(void);

#endif
