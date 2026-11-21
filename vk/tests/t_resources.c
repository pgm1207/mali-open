/* SPDX-License-Identifier: MIT */
/* M5+M7 end-to-end (offline, fake backend): buffer + image + view + sampler + memory + bind, descriptor set layout/pool/set,
 * vkUpdateDescriptorSets and vkUpdateDescriptorSetWithTemplate, then the packed descriptor bytes of the set are compared with the
 * packers' output. White-box: linked statically with the driver objects (build.sh), calls the entrypoints through
 * vk_icdGetInstanceProcAddr exactly like a loader would, and reads the set memory through the driver's own structs.
 *   t_resources            (needs MALI_BACKEND=fake and MALI_FAKE_GPU=v11|v13 in the environment). Exit 0 = pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kbase_draw.h"
#include "mali_image.h"
#include "mali_pipeline_layout.h"
#include "mali_queue.h"
#include "vk_buffer.h"

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);

static int g_fail;

/* Linked with -Wl,--wrap=memset: counts memset(NULL, _, 0), which is UB even for n == 0 (UBSan nonnull-attribute). */
extern void *__real_memset(void *, int, size_t);
static int g_null_memset;
void *__wrap_memset(void *p, int c, size_t n);
void *__wrap_memset(void *p, int c, size_t n)
{
   if (!p && !n)
      g_null_memset++;
   return __real_memset(p, c, n);
}
#define CHECK(c, ...)                                                                                                    \
   do {                                                                                                                  \
      if (!(c)) {                                                                                                        \
         printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", __FILE__, __LINE__, #c);                           \
         printf("" __VA_ARGS__);                                                                                         \
         printf("\"}\n");                                                                                                \
         g_fail = 1;                                                                                                     \
         return 1;                                                                                                       \
      }                                                                                                                  \
   } while (0)

static VkInstance inst;
static VkDevice dev;
static PFN_vkVoidFunction (*gdpa)(VkDevice, const char *);
#define F(n) static PFN_vk##n p_##n;
#define LOAD(n) do { p_##n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!p_##n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; } } while (0)
F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements)
F(CreateImageView) F(DestroyImageView) F(CreateSampler) F(DestroySampler) F(CreateBufferView) F(DestroyBufferView)
F(AllocateMemory) F(FreeMemory) F(BindBufferMemory) F(BindImageMemory) F(CreateDescriptorSetLayout) F(DestroyDescriptorSetLayout)
F(CreateDescriptorPool) F(DestroyDescriptorPool) F(AllocateDescriptorSets) F(FreeDescriptorSets) F(UpdateDescriptorSets)
F(CreateDescriptorUpdateTemplateKHR) F(DestroyDescriptorUpdateTemplateKHR) F(UpdateDescriptorSetWithTemplateKHR)
F(CreatePipelineLayout) F(DestroyPipelineLayout) F(DestroyDevice)

static int
same(const void *a, const void *b, size_t n, const char *what)
{
   if (!memcmp(a, b, n)) return 0;
   const uint32_t *x = a, *y = b;
   printf("{\"fail\":\"bytes\",\"what\":\"%s\"", what);
   for (size_t i = 0; i < n / 4; i++)
      printf(",\"w%zu\":\"%08x/%08x\"", i, x[i], y[i]);
   printf("}\n");
   g_fail = 1;
   return 1;
}

