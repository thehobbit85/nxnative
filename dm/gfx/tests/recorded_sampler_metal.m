#import <Metal/Metal.h>

int metal_has_sampler_reduction(void) {
    return [MTLCreateSystemDefaultDevice() supportsFamily:MTLGPUFamilyApple10];
}
