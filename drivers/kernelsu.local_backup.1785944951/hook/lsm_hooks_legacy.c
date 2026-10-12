// KernelSU-Next: no-op LSM hooks for kernels without
// include/linux/lsm_hooks.h (e.g. Samsung 3.18).
#include <linux/init.h>

void __init ksu_lsm_hook_init(void)
{
}