static int
run(void)
{
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   CHECK(ci(&ici, NULL, &inst) == VK_SUCCESS, "instance");
   PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)vk_icdGetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
   uint32_t n = 1;
   VkPhysicalDevice pd;
   CHECK(en(inst, &n, &pd) == VK_SUCCESS && n == 1, "no device (MALI_BACKEND=fake?)");
   PFN_vkCreateDevice cd = (PFN_vkCreateDevice)vk_icdGetInstanceProcAddr(inst, "vkCreateDevice");
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   const char *exts[] = {"VK_KHR_descriptor_update_template"};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts};
   CHECK(cd(pd, &dci, NULL, &dev) == VK_SUCCESS, "device");
   gdpa = (PFN_vkVoidFunction(*)(VkDevice, const char *))vk_icdGetInstanceProcAddr(inst, "vkGetDeviceProcAddr");
   LOAD(CreateBuffer); LOAD(DestroyBuffer); LOAD(GetBufferMemoryRequirements); LOAD(CreateImage); LOAD(DestroyImage);
   LOAD(GetImageMemoryRequirements); LOAD(CreateImageView); LOAD(DestroyImageView); LOAD(CreateSampler); LOAD(DestroySampler);
   LOAD(CreateBufferView); LOAD(DestroyBufferView); LOAD(AllocateMemory); LOAD(FreeMemory); LOAD(BindBufferMemory);
   LOAD(BindImageMemory); LOAD(CreateDescriptorSetLayout); LOAD(DestroyDescriptorSetLayout); LOAD(CreateDescriptorPool);
   LOAD(DestroyDescriptorPool); LOAD(AllocateDescriptorSets); LOAD(FreeDescriptorSets); LOAD(UpdateDescriptorSets);
   LOAD(CreateDescriptorUpdateTemplateKHR); LOAD(DestroyDescriptorUpdateTemplateKHR); LOAD(UpdateDescriptorSetWithTemplateKHR);
   LOAD(CreatePipelineLayout); LOAD(DestroyPipelineLayout); LOAD(DestroyDevice);
   struct mali_device *md = mali_device_from_handle(dev);
   const unsigned arch = mali_device_pdev(md)->gpu.arch_major;

   /* buffer */
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buf;
   CHECK(p_CreateBuffer(dev, &bci, NULL, &buf) == VK_SUCCESS, "buffer");
   VkMemoryRequirements br, ir;
   p_GetBufferMemoryRequirements(dev, buf, &br);
   CHECK(br.size == 4096 && br.alignment >= 256 && br.memoryTypeBits == 3, "buffer reqs %llu/%llu/%x", (unsigned long long)br.size,
         (unsigned long long)br.alignment, br.memoryTypeBits);
   /* image */
   VkImageCreateInfo ici2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {8, 8, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img;
   CHECK(p_CreateImage(dev, &ici2, NULL, &img) == VK_SUCCESS, "image");
   p_GetImageMemoryRequirements(dev, img, &ir);
   CHECK(ir.size >= 8 * 8 * 4 && ir.alignment >= 64, "image reqs %llu", (unsigned long long)ir.size);
   /* memory + bind */
   uint64_t img_off = (br.size + ir.alignment - 1) & ~(ir.alignment - 1);
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = img_off + ir.size, .memoryTypeIndex = 0};
   VkDeviceMemory mem;
   CHECK(p_AllocateMemory(dev, &mai, NULL, &mem) == VK_SUCCESS, "memory");
   CHECK(p_BindBufferMemory(dev, buf, mem, 0) == VK_SUCCESS, "bind buffer");
   CHECK(p_BindImageMemory(dev, img, mem, img_off) == VK_SUCCESS, "bind image");
   struct mali_memory *mm = mali_memory_from_handle(mem);
   struct mali_buffer *mb = mali_buffer_from_handle(buf);
   CHECK(mb->vk.device_address == mm->bo.gpu_va, "buffer va");
   CHECK(mali_image_from_handle(img)->va == mm->bo.gpu_va + img_off, "image va");
   /* F (#240): Vulkan 1.4 moves the usage mask into VkBufferUsageFlags2CreateInfoKHR, and VkBufferCreateInfo::usage must
    * then be 0. The front end reads vk.usage for every usage decision (INDIRECT: host touch at bind time, indirect memory
    * types), so a chain-only buffer must still record INDIRECT. */
   VkBufferUsageFlags2CreateInfoKHR u2ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
                                            .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
   VkBufferCreateInfo u2bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .pNext = &u2ci,
                               .usage = 0, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer u2buf;
   CHECK(p_CreateBuffer(dev, &u2bci, NULL, &u2buf) == VK_SUCCESS, "chain-only usage buffer");
   const VkBufferUsageFlags u2 = mali_buffer_from_handle(u2buf)->vk.usage;
   CHECK(u2 & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "VkBufferUsageFlags2CreateInfoKHR ignored: usage %08x", u2);
   CHECK(u2 & VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "chain-only TRANSFER_SRC lost: usage %08x", u2);
   p_DestroyBuffer(dev, u2buf, NULL);
   /* negative control: the bit must come from the mask, not from "any chain is indirect" */
   u2ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
   CHECK(p_CreateBuffer(dev, &u2bci, NULL, &u2buf) == VK_SUCCESS, "chain-only TRANSFER_SRC buffer");
   CHECK(!(mali_buffer_from_handle(u2buf)->vk.usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT),
         "a chain-only TRANSFER_SRC buffer gained INDIRECT");
   p_DestroyBuffer(dev, u2buf, NULL);
   /* view, sampler, texel buffer view */
   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView view;
   CHECK(p_CreateImageView(dev, &vci, NULL, &view) == VK_SUCCESS, "view");
   VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE};
   VkSampler smp;
   CHECK(p_CreateSampler(dev, &sci, NULL, &smp) == VK_SUCCESS, "sampler");
   VkBufferViewCreateInfo bvci = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buf,
                                  .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 256, .range = 1024};
   VkBufferView bview;
   CHECK(p_CreateBufferView(dev, &bvci, NULL, &bview) == VK_SUCCESS, "buffer view");

   /* layout: 0 UBO, 1 combined image sampler, 2 storage buffer, 3 uniform texel buffer */
   VkDescriptorSetLayoutBinding lb[4] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, NULL},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL},
      {3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL},
   };
   VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = lb};
   VkDescriptorSetLayout dsl;
   CHECK(p_CreateDescriptorSetLayout(dev, &lci, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout pl;
   CHECK(p_CreatePipelineLayout(dev, &plci, NULL, &pl) == VK_SUCCESS, "pipeline layout");
   struct mali_flat_layout fl;
   CHECK(mali_pipeline_layout_flatten(vk_pipeline_layout_from_handle(pl), &fl) == VK_SUCCESS && fl.c.nsets == 1 &&
            fl.c.set[0].slot_count == 5, "flatten slots=%u", fl.c.set[0].slot_count);
   VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2}};
   VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 4, .pPoolSizes = ps};
   VkDescriptorPool pool;
   CHECK(p_CreateDescriptorPool(dev, &pci, NULL, &pool) == VK_SUCCESS, "pool");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet set;
   CHECK(p_AllocateDescriptorSets(dev, &dai, &set) == VK_SUCCESS, "set");

   {  /* zero-capacity pool + zero-binding layout: alloc_set must not memset(NULL, 0, 0) */
      VkDescriptorSetLayoutCreateInfo elci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      VkDescriptorSetLayout edsl;
      CHECK(p_CreateDescriptorSetLayout(dev, &elci, NULL, &edsl) == VK_SUCCESS, "empty dsl");
      VkDescriptorPoolCreateInfo epci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1};
      VkDescriptorPool epool;
      CHECK(p_CreateDescriptorPool(dev, &epci, NULL, &epool) == VK_SUCCESS, "empty pool");
      VkDescriptorSetAllocateInfo edai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = epool,
                                          .descriptorSetCount = 1, .pSetLayouts = &edsl};
      VkDescriptorSet eset;
      int before = g_null_memset;
      CHECK(p_AllocateDescriptorSets(dev, &edai, &eset) == VK_SUCCESS, "empty set");
      CHECK(g_null_memset == before, "memset(NULL,0,0) in alloc_set");
      p_DestroyDescriptorPool(dev, epool, NULL);
      p_DestroyDescriptorSetLayout(dev, edsl, NULL);
   }

   VkDescriptorBufferInfo bi0 = {buf, 256, 512}, bi2 = {buf, 1024, VK_WHOLE_SIZE};
   VkDescriptorImageInfo ii = {smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet w[4] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi0},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi2},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &bview},
   };
   p_UpdateDescriptorSets(dev, 4, w, 0, NULL);

   /* expected bytes, built independently of the set */
   struct mali_dset *s = mali_dset_from_handle(set);
   const uint64_t va = mm->bo.gpu_va;
   struct mali_hw_desc e[5], tex_e;
   mali_pack_buffer(arch, &e[0], va + 256, 512);
   struct mali_image_view *iv = mali_image_view_from_handle(view);
   CHECK(mali_image_view_desc(view, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &tex_e) == 0, "view desc");
   CHECK(iv->payload_va != 0 && tex_e.w[4] == (uint32_t)iv->payload_va, "texture payload pointer");
   e[1] = tex_e;
   /* M5 sampler = libkbase's nearest/clamp sampler (mali_pack_sampler) plus genxml defaults (bits 21, 26) and maxLod (word 1) */
   struct mali_hw_desc ks;
   mali_pack_sampler(arch, &ks, 1, 1, 9);
   e[2] = mali_sampler_from_handle(smp)->hw;
   CHECK((e[2].w[0] ^ ks.w[0]) == ((1u << 21) | (1u << 26)), "sampler word0 %08x vs %08x", e[2].w[0], ks.w[0]);
   CHECK(!memcmp(&e[2].w[2], &ks.w[2], 24), "sampler tail");
   mali_pack_buffer(arch, &e[3], va + 1024, 4096 - 1024);
   struct mali_fmt f;
   CHECK(mali_format_get(arch, VK_FORMAT_R32G32B32A32_SFLOAT, &f), "fmt");
   mali_pack_texel_buffer(&e[4], &f, va + 256, 1024);
   CHECK(s->layout->slot_count == 5, "slot_count");
   /* slots: 0 UBO, 1-2 texture + sampler, 3 SSBO, 4 texel buffer */
   if (same(s->cpu + 0 * 32, &e[0], 32, "ubo")) return 1;
   if (same(s->cpu + 1 * 32, &e[1], 32, "texture")) return 1;
   if (same(s->cpu + 2 * 32, &e[2], 32, "sampler")) return 1;
   if (same(s->cpu + 3 * 32, &e[3], 32, "ssbo whole-size")) return 1;
   if (same(s->cpu + 4 * 32, &e[4], 32, "texel buffer")) return 1;
   /* spot checks on literal bytes: buffer descriptor = size, va; texture is a 2D RGBA8 8x8 */
   const uint32_t *u = (const uint32_t *)s->cpu;
   CHECK(u[1] == 512 && u[2] == (uint32_t)(va + 256) && u[3] == (uint32_t)((va + 256) >> 32), "ubo literal %u", u[1]);
   CHECK(((const uint32_t *)(s->cpu + 4 * 32))[4] == 16 && ((const uint32_t *)(s->cpu + 4 * 32))[1] == 1024, "texel literal");
   CHECK(s->va != 0 && s->cpu != NULL, "set memory");

   /* template: rewrite everything with different offsets, compare with a plain update of the same values */
   struct {
      VkDescriptorBufferInfo ubo, ssbo;
      VkDescriptorImageInfo img;
   } td = {{buf, 512, 256}, {buf, 2048, 128}, ii};
   VkDescriptorUpdateTemplateEntry te[3] = {
      {0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, offsetof(__typeof__(td), ubo), sizeof(VkDescriptorBufferInfo)},
      {1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, offsetof(__typeof__(td), img), sizeof(VkDescriptorImageInfo)},
      {2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, offsetof(__typeof__(td), ssbo), sizeof(VkDescriptorBufferInfo)},
   };
   VkDescriptorUpdateTemplateCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
      .descriptorUpdateEntryCount = 3, .pDescriptorUpdateEntries = te,
      .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, .descriptorSetLayout = dsl};
   VkDescriptorUpdateTemplate tpl;
   CHECK(p_CreateDescriptorUpdateTemplateKHR(dev, &tci, NULL, &tpl) == VK_SUCCESS, "template");
   p_UpdateDescriptorSetWithTemplateKHR(dev, set, tpl, &td);
   mali_pack_buffer(arch, &e[0], va + 512, 256);
   mali_pack_buffer(arch, &e[3], va + 2048, 128);
   if (same(s->cpu + 0 * 32, &e[0], 32, "template ubo")) return 1;
   if (same(s->cpu + 1 * 32, &e[1], 32, "template texture")) return 1;
   if (same(s->cpu + 3 * 32, &e[3], 32, "template ssbo")) return 1;
   if (same(s->cpu + 4 * 32, &e[4], 32, "template texel untouched")) return 1;
   p_DestroyDescriptorUpdateTemplateKHR(dev, tpl, NULL);

   /* free, reset, teardown */
   p_DestroyDescriptorPool(dev, pool, NULL);
   p_DestroyPipelineLayout(dev, pl, NULL);
   p_DestroyDescriptorSetLayout(dev, dsl, NULL);
   p_DestroyBufferView(dev, bview, NULL);
   p_DestroySampler(dev, smp, NULL);
   p_DestroyImageView(dev, view, NULL);
   p_DestroyImage(dev, img, NULL);
   p_DestroyBuffer(dev, buf, NULL);
   p_FreeMemory(dev, mem, NULL);
   p_DestroyDevice(dev, NULL);
   return 0;
}

