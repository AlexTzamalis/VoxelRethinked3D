#pragma once
// Resource binding slots (Section 7.3 of the plan).
//
// Every resource lives in a (set, binding) pair with a fixed meaning:
//   set 0: per frame data (camera, time, sun, fog), updated once per frame
//   set 1: per pass data, updated once per pass
//   set 2: per material or draw batch data (texture arrays, chunk metadata)
//
// The OpenGL backend flattens (set, binding) into one binding index, 16
// slots per set, and applies the same rule to CommandList::bind* calls.
// A Vulkan backend would use real descriptor sets. Shaders never hard code
// binding numbers; they always go through these macros.
//
// Storage images (CommandList::bindStorageImage) have their own binding
// space with only 8 guaranteed units in OpenGL, so they flatten as
// set * 2 + slot: two image slots per set, sets 0 to 3. The format is part
// of the declaration, for example VXE_IMAGE_BINDING(1, 0, r32f).
#ifdef VXE_VULKAN
#define VXE_BINDING(setIndex, bindingIndex) layout(set = setIndex, binding = bindingIndex)
#define VXE_BINDING_STD430(setIndex, bindingIndex) layout(std430, set = setIndex, binding = bindingIndex)
#define VXE_BINDING_STD140(setIndex, bindingIndex) layout(std140, set = setIndex, binding = bindingIndex)
#define VXE_IMAGE_BINDING(setIndex, bindingIndex, imageFormat)                                                  layout(imageFormat, set = setIndex, binding = bindingIndex)
#else
#define VXE_BINDING(setIndex, bindingIndex) layout(binding = (setIndex * 16 + bindingIndex))
#define VXE_BINDING_STD430(setIndex, bindingIndex) layout(std430, binding = (setIndex * 16 + bindingIndex))
#define VXE_BINDING_STD140(setIndex, bindingIndex) layout(std140, binding = (setIndex * 16 + bindingIndex))
#define VXE_IMAGE_BINDING(setIndex, bindingIndex, imageFormat)                                                  layout(imageFormat, binding = (setIndex * 2 + bindingIndex))
#endif
