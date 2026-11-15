/*
 * vk_filter_probe: how far a Vulkan driver's own linear filter of an 8-bit unsigned normalized texture lands from
 * Metal's, at the coordinate metal2vulkan hands the device for it (its `Unorm8LinearFilter` deviation,
 * crates/metal2vulkan/src/lower/sampler/native.rs: the centre of Metal's 1/256 footprint step).
 *
 * Metal reads the texel-space position X of a coordinate u (wrapped into one period first on a repeating or mirrored
 * axis, then scaled by the size N) as the step q = floor(256 (X - 1/2) + 1/2): the taps q >> 8 and the one after it,
 * weighed by (q & 255) / 256. metal2vulkan's native arm gives the device the coordinate of the centre of that step,
 * computed in the shader as `cell` does: X = (u - start) N, c = floor(256 X) + [frac(256 X) >= 1/2], c / 256 / N (all
 * in f32). The device then reads Metal's taps and weights if it has 8 subtexel bits, and only its blend is its own.
 *
 * The probe samples 19x13 textures of R8, RG8, RGBA8 and BGRA8 unorm (random bytes) through a compute shader
 * (OpImageSampleExplicitLod at level 0, SPIR-V below) at 4096 coordinates per address mode (a third within an f32 unit
 * or two of a 1/256 step's edge, the rest uniform in [-1.5, 2.5]), under clamp to edge, repeat, mirrored repeat and
 * clamp to (transparent) border, linear filters, no mipmaps, normalized coordinates, and compares each channel the
 * format has with Metal's filter computed exactly on the CPU (the bilinear sum of the codes at Metal's taps and
 * weights, in rationals, over 255). It prints the largest difference in codes (1/255) per format and mode, and the
 * largest of all; the deviation holds where no channel is one code or more away.
 *
 *   vk_filter_probe [--mali-v6] [--nearest-wrap] [--told-step] [--verbose] [DRIVER]
 *     DRIVER          the ICD's path (else VK_SMOKE_DRIVER, else the program is linked to it)
 *     --nearest-wrap  the native coordinate as metal2vulkan computed it before Scion's 0012: the wrap of a repeating or
 *                     mirrored axis the plain f32 subtraction, rounded to nearest, where Metal rounds it up
 *     --told-step     compare with the filter at the step the coordinate names: the device's blend alone
 *     --verbose       print every channel one code or more away
 *
 * On PanVK (Mesa 25.0.7, Scion's patches) on the Galaxy S9's Mali-G72, 15 November 2026: 0.89 of a code at most (0.84
 * under clamp to edge), the same with --told-step; with --nearest-wrap 1.35 codes, on repeating and mirrored axes where
 * -1 < u < 0.
 *
 * Copyright (c) 2026 Scion contributors. MIT licence (see the repository's LICENSE).
 */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/*
 * The compute shader, SPIR-V 1.0 written out by hand:
 *
 *   OpEntryPoint GLCompute %main "main" %gid; LocalSize 64 1 1
 *   set 0: binding 0 a combined image sampler (2D float), binding 1 BufferBlock { vec2[] }, binding 2 { vec4[] }
 *   main: x = gid.x; out[x] = OpImageSampleExplicitLod(tex, in[x], Lod 0.0)
 */