/* kb_draw_attr_instance (VK_EXT_vertex_attribute_divisor descriptor encoding): type, frequency, R/E/D against an independent 128-bit-free
 * reconstruction: (D + 2^31) * divisor must equal 2^(32+R) to within one divisor (E records the rounding direction). */
static int
test_attr_instance(void)
{
   uint32_t a[8];
   kb_draw_attr_1d(a, 0x123, 0, 18, 16, 8);
   kb_draw_attr_instance(a, 1);
   CHECK(((a[0] >> 4) & 0xf) == 1 && !(a[0] & (1u << 8)) && ((a[1] >> 6) & 3) == 1 && a[5] == 0 && a[2] == 16 && a[3] == 18 && a[4] == 8,
         "divisor 1: %08x %08x", a[0], a[1]);
   kb_draw_attr_1d(a, 0x123, 0, 18, 16, 8);
   kb_draw_attr_instance(a, 8);
   CHECK(((a[0] >> 4) & 0xf) == 2 && ((a[1] >> 24) & 31) == 3 && !((a[1] >> 29) & 1) && a[5] == 0, "divisor 8 (POT): %08x %08x %08x", a[0], a[1], a[5]);
   static const struct { uint32_t div, d, r, e; } npot[] = {{3, 715827883, 1, 0}, {5, 1288490189, 2, 0}, {7, 306783378, 2, 1}, {6, 715827883, 2, 0},
                                                           {1000, 51539608, 9, 0}, {65535, 32769, 15, 0}, {0xfffffff, 8, 27, 1}};
   for (unsigned i = 0; i < sizeof npot / sizeof *npot; i++) {
      kb_draw_attr_1d(a, 0x123, 0, 18, 16, 8);
      kb_draw_attr_instance(a, npot[i].div);
      CHECK(((a[0] >> 4) & 0xf) == 3 && ((a[1] >> 6) & 3) == 1 && ((a[1] >> 24) & 31) == npot[i].r && ((a[1] >> 29) & 1) == npot[i].e && a[5] == npot[i].d,
            "divisor %u: type %u R %u E %u D %u", npot[i].div, (a[0] >> 4) & 0xf, (a[1] >> 24) & 31, (a[1] >> 29) & 1, a[5]);
   }
   for (uint32_t d = 3; d < 5000; d += 2) {   /* odd: always NPOT */
      kb_draw_attr_1d(a, 0x123, 0, 18, 16, 8);
      kb_draw_attr_instance(a, d);
      unsigned r = (a[1] >> 24) & 31;
      uint64_t m = (uint64_t)((uint64_t)a[5] + ((uint64_t)1 << 31)) * d, want = (uint64_t)1 << (32 + r);
      CHECK(r == 31 - (unsigned)__builtin_clz(d) && (m > want ? m - want : want - m) <= d / 2 + 1, "divisor %u: D %u R %u", d, a[5], r);
   }
   printf("{\"test\":\"attr_instance\",\"ok\":true}\n");
   return 0;
}

