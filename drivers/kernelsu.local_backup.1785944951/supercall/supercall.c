#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/utsname.h> // utsname() and uts_sem

#include "uapi/supercall.h"
#include "supercall/internal.h"
#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/manager_identity.h"

#include "sulog/event.h"

uint32_t ksuver_override = 0;

static dev_t ksu_devt;
static struct cdev ksu_cdev;
static struct class *ksu_class;
static struct device *ksu_dev;
static struct device *ksu_dev_alias;
static bool ksu_dev_ready;
static struct delayed_work ksu_dev_fixup_work;
static bool ksu_dev_fixup_work_inited;
static int ksu_dev_fixup_attempts;
static atomic_t ksu_dev_open_log_budget = ATOMIC_INIT(20);
static atomic_t ksu_dev_ioctl_log_budget = ATOMIC_INIT(160);
static atomic_t ksu_anon_ioctl_log_budget = ATOMIC_INIT(160);

static void ksu_autocrown_current_uid(void)
{
	uid_t uid;

	if (ksu_is_manager_appid_valid())
		return;

	uid = current_uid().val;
	if (uid < 10000)
		return;

	ksu_set_manager_appid(uid % KSU_PER_USER_RANGE);
	pr_info("kernelsu: auto manager_appid=%d (uid=%d)\n",
		ksu_get_manager_appid(), uid);
}

static int anon_ksu_release(struct inode *inode, struct file *filp)
{
	pr_info("ksu fd released\n");
	return 0;
}

static long anon_ksu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long ret;

	ksu_autocrown_current_uid();
	ret = ksu_supercall_handle_ioctl(cmd, (void __user *)arg);
	if (atomic_dec_if_positive(&ksu_anon_ioctl_log_budget) >= 0) {
		pr_info("ksu anon ioctl: cmd=0x%x pid=%d uid=%d ret=%ld\n",
			cmd, current->pid, current_uid().val, ret);
	}
	return ret;
}

static int ksu_dev_open(struct inode *inode, struct file *file)
{
	ksu_autocrown_current_uid();
	if (atomic_dec_if_positive(&ksu_dev_open_log_budget) >= 0) {
		pr_info("kernelsu: open by pid=%d uid=%d\n",
			current->pid, current_uid().val);
	}
	return 0;
}

static int ksu_dev_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long ksu_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret;

	ret = ksu_supercall_handle_ioctl(cmd, (void __user *)arg);
	if (atomic_dec_if_positive(&ksu_dev_ioctl_log_budget) >= 0) {
		pr_info("kernelsu: ioctl cmd=0x%x pid=%d uid=%d ret=%ld\n",
			cmd, current->pid, current_uid().val, ret);
	}
	return ret;
}

static const struct file_operations ksu_dev_fops = {
	.owner = THIS_MODULE,
	.open = ksu_dev_open,
	.release = ksu_dev_release,
	.unlocked_ioctl = ksu_dev_ioctl,
	.compat_ioctl = ksu_dev_ioctl,
};

