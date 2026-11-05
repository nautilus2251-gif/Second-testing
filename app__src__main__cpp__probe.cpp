// Mali-G52 Vulkan probe: finds out which device/pipeline setups the driver rejects.
// Headless (no surface needed). Results go to a string shown on screen and saved to a file.
#include <jni.h>
#include <android/log.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

std::string g_out;
std::map<std::string, std::vector<uint32_t>> g_spv;

void line(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  g_out += buf;
  g_out += '\n';
  __android_log_print(ANDROID_LOG_INFO, "G52Probe", "%s", buf);
}

const char* vkr(VkResult r) {
  switch (r) {
#define R(x) \
  case x:    \
    return #x;
    R(VK_SUCCESS)
    R(VK_NOT_READY)
    R(VK_TIMEOUT)
    R(VK_INCOMPLETE)
    R(VK_ERROR_OUT_OF_HOST_MEMORY)
    R(VK_ERROR_OUT_OF_DEVICE_MEMORY)
    R(VK_ERROR_INITIALIZATION_FAILED)
    R(VK_ERROR_DEVICE_LOST)
    R(VK_ERROR_MEMORY_MAP_FAILED)
    R(VK_ERROR_LAYER_NOT_PRESENT)
    R(VK_ERROR_EXTENSION_NOT_PRESENT)
    R(VK_ERROR_FEATURE_NOT_PRESENT)
    R(VK_ERROR_INCOMPATIBLE_DRIVER)
    R(VK_ERROR_TOO_MANY_OBJECTS)
    R(VK_ERROR_FORMAT_NOT_SUPPORTED)
    R(VK_ERROR_FRAGMENTED_POOL)
    R(VK_ERROR_UNKNOWN)
#undef R
    default: {
      static thread_local char b[40];
      snprintf(b, sizeof b, "VkResult(%d)", (int)r);
      return b;
    }
  }
}

const char* yn(VkBool32 b) { return b ? "yes" : "no"; }

std::string verStr(uint32_t v) {
  char b[48];
  snprintf(b, sizeof b, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
           VK_API_VERSION_PATCH(v));
  return b;
}

