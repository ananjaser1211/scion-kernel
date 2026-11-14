/*
 * SPDX-License-Identifier: MIT
 * vk_hostcoh: whether a Vulkan driver's imported host memory (VK_EXT_external_memory_host) is seen coherently by the GPU,
 * line by line, through the driver itself (no loader): what metal2vulkan does with a Metal no-copy buffer.
 *
 *   vk_hostcoh ICD [MB] [ROUNDS] [MODE]
 *
 * ICD is the driver by path (/redwood/lib/libvulkan_panfrost.dylib). Each round: the CPU writes a pattern into the imported
 * pages, the GPU copies them into a buffer of its own memory and then fills the imported pages with another value; the CPU
 * reads both. MODE says what keeps the processors' caches out of the way:
 *   none   nothing;
 *   vk     vkFlushMappedMemoryRanges before the submit, vkInvalidateMappedMemoryRanges after the wait (the driver's job);
 *   civac  DC CIVAC over every line by this program, before the submit and after the wait (the default).
 * Every 64-byte line the GPU read wrong (the copy) or the CPU read wrong (the fill) is counted, with the first few said.
 * Exit status 0 when nothing was lost. Run with PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 for PanVK on the G72. Built with the
 * iPhoneOS SDK and Mesa's Vulkan headers:
 *   xnu-clang -arch arm64 -isysroot $SDK -O2 -I$MESA/include -o vk_hostcoh tools/metal/vk_hostcoh.c
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;

#define IFN(n) PFN_##n n = (PFN_##n) gipa(inst, #n)
#define DFN(n) PFN_##n n = (PFN_##n) gdpa(dev, #n)

static double
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void
civac(void *p, size_t n)
{
	uintptr_t a = (uintptr_t) p & ~(uintptr_t) 63, e = (uintptr_t) p + n;

	__asm__ volatile ("dsb sy" ::: "memory");
	for (; a < e; a += 64) {
		__asm__ volatile ("dc civac, %0" :: "r" (a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint32_t
find_type(VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

int
main(int argc, char **argv)
{
	const char *icd = argc > 1 ? argv[1] : "/redwood/lib/libvulkan_panfrost.dylib";
	size_t size = (argc > 2 ? strtoul(argv[2], NULL, 0) : 4) << 20;
	int rounds = argc > 3 ? atoi(argv[3]) : 20;
	const char *mode = argc > 4 ? argv[4] : "civac";
	int m_vk = !strcmp(mode, "vk"), m_civac = !strcmp(mode, "civac");
	void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);

	if (lib == NULL) {
		fprintf(stderr, "vk_hostcoh: %s\n", dlerror());
		return 2;
	}
	gipa = (PFN_vkGetInstanceProcAddr) dlsym(lib, "vk_icdGetInstanceProcAddr");
	if (gipa == NULL) {
		fprintf(stderr, "vk_hostcoh: no vk_icdGetInstanceProcAddr in %s\n", icd);
		return 2;
	}
	PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
		fprintf(stderr, "vk_hostcoh: vkCreateInstance failed\n");
		return 2;
	}
	IFN(vkEnumeratePhysicalDevices);
	IFN(vkGetPhysicalDeviceMemoryProperties);
	IFN(vkCreateDevice);
	gdpa = (PFN_vkGetDeviceProcAddr) gipa(inst, "vkGetDeviceProcAddr");
	uint32_t n = 1;
	VkPhysicalDevice pd;
	if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || n == 0) {
		fprintf(stderr, "vk_hostcoh: no device\n");
		return 2;
	}
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
		                        .pQueuePriorities = &prio };
	const char *ext[] = { "VK_EXT_external_memory_host", "VK_KHR_external_memory" };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		                   .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext };
	if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
		fprintf(stderr, "vk_hostcoh: vkCreateDevice with VK_EXT_external_memory_host failed\n");
		return 2;
	}
	DFN(vkGetDeviceQueue);
	DFN(vkCreateBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkCmdCopyBuffer);
	DFN(vkCmdFillBuffer);
	DFN(vkCmdPipelineBarrier);
	DFN(vkQueueSubmit);
	DFN(vkQueueWaitIdle);
	DFN(vkFlushMappedMemoryRanges);
	DFN(vkInvalidateMappedMemoryRanges);
	DFN(vkGetMemoryHostPointerPropertiesEXT);
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	uint32_t *host = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	memset(host, 0, size);
	VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
	vkGetMemoryHostPointerPropertiesEXT(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
	VkExternalMemoryBufferCreateInfo ebci = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
		                                  .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebci, .size = size,
		                   .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer hb, db;
	vkCreateBuffer(dev, &bci, NULL, &hb);
	bci.pNext = NULL;
	vkCreateBuffer(dev, &bci, NULL, &db);
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, hb, &mr);
	uint32_t ht = find_type(&mp, mr.memoryTypeBits & hp.memoryTypeBits, 0);
	VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
		                                 .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
		                                 .pHostPointer = host };
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
		                     .memoryTypeIndex = ht };
	VkDeviceMemory hm, dm;
	VkResult r = ht == UINT32_MAX ? VK_ERROR_INVALID_EXTERNAL_HANDLE : vkAllocateMemory(dev, &mai, NULL, &hm);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "vk_hostcoh: the host pages were not imported (types %#x, %d)\n", hp.memoryTypeBits, r);
		return 2;
	}
	vkBindBufferMemory(dev, hb, hm, 0);
	vkGetBufferMemoryRequirements(dev, db, &mr);
	mai.pNext = NULL;
	mai.memoryTypeIndex = find_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	vkAllocateMemory(dev, &mai, NULL, &dm);
	vkBindBufferMemory(dev, db, dm, 0);
	uint32_t *dmap;
	vkMapMemory(dev, dm, 0, size, 0, (void **) &dmap);
	printf("vk_hostcoh: %zu MB imported in type %u (flags %#x), mode %s\n", size >> 20, ht,
	    mp.memoryTypes[ht].propertyFlags, mode);

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	VkCommandPool pool;
	vkCreateCommandPool(dev, &cpci, NULL, &pool);
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	vkAllocateCommandBuffers(dev, &cbai, &cb);
	VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = hm, .size = VK_WHOLE_SIZE };
	size_t words = size / 4, lines = size / 64;
	unsigned long lost_in = 0, lost_out = 0;
	double t_cpu = 0, t_gpu = 0;

	for (int round = 1; round <= rounds; round++) {
		uint32_t pat = 0x10000000u * (round & 7) + round, fill = ~pat;
		double t0 = now_ms();

		for (size_t i = 0; i < words; i++) {
			host[i] = pat ^ (uint32_t) i;
		}
		if (m_civac) {
			civac(host, size);
		} else if (m_vk) {
			vkFlushMappedMemoryRanges(dev, 1, &range);
		}
		double t1 = now_ms();
		VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		vkBeginCommandBuffer(cb, &bi);
		VkBufferCopy c = { 0, 0, size };
		vkCmdCopyBuffer(cb, hb, db, 1, &c);
		VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			               .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL,
		    0, NULL);
		vkCmdFillBuffer(cb, hb, 0, size, fill);
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
		    NULL);
		vkEndCommandBuffer(cb);
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
		vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
		vkQueueWaitIdle(q);
		double t2 = now_ms();
		if (m_civac) {
			civac(host, size);
		} else if (m_vk) {
			vkInvalidateMappedMemoryRanges(dev, 1, &range);
		}
		t_cpu += (t1 - t0) + (now_ms() - t2);
		t_gpu += t2 - t1;

		unsigned long bad_in = 0, bad_out = 0, said = 0;
		for (size_t l = 0; l < lines; l++) {
			int in = 0, out = 0;
			for (size_t w = l * 16; w < l * 16 + 16; w++) {
				in |= dmap[w] != (pat ^ (uint32_t) w);
				out |= host[w] != fill;
			}
			if ((in || out) && said++ < 4) {
				printf("  round %d line %zu (page %zu, +%#zx): GPU read %#x for %#x%s; CPU read %#x for %#x%s\n", round,
				    l, l * 64 / 16384, (l * 64) % 16384, dmap[l * 16], pat ^ (uint32_t) (l * 16), in ? " LOST" : "",
				    host[l * 16], fill, out ? " LOST" : "");
			}
			bad_in += in;
			bad_out += out;
		}
		lost_in += bad_in;
		lost_out += bad_out;
		if (bad_in || bad_out) {
			printf("round %d: %lu of %zu lines read wrong by the GPU, %lu by the CPU\n", round, bad_in, lines, bad_out);
		}
	}
	printf("vk_hostcoh: %d rounds of %zu lines: %lu lost into the GPU, %lu out of it; CPU %.2f ms, GPU %.2f ms a round\n",
	    rounds, lines, lost_in, lost_out, t_cpu / rounds, t_gpu / rounds);
	return lost_in || lost_out;
}
