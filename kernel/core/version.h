/*
 * Kernel version. Modules declare the minimum version they need.
 */

#ifndef CORE_VERSION_H
#define CORE_VERSION_H

#define KERNEL_VERSION(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))

#define KERNEL_VERSION_MAJOR 0
#define KERNEL_VERSION_MINOR 10
#define KERNEL_VERSION_PATCH 0
#define KERNEL_VERSION_CODE  KERNEL_VERSION(KERNEL_VERSION_MAJOR, KERNEL_VERSION_MINOR, KERNEL_VERSION_PATCH)
#define KERNEL_VERSION_STRING "0.10.0"

#endif