VkShaderModule makeShader(VkDevice d, const char* name, bool log = true) {
  auto it = g_spv.find(name);
  if (it == g_spv.end()) {
    if (log) line("    (shader %s missing from assets)", name);
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = it->second.size() * 4;
  ci.pCode = it->second.data();
  VkShaderModule m = VK_NULL_HANDLE;
  VkResult r = vkCreateShaderModule(d, &ci, nullptr, &m);
  if (r != VK_SUCCESS) {
    if (log) line("    vkCreateShaderModule(%s) -> %s", name, vkr(r));
    return VK_NULL_HANDLE;
  }
  return m;
}

VkRenderPass makeRenderPass(VkDevice d, VkFormat color, VkFormat depth) {
  VkAttachmentDescription att[2]{};
  att[0].format = color;
  att[0].samples = VK_SAMPLE_COUNT_1_BIT;
  att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att[1] = att[0];
  att[1].format = depth;
  att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sp{};
  sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sp.colorAttachmentCount = 1;
  sp.pColorAttachments = &cref;
  if (depth != VK_FORMAT_UNDEFINED) sp.pDepthStencilAttachment = &dref;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = depth != VK_FORMAT_UNDEFINED ? 2 : 1;
  rp.pAttachments = att;
  rp.subpassCount = 1;
  rp.pSubpasses = &sp;
  VkRenderPass out = VK_NULL_HANDLE;
  VkResult r = vkCreateRenderPass(d, &rp, nullptr, &out);
  if (r != VK_SUCCESS) {
    line("    vkCreateRenderPass -> %s", vkr(r));
    return VK_NULL_HANDLE;
  }
  return out;
}

struct GfxOpts {
  const char* vs = "tri_vert";
  const char* fs = "tri_frag";
  bool vertexInput = false;
  bool dynamicRendering = false;
  bool robustness = false;
  VkFormat dynFormat = VK_FORMAT_R8G8B8A8_UNORM;
};

VkResult makeGraphics(VkDevice d, VkPipelineLayout layout, VkRenderPass rp, VkPipelineCache cache,
                      const GfxOpts& o, std::string* note) {
  VkShaderModule vs = makeShader(d, o.vs, false);
  VkShaderModule fs = makeShader(d, o.fs, false);
  if (!vs || !fs) {
    if (vs) vkDestroyShaderModule(d, vs, nullptr);
    if (fs) vkDestroyShaderModule(d, fs, nullptr);
    *note = " (shader module creation failed)";
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkPipelineShaderStageCreateInfo st[2]{};
  st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  st[0].module = vs;
  st[0].pName = "main";
  st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  st[1].module = fs;
  st[1].pName = "main";

  VkVertexInputBindingDescription bind{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  if (o.vertexInput) {
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;
  }
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dsci{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dsci.dynamicStateCount = 2;
  dsci.pDynamicStates = dyn;

  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = st;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dsci;
  gp.layout = layout;
  gp.renderPass = o.dynamicRendering ? VK_NULL_HANDLE : rp;

  VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkPipelineRobustnessCreateInfoEXT rob{VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT};
  const void* chain = nullptr;
  if (o.robustness) {
    rob.storageBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
    rob.uniformBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
    rob.vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
    rob.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT;
    rob.pNext = const_cast<void*>(chain);
    chain = &rob;
  }
  if (o.dynamicRendering) {
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &o.dynFormat;
    rend.pNext = chain;
    chain = &rend;
  }
  gp.pNext = chain;

  VkPipeline p = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(d, cache, 1, &gp, nullptr, &p);
  if (p) vkDestroyPipeline(d, p, nullptr);
  vkDestroyShaderModule(d, vs, nullptr);
  vkDestroyShaderModule(d, fs, nullptr);
  return r;
}

VkResult makeCompute(VkDevice d, VkPipelineLayout layout, const char* cs) {
  VkShaderModule m = makeShader(d, cs, false);
  if (!m) return VK_ERROR_INITIALIZATION_FAILED;
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  ci.stage.module = m;
  ci.stage.pName = "main";
  ci.layout = layout;
  VkPipeline p = VK_NULL_HANDLE;
  VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
  if (p) vkDestroyPipeline(d, p, nullptr);
  vkDestroyShaderModule(d, m, nullptr);
  return r;
}

void report(const char* label, VkResult r, const std::string& note = "") {
  line("  %-44s %s%s", label, vkr(r), note.c_str());
}

struct Variant {
  const char* name;
  bool int64int16;
  bool bda;
  bool robust;
  bool dyn;
};

struct DevInfo {
  VkPhysicalDevice pd;
  uint32_t api;  // effective device api version
  uint32_t qfam;
  bool hasRobustExt, hasBda, hasDyn, hasI64, hasI16;
};

void runMatrix(VkDevice d, const DevInfo& di, const Variant& v) {
  // Pipeline layouts: plume-style (3 empty set layouts + 1 push range) and a bare one.
  VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  VkDescriptorSetLayout sets[3] = {};
  for (auto& s : sets) vkCreateDescriptorSetLayout(d, &dsl, nullptr, &s);
  VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, 16};
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 3;
  plci.pSetLayouts = sets;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VkPipelineLayout layoutP = VK_NULL_HANDLE, layoutBare = VK_NULL_HANDLE;
  VkResult lr = vkCreatePipelineLayout(d, &plci, nullptr, &layoutP);
  VkPipelineLayoutCreateInfo bare{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkResult lr2 = vkCreatePipelineLayout(d, &bare, nullptr, &layoutBare);
  if (lr != VK_SUCCESS || lr2 != VK_SUCCESS) {
    line("  layout creation: plume-style %s, bare %s", vkr(lr), vkr(lr2));
  }

  VkRenderPass rpRgba8 = makeRenderPass(d, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED);
  VkRenderPass rp1010 = makeRenderPass(d, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_UNDEFINED);
  VkRenderPass rpF16 = makeRenderPass(d, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED);
  VkRenderPass rpD24 = makeRenderPass(d, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D24_UNORM_S8_UINT);
  VkRenderPass rpD32 = makeRenderPass(d, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT_S8_UINT);

  VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  VkPipelineCache cache = VK_NULL_HANDLE;
  vkCreatePipelineCache(d, &pci, nullptr, &cache);

  std::string note;
  GfxOpts o;
  report("graphics: bare layout, rgba8 pass", makeGraphics(d, layoutBare, rpRgba8, VK_NULL_HANDLE, o, &note), note);
  report("graphics: plume-style layout (3 sets+push)", makeGraphics(d, layoutP, rpRgba8, VK_NULL_HANDLE, o, &note), note);
  report("graphics: with VkPipelineCache", makeGraphics(d, layoutP, rpRgba8, cache, o, &note), note);
  o.vertexInput = true;
  o.vs = "vin_vert";
  report("graphics: vertex input (vec3)", makeGraphics(d, layoutP, rpRgba8, VK_NULL_HANDLE, o, &note), note);
  o = GfxOpts();
  report("graphics: A2B10G10R10 pass", makeGraphics(d, layoutP, rp1010, VK_NULL_HANDLE, o, &note), note);
  report("graphics: RGBA16F pass", makeGraphics(d, layoutP, rpF16, VK_NULL_HANDLE, o, &note), note);
  report("graphics: D24S8 depth pass", makeGraphics(d, layoutP, rpD24, VK_NULL_HANDLE, o, &note), note);
  report("graphics: D32S8 depth pass", makeGraphics(d, layoutP, rpD32, VK_NULL_HANDLE, o, &note), note);
  o.fs = "tri_frag_spv15";
  report("graphics: SPIR-V 1.5 shaders", makeGraphics(d, layoutP, rpRgba8, VK_NULL_HANDLE, o, &note), note);
  o.fs = "i64_frag";
  report("graphics: Int64 shader capability", makeGraphics(d, layoutP, rpRgba8, VK_NULL_HANDLE, o, &note), note);
  o.fs = "i16_frag";
  report("graphics: Int16 shader capability", makeGraphics(d, layoutP, rpRgba8, VK_NULL_HANDLE, o, &note), note);
  o.fs = "bda_frag";
  report("graphics: buffer device address shader", makeGraphics(d, layoutP, rpRgba8, VK_NULL_HANDLE, o, &note), note);
  if (v.robust) {
    o = GfxOpts();
    o.robustness = true;
    report("graphics: pipeline robustness struct (off)", makeGraphics(d, layoutP, rpRgba8, VK_NULL_HANDLE, o, &note), note);
  }
  if (v.dyn) {
    o = GfxOpts();
    o.dynamicRendering = true;
    report("graphics: dynamic rendering (rgba8)", makeGraphics(d, layoutP, VK_NULL_HANDLE, VK_NULL_HANDLE, o, &note), note);
  }
  report("compute: empty shader", makeCompute(d, layoutP, "comp"));

  if (cache) vkDestroyPipelineCache(d, cache, nullptr);
  for (VkRenderPass rp : {rpRgba8, rp1010, rpF16, rpD24, rpD32})
    if (rp) vkDestroyRenderPass(d, rp, nullptr);
  if (layoutP) vkDestroyPipelineLayout(d, layoutP, nullptr);
  if (layoutBare) vkDestroyPipelineLayout(d, layoutBare, nullptr);
  for (auto s : sets)
    if (s) vkDestroyDescriptorSetLayout(d, s, nullptr);
  (void)di;
}

void probe() {
  uint32_t instVer = VK_API_VERSION_1_0;
  auto enumVer = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  if (enumVer) enumVer(&instVer);
  line("Instance Vulkan version: %s", verStr(instVer).c_str());
  uint32_t useVer = std::min(instVer, (uint32_t)VK_API_VERSION_1_3);

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "G52Probe";
  app.apiVersion = useVer;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance inst = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, nullptr, &inst);
  line("vkCreateInstance -> %s", vkr(r));
  if (r != VK_SUCCESS) return;

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  std::vector<VkPhysicalDevice> pds(n);
  vkEnumeratePhysicalDevices(inst, &n, pds.data());
  line("Physical devices: %u", n);

  for (VkPhysicalDevice pd : pds) {
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    const auto& pr = p2.properties;
    uint32_t api = std::min(pr.apiVersion, useVer);
    line("");
    line("== Device: %s", pr.deviceName);
    line("   vendor 0x%X device 0x%X driverVersion 0x%X", pr.vendorID, pr.deviceID, pr.driverVersion);
    line("   device api %s (using %s)", verStr(pr.apiVersion).c_str(), verStr(api).c_str());
    line("   driver: %s | %s (id %d)", drv.driverName, drv.driverInfo, (int)drv.driverID);

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, exts.data());
    auto hasExt = [&](const char* nm) {
      for (auto& e : exts)
        if (!strcmp(e.extensionName, nm)) return true;
      return false;
    };
    std::string row = "   extensions (" + std::to_string(ne) + "):";
    for (auto& e : exts) {
      if (row.size() + strlen(e.extensionName) > 110) {
        line("%s", row.c_str());
        row = "     ";
      }
      row += std::string(" ") + e.extensionName;
    }
    line("%s", row.c_str());

    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDevicePipelineRobustnessFeaturesEXT fpr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    void** tail = &f2.pNext;
    if (api >= VK_API_VERSION_1_2) { *tail = &f12; tail = &f12.pNext; }
    if (api >= VK_API_VERSION_1_3) { *tail = &f13; tail = &f13.pNext; }
    bool robustExt = hasExt("VK_EXT_pipeline_robustness");
    if (robustExt) { *tail = &fpr; tail = &fpr.pNext; }
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    const auto& f = f2.features;
    line("   features: int64 %s, int16 %s, geometryShader %s, clipDistance %s, cullDistance %s",
         yn(f.shaderInt64), yn(f.shaderInt16), yn(f.geometryShader), yn(f.shaderClipDistance),
         yn(f.shaderCullDistance));
    line("             depthClamp %s, fillModeNonSolid %s, independentBlend %s, dualSrcBlend %s, robustBufferAccess %s",
         yn(f.depthClamp), yn(f.fillModeNonSolid), yn(f.independentBlend), yn(f.dualSrcBlend),
         yn(f.robustBufferAccess));
    if (api >= VK_API_VERSION_1_2)
      line("   1.2: bufferDeviceAddress %s, descriptorIndexing %s, runtimeDescriptorArray %s, shaderFloat16 %s, timelineSemaphore %s",
           yn(f12.bufferDeviceAddress), yn(f12.descriptorIndexing), yn(f12.runtimeDescriptorArray),
           yn(f12.shaderFloat16), yn(f12.timelineSemaphore));
    if (api >= VK_API_VERSION_1_3)
      line("   1.3: dynamicRendering %s, synchronization2 %s, maintenance4 %s",
           yn(f13.dynamicRendering), yn(f13.synchronization2), yn(f13.maintenance4));
    if (robustExt) line("   pipelineRobustness feature: %s", yn(fpr.pipelineRobustness));

    struct FmtT { const char* n; VkFormat f; };
    for (FmtT ft : {FmtT{"RGBA8", VK_FORMAT_R8G8B8A8_UNORM}, FmtT{"A2B10G10R10", VK_FORMAT_A2B10G10R10_UNORM_PACK32},
                    FmtT{"RGBA16F", VK_FORMAT_R16G16B16A16_SFLOAT}, FmtT{"D24S8", VK_FORMAT_D24_UNORM_S8_UINT},
                    FmtT{"D32S8", VK_FORMAT_D32_SFLOAT_S8_UINT}}) {
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(pd, ft.f, &fp);
      bool att = fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
      line("   format %-12s attachment %s", ft.n, yn(att));
    }

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
    uint32_t qfam = 0;
    for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }

    DevInfo di{pd, api, qfam, robustExt, f12.bufferDeviceAddress == VK_TRUE, f13.dynamicRendering == VK_TRUE,
               f.shaderInt64 == VK_TRUE, f.shaderInt16 == VK_TRUE};

    const Variant variants[] = {
        {"V0 plain (no extra features)", false, false, false, false},
        {"V1 + int64/int16 features", true, false, false, false},
        {"V2 + bufferDeviceAddress", true, true, false, false},
        {"V3 + pipeline robustness ext", true, true, true, false},
        {"V4 + dynamic rendering", true, true, false, true},
    };
    for (const Variant& v : variants) {
      line("");
      line("-- %s", v.name);
      if ((v.int64int16 && !(di.hasI64 && di.hasI16)) || (v.bda && !di.hasBda) ||
          (v.robust && !di.hasRobustExt) || (v.dyn && !di.hasDyn)) {
        line("   skipped: not supported by this device");
        continue;
      }
      VkPhysicalDeviceFeatures2 ef2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
      VkPhysicalDevicePipelineRobustnessFeaturesEXT epr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
      void** t = &ef2.pNext;
      std::vector<const char*> en;
      if (v.int64int16) { ef2.features.shaderInt64 = VK_TRUE; ef2.features.shaderInt16 = VK_TRUE; }
      if (v.bda) { e12.bufferDeviceAddress = VK_TRUE; *t = &e12; t = &e12.pNext; }
      if (v.dyn) { e13.dynamicRendering = VK_TRUE; *t = &e13; t = &e13.pNext; }
      if (v.robust) { epr.pipelineRobustness = VK_TRUE; *t = &epr; t = &epr.pNext; en.push_back("VK_EXT_pipeline_robustness"); }
      float prio = 1.0f;
      VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      qci.queueFamilyIndex = qfam;
      qci.queueCount = 1;
      qci.pQueuePriorities = &prio;
      VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      dci.pNext = &ef2;
      dci.queueCreateInfoCount = 1;
      dci.pQueueCreateInfos = &qci;
      dci.enabledExtensionCount = (uint32_t)en.size();
      dci.ppEnabledExtensionNames = en.data();
      VkDevice dev = VK_NULL_HANDLE;
      VkResult dr = vkCreateDevice(pd, &dci, nullptr, &dev);
      line("   vkCreateDevice -> %s", vkr(dr));
      if (dr != VK_SUCCESS) continue;
      runMatrix(dev, di, v);
      vkDestroyDevice(dev, nullptr);
    }
  }
  vkDestroyInstance(inst, nullptr);
  line("");
  line("Done.");
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_com_probe_g52_Probe_run(JNIEnv* env, jclass, jobjectArray names,
                                                                    jobjectArray blobs) {
  g_out.clear();
  g_spv.clear();
  jsize n = env->GetArrayLength(names);
  for (jsize i = 0; i < n; i++) {
    jstring js = (jstring)env->GetObjectArrayElement(names, i);
    const char* nm = env->GetStringUTFChars(js, nullptr);
    jbyteArray jb = (jbyteArray)env->GetObjectArrayElement(blobs, i);
    jsize len = env->GetArrayLength(jb);
    std::vector<uint32_t> words((len + 3) / 4);
    env->GetByteArrayRegion(jb, 0, len, reinterpret_cast<jbyte*>(words.data()));
    g_spv[nm] = std::move(words);
    env->ReleaseStringUTFChars(js, nm);
  }
  probe();
  return env->NewStringUTF(g_out.c_str());
}
