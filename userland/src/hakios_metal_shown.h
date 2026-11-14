/*
 * Whether this process may be shown the Metal device, by the level of boot-arg metal (sysctl kern.bootargs) and the file
 * HakiosMetal.clients beside the bundle: shared by the two builds of the plug-in, src/HakiosMetal.m and (METAL=vk)
 * src/metalvk/HakiosMetalVK.mm. See hakios_metal_shown.c.
 */
#ifndef HAKIOS_METAL_SHOWN_H
#define HAKIOS_METAL_SHOWN_H
#ifdef __cplusplus
extern "C" {
#endif
/* 1 if shown; say (may be NULL) is told why or why not, one line */
int hakios_metal_shown(void (*say)(const char *line));
/* the path of a file beside the bundle (NAME in the directory that holds HakiosMetal.bundle) into PATH; 1 if it is readable */
int hakios_metal_beside_bundle(const char *name, char *path, unsigned long size);
/*
 * The Vulkan driver the METAL=vk plug-in opens, into PATH: the environment's HAKIOS_VK_ICD, else the first line of HakiosMetal.icd
 * beside the bundle (an absolute path), else /redwood/lib/libhakios.vk.dylib. say (may be NULL) is told when the file names none.
 */
void hakios_metal_vk_driver(char *path, unsigned long size, void (*say)(const char *line));
/* 1 if PATH is PanVK, Mesa's Vulkan driver for the Mali (a file named libvulkan_panfrost*) */
int hakios_metal_vk_driver_is_panvk(const char *path);
/* 1 if this process is the window server (backboardd) */
int hakios_metal_is_window_server(void);
#ifdef __cplusplus
}
#endif
#endif
