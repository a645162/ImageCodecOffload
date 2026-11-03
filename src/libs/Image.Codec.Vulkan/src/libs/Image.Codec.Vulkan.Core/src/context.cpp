/*
 * File: context.cpp
 * Brief: Context.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#include "context.h"
#include <vector>
#include <cstring>

namespace vic {
namespace {

// 设备选择：
//   * Options::device_index >= 0 时直接取该下标（多卡机器上指定核显/独显）；
//   * 否则按类型打分：独显 > 核显 > 其它（与历史行为一致）。
// 请求 fp64 时，候选设备还必须支持 shaderFloat64——指标累加必须用 double，
// 不支持就该让调用方看到「这台设备不行」，而不是跑出一个 float 结果。
bool pickPhysicalDevice(VkInstance inst, const Context::Options &options,
                        VkPhysicalDevice &out, DeviceInfo &info,
                        uint32_t &computeFamily, bool &externalWin32,
                        std::string &error) {
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  if (n == 0) {
    error = "no Vulkan physical device";
    return false;
  }
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(inst, &n, devs.data());

  auto score = [](VkPhysicalDevice d) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(d, &p);
    return p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
           : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                    : 0;
  };

  const int explicit_index = options.device_index;
  if (explicit_index >= 0 && static_cast<uint32_t>(explicit_index) >= n) {
    error = "device index " + std::to_string(explicit_index) +
            " out of range (" + std::to_string(n) + " device(s))";
    return false;
  }

  for (int pass = 2; pass >= 0; --pass) {
    for (uint32_t di = 0; di < devs.size(); ++di) {
      auto d = devs[di];
      if (explicit_index >= 0 && static_cast<int>(di) != explicit_index)
        continue;
      if (explicit_index < 0 && score(d) != pass) continue;
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(d, &p);

      VkPhysicalDeviceFeatures features{};
      vkGetPhysicalDeviceFeatures(d, &features);
      if (options.request_float64 && options.require_float64 &&
          !features.shaderFloat64) {
        if (explicit_index >= 0) {
          error = std::string("device '") + p.deviceName +
                  "' does not support shaderFloat64";
          return false;
        }
        continue; // 自动选择时跳过不满足硬性要求的设备
      }

      uint32_t qn = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qf(qn);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
      int bestQ = -1;
      for (uint32_t i = 0; i < qn; ++i)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
          bestQ = static_cast<int>(i);
          break;
        }
      if (bestQ < 0) continue;

      uint32_t en = 0;
      vkEnumerateDeviceExtensionProperties(d, nullptr, &en, nullptr);
      std::vector<VkExtensionProperties> exts(en);
      vkEnumerateDeviceExtensionProperties(d, nullptr, &en, exts.data());
      // VK_KHR_external_memory_win32 常量只由 vulkan_win32.h 提供，Linux 上
      // 根本没有这个扩展，编解码路径也不需要它。
      bool extWin32 = false;
#ifdef _WIN32
      for (auto &e : exts)
        if (std::strcmp(e.extensionName,
                        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0)
          extWin32 = true;
#else
      (void)exts;
#endif

      out = d;
      info.name = p.deviceName;
      info.apiVersion = p.apiVersion;
      info.supportsExternalMemoryWin32 = extWin32;
      info.supportsFloat64 = features.shaderFloat64 != VK_FALSE;
      computeFamily = static_cast<uint32_t>(bestQ);
      externalWin32 = extWin32;
      return true;
    }
  }

  if (options.request_float64 && options.require_float64) {
    error = "no Vulkan device with compute queue and shaderFloat64";
  } else {
    error = "no Vulkan device with a compute queue";
  }
  return false;
}

} // namespace

Context::~Context() { destroy(); }

Context::Context(Context &&other) noexcept { *this = std::move(other); }

Context &Context::operator=(Context &&other) noexcept {
  if (this != &other) {
    destroy();
    instance_ = other.instance_;
    physicalDevice_ = other.physicalDevice_;
    device_ = other.device_;
    queue_ = other.queue_;
    queueFamily_ = other.queueFamily_;
    info_ = std::move(other.info_);
    lastError_ = std::move(other.lastError_);
    enabledDeviceExtensions_ = std::move(other.enabledDeviceExtensions_);
#ifdef _WIN32
    vkGetMemoryWin32Handle_ = other.vkGetMemoryWin32Handle_;
#endif
    extBufferProps_ = other.extBufferProps_;
    imageFormatProps2_ = other.imageFormatProps2_;
    enabledExternalWin32_ = other.enabledExternalWin32_;
    float64Enabled_ = other.float64Enabled_;
    other.instance_ = VK_NULL_HANDLE;
    other.physicalDevice_ = VK_NULL_HANDLE;
    other.device_ = VK_NULL_HANDLE;
    other.queue_ = VK_NULL_HANDLE;
#ifdef _WIN32
    other.vkGetMemoryWin32Handle_ = nullptr;
#endif
    other.extBufferProps_ = nullptr;
    other.imageFormatProps2_ = nullptr;
    other.enabledExternalWin32_ = false;
    other.float64Enabled_ = false;
  }
  return *this;
}

Result Context::init() { return init(Options{}); }

Result Context::init(const Options &options) {
  lastError_.clear();

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "Image.Codec.Vulkan";
  app.apiVersion = VK_API_VERSION_1_0;

  // VK_KHR_external_memory_capabilities 是设备级 VK_KHR_external_memory 的声明依赖，
  // 提供 vkGetPhysicalDeviceExternalBuffer/ImageFormatProperties(KHR) 导出能力查询。
  // VK_KHR_get_physical_device_properties2 提供 vkGetPhysicalDeviceImageFormatProperties2KHR
  // （不启用则 loader 返回 null，image 导出能力查询失效）。
  //
  // 这两个扩展只服务 GL 互操作（Windows），但都是实例级且普及度极高。仍然按
  // 「有就开、没有不算错」处理：编解码与指标不需要它们，Linux 上的某些软件
  // 实现（lavapipe 之类）不提供时不该把整个 Context 卡死。
  std::vector<const char *> instExts;
  {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> avail(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, avail.data());
    const auto has = [&avail](const char *name) {
      for (const auto &e : avail)
        if (std::strcmp(e.extensionName, name) == 0) return true;
      return false;
    };
    if (has(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME))
      instExts.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
    if (has(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
      instExts.push_back(
          VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
  }

  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = static_cast<uint32_t>(instExts.size());
  ici.ppEnabledExtensionNames = instExts.empty() ? nullptr : instExts.data();
  if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
    lastError_ = "vkCreateInstance failed";
    return Result::InitFailed;
  }

  // vcpkg vulkan-1.lib 不导出这两个 -KHR 符号，经 vkGetInstanceProcAddr 运行时解析。
  extBufferProps_ =
      reinterpret_cast<PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR>(
          vkGetInstanceProcAddr(
              instance_, "vkGetPhysicalDeviceExternalBufferPropertiesKHR"));
  imageFormatProps2_ =
      reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2KHR>(
          vkGetInstanceProcAddr(
              instance_, "vkGetPhysicalDeviceImageFormatProperties2KHR"));

  bool externalWin32 = false;
  if (!pickPhysicalDevice(instance_, options, physicalDevice_, info_,
                          queueFamily_, externalWin32, lastError_)) {
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    return Result::InitFailed;
  }
  enabledExternalWin32_ = externalWin32;

  float priority = 1.0f;
  VkDeviceQueueCreateInfo dq{};
  dq.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  dq.queueFamilyIndex = queueFamily_;
  dq.queueCount = 1;
  dq.pQueuePriorities = &priority;

  // 设备扩展：Win32 导出（仅 Windows，且设备支持时）+ 调用方额外要求的。
  // 额外扩展先查设备是否真的支持——请求一个不存在的扩展会让 vkCreateDevice
  // 直接失败，而调用方（例如内存预算查询）本来就能降级。
  {
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &en,
                                         nullptr);
    std::vector<VkExtensionProperties> avail(en);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &en,
                                         avail.data());
    const auto supported = [&avail](const char *name) {
      for (const auto &e : avail)
        if (std::strcmp(e.extensionName, name) == 0) return true;
      return false;
    };
    std::vector<const char *> devExts;
#ifdef _WIN32
    if (enabledExternalWin32_) {
      devExts.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
      devExts.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    }
#endif
    for (const char *name : options.extra_device_extensions) {
      if (name == nullptr) continue;
      if (!supported(name)) continue;
      bool dup = false;
      for (const char *existing : devExts) {
        if (std::strcmp(existing, name) == 0) {
          dup = true;
          break;
        }
      }
      if (!dup) devExts.push_back(name);
    }

    // 只打开需要的特性：shaderFloat64 是请求了才开，其余保持关闭。
    VkPhysicalDeviceFeatures enabled{};
    if (options.request_float64 && info_.supportsFloat64) {
      enabled.shaderFloat64 = VK_TRUE;
    }

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dq;
    dci.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
    dci.ppEnabledExtensionNames = devExts.empty() ? nullptr : devExts.data();
    dci.pEnabledFeatures = &enabled;
    if (vkCreateDevice(physicalDevice_, &dci, nullptr, &device_) !=
        VK_SUCCESS) {
      vkDestroyInstance(instance_, nullptr);
      instance_ = VK_NULL_HANDLE;
      lastError_ = "vkCreateDevice failed";
      return Result::InitFailed;
    }
    float64Enabled_ = enabled.shaderFloat64 == VK_TRUE;
    enabledDeviceExtensions_.clear();
    for (const char *name : devExts)
      enabledDeviceExtensions_.emplace_back(name);
  }

  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
#ifdef _WIN32
  vkGetMemoryWin32Handle_ = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
      vkGetDeviceProcAddr(device_, "vkGetMemoryWin32HandleKHR"));
#endif
  return Result::Ok;
}

bool Context::deviceExtensionEnabled(const char *name) const {
  if (name == nullptr) return false;
  for (const auto &ext : enabledDeviceExtensions_) {
    if (ext == name) return true;
  }
  return false;
}

void Context::destroy() {
  if (device_) vkDestroyDevice(device_, nullptr);
  if (instance_) vkDestroyInstance(instance_, nullptr);
  device_ = VK_NULL_HANDLE;
  instance_ = VK_NULL_HANDLE;
}

Result Context::createExportableMemory(const VkMemoryRequirements &req,
                                       VkMemoryPropertyFlags props,
                                       bool exportable,
                                       VkDeviceMemory &out) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mp);

  // 导出内存类型校验（Vulkan 1.4 机制）：
  // 资源在创建时已通过 VkExternalMemory(Buffer|Image)CreateInfo::handleTypes
  // 声明导出句柄类型，规范保证 vkGet(Buffer|Image)MemoryRequirements 返回的
  // memoryTypeBits 已过滤为"仅包含支持该句柄类型的 memory type"
  // （见规范 External Memory 章节：不支持句柄类型的类型会被过滤掉）。
  // 因此从 req.memoryTypeBits 中选取的类型天然满足导出句柄的类型要求
  // （覆盖旧 VUID-VkExportMemoryAllocateInfo-handleTypes-00656）。
  // exportable 为按 VUID-09860 对具体资源查询 VkExternalBufferProperties /
  // VkExternalImageFormatProperties 得到的 EXPORTABLE_BIT 支持性结果，
  // 仅在该资源确实可导出时才链入 VkExportMemoryAllocateInfo。
  int best = -1;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) == 0) continue;
    if ((mp.memoryTypes[i].propertyFlags & props) != props) continue;
    best = static_cast<int>(i);
    if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      break;
  }
  if (best < 0) return Result::Error;

  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = static_cast<uint32_t>(best);

#ifdef _WIN32
  VkExportMemoryAllocateInfo exportInfo{};
  if (enabledExternalWin32_ && exportable) {
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    ai.pNext = &exportInfo;
  }
#else
  // 非 Windows 没有 VK_KHR_external_memory_win32：exportable 无从表达，
  // 这里按普通设备内存分配（GL 互操作本来也不在其它平台提供）。
  (void)exportable;
#endif

  if (vkAllocateMemory(device_, &ai, nullptr, &out) != VK_SUCCESS)
    return Result::Error;
  return Result::Ok;
}

} // namespace vic