static const uint32_t shader[] = {
	0x07230203, 0x00010000, 0x00000000, 0x00000023, 0x00000000, 0x00020011,
	0x00000001, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
	0x00000001, 0x6e69616d, 0x00000000, 0x00000002, 0x00060010, 0x00000001,
	0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00040047, 0x00000002,
	0x0000000b, 0x0000001c, 0x00040047, 0x00000003, 0x00000022, 0x00000000,
	0x00040047, 0x00000003, 0x00000021, 0x00000000, 0x00040047, 0x00000004,
	0x00000006, 0x00000008, 0x00050048, 0x00000005, 0x00000000, 0x00000023,
	0x00000000, 0x00030047, 0x00000005, 0x00000003, 0x00040047, 0x00000006,
	0x00000022, 0x00000000, 0x00040047, 0x00000006, 0x00000021, 0x00000001,
	0x00040047, 0x00000007, 0x00000006, 0x00000010, 0x00050048, 0x00000008,
	0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000008, 0x00000003,
	0x00040047, 0x00000009, 0x00000022, 0x00000000, 0x00040047, 0x00000009,
	0x00000021, 0x00000002, 0x00020013, 0x0000000a, 0x00030021, 0x0000000b,
	0x0000000a, 0x00030016, 0x0000000c, 0x00000020, 0x00040017, 0x0000000d,
	0x0000000c, 0x00000002, 0x00040017, 0x0000000e, 0x0000000c, 0x00000004,
	0x00040015, 0x0000000f, 0x00000020, 0x00000000, 0x00040017, 0x00000010,
	0x0000000f, 0x00000003, 0x00040020, 0x00000011, 0x00000001, 0x00000010,
	0x0004003b, 0x00000011, 0x00000002, 0x00000001, 0x00090019, 0x00000012,
	0x0000000c, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
	0x00000000, 0x0003001b, 0x00000013, 0x00000012, 0x00040020, 0x00000014,
	0x00000000, 0x00000013, 0x0004003b, 0x00000014, 0x00000003, 0x00000000,
	0x0003001d, 0x00000004, 0x0000000d, 0x0003001e, 0x00000005, 0x00000004,
	0x00040020, 0x00000015, 0x00000002, 0x00000005, 0x0004003b, 0x00000015,
	0x00000006, 0x00000002, 0x00040020, 0x00000016, 0x00000002, 0x0000000d,
	0x0003001d, 0x00000007, 0x0000000e, 0x0003001e, 0x00000008, 0x00000007,
	0x00040020, 0x00000017, 0x00000002, 0x00000008, 0x0004003b, 0x00000017,
	0x00000009, 0x00000002, 0x00040020, 0x00000018, 0x00000002, 0x0000000e,
	0x0004002b, 0x0000000f, 0x00000019, 0x00000000, 0x0004002b, 0x0000000c,
	0x0000001a, 0x00000000, 0x00050036, 0x0000000a, 0x00000001, 0x00000000,
	0x0000000b, 0x000200f8, 0x0000001b, 0x0004003d, 0x00000010, 0x0000001c,
	0x00000002, 0x00050051, 0x0000000f, 0x0000001d, 0x0000001c, 0x00000000,
	0x00060041, 0x00000016, 0x0000001e, 0x00000006, 0x00000019, 0x0000001d,
	0x0004003d, 0x0000000d, 0x0000001f, 0x0000001e, 0x0004003d, 0x00000013,
	0x00000020, 0x00000003, 0x00070058, 0x0000000e, 0x00000021, 0x00000020,
	0x0000001f, 0x00000002, 0x0000001a, 0x00060041, 0x00000018, 0x00000022,
	0x00000009, 0x00000019, 0x0000001d, 0x0003003e, 0x00000022, 0x00000021,
	0x000100fd, 0x00010038,
};

#define W 19
#define H 13
#define COORDS 4096

static PFN_vkGetInstanceProcAddr gipa;
#define GET(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DGET(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

enum mode { CLAMP, REPEAT, MIRROR, BORDER, MODES };
static const char *mode_name[MODES] = { "clamp to edge", "repeat", "mirrored repeat", "clamp to zero" };
static const VkSamplerAddressMode vk_mode[MODES] = { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
	VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER };

struct format {
	const char *name;
	VkFormat vk;
	int bytes;
	int channels;           /* the channels the format has, as sampled: r, g, b, a */
	int byte_of[4];         /* the byte of a texel each sampled channel reads */
};
static const struct format formats[] = {
	{ "R8Unorm", VK_FORMAT_R8_UNORM, 1, 1, { 0 } },
	{ "RG8Unorm", VK_FORMAT_R8G8_UNORM, 2, 2, { 0, 1 } },
	{ "RGBA8Unorm", VK_FORMAT_R8G8B8A8_UNORM, 4, 4, { 0, 1, 2, 3 } },
	{ "BGRA8Unorm", VK_FORMAT_B8G8R8A8_UNORM, 4, 4, { 2, 1, 0, 3 } },
};