static char *ksu_devnode(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static int ksu_fixup_one_devnode(const char *path_str)
{
	struct path path;
	struct iattr newattrs;
	int err;

	err = kern_path(path_str, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	memset(&newattrs, 0, sizeof(newattrs));
	newattrs.ia_valid = ATTR_MODE;
	newattrs.ia_mode = (path.dentry->d_inode->i_mode & S_IFMT) | 0666;

	err = notify_change(path.dentry, &newattrs, NULL);
	path_put(&path);
	return err;
}

static void ksu_dev_fixup_workfn(struct work_struct *work)
{
	ksu_fixup_one_devnode("/dev/kernelsu");
	ksu_fixup_one_devnode("/dev/ksu");

	if (++ksu_dev_fixup_attempts < 15)
		schedule_delayed_work(&ksu_dev_fixup_work, msecs_to_jiffies(2000));
}

void ksu_dev_fixup_perms_async(void)
{
	if (!ksu_dev_fixup_work_inited) {
		INIT_DELAYED_WORK(&ksu_dev_fixup_work, ksu_dev_fixup_workfn);
		ksu_dev_fixup_work_inited = true;
	}

	ksu_dev_fixup_attempts = 0;
	schedule_delayed_work(&ksu_dev_fixup_work, msecs_to_jiffies(1500));
}

int ksu_dev_init(void)
{
	int ret;

	if (ksu_dev_ready)
		return 0;

	ksu_dev_ready = false;

	ret = alloc_chrdev_region(&ksu_devt, 0, 2, "kernelsu");
	if (ret)
		return ret;

	cdev_init(&ksu_cdev, &ksu_dev_fops);
	ksu_cdev.owner = THIS_MODULE;

	ret = cdev_add(&ksu_cdev, ksu_devt, 2);
	if (ret)
		goto err_unregister;

	ksu_class = class_create(THIS_MODULE, "kernelsu");
	if (IS_ERR(ksu_class)) {
		ret = PTR_ERR(ksu_class);
		ksu_class = NULL;
		goto err_cdev_del;
	}

	ksu_class->devnode = ksu_devnode;

	ksu_dev = device_create(ksu_class, NULL, ksu_devt, NULL, "kernelsu");
	if (IS_ERR(ksu_dev)) {
		ret = PTR_ERR(ksu_dev);
		ksu_dev = NULL;
		goto err_class_destroy;
	}

	ksu_dev_alias = device_create(ksu_class, NULL,
				      MKDEV(MAJOR(ksu_devt), MINOR(ksu_devt) + 1),
				      NULL, "ksu");
	if (IS_ERR(ksu_dev_alias)) {
		ret = PTR_ERR(ksu_dev_alias);
		ksu_dev_alias = NULL;
		goto err_dev_destroy;
	}

	pr_info("kernelsu dev created: /dev/kernelsu (%d:%d), /dev/ksu (%d:%d)\n",
		MAJOR(ksu_devt), MINOR(ksu_devt),
		MAJOR(ksu_devt), MINOR(ksu_devt) + 1);

	ksu_dev_ready = true;
	ksu_dev_fixup_perms_async();
	return 0;

err_dev_destroy:
	device_destroy(ksu_class, ksu_devt);
	ksu_dev = NULL;
err_class_destroy:
	class_destroy(ksu_class);
	ksu_class = NULL;
err_cdev_del:
	cdev_del(&ksu_cdev);
err_unregister:
	unregister_chrdev_region(ksu_devt, 2);
	return ret;
}

void ksu_dev_exit(void)
{
	if (!ksu_dev_ready)
		return;

	if (ksu_dev_fixup_work_inited) {
		cancel_delayed_work_sync(&ksu_dev_fixup_work);
		ksu_dev_fixup_work_inited = false;
	}

	if (ksu_dev_alias && ksu_class)
		device_destroy(ksu_class,
			       MKDEV(MAJOR(ksu_devt), MINOR(ksu_devt) + 1));
	ksu_dev_alias = NULL;

	if (ksu_dev && ksu_class)
		device_destroy(ksu_class, ksu_devt);
	ksu_dev = NULL;

	if (ksu_class)
		class_destroy(ksu_class);
	ksu_class = NULL;

	cdev_del(&ksu_cdev);
	unregister_chrdev_region(ksu_devt, 2);
	ksu_dev_ready = false;
}

static const struct file_operations anon_ksu_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = anon_ksu_ioctl,
	.compat_ioctl = anon_ksu_ioctl,
	.release = anon_ksu_release,
};

int ksu_install_fd(void)
{
	struct file *filp;
	int fd;

	ksu_autocrown_current_uid();

	// Get unused fd
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		pr_err("ksu_install_fd: failed to get unused fd\n");
		return fd;
	}

	// Create anonymous inode file
	filp = anon_inode_getfile("[ksu_driver]", &anon_ksu_fops, NULL, O_RDWR | O_CLOEXEC);
	if (IS_ERR(filp)) {
		pr_err("ksu_install_fd: failed to create anon inode file\n");
		put_unused_fd(fd);
		return PTR_ERR(filp);
	}

	// Install fd
	fd_install(fd, filp);

	pr_info("ksu fd installed: %d for pid %d\n", fd, current->pid);

	return fd;
}

