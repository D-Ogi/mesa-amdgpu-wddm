// SPDX-License-Identifier: MIT
// Public API regression. A single API case per process; no registry or deployment changes.
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void require(bool value, const char *what) {
   if (!value) { std::fprintf(stderr, "FAIL %s (Win32=%lu)\n", what, GetLastError()); std::exit(1); }
}
static void vkcheck(VkResult result, const char *what) {
   std::printf("%s = %d\n", what, result); std::fflush(stdout); require(result == VK_SUCCESS, what);
}
static void hrcheck(HRESULT result, const char *what) {
   std::printf("%s = %08lx\n", what, static_cast<unsigned long>(result)); std::fflush(stdout); require(SUCCEEDED(result), what);
}
static std::vector<uint32_t> read_spv(const char *directory, const char *name) {
   std::string path = std::string(directory) + "/" + name + ".spv";
   FILE *f = nullptr; fopen_s(&f, path.c_str(), "rb"); require(f != nullptr, path.c_str());
   fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f); require(size > 0 && size < 1048576 && size % 4 == 0, "SPIR-V size");
   std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
   require(fread(words.data(), 1, size, f) == static_cast<size_t>(size), "SPIR-V read"); fclose(f); return words;
}
static void on_small_stack(LPTHREAD_START_ROUTINE work, void *arg) {
   HANDLE t = CreateThread(nullptr, 64 * 1024, work, arg, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
   require(t != nullptr, "CreateThread 64KiB reserve");
   require(WaitForSingleObject(t, 90000) == WAIT_OBJECT_0, "small thread completed within 90s");
   DWORD code = 1; require(GetExitCodeThread(t, &code) && code == 0, "small thread exit"); CloseHandle(t);
}
static void log_stack() {
   char marker; MEMORY_BASIC_INFORMATION m{}; require(VirtualQuery(&marker, &m, sizeof(m)) != 0, "VirtualQuery");
   auto remaining = reinterpret_cast<uintptr_t>(&marker) - reinterpret_cast<uintptr_t>(m.AllocationBase);
   std::printf("caller thread=%lu remaining_reserved=%zu\n", GetCurrentThreadId(), static_cast<size_t>(remaining));
   require(remaining <= 64 * 1024, "caller actually small"); std::fflush(stdout);
}

struct VulkanCase {
   PFN_vkGetInstanceProcAddr gipa{}; PFN_vkGetDeviceProcAddr gdpa{};
   VkInstance instance{}; VkDevice device{}; VkPipelineLayout layout{};
   VkShaderModule vs{}, fs{}, cs{}, rg{}; VkRenderPass render_pass{};
   const char *mode{}; std::vector<uint32_t> vertex, fragment;
   template<class T> T get(const char *name) { auto p = gdpa(device, name); require(p != nullptr, name); return reinterpret_cast<T>(p); }
};
#define DEVICE(c, name) (c->get<PFN_vk##name>("vk" #name))

static DWORD WINAPI compile_vulkan(void *opaque) {
   log_stack(); auto *c = static_cast<VulkanCase *>(opaque);
   if (!strcmp(c->mode, "shader-object")) {
      VkShaderCreateInfoEXT info[2]{}; VkShaderEXT shaders[2]{};
      for (unsigned i = 0; i < 2; ++i) {
         info[i].sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
         info[i].flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
         info[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
         info[i].nextStage = i ? 0 : VK_SHADER_STAGE_FRAGMENT_BIT;
         info[i].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
         const auto &words = i ? c->fragment : c->vertex;
         info[i].codeSize = words.size() * sizeof(uint32_t); info[i].pCode = words.data(); info[i].pName = "main";
      }
      vkcheck(DEVICE(c, CreateShadersEXT)(c->device, 2, info, nullptr, shaders), "vkCreateShadersEXT");
      for (auto shader : shaders) DEVICE(c, DestroyShaderEXT)(c->device, shader, nullptr);
   } else {
      VkPipeline pipeline{};
      VkPipelineShaderStageCreateInfo stages[2]{};
      for (auto &s : stages) { s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; s.pName = "main"; }
      if (!strcmp(c->mode, "compute")) {
         VkComputePipelineCreateInfo p{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
         stages[0].stage = VK_SHADER_STAGE_COMPUTE_BIT; stages[0].module = c->cs;
         p.stage = stages[0]; p.layout = c->layout;
         vkcheck(DEVICE(c, CreateComputePipelines)(c->device, VK_NULL_HANDLE, 1, &p, nullptr, &pipeline), "vkCreateComputePipelines");
      } else if (!strcmp(c->mode, "rt")) {
         stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR; stages[0].module = c->rg;
         VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
         group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; group.generalShader = 0;
         group.closestHitShader = group.anyHitShader = group.intersectionShader = VK_SHADER_UNUSED_KHR;
         VkRayTracingPipelineCreateInfoKHR p{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
         p.stageCount = 1; p.pStages = stages; p.groupCount = 1; p.pGroups = &group; p.maxPipelineRayRecursionDepth = 1; p.layout = c->layout;
         vkcheck(DEVICE(c, CreateRayTracingPipelinesKHR)(c->device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &p, nullptr, &pipeline), "vkCreateRayTracingPipelinesKHR");
      } else {
         stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = c->vs;
         stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = c->fs;
         VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
         VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
         VkViewport vp{0, 0, 8, 8, 0, 1}; VkRect2D rect{{0,0},{8,8}};
         VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1; viewport.pViewports = &vp; viewport.pScissors = &rect;
         VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1; rs.polygonMode = VK_POLYGON_MODE_FILL;
         VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
         VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask = 15;
         VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &blend;
         VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
         p.stageCount = 2; p.pStages = stages; p.pVertexInputState = &vi; p.pInputAssemblyState = &ia; p.pViewportState = &viewport;
         p.pRasterizationState = &rs; p.pMultisampleState = &ms; p.pColorBlendState = &cb; p.layout = c->layout; p.renderPass = c->render_pass;
         vkcheck(DEVICE(c, CreateGraphicsPipelines)(c->device, VK_NULL_HANDLE, 1, &p, nullptr, &pipeline), "vkCreateGraphicsPipelines");
      }
      DEVICE(c, DestroyPipeline)(c->device, pipeline, nullptr);
   }
   return 0;
}

static int test_vulkan(const char *icd, const char *mode, const char *spv_dir) {
   char full[MAX_PATH]; require(GetFullPathNameA(icd, MAX_PATH, full, nullptr) > 0, "ICD path");
   HMODULE dll = LoadLibraryExA(full, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
   require(dll != nullptr, "load ICD");
   VulkanCase c; c.mode = mode;
   c.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll, "vk_icdGetInstanceProcAddr")); require(c.gipa != nullptr, "ICD entry");
   VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "bd136-stack"; app.apiVersion = VK_API_VERSION_1_3;
   VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
   vkcheck(reinterpret_cast<PFN_vkCreateInstance>(c.gipa(nullptr, "vkCreateInstance"))(&ci, nullptr, &c.instance), "vkCreateInstance");
#define INSTANCE(name) reinterpret_cast<PFN_vk##name>(c.gipa(c.instance, "vk" #name))
   uint32_t count = 0; vkcheck(INSTANCE(EnumeratePhysicalDevices)(c.instance, &count, nullptr), "enumerate count");
   std::vector<VkPhysicalDevice> devices(count); vkcheck(INSTANCE(EnumeratePhysicalDevices)(c.instance, &count, devices.data()), "enumerate devices");
   VkPhysicalDevice physical{};
   for (auto d : devices) { VkPhysicalDeviceProperties p{}; INSTANCE(GetPhysicalDeviceProperties)(d, &p); if (p.vendorID == 0x1002) { physical = d; std::printf("adapter %s device=%04x\n", p.deviceName, p.deviceID); break; } }
   if (!physical) { std::puts("UNAVAILABLE: this ICD has no AMD adapter here (not PASS)"); INSTANCE(DestroyInstance)(c.instance, nullptr); FreeLibrary(dll); return 77; }
   uint32_t n = 0; INSTANCE(GetPhysicalDeviceQueueFamilyProperties)(physical, &n, nullptr);
   std::vector<VkQueueFamilyProperties> queues(n); INSTANCE(GetPhysicalDeviceQueueFamilyProperties)(physical, &n, queues.data());
   uint32_t family = n; for (uint32_t i = 0; i < n; ++i) if ((queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { family = i; break; }
   require(family != n, "graphics+compute queue");
   VkPhysicalDeviceShaderObjectFeaturesEXT so{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
   VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
   VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
   VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
   VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
   VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
   features.pNext = &f12; f12.pNext = &f13;
   bool ray = !strcmp(mode, "rt"), shader_object = !strcmp(mode, "shader-object");
   if (ray) { f13.pNext = &accel; accel.pNext = &rt; }
   if (shader_object) f13.pNext = &so;
   INSTANCE(GetPhysicalDeviceFeatures2)(physical, &features);
   require(!ray || (accel.accelerationStructure && rt.rayTracingPipeline && f12.bufferDeviceAddress), "ray tracing support");
   require(!shader_object || so.shaderObject, "shader object support");
   const char *extensions[3]{}; uint32_t ec = 0;
   if (ray) { extensions[ec++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME; extensions[ec++] = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME; extensions[ec++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME; }
   if (shader_object) extensions[ec++] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
   float priority = 1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
   VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.pNext = &features; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = ec; di.ppEnabledExtensionNames = extensions;
   vkcheck(INSTANCE(CreateDevice)(physical, &di, nullptr, &c.device), "vkCreateDevice (large control stack)");
   c.gdpa = INSTANCE(GetDeviceProcAddr);
   VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; vkcheck(c.get<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(c.device, &li, nullptr, &c.layout), "layout");
   auto module = [&](const char *name) { auto words = read_spv(spv_dir, name); VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = words.size() * 4; si.pCode = words.data(); VkShaderModule result{}; vkcheck(c.get<PFN_vkCreateShaderModule>("vkCreateShaderModule")(c.device, &si, nullptr, &result), name); return result; };
   c.vertex = read_spv(spv_dir, "stack.vert"); c.fragment = read_spv(spv_dir, "stack.frag");
   if (ray) c.rg = module("stack.rgen"); else if (!strcmp(mode, "compute")) c.cs = module("stack.comp");
   else if (!shader_object) {
      c.vs = module("stack.vert"); c.fs = module("stack.frag");
      VkAttachmentDescription a{}; a.format = VK_FORMAT_R8G8B8A8_UNORM; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
      VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ri.attachmentCount = 1; ri.pAttachments = &a; ri.subpassCount = 1; ri.pSubpasses = &sub;
      vkcheck(c.get<PFN_vkCreateRenderPass>("vkCreateRenderPass")(c.device, &ri, nullptr, &c.render_pass), "render pass");
   }
   on_small_stack(compile_vulkan, &c);
   if (c.render_pass) c.get<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(c.device, c.render_pass, nullptr);
   for (auto m : {c.vs,c.fs,c.cs,c.rg}) if (m) c.get<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(c.device, m, nullptr);
   c.get<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(c.device, c.layout, nullptr);
   c.get<PFN_vkDestroyDevice>("vkDestroyDevice")(c.device, nullptr); INSTANCE(DestroyInstance)(c.instance, nullptr);
   FreeLibrary(dll); std::puts("PASS public Vulkan compile on 64KiB caller"); return 0;
}

struct D3DCase { ID3D11Device *d{}; ID3D11DeviceContext *c{}; ID3DBlob *vs{}, *ps{}; };
static DWORD WINAPI compile_d3d(void *opaque) {
   log_stack(); auto *t = static_cast<D3DCase *>(opaque);
   ID3D11VertexShader *vs{}; ID3D11PixelShader *ps{};
   hrcheck(t->d->CreateVertexShader(t->vs->GetBufferPointer(), t->vs->GetBufferSize(), nullptr, &vs), "CreateVertexShader");
   hrcheck(t->d->CreatePixelShader(t->ps->GetBufferPointer(), t->ps->GetBufferSize(), nullptr, &ps), "CreatePixelShader");
   D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 8; desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
   ID3D11Texture2D *color{}, *readback{}; ID3D11RenderTargetView *rtv{};
   hrcheck(t->d->CreateTexture2D(&desc, nullptr, &color), "color texture"); hrcheck(t->d->CreateRenderTargetView(color, nullptr, &rtv), "RTV");
   desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; hrcheck(t->d->CreateTexture2D(&desc, nullptr, &readback), "readback texture");
   D3D11_VIEWPORT vp{0,0,8,8,0,1}; t->c->RSSetViewports(1, &vp); t->c->OMSetRenderTargets(1, &rtv, nullptr);
   t->c->VSSetShader(vs, nullptr, 0); t->c->PSSetShader(ps, nullptr, 0); t->c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   float clear[4] = {1,0,0,1}; t->c->ClearRenderTargetView(rtv, clear); t->c->Draw(3,0); t->c->CopyResource(readback, color);
   D3D11_MAPPED_SUBRESOURCE map{}; hrcheck(t->c->Map(readback, 0, D3D11_MAP_READ, 0, &map), "first draw readback");
   auto pixel = *(reinterpret_cast<const uint32_t *>(static_cast<const char *>(map.pData) + 4 * map.RowPitch) + 4);
   std::printf("pixel=%08x expected=ff00ff00\n", pixel); t->c->Unmap(readback,0); require(pixel == 0xff00ff00, "green draw (red clear is negative control)");
   t->c->ClearState(); t->c->Flush(); rtv->Release(); color->Release(); readback->Release(); vs->Release(); ps->Release(); return 0;
}
static int test_d3d(bool cpu) {
   // Inbox API, adapter selected explicitly. The operator selects this exe in the desktop router.
   IDXGIFactory1 *factory{}; hrcheck(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)), "DXGI factory");
   IDXGIAdapter1 *adapter{};
   for (UINT i=0;;++i) { IDXGIAdapter1 *a{}; if (factory->EnumAdapters1(i,&a) == DXGI_ERROR_NOT_FOUND) break; DXGI_ADAPTER_DESC1 desc{}; a->GetDesc1(&desc); if (desc.VendorId == 0x1002) { adapter=a; break; } a->Release(); }
   if (!adapter) { factory->Release(); std::puts("UNAVAILABLE: no AMD adapter (not PASS)"); return 77; }
   D3DCase t; D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_10_0, got{};
   hrcheck(D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&requested,1,D3D11_SDK_VERSION,&t.d,&got,&t.c), "D3D11 device");
   HMODULE zink = GetModuleHandleW(cpu ? L"bc250d3d.dll" : L"bc250d3d_zink.dll");
   require(zink != nullptr, "loaded expected desktop UMD: route must be configured by the operator");
   char path[MAX_PATH], expected[MAX_PATH], full[MAX_PATH];
   require(GetModuleFileNameA(zink,path,MAX_PATH)>0,"desktop UMD module path");
   std::printf("desktop_umd=%s\n",path);
   DWORD length = GetEnvironmentVariableA("MESA_TEST_EXPECT_UMD",expected,MAX_PATH);
   require(length > 0 && length < MAX_PATH, "expected UMD path must be supplied by the runner");
   length = GetFullPathNameA(expected,MAX_PATH,full,nullptr);
   require(length > 0 && length < MAX_PATH && !_stricmp(path,full), "loaded UMD matches the selected artifact");
   const char *vs = "float4 main(uint i:SV_VertexID):SV_Position { float2 p=float2((i<<1)&2,i&2); return float4(p*float2(2,-2)+float2(-1,1),0,1); }";
   const char *ps = "float4 main():SV_Target { return float4(0,1,0,1); }";
   hrcheck(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&t.vs,nullptr),"DXBC VS");
   hrcheck(D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&t.ps,nullptr),"DXBC PS");
   on_small_stack(compile_d3d,&t);
   t.vs->Release(); t.ps->Release(); t.c->Release(); t.d->Release(); adapter->Release(); factory->Release();
   std::puts("PASS D3D11 shader creation and first draw on 64KiB caller"); return 0;
}
int main(int argc, char **argv) {
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   SetEnvironmentVariableA("MESA_COMPILE_STACK_STATS","1"); SetEnvironmentVariableA("MESA_SHADER_CACHE_DISABLE","true");
   if (argc == 2 && !strcmp(argv[1],"d3d11")) return test_d3d(false);
   if (argc == 2 && !strcmp(argv[1],"d3d11-cpu")) return test_d3d(true);
   if (argc != 5 || strcmp(argv[1],"vulkan") ||
       (strcmp(argv[3],"shader-object") && strcmp(argv[3],"graphics") && strcmp(argv[3],"compute") && strcmp(argv[3],"rt"))) {
      std::puts("api-stack {d3d11|d3d11-cpu} | api-stack vulkan ABS_ICD {shader-object|graphics|compute|rt} SPV_DIR"); return 2;
   }
   return test_vulkan(argv[2],argv[3],argv[4]);
}
