# DXVK / VKD3D-Proton gap (ours vs what they need), re-verified 2026-11-18 against upstream source

(a) = the project refuses to create a device without it, (b) = common game paths, (c) = optional. Effort S/M/L. "HW" = does Mali + kbase already do it
(PanVK on the same GPU is the oracle). Source of truth for what is advertised: `t_dxvk_caps` + `mali_physical_device.c`.

**Verified from source** (doitsujin/dxvk `src/dxvk/dxvk_device_info.cpp` master, v2.7.1, v2.0 `dxvk_adapter.cpp`; vkd3d-proton `libs/vkd3d/device.c`):
- DXVK 2.x AND 3.x list these core features as REQUIRED (`ENABLE_FEATURE(..., true)`): depthBiasClamp, depthClamp, dualSrcBlend, fillModeNonSolid, fragmentStoresAndAtomics,
  fullDrawIndexUint32, **geometryShader**, imageCubeArray, independentBlend, multiDrawIndirect, multiViewport, occlusionQueryPrecise, **robustBufferAccess**,
  **sampleRateShading**, samplerAnisotropy, shaderClipDistance, **shaderCullDistance**, shaderImageGatherExtended, shaderInt16, shaderInt64,
  shaderSampledImageArrayDynamicIndexing, textureCompressionBC. tessellationShader, logicOp, depthBounds, shaderFloat64, wideLines are optional.
  Required extensions/features: KHR_swapchain, EXT_depth_clip_enable, KHR_load_store_op_none, KHR_maintenance5/6, EXT_robustness2 (**robustBufferAccess2** + nullDescriptor; robustImageAccess2 is not required by DXVK).
- EXT_transform_feedback is OPTIONAL in the code (`ENABLE_EXT_FEATURE(extTransformFeedback, transformFeedback, false)`, master and 2.7.1), although the DXVK 3.x wiki lists it as required. Without it D3D11 stream-out is unavailable.
- Wiki: DXVK 3.x needs Vulkan 1.4, >=256 B push constants, descriptor indexing, scalarBlockLayout, 8/16/64-bit ints. Checked against the code at pin 25ca63f1 (DXVK 3.1.1): the Vulkan 1.4 claim is **wrong** (`DxvkVulkanApiVersion = VK_API_VERSION_1_3`, `dxvk_instance.h:12`; our 1.3.363 passes); 256 B push constants and `descriptorIndexing` are real hard gates (`dxvk_device_info.cpp:790`, `:899`). See docs/STATUS.md.
- VKD3D-Proton does NOT require geometryShader or tessellationShader (device.c only traces them); it requires robustBufferAccess2 + robustImageAccess2, nullDescriptor,
  samplerMirrorClampToEdge, shaderDrawParameters, **VK_KHR_push_descriptor**, maintenance5 + maintenance6, descriptor indexing (1,000,000 update-after-bind, all types but UBO). image_view_min_lod is recommended.
- Consequence: the hard blockers for DXVK are geometryShader, robustBufferAccess(2), sampleRateShading, shaderCullDistance; for VKD3D-Proton robustBufferAccess2/robustImageAccess2, push_descriptor and full descriptor indexing.

| item | class | effort | HW / state |
|---|---|---|---|
| VK_KHR_swapchain, VK_EXT_robustness2 (nullDescriptor) | a | - | done |
| VK_EXT_depth_clip_enable | a | S | done: ZSD clip bit was already driven by the runtime state; test `t_vk_draw fx_dclip_*` (pixels, hardware) |
| VK_KHR_load_store_op_none | a | S | done: NONE load is a discard like DONT_CARE (no preload draw); only LOAD preloads (per attachment, colour/depth/stencil); colour is always stored |
| VK_KHR_maintenance6 | a | S | done: BindDescriptorSets2/PushConstants2 forward to 1.0 paths; no descriptor-buffer parts |
| VK_KHR_maintenance5 | a | M | HW yes. Needs module-less stages (mali_pipeline.c:80), CmdBindIndexBuffer2, vkGetRenderingAreaGranularity, vkGetDeviceImageSubresourceLayout, VK_FORMAT_A1B5G5R5/A8_UNORM, default point size |
| depth/stencil loadOp LOAD (core, not an extension) | a+ | L | done on v11 hardware (Mali-G615, `t_vk_draw load_depth load_depth16 load_stencil`, farm 20261117-182858-25ecccaf); v13 pixels untested. Preload draw (`mali_meta_preload`, `shader/preload_zs.frag`) writes gl_FragDepth / stencil export, as PanVK does. D32_SFLOAT and D16_UNORM depth, stencil, single-sampled. Still rejected: MSAA depth/stencil LOAD, D24 depth LOAD (`mali_cmd.c` "depth loadOp LOAD ..."). What was wrong: `mali_compile.c` never recorded FRAG_RESULT_DEPTH/STENCIL in `outputs_written` before `pan_nir_lower_fs_outputs`, so `fs.writes_depth/stencil` were 0 for every shader: no ZSD depth source = Shader, no stencil-from-shader, no force-late ZS (`mali_cs_emit.c`). Same bug made any application gl_FragDepth shader wrong. Also needed: Z/S-only FS has no render target in the DCD (rt mask, write mask, blend count 0), otherwise the GPU hangs |
| VK_EXT_vertex_attribute_divisor | a | M | done for zero divisor; divisor > 1 stays off (hardware divides firstInstance + i) |
| VK_EXT_custom_border_color | a | M | done (`t_vk_feat cbc`) |
| VK_KHR_calibrated_timestamps | a | S | done: backend `gpu_timestamp` (GET_CPU_GPU_TIMEINFO) + runtime `vk_common_GetCalibratedTimestampsKHR`; `t_dxvk_caps` |
| VK_EXT_memory_budget | a | S | done: usage = live vkAllocateMemory bytes, budget = heap; `t_dxvk_caps` alloc/free check |
| VK_EXT_extended_dynamic_state3 | c | M | runtime tracks the state; each dynamic state must be honoured in mali_cmd.c snapshot (sample mask, blend, polygon mode, ...) |
| VK_KHR_pipeline_library + VK_EXT_graphics_pipeline_library | a (3.x) | L | needs pipeline split compile + link; optional in DXVK 2.7 |
| core: robustBufferAccess, sampleRateShading (env flag only), shaderCullDistance | a | M each | HW/compiler can; robustBufferAccess needs bounds-checked lowering |
| core: dualSrcBlend | a | M | done (blend shader, attachment 0; `t_vk_draw fx_dual_*`) |
| core: geometryShader | a (DXVK) | L | REQUIRED by DXVK 2.x/3.x (verified); not by VKD3D-Proton. Valhall has no GS hardware and PanVK does not offer it; needs compiler-side emulation (compute pass) |
| core: descriptorIndexing (full) | a | L | needs update-after-bind for all types (only sampled images/samplers today) |
| Sample-count 2/8, shaderStorageImageMultisample, tessellation | b/c | L | not planned |

| VK_EXT_transform_feedback | c (code) / required (3.x wiki) | L | no XFB hardware on Valhall; needs NIR lowering of VS outputs to buffer writes |
| VK_KHR_push_descriptor | a (VKD3D-Proton) | M | see worktree feat/push-descriptor |
| VK_EXT_image_view_min_lod | c (VKD3D-Proton recommended) | S/M | per-view LOD clamp in the image descriptor |