int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
			  void __user **arg)
{
	if (magic1 != KSU_INSTALL_MAGIC1)
		return 0;

#ifdef CONFIG_KSU_DEBUG
	pr_info("sys_reboot: intercepted call! magic: 0x%x id: %d\n", magic1,
		magic2);
#endif

	// Check if this is a request to install KSU fd
	if (magic2 == KSU_INSTALL_MAGIC2) {
		int fd = ksu_install_fd();
		// downstream: dereference all arg usage!
		if (copy_to_user((void __user *)*arg, &fd, sizeof(fd))) {
			pr_err("install ksu fd reply err\n");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		close_fd(fd);
#else
		__close_fd(current->files, fd);
#endif
		}
		return 0;
	}

	// extensions 
	u64 reply = (u64)*arg;

	if (magic2 == CHANGE_MANAGER_UID) {
		// only root is allowed for this command
		if (current_uid().val != 0)
			return 0;

		pr_info("sys_reboot: ksu_set_manager_appid to: %d\n", cmd);
		ksu_set_manager_appid(cmd);

		if (cmd == ksu_get_manager_appid()) {
			if (copy_to_user((void __user *)*arg, &reply, sizeof(reply)))
				pr_info("sys_reboot: reply fail\n");
		}

		return 0;
	}
	
	if (magic2 == GET_SULOG_DUMP_V2) {
		// only root is allowed for this command
		if (current_uid().val != 0)
			return 0;

		int ret = ksu_sulog_handle_compat_dump((void __user *)*arg);
		if (ret)
			return 0;

		if (copy_to_user((void __user *)*arg, &reply, sizeof(reply) ))
			return 0;
	}

	if (magic2 == CHANGE_KSUVER) {
		// only root is allowed for this command
		if (current_uid().val != 0)
			return 0;

		pr_info("sys_reboot: ksu_change_ksuver to: %d\n", cmd);
		ksuver_override = cmd;

		if (copy_to_user((void __user *)*arg, &reply, sizeof(reply) ))
			return 0;
	}

	// WARNING!!! triple ptr zone! ***
	// https://wiki.c2.com/?ThreeStarProgrammer
	if (magic2 == CHANGE_SPOOF_UNAME) {
		// only root is allowed for this command 
		if (current_uid().val != 0)
			return 0;

		char release_buf[65];
		char version_buf[65];
		static char original_release_buf[65] = {0};
		static char original_version_buf[65] = {0};

		// basically void * void __user * void __user *arg
		void ***ppptr = (uintptr_t)arg;

		// user pointer storage
		// init this as zero so this works on 32-on-64 compat (LE)
		uint64_t u_pptr = 0;
		uint64_t u_ptr = 0;

		pr_info("sys_reboot: ppptr: 0x%lx \n", ppptr);

		// arg here is ***, dereference to pull out **
		if (copy_from_user(&u_pptr, (void __user *)*ppptr, sizeof(u_pptr)))
			return 0;

		pr_info("sys_reboot: u_pptr: 0x%lx \n", u_pptr);

		// now we got the __user **
		// we cannot dereference this as this is __user
		// we just do another copy_from_user to get it
		if (copy_from_user(&u_ptr, (void __user *)u_pptr, sizeof(u_ptr)))
			return 0;

		pr_info("sys_reboot: u_ptr: 0x%lx \n", u_ptr);

		// for release
		if (strncpy_from_user(release_buf, (char __user *)u_ptr, sizeof(release_buf)) < 0)
			return 0;
		release_buf[sizeof(release_buf) - 1] = '\0'; 

		// for version
		if (strncpy_from_user(version_buf, (char __user *)(u_ptr + strlen(release_buf) + 1), sizeof(version_buf)) < 0)
			return 0;
		version_buf[sizeof(version_buf) - 1] = '\0'; 

		if (original_release_buf[0] == '\0') {
			struct new_utsname *u_curr = utsname();
			// we save current version as the original before modifying
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
			strscpy(original_release_buf, u_curr->release, sizeof(original_release_buf));
			strscpy(original_version_buf, u_curr->version, sizeof(original_version_buf));
#else
			strlcpy(original_release_buf, u_curr->release, sizeof(original_release_buf));
			strlcpy(original_version_buf, u_curr->version, sizeof(original_version_buf));
#endif
			pr_info("sys_reboot: original uname saved: %s %s\n", original_release_buf, original_version_buf);
		}

		// so user can reset
		if (!strcmp(release_buf, "default") || !strcmp(version_buf, "default") ) {
			memcpy(release_buf, original_release_buf, sizeof(release_buf));
			memcpy(version_buf, original_version_buf, sizeof(version_buf));
		}

		pr_info("sys_reboot: spoofing kernel to: %s - %s\n", release_buf, version_buf);

		struct new_utsname *u = utsname();

		down_write(&uts_sem);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
		strscpy(u->release, release_buf, sizeof(u->release));
		strscpy(u->version, version_buf, sizeof(u->version));
#else
		strlcpy(u->release, release_buf, sizeof(u->release));
		strlcpy(u->version, version_buf, sizeof(u->version));
#endif
		up_write(&uts_sem);

		// we write our confirmation on **
		if (copy_to_user((void __user *)*arg, &reply, sizeof(reply)))
			return 0;
	}

	return 0;
}

#ifdef KSU_KPROBES_HOOK
static int reboot_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	int magic1 = (int)PT_REGS_PARM1(real_regs);
	int magic2 = (int)PT_REGS_PARM2(real_regs);
	unsigned int cmd = (unsigned int)PT_REGS_PARM3(real_regs);
	unsigned long arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(real_regs);
	unsigned long reply = (unsigned long)arg4;

	return ksu_handle_sys_reboot(magic1, magic2, cmd, (void __user **)&arg4);
}

static struct kprobe reboot_kp = {
	.symbol_name = REBOOT_SYMBOL,
	.pre_handler = reboot_handler_pre,
};
#endif

void __init ksu_supercalls_init(void)
{
	int i;

	ksu_supercall_dump_commands();

#ifdef KSU_KPROBES_HOOK
	int rc = register_kprobe(&reboot_kp);
	if (rc) {
		pr_err("reboot kprobe failed: %d\n", rc);
	} else {
		pr_info("reboot kprobe registered successfully\n");
	}
#endif
}

void __exit ksu_supercalls_exit(void){
	struct mount_entry *entry, *tmp;

#ifdef KSU_KPROBES_HOOK
	unregister_kprobe(&reboot_kp);
#endif

	ksu_dev_exit();

	ksu_supercall_cleanup_state();
}