static uint32_t
find_memory(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

/* the start of u's period on a wrapping axis (metal2vulkan's native coordinate: floor(u), 2 floor(u / 2)) */
static float
period_start(enum mode m, float u)
{
	if (m == REPEAT)
		return floorf(u);
	if (m == MIRROR)
		return floorf(u * 0.5f) * 2.0f;
	return 0.0f;
}

/*
 * the coordinate metal2vulkan's native arm hands the device on an axis of size n (`cell`, in f32): the wrap rounded up as
 * Metal rounds it (`position::wrap`; the sizes here are not powers of two), or by the plain subtraction (--nearest-wrap)
 */
static float to_f32(double d, int up);
static int nearest_wrap; /* --nearest-wrap: the cell wraps by the plain f32 subtraction (metal2vulkan before Scion 0012) */

/* the step's c = q + 128 the coordinate names (as an f32 integer) */
static float
cell_c(enum mode m, float u, uint32_t n)
{
	volatile float w = !nearest_wrap && (m == REPEAT || m == MIRROR) ? to_f32((double)u - (double)period_start(m, u), 1) :
	    u - period_start(m, u);
	volatile float x = w * (float)n;
	volatile float y = x * 256.0f;
	float fl = floorf(y);
	return fl + (y - fl >= 0.5f ? 1.0f : 0.0f);
}

static float
cell(enum mode m, float u, uint32_t n)
{
	return cell_c(m, u, n) * (1.0f / 256.0f) / (float)n;
}

/* d rounded to an f32: upward (up), or to nearest with ties away from zero */
static float
to_f32(double d, int up)
{
	float lo = (float)d;

	if ((double)lo > d)
		lo = nextafterf(lo, -INFINITY);
	if ((double)lo == d)
		return lo;
	float hi = nextafterf(lo, INFINITY);
	if (up)
		return hi;
	double below = d - (double)lo, above = (double)hi - d;
	if (below != above)
		return below < above ? lo : hi;
	return d < 0 ? lo : hi;
}

static int told;        /* --told-step: compare with the step the coordinate names (the device's blend alone), not Metal's */
static int verbose;     /* --verbose: every channel one code or more away */

/*
 * Metal's step q of u on an axis of size n (not a power of two), as metal2vulkan's position module states Apple's
 * texture unit computes it: the wrap u - floor(u) (repeat) or u - 2 floor(u / 2) (mirrored) rounded up to an f32, the
 * product with n rounded to an f32 with ties away from zero, then q = floor(256 X + 1/2) - 128.
 */
static long
metal_step(enum mode m, float u, uint32_t n)
{
	if (told)
		return (long)cell_c(m, u, n) - 128;
	float w = m == REPEAT || m == MIRROR ? to_f32((double)u - (double)period_start(m, u), 1) : u;
	float x = to_f32((double)w * n, 0);
	return (long)floor(256.0 * x + 0.5) - 128;
}

/* tap i on an axis of size n under mode m: its index, or -1 for the border */
static int
tap(enum mode m, long i, int n)
{
	switch (m) {
	case CLAMP:
		return i < 0 ? 0 : i >= n ? n - 1 : (int)i;
	case REPEAT:
		return (int)(((i % n) + n) % n);
	case MIRROR: {
		long t = ((i % (2 * n)) + 2 * n) % (2 * n);
		return (int)(t < n ? t : 2 * n - 1 - t);
	}
	default:
		return i < 0 || i >= n ? -1 : (int)i;
	}
}

static uint32_t
rnd(void)
{
	static uint32_t s = 0x9e3779b9;
	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	return s;
}

/* a coordinate: a third near a 1/256 step's edge of an axis of size n, the rest in [-1.5, 2.5] */
static float
coordinate(int k, uint32_t n)
{
	if (k % 3 == 0) {
		long edge = (long)(rnd() % (256 * 4 * n)) - 256 * n;   /* an edge in [-n, 3n) texels */
		float u = (float)((0.5 + (edge - 0.5) / 256.0) / n);
		int ulps = (int)(rnd() % 5) - 2;
		for (; ulps > 0; ulps--)
			u = nextafterf(u, INFINITY);
		for (; ulps < 0; ulps++)
			u = nextafterf(u, -INFINITY);
		return u;
	}
	return -1.5f + 4.0f * (float)(rnd() & 0xffffff) / 16777216.0f;
}

int
main(int argc, char **argv)
{
	const char *path = getenv("VK_SMOKE_DRIVER");


	for (int a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "--mali-v6"))
			setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
		else if (!strcmp(argv[a], "--nearest-wrap"))
			nearest_wrap = 1;
		else if (!strcmp(argv[a], "--told-step"))
			told = 1;
		else if (!strcmp(argv[a], "--verbose"))
			verbose = 1;
		else
			path = argv[a];
	}
	void *h = RTLD_DEFAULT;
	if (path && (h = dlopen(path, RTLD_NOW | RTLD_LOCAL)) == NULL) {
		printf("vk_filter_probe: cannot open %s: %s\n", path, dlerror());
		return 2;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
	if (!gipa)
		gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
	if (!gipa) {
		printf("vk_filter_probe: no vk_icdGetInstanceProcAddr\n");
		return 2;
	}
	typedef VkResult (*negotiate_t)(uint32_t *);
	negotiate_t negotiate = (negotiate_t)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
	if (negotiate) {
		uint32_t v = 5;
		negotiate(&v);
	}

	GET(NULL, vkCreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_filter_probe",
		.apiVersion = VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	if (!vkCreateInstance || vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
		printf("vk_filter_probe: no instance\n");
		return 2;
	}
	GET(inst, vkEnumeratePhysicalDevices);
	GET(inst, vkGetPhysicalDeviceProperties);
	GET(inst, vkGetPhysicalDeviceQueueFamilyProperties);
	GET(inst, vkGetPhysicalDeviceMemoryProperties);
	GET(inst, vkCreateDevice);
	GET(inst, vkGetDeviceProcAddr);
	uint32_t n = 1;
	VkPhysicalDevice pd;
	if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || n == 0) {
		printf("vk_filter_probe: no physical device (PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 or --mali-v6 on a Mali-G72?)\n");
		return 2;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("vk_filter_probe: \"%s\", driver %#x, vendor %#x, device %#x, subTexelPrecisionBits %u\n", props.deviceName,
	    props.driverVersion, props.vendorID, props.deviceID, props.limits.subTexelPrecisionBits);
	uint32_t nq = 16, family = 0;
	VkQueueFamilyProperties qf[16];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
	for (family = 0; family < nq && !(qf[family].queueFlags & VK_QUEUE_COMPUTE_BIT); family++)
		;
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci };
	VkDevice dev;
	if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
		printf("vk_filter_probe: no device\n");
		return 2;
	}
	DGET(vkGetDeviceQueue);
	DGET(vkCreateBuffer);
	DGET(vkGetBufferMemoryRequirements);
	DGET(vkAllocateMemory);
	DGET(vkBindBufferMemory);
	DGET(vkMapMemory);
	DGET(vkCreateImage);
	DGET(vkGetImageMemoryRequirements);
	DGET(vkBindImageMemory);
	DGET(vkCreateImageView);
	DGET(vkCreateSampler);
	DGET(vkCreateCommandPool);
	DGET(vkAllocateCommandBuffers);
	DGET(vkBeginCommandBuffer);
	DGET(vkEndCommandBuffer);
	DGET(vkResetCommandBuffer);
	DGET(vkCmdPipelineBarrier);
	DGET(vkCmdCopyBufferToImage);
	DGET(vkQueueSubmit);
	DGET(vkQueueWaitIdle);
	DGET(vkCreateShaderModule);
	DGET(vkCreateDescriptorSetLayout);
	DGET(vkCreatePipelineLayout);
	DGET(vkCreateComputePipelines);
	DGET(vkCreateDescriptorPool);
	DGET(vkAllocateDescriptorSets);
	DGET(vkUpdateDescriptorSets);
	DGET(vkCmdBindPipeline);
	DGET(vkCmdBindDescriptorSets);
	DGET(vkCmdDispatch);
	VkQueue queue;
	vkGetDeviceQueue(dev, family, 0, &queue);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);

	/* one host-visible buffer: the texels (at 0), the coordinates (at 4096) and the results (after them) */
	const VkDeviceSize texels_at = 0, coords_at = 4096, results_at = coords_at + COORDS * 8;
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = results_at + COORDS * 16,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buf;
	VkMemoryRequirements mr;
	VkDeviceMemory bmem;
	uint8_t *mem;
	vkCreateBuffer(dev, &bci, NULL, &buf);
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = find_memory(&mp, mr.memoryTypeBits,
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &bmem) != VK_SUCCESS ||
	    vkBindBufferMemory(dev, buf, bmem, 0) != VK_SUCCESS ||
	    vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&mem) != VK_SUCCESS) {
		printf("vk_filter_probe: no host-visible, coherent buffer memory\n");
		return 2;
	}
	float *coords = (float *)(mem + coords_at), *results = (float *)(mem + results_at);

	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(shader), .pCode = shader };
	VkShaderModule sm;
	vkCreateShaderModule(dev, &smci, NULL, &sm);
	VkDescriptorSetLayoutBinding binds[3] = {
		{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
	};
	VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 3, .pBindings = binds };
	VkDescriptorSetLayout dsl;
	vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout pl;
	vkCreatePipelineLayout(dev, &plci, NULL, &pl);
	VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
		.layout = pl };
	VkPipeline pipe;
	if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe) != VK_SUCCESS) {
		printf("vk_filter_probe: the shader did not compile\n");
		return 2;
	}
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32 } };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 16, .poolSizeCount = 2, .pPoolSizes = ps };
	VkDescriptorPool dp;
	vkCreateDescriptorPool(dev, &dpci, NULL, &dp);
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
	VkCommandPool pool;
	vkCreateCommandPool(dev, &cpci, NULL, &pool);
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	vkAllocateCommandBuffers(dev, &cbai, &cb);
	VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };

	double worst = 0;
	long sampled = 0;
	for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		const struct format *fm = &formats[f];
		uint8_t texels[W * H * 4];
		for (int k = 0; k < W * H * fm->bytes; k++)
			texels[k] = (uint8_t)rnd();
		memcpy(mem + texels_at, texels, sizeof(texels));

		VkImageCreateInfo imci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
			.format = fm->vk, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
		VkImage img;
		VkDeviceMemory imem;
		vkCreateImage(dev, &imci, NULL, &img);
		vkGetImageMemoryRequirements(dev, img, &mr);
		VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
			.memoryTypeIndex = find_memory(&mp, mr.memoryTypeBits, 0) };
		vkAllocateMemory(dev, &imai, NULL, &imem);
		vkBindImageMemory(dev, img, imem, 0);
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
			.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fm->vk, .subresourceRange = range };
		VkImageView view;
		vkCreateImageView(dev, &ivci, NULL, &view);

		vkResetCommandBuffer(cb, 0);
		vkBeginCommandBuffer(cb, &cbbi);
		VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = range };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
		    0, NULL, 1, &b1);
		VkBufferImageCopy copy = { .bufferOffset = texels_at,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
		vkCmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		VkImageMemoryBarrier b2 = b1;
		b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
		    0, NULL, 1, &b2);
		vkEndCommandBuffer(cb);
		vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
		vkQueueWaitIdle(queue);

		for (int m = 0; m < MODES; m++) {
			VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				.magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
				.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = vk_mode[m],
				.addressModeV = vk_mode[m], .addressModeW = vk_mode[m], .maxLod = 0.0f,
				.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
			VkSampler smp;
			vkCreateSampler(dev, &sci, NULL, &smp);
			VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
			VkDescriptorSet ds;
			vkAllocateDescriptorSets(dev, &dsai, &ds);
			VkDescriptorImageInfo dii = { smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			VkDescriptorBufferInfo in = { buf, coords_at, COORDS * 8 }, out = { buf, results_at, COORDS * 16 };
			VkWriteDescriptorSet wds[3] = {
				{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
				  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				  .pImageInfo = &dii },
				{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
				  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				  .pBufferInfo = &in },
				{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 2,
				  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				  .pBufferInfo = &out },
			};
			vkUpdateDescriptorSets(dev, 3, wds, 0, NULL);

			float u[COORDS], v[COORDS];
			for (int k = 0; k < COORDS; k++) {
				u[k] = coordinate(k, W);
				v[k] = coordinate(k + 1, H);
				coords[2 * k] = cell((enum mode)m, u[k], W);
				coords[2 * k + 1] = cell((enum mode)m, v[k], H);
			}
			memset(results, 0xff, COORDS * 16);
			vkResetCommandBuffer(cb, 0);
			vkBeginCommandBuffer(cb, &cbbi);
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
			vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
			vkCmdDispatch(cb, COORDS / 64, 1, 1);
			VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb,
			    0, NULL, 0, NULL);
			vkEndCommandBuffer(cb);
			if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS || vkQueueWaitIdle(queue) != VK_SUCCESS) {
				printf("vk_filter_probe: the dispatch failed\n");
				return 2;
			}

			double most = 0;
			int at = -1, at_c = 0;
			for (int k = 0; k < COORDS; k++) {
				long qx = metal_step((enum mode)m, u[k], W), qy = metal_step((enum mode)m, v[k], H);
				long x0 = (long)floor(qx / 256.0), y0 = (long)floor(qy / 256.0);
				long wx = qx - 256 * x0, wy = qy - 256 * y0;
				int xs[2] = { tap((enum mode)m, x0, W), tap((enum mode)m, x0 + 1, W) };
				int ys[2] = { tap((enum mode)m, y0, H), tap((enum mode)m, y0 + 1, H) };
				long weights[4] = { (256 - wx) * (256 - wy), wx * (256 - wy), (256 - wx) * wy, wx * wy };
				for (int c = 0; c < fm->channels; c++) {
					long sum = 0;   /* in 1/65536 of a code */
					for (int t = 0; t < 4; t++) {
						int x = xs[t & 1], y = ys[t >> 1];
						long code = x < 0 || y < 0 ? 0 : texels[(y * W + x) * fm->bytes + fm->byte_of[c]];
						sum += weights[t] * code;
					}
					double d = fabs((double)results[4 * k + c] * 255.0 - (double)sum / 65536.0);
					if (verbose && d >= 1.0)
						printf("  k %d (%s) q %ld,%ld w %ld,%ld taps %d,%d %d,%d codes %d %d %d %d: device %.4f metal %.4f\n",
						    k, k % 3 == 0 ? "edge" : "uniform", qx, qy, wx, wy, xs[0], xs[1], ys[0], ys[1],
						    xs[0] < 0 || ys[0] < 0 ? 0 : texels[(ys[0] * W + xs[0]) * fm->bytes + fm->byte_of[c]],
						    xs[1] < 0 || ys[0] < 0 ? 0 : texels[(ys[0] * W + xs[1]) * fm->bytes + fm->byte_of[c]],
						    xs[0] < 0 || ys[1] < 0 ? 0 : texels[(ys[1] * W + xs[0]) * fm->bytes + fm->byte_of[c]],
						    xs[1] < 0 || ys[1] < 0 ? 0 : texels[(ys[1] * W + xs[1]) * fm->bytes + fm->byte_of[c]],
						    results[4 * k + c] * 255.0, sum / 65536.0);
					if (!(d <= most)) {
						most = isnan(d) ? INFINITY : d;
						at = k;
						at_c = c;
					}
				}
				sampled++;
			}
			printf("vk_filter_probe: %-10s %-15s largest difference %.4f codes (coordinate %d (%.9g, %.9g), "
			    "channel %d)\n", fm->name, mode_name[m], most, at, at >= 0 ? u[at] : 0.0, at >= 0 ? v[at] : 0.0,
			    at_c);
			worst = most > worst ? most : worst;
		}
	}
	printf("vk_filter_probe: %ld samples; the largest difference from Metal's filter %.4f codes: %s\n", sampled,
	    worst, worst < 1.0 ? "within one code" : "ONE CODE OR MORE");
	return worst < 1.0 ? 0 : 1;
}