static int g_allocs, g_frees;
static void *
cnt_alloc(void *u, size_t n, size_t al, VkSystemAllocationScope sc)
{
   g_allocs++;
   return aligned_alloc(al > 16 ? al : 16, (n + 15) & ~(size_t)15);
}
static void *
cnt_realloc(void *u, void *p, size_t n, size_t al, VkSystemAllocationScope sc)
{
   if (!p) g_allocs++;
   return realloc(p, n);
}
static void
cnt_free(void *u, void *p)
{
   if (p) g_frees++;
   free(p);
}

/* Leak regressions: event/query-pool BOs freed through the owning device, foreign-set free, pool allocator callbacks. */
static int
test_leaks(void)
{
   uint32_t n = 1;
   VkPhysicalDevice pd;
   PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)vk_icdGetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
   CHECK(en(inst, &n, &pd) == VK_SUCCESS && n == 1, "pd");
   PFN_vkCreateDevice cd = (PFN_vkCreateDevice)vk_icdGetInstanceProcAddr(inst, "vkCreateDevice");
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   VkDevice A, B;
   CHECK(cd(pd, &dci, NULL, &A) == VK_SUCCESS, "device A");
   PFN_vkVoidFunction (*gp)(VkDevice, const char *) = (PFN_vkVoidFunction(*)(VkDevice, const char *))vk_icdGetInstanceProcAddr(inst, "vkGetDeviceProcAddr");
   PFN_vkCreateEvent cev = (PFN_vkCreateEvent)gp(A, "vkCreateEvent");
   PFN_vkDestroyEvent dev_ = (PFN_vkDestroyEvent)gp(A, "vkDestroyEvent");
   PFN_vkCreateQueryPool cqp = (PFN_vkCreateQueryPool)gp(A, "vkCreateQueryPool");
   PFN_vkDestroyQueryPool dqp = (PFN_vkDestroyQueryPool)gp(A, "vkDestroyQueryPool");
   PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)gp(A, "vkDestroyDevice");
   PFN_vkCreateDescriptorSetLayout cdsl = (PFN_vkCreateDescriptorSetLayout)gp(A, "vkCreateDescriptorSetLayout");
   PFN_vkDestroyDescriptorSetLayout ddsl = (PFN_vkDestroyDescriptorSetLayout)gp(A, "vkDestroyDescriptorSetLayout");
   PFN_vkCreateDescriptorPool cdp = (PFN_vkCreateDescriptorPool)gp(A, "vkCreateDescriptorPool");
   PFN_vkDestroyDescriptorPool ddp = (PFN_vkDestroyDescriptorPool)gp(A, "vkDestroyDescriptorPool");
   PFN_vkAllocateDescriptorSets ads = (PFN_vkAllocateDescriptorSets)gp(A, "vkAllocateDescriptorSets");
   PFN_vkFreeDescriptorSets fds = (PFN_vkFreeDescriptorSets)gp(A, "vkFreeDescriptorSets");
   CHECK(cev && dev_ && cqp && dqp && dd && cdsl && ddsl && cdp && ddp && ads && fds, "entrypoints");
   struct mali_device *ma = mali_device_from_handle(A);
   unsigned base = mali_fake_live_bos(ma->kdev);

   VkEventCreateInfo eci = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
   VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 8};
   VkEvent ev;
   VkQueryPool qp;
   CHECK(cev(A, &eci, NULL, &ev) == VK_SUCCESS, "event");
   CHECK(cqp(A, &qci, NULL, &qp) == VK_SUCCESS, "qpool");
   CHECK(mali_fake_live_bos(ma->kdev) > base, "event/qpool BOs live on A");
   CHECK(cd(pd, &dci, NULL, &B) == VK_SUCCESS, "device B");   /* B is now the bound device (g_cur) */
   struct mali_device *mb = mali_device_from_handle(B);
   unsigned bbase = mali_fake_live_bos(mb->kdev);
   dev_(A, ev, NULL);
   dqp(A, qp, NULL);
   CHECK(mali_fake_live_bos(ma->kdev) == base, "A BOs freed via A: %u vs %u", mali_fake_live_bos(ma->kdev), base);
   CHECK(mali_fake_live_bos(mb->kdev) == bbase, "B untouched");

   /* create on A while B is the bound device, destroy B first, then free A's objects (no dangling owner, BOs go back to A) */
   CHECK(cev(A, &eci, NULL, &ev) == VK_SUCCESS, "event while B bound");
   CHECK(cqp(A, &qci, NULL, &qp) == VK_SUCCESS, "qpool while B bound");
   CHECK(mali_fake_live_bos(ma->kdev) > base, "BOs live on A (B bound)");
   CHECK(mali_fake_live_bos(mb->kdev) == bbase, "nothing allocated on B");
   dd(B, NULL);
   B = VK_NULL_HANDLE;
   dev_(A, ev, NULL);
   dqp(A, qp, NULL);
   CHECK(mali_fake_live_bos(ma->kdev) == base, "A BOs freed after B destroyed: %u vs %u", mali_fake_live_bos(ma->kdev), base);
   CHECK(cd(pd, &dci, NULL, &B) == VK_SUCCESS, "device B again");
   mb = mali_device_from_handle(B);

   /* wrong-pool free must not unlink/free/decrement */
   VkDescriptorSetLayoutBinding bd = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL};
   VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bd};
   VkDescriptorSetLayout l;
   CHECK(cdsl(A, &lci, NULL, &l) == VK_SUCCESS, "layout");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
   VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                     .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkAllocationCallbacks ac = {.pfnAllocation = cnt_alloc, .pfnReallocation = cnt_realloc, .pfnFree = cnt_free};
   VkDescriptorPool p1, p2;
   g_allocs = g_frees = 0;
   CHECK(cdp(A, &pci, &ac, &p1) == VK_SUCCESS, "pool1");
   CHECK(g_allocs > 0, "pool create used app allocator");
   CHECK(cdp(A, &pci, NULL, &p2) == VK_SUCCESS, "pool2");
   VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p1, .descriptorSetCount = 1, .pSetLayouts = &l};
   VkDescriptorSet s;
   CHECK(ads(A, &ai, &s) == VK_SUCCESS, "alloc set");
   struct mali_dpool *dp1 = mali_dpool_from_handle(p1);
   CHECK(dp1->nsets == 1, "nsets 1");
   CHECK(fds(A, p2, 1, &s) == VK_SUCCESS, "free via wrong pool");
   CHECK(dp1->nsets == 1 && dp1->sets, "set still live in p1 (nsets %u)", dp1->nsets);
   CHECK(fds(A, p1, 1, &s) == VK_SUCCESS && dp1->nsets == 0, "free via owner");
   ddp(A, p1, &ac);
   CHECK(g_frees == g_allocs, "allocator balanced: %d allocs %d frees", g_allocs, g_frees);
   ddp(A, p2, NULL);
   ddsl(A, l, NULL);
   dd(B, NULL);
   dd(A, NULL);
   printf("{\"test\":\"leaks\",\"ok\":true}\n");
   return 0;
}

int
main(void)
{
   const char *b = getenv("MALI_BACKEND");
   if (!b || strcmp(b, "fake")) {
      printf("{\"test\":\"t_resources\",\"pass\":false,\"error\":\"needs MALI_BACKEND=fake\"}\n");
      return 1;
   }
   int r = test_attr_instance();
   if (!r) r = run();
   if (!r) r = test_leaks();
   printf("{\"test\":\"t_resources\",\"pass\":%s}\n", r || g_fail ? "false" : "true");
   return r || g_fail;
}
