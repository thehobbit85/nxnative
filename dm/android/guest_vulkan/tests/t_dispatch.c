// Calls only registered guest entry points; the native loader is not the test's API.
#define VK_NO_PROTOTYPES
#include "hle.h"
#include <vulkan/vulkan.h>
#include <assert.h>
#include <sys/mman.h>

static uintptr_t guest_low, guest_high;
static unsigned stack_switches;
int dm_guest_as_contains(uintptr_t p) {
    int guest = p >= guest_low && p < guest_high;
    stack_switches += guest;
    return guest;
}

static PFN_vkVoidFunction import(const char *name) {
    unsigned long bytes;
    const hle_ent *entries = DM_SECTION_DATA(swhle, &bytes);
    for (unsigned long i = 0; i < bytes / sizeof *entries; ++i)
        if (!strcmp(entries[i].name, name)) return (PFN_vkVoidFunction)entries[i].fn;
    return NULL;
}
static PFN_vkGetInstanceProcAddr gipa;
#ifdef TEST_RESOURCES
#include "resource_checks.h"
#endif
static void dispatch_test(void) {
    assert(!gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    assert(!gipa(VK_NULL_HANDLE, "vkDefinitelyNotAnEntryPoint"));
    assert(!gipa(VK_NULL_HANDLE, NULL));
    PFN_vkEnumerateInstanceVersion version = (void *)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t v; assert(version && version(&v) == VK_SUCCESS && v == VK_API_VERSION_1_1);
    PFN_vkEnumerateInstanceExtensionProperties extensions = (void *)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 99; assert(extensions("not-a-layer", &n, NULL) == VK_ERROR_LAYER_NOT_PRESENT && n == 0);
    assert(extensions(NULL, &n, NULL) == VK_SUCCESS && n);
    // Enumeration storage belongs on the heap: this entire fixture also runs on
    // an 8 KiB guest stack, before a call enters the larger driver stack.
    VkExtensionProperties *ex = calloc(8, sizeof *ex); assert(ex); uint32_t capacity = 0;
    assert(extensions(NULL, &capacity, ex) == VK_INCOMPLETE && capacity == 0);
    capacity = 8; assert(extensions(NULL, &capacity, ex) == VK_SUCCESS && capacity == n);
    for (uint32_t i = 0; i < n; ++i) assert(!strcmp(ex[i].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME));
    free(ex);
    PFN_vkCreateInstance create = (void *)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    assert(create && import("vkCreateInstance") == (PFN_vkVoidFunction)create);
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    const char *ext = "VK_ANDROID_external_memory_android_hardware_buffer";
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext};
    VkInstance instances[2] = {0};
    assert(create(&ci, NULL, &instances[0]) == VK_ERROR_EXTENSION_NOT_PRESENT && !instances[0]);
    ext = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    const char *layer = "not-a-layer";
    ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = &layer;
    assert(create(&ci, NULL, &instances[0]) == VK_ERROR_LAYER_NOT_PRESENT && !instances[0]);
    ci.enabledLayerCount = 0;
    VkDebugReportCallbackCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};
    ci.pNext = &debug;
    assert(create(&ci, NULL, &instances[0]) == VK_ERROR_INITIALIZATION_FAILED && !instances[0]);
    ci.pNext = NULL;
    for (unsigned i = 0; i < 2; ++i) assert(create(&ci, NULL, &instances[i]) == VK_SUCCESS);
    assert(instances[0] != instances[1]);
    VkDevice devices[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        VkInstance instance = instances[i];
        assert(!gipa(instance, "vkCreateAndroidSurfaceKHR")); // Host-only window API never leaks.
        assert(!gipa(instance, "vkNotReallyKHR"));
        PFN_vkEnumeratePhysicalDevices physical = (void *)gipa(instance, "vkEnumeratePhysicalDevices");
        VkPhysicalDevice pd; n = 1; VkResult r = physical(instance, &n, &pd);
        assert((r == VK_SUCCESS || r == VK_INCOMPLETE) && n == 1);
        PFN_vkGetPhysicalDeviceProperties2 props = (void *)gipa(instance, "vkGetPhysicalDeviceProperties2");
        PFN_vkGetPhysicalDeviceProperties2 alias = (void *)gipa(instance, "vkGetPhysicalDeviceProperties2KHR");
        assert(props && alias == props);
        VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        alias(pd, &p); assert(p.properties.deviceName[0]);
        PFN_vkGetPhysicalDeviceQueueFamilyProperties queues = (void *)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        n = 0; queues(pd, &n, NULL); assert(n);
        VkQueueFamilyProperties *q = calloc(n, sizeof *q); assert(q); queues(pd, &n, q);
        uint32_t family = 0; while (family < n && !(q[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        assert(family < n); free(q);
        float priority = 1;
        VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
        PFN_vkCreateDevice create_device = (void *)gipa(instance, "vkCreateDevice");
        ext = "VK_EXT_not_implemented"; di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = &ext;
        assert(create_device(pd, &di, NULL, &devices[i]) == VK_ERROR_EXTENSION_NOT_PRESENT && !devices[i]);
        di.enabledExtensionCount = 0;
#ifdef TEST_RESOURCES
        PFN_vkEnumerateDeviceExtensionProperties de = (void *)gipa(instance, "vkEnumerateDeviceExtensionProperties");
        VkExtensionProperties *supported = calloc(16, sizeof *supported); assert(supported); uint32_t supported_count = 16;
        assert(de(pd, NULL, &supported_count, supported) == VK_SUCCESS);
        const char *enabled[16];
        for (uint32_t k = 0; k < supported_count; ++k) enabled[k] = supported[k].extensionName;
        di.enabledExtensionCount = supported_count; di.ppEnabledExtensionNames = enabled;
#endif
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        di.pNext = &features;
        assert(create_device(pd, &di, NULL, &devices[i]) == VK_SUCCESS);
#ifdef TEST_RESOURCES
        free(supported);
#endif
        PFN_vkGetDeviceProcAddr gdpa = (void *)gipa(instance, "vkGetDeviceProcAddr");
        assert(gdpa && !gdpa(devices[i], "vkCreateInstance") && !gdpa(devices[i], "vkGetPhysicalDeviceProperties"));
        assert(!gdpa(devices[i], "vkCreateAndroidSurfaceKHR") && !gdpa(devices[i], "vkNotReallyKHR"));
        PFN_vkGetDeviceQueue queue = (void *)gdpa(devices[i], "vkGetDeviceQueue");
        VkQueue handle; assert(queue); queue(devices[i], family, 0, &handle); assert(handle);
#ifdef TEST_RESOURCES
        resource_checks(instance, pd, devices[i], gdpa);
#endif
    }
    for (int i = 1; i >= 0; --i) {
        PFN_vkDestroyDevice dd = (void *)gipa(instances[i], "vkDestroyDevice");
        PFN_vkDestroyInstance di = (void *)gipa(instances[i], "vkDestroyInstance");
        dd(devices[i], NULL); di(instances[i], NULL);
    }
}

extern void on_stack(uintptr_t top, void (*fn)(void));
__asm__(".text\n.global on_stack\non_stack:\n"
        "stp x19,x30,[sp,#-16]!\nmov x19,sp\nmov sp,x0\nblr x1\nmov sp,x19\nldp x19,x30,[sp],#16\nret\n");
int main(void) {
    gipa = (void *)import("vkGetInstanceProcAddr");
    assert(gipa && "Android guest Vulkan loader must be registered");
    dispatch_test();
    size_t page = 16384, stack = 8192;
    uint8_t *memory = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED && !mprotect(memory, page, PROT_NONE));
    guest_low = (uintptr_t)memory + page; guest_high = guest_low + stack;
    on_stack(guest_high, dispatch_test);
    assert(stack_switches > 20); munmap(memory, page * 2);
    puts("guest Vulkan dispatch: scopes, extension filtering, two independent devices and guarded 8 KiB guest stack passed");
}
