# Tutorial: Bypass Play Integrity DEVICE + STRONG trên ROM port Android 10
## Phương án A — KernelSU-Next built-in vào kernel + module stack pre-baked vào ROM

> **Áp dụng cho**: `G950X_A10_Exynos8895` (Galaxy S8 — dreamlte/dream2lte) và `J730X_A10_Exynos7870` (Galaxy J7 2017 — j7y17lte)
>
> **Mục tiêu**: Sau khi flash ROM xong → khởi động → chạy `Play Integrity API Checker` → pass `MEETS_DEVICE_INTEGRITY` + `MEETS_STRONG_INTEGRITY` mà **KHÔNG cần cài đặt gì thêm**.

---

## 0. Cảnh báo bắt buộc

1. **Keybox là tài sản cá nhân** — không chia sẻ. Google revoke keybox lộ ra ngoài trong vài ngày.
2. **Không có keybox hợp lệ = không có DEVICE/STRONG**. KernelSU-Next chỉ là "ống dẫn", không tạo ra keybox.
3. Bài này nhằm mục đích kỹ thuật/cá nhân. Tuân thủ ToS của Google và pháp luật nơi bạn ở.
4. Mọi thao tác kernel/ROM có rủi ro brick. **Backup EFS và ROM gốc trước khi thử.**

---

## 1. Kiến thức nền tảng (đọc kỹ trước khi làm)

### 1.1 Luồng Play Integrity trên Android 10

```
App kiểm tra → Google Play Services (GMS) → DroidGuard
    ├── Đọc build fingerprint (qua shared memory property_service)
    ├── Đọc /proc, /sys, SELinux state
    └── Key Attestation → Keymaster 4.0 HAL → TrustZone (TEE)
```

### 1.2 Vai trò từng thành phần

| Thành phần | Vai trò | Pass verdict |
|---|---|---|
| **KernelSU-Next** (trong kernel) | Root ngầm + Zygisk sẵn có, không cần app root sau flash | — |
| **Zygisk** (có sẵn trong KernelSU-Next) | Inject vào GMS / keymaster HAL process | — |
| **PlayIntegrityFix (PIF)** | Spoof fingerprint vào GMS process | BASIC |
| **TrickyStore** | Spoof key attestation, dùng keybox trả cert chain, báo bootloader locked | DEVICE + STRONG |
| **keybox.xml** | Cặp khóa EC + cert chain của thiết bị Google | DEVICE + STRONG |

### 1.3 Tại sao Android 10 (kernel 4.4) lại dùng nhánh "legacy" của KernelSU-Next

- G950X/J730X dùng kernel **4.4.x** (Exynos), **non-GKI**.
- KernelSU-Next hỗ trợ non-GKI từ kernel 4.x trở xuống (backport tới 4.14 và sớm hơn).
- Kernel 4.4 không có `CONFIG_KPROBES` ổn định trên mọi bản Samsung → ưu tiên **manual integrate** (mục 5). Nếu kprobe chạy được trên kernel của bạn thì dùng mục 4 (nhanh hơn).

---

## 2. Chuẩn bị môi trường

### 2.1 Máy build (Linux — bắt buộc)

- Ubuntu 20.04/22.04 (hoặc WSL2 nếu bạn đang ở Windows, nhưng khuyên dùng máy Linux/VM riêng)
- Disk trống ≥ 60GB, RAM ≥ 8GB
- Các gói cần thiết:

```bash
sudo apt update && sudo apt install -y \
    git make gcc g++ flex bison libssl-dev libncurses5-dev \
    bc python3 python3-pip cpio device-tree-compiler \
    android-tools-mkbootimg unzip zip
```

### 2.2 Toolchain (GCC ARM64 — dành cho kernel 4.4 Samsung)

Samsung kernel 4.4 Exynos **không build được với Clang**, phải dùng GCC ARM64:

```bash
mkdir -p ~/a10/toolchains && cd ~/a10/toolchains
# GCC 8.3 ARM64 (linaro) — phổ biến cho Exynos 8895/7870
wget https://releases.linaro.org/components/toolchain/binaries/latest-7/aarch64-linux-gnu/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu.tar.xz
tar -xf gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu.tar.xz
export CROSS_COMPILE=$HOME/a10/toolchains/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-
```

> **Mẹo**: Nếu ROM port bạn dùng có kernel source đi kèm (thường các dev port để kèm `Kernel/` trong thư mục ROM), dùng đúng toolchain họ ghi trong `README`/`build.sh` để tránh lỗi version.

---

## 3. Lấy kernel source

### 3.1 Nguồn kernel source

1. **opensource.samsung.com** — tải kernel source chính hãng theo model:
   - SM-G950F / SM-G955F (Exynos 8895): package `SM-G950F_..._KernelSource` cho nền One UI 2 / Android 10
   - SM-J730F (Exynos 7870): package tương ứng
2. **ROM port dev** — một số dev đăng kernel source đã port lên GitHub (tìm `exynos8895 android10 kernel`, `j730 android10 kernel`)
3. **Kernel của chính ROM** — nếu ROM bạn đang dùng có công khai source, ưu tiên nó (đã có sẵn config + dtb + ramdisk khớp).

```bash
mkdir -p ~/a10/kernel && cd ~/a10/kernel
# Ví dụ nếu dev đăng trên GitHub:
git clone https://github.com/<dev>/exynos8895_android10_kernel.git kernel-g950
cd kernel-g950
```

> ⚠️ **Quan trọng**: Kernel phải build ra được `boot.img` boot được **trước khi** thêm KernelSU-Next. Nếu chưa build được kernel, dừng lại — phần còn lại vô nghĩa.

---

## 4. Tích hợp KernelSU-Next — Cách 1: kprobe (nhanh)

> Chỉ dùng cách này nếu kernel của bạn có `CONFIG_KPROBES` hoạt động ổn định. Sau khi build xong mà bootloop → chuyển sang cách 2 (mục 5).

```bash
cd ~/a10/kernel/kernel-g950

# Tải và chạy script tích hợp KernelSU-Next (nhánh legacy cho non-GKI)
curl -LSs "https://raw.githubusercontent.com/KernelSU-Next/KernelSU-Next/next/kernel/setup.sh" | bash -s legacy
```

Sau đó mở file defconfig (mục 6) và thêm:

```text
CONFIG_KPROBES=y
CONFIG_KPROBE_EVENTS=y
CONFIG_KSU_KPROBE_HOOKS=y
CONFIG_KSU=y
```

Build và test. Nếu kprobe hỏng (bootloop) → **chuyển mục 5**.

---

## 5. Tích hợp KernelSU-Next — Cách 2: manual (khuyến nghị cho kernel 4.4 Samsung)

### 5.1 Thêm source

```bash
cd ~/a10/kernel/kernel-g950
curl -LSs "https://raw.githubusercontent.com/KernelSU-Next/KernelSU-Next/next/kernel/setup.sh" | bash -s legacy
```

### 5.2 Patch 5 hàm trong kernel source

Tìm và chèn các đoạn gọi `ksu_handle_*` theo đúng vị trí:

**a) `fs/exec.c`** — trong `do_execveat_common()`:

```c
#include <linux/fs.h>
/* ... */
#ifdef CONFIG_KSU
extern int ksu_handle_execveat(int *fd, struct filename **filename_ptr,
                               void *argv, void *envp, int *flags);
#endif
/* ngay trước return retval; trong do_execveat_common */
#ifdef CONFIG_KSU
    ksu_handle_execveat(&fd, &filename, argv, envp, &flags);
#endif
```

**b) `fs/open.c`** — trong `SYSCALL_DEFINE3(openat, ...)`:

```c
#ifdef CONFIG_KSU
extern int ksu_handle_openat(int *dfd, const char __user **filename,
                             int *flags, umode_t *mode);
#endif
/* đầu hàm, sau khi khai báo */
#ifdef CONFIG_KSU
    ksu_handle_openat(&dfd, &filename, &flags, &mode);
#endif
```

**c) `fs/read_write.c`** — trong `vfs_read()`:

```c
#ifdef CONFIG_KSU
extern int ksu_handle_vfs_read(struct file **file_ptr, char __user **buf_ptr,
                               size_t *count_ptr, loff_t **pos_ptr);
#endif
/* ngay đầu hàm vfs_read */
#ifdef CONFIG_KSU
    ksu_handle_vfs_read(&file, &buf, &count, &pos);
#endif
```

**d) `fs/stat.c`** — trong `SYSCALL_DEFINE4(newfstatat, ...)`:

```c
#ifdef CONFIG_KSU
extern int ksu_handle_stat(int *dfd, const char __user **filename, int *flags);
#endif
#ifdef CONFIG_KSU
    ksu_handle_stat(&dfd, &filename, &flags);
#endif
```

**e) `kernel/reboot.c`** — trong `SYSCALL_DEFINE4(reboot, ...)`:

```c
#ifdef CONFIG_KSU
extern int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
                                 void __user **arg);
#endif
#ifdef CONFIG_KSU
    ksu_handle_sys_reboot(magic1, magic2, cmd, &arg);
#endif
```

> Các đoạn `extern` chỉ nên nằm dưới `#ifdef CONFIG_KSU` để khi tắt KSU vẫn build sạch.

### 5.3 Bật `CONFIG_KSU` trong defconfig (xem mục 6)

---

## 6. Cấu hình defconfig

Defconfig của Samsung nằm ở:
```text
arch/arm64/configs/
# thường: exynos8895_defconfig  (G950X)
# hoặc:   exynos7870_defconfig  (J730X)
# có thể nằm trong arch/arm64/configs/vendor/ trên vài bản
```

```bash
cd ~/a10/kernel/kernel-g950
ls arch/arm64/configs/            # tìm đúng tên
```

Thêm các dòng vào CUỐI defconfig:

```text
# KernelSU Next
CONFIG_KSU=y
# Nếu dùng kprobe (mục 4):
CONFIG_KPROBES=y
CONFIG_KPROBE_EVENTS=y
CONFIG_KSU_KPROBE_HOOKS=y
```

> Kiểm tra không trùng/thiếu phụ thuộc: chạy `make ARCH=arm64 menuconfig` → Search `KSU` / `KPROBES` → bật các mục phụ thuộc nếu bị tắt. Nếu build báo thiếu `CONFIG_MODULES`, bật `CONFIG_MODULES=y`.

---

## 7. Build kernel

```bash
cd ~/a10/kernel/kernel-g950

export ARCH=arm64
export CROSS_COMPILE=$HOME/a10/toolchains/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-

# 1. Make defconfig
make exynos8895_defconfig        # đổi tên cho đúng device

# 2. Build
make -j$(nproc)                  # hoặc make -j8 nếu RAM ít

# 3. Sản phẩm
ls -la arch/arm64/boot/Image     # kernel Image mới (đã có KernelSU-Next)
```

> - Nếu ROM dùng `Image.gz-dtb` hoặc `boot.img` kiểu Samsung, output có thể khác → xem `arch/arm64/boot/` để biết.
> - **Test trước**: đóng gói kernel này thành boot.img thử, boot thử trên máy. Xác nhận trong terminal: `su` hoạt động (KernelSU đã built-in) trước khi làm phần ROM.

---

## 8. Repack boot.img và thay vào ROM

Cấu trúc ROM của bạn có sẵn:
```text
vnrom/kernel/boot.img       ← kernel+ramdisk đóng gói
vnrom/kernel/boot.tar       ← bản cho Odin
vnrom/kernel/dreamlte.img   ← tên theo device
vnrom/kernel/ramdisk.img    ← ramdisk tách riêng
```

### 8.1 Dùng Android Image Kitchen (AIK) để repack

```bash
# Trên Windows (hoặc Linux): tải Android Image Kitchen (AIK)
# Giải nén AIK, đặt boot.img gốc vào, chạy:
./unpackimg.sh boot.img
# → thu được split_img/ (header, ramdisk) và ramdisk/ (nội dung)
# → thay split_img/Image bằng kernel Image vừa build (đổi tên đúng như cũ)
./repackimg.sh
# → sinh boot-new.img
```

### 8.2 Cập nhật vào ROM zip

- Đổi `boot-new.img` → ghi đè `vnrom/kernel/boot.img`
- Nếu updater-script của ROM dùng `package_extract_file("vnrom/kernel/boot.img", ...)` thì giữ nguyên tên.
- `ramdisk.img` giữ nguyên (chỉ kernel thay đổi) trừ khi kernel mới cần ramdisk khác.
- Nếu flash bằng Odin: tạo lại `boot.tar` từ boot.img mới:
  ```bash
  tar -cf boot.tar boot.img
  ```

> ⚠️ **Kiểm tra META-INF/com/google/android/updater-script** để xem đúng đường dẫn kernel được extract, tránh đặt sai.

---

## 9. Pre-bake module stack vào ROM (chạy ngay sau flash)

> Đây là điểm mấu chốt để "flash xong là có ngay". KernelSU-Next quét `/data/adb/modules/` lúc boot. Ta dùng **script lúc flash** (recovery) để dựng sẵn `/data/adb/modules/` + file keybox trước khi boot Android lần đầu.

### 9.1 Chuẩn bị các module zip

Các file bạn đã có trong `play_integrity/`:
```text
PIF-Next_v3.0_release.zip
PlayIntegrityFix_v4.7-1-inject-s.zip
PlayIntegrityFix-Strong Integrity🔥-v1.7.zip
specter-v1.4.4-17.zip
keybox.xml
keybox_fateh7.xml
```

> **KernelSU-Next v3.x có sẵn Zygisk** — không cần module ZygiskNext riêng. Nếu bản bạn dùng không có, tải thêm `ZygiskNext` từ `https://github.com/Dr-TSNG/ZygiskNext/releases`.

Giải nén từng module zip để lấy cấu trúc thư mục:
```bash
mkdir -p ~/a10/prebake && cd ~/a10/prebake
unzip ~/a10/play_integrity/PlayIntegrityFix_v4.7-1-inject-s.zip -d pif
unzip ~/a10/play_integrity/specter-v1.4.4-17.zip -d specter
# TrickyStore tải từ https://github.com/5ec1cff/TrickyStore/releases
unzip ~/a10/play_integrity/TrickyStore_*.zip -d trickystore
```

### 9.2 Tạo thư mục pre-bake cho ROM

```text
G950X_A10_Exynos8895/
├── vnrom/
│   ├── kernel/                          ← boot.img mới (đã có KSU)
│   └── scripts/
│       ├── ... (init.sh, mount.sh, end.sh)
│       └── playintegrity.sh             ← SCRIPT MỚI
├── system/system/vnrom/
│   └── pi_modules/                      ← chứa các module zip
│       ├── pif.zip
│       ├── trickystore.zip
│       ├── specter.zip
│       └── keybox.xml                   ← keybox hiện hành (bản chưa revoked)
```

### 9.3 Script `playintegrity.sh` (chạy trong recovery, cuối quá trình flash)

Tạo file `vnrom/scripts/playintegrity.sh`:

```sh
#!/sbin/sh
# Play Integrity pre-bake — chạy lúc flash (recovery)
# Được gọi từ updater-script SAU khi mount /data

DATA=/data
MODDIR=/data/adb/modules
TSDIR=/data/adb/tricky_store
SRC=/system/system/vnrom/pi_modules

# Mount data nếu chưa mount
mount /data 2>/dev/null || mount -o rw /dev/block/by-name/userdata $DATA 2>/dev/null

# Dọn cũ
rm -rf $MODDIR/playintegrityfix $MODDIR/tricky_store $MODDIR/specter

# 1. PIF
mkdir -p $MODDIR/playintegrityfix
cd $MODDIR/playintegrityfix
unzip -o $SRC/pif.zip 2>/dev/null
[ -f customize.sh ] && sh customize.sh
echo "date: $(date)" > update.log

# 2. TrickyStore
mkdir -p $MODDIR/tricky_store
cd $MODDIR/tricky_store
unzip -o $SRC/trickystore.zip 2>/dev/null
[ -f customize.sh ] && sh customize.sh

# 3. Specter (tùy chọn, hỗ trợ thêm)
mkdir -p $MODDIR/specter
cd $MODDIR/specter
unzip -o $SRC/specter.zip 2>/dev/null
[ -f customize.sh ] && sh customize.sh

# 4. Keybox cho TrickyStore
mkdir -p $TSDIR
cp $SRC/keybox.xml $TSDIR/keybox.xml
chmod 644 $TSDIR/keybox.xml

# Đảm bảo quyền
chmod -R 755 $MODDIR
sync
```

### 9.4 Gọi script từ `updater-script`

Tìm trong `META-INF/com/google/android/updater-script` chỗ cuối (sau khi mount `/system` và `/data`):

```edify
package_extract_file("vnrom/scripts/playintegrity.sh", "/tmp/playintegrity.sh");
set_metadata("/tmp/playintegrity.sh", "uid", 0, "gid", 0, "mode", 0755);
run_program("/sbin/sh", "/tmp/playintegrity.sh");
```

> Một số ROM Samsung dùng `META-INF/com/google/android/aroma-config` hoặc `update-binary` custom. Xem cách ROM gọi `init.sh`/`end.sh` hiện tại để gọi tương tự.

### 9.5 Cách hoạt động sau flash

1. Flash xong → recovery dựng sẵn `/data/adb/modules/*` + `/data/adb/tricky_store/keybox.xml`
2. Boot Android → kernel có KernelSU-Next built-in → `ksud` quét `/data/adb/modules` → load PIF/TrickyStore/Specter
3. GMS chạy → PIF spoof fingerprint (BASIC) + TrickyStore can thiệp attestation bằng keybox (DEVICE/STRONG)
4. **Không cần cài app nào**

---

## 10. Cấu hình keybox (TrickyStore)

### 10.1 Kiểm tra keybox còn sống không

Keybox bị Google revoke theo CRL. Kiểm tra trước khi nhúng:

```bash
# Lấy serial của cert trong keybox.xml
# Tìm <Certificate> bên trong <Keybox>, giải mã base64, lấy serial
openssl x509 -in cert.der -inform DER -noout -serial

# Tra cứu trên CRL của Google
curl -s "https://android.googleapis.com/attestation/status" -o status.txt
# (endpoint công khai dùng để tra trạng thái revoke)
```

> Có tool sẵn: **Keybox Checker** (web/ứng dụng) hoặc dùng `Key Attestation Demo` (vvb2060) trên máy để xem keybox hiện tại có pass không.

### 10.2 Chọn keybox đưa vào ROM

- Ưu tiên keybox **chưa bị lộ công khai**, mới cài được.
- 2 file `keybox.xml` / `keybox_fateh7.xml` trong `play_integrity/` — test từng cái bằng cách đặt vào `/data/adb/tricky_store/keybox.xml`, reboot, chạy checker. File nào pass DEVICE+STRONG thì nhúng file đó.

### 10.3 Cấu hình TrickyStore báo bootloader locked

Mặc định TrickyStore đã spoof bootloader as locked. Nếu cần tinh chỉnh, file config ở `/data/adb/tricky_store/config.ini` (tạo nếu chưa có):

```ini
# Mức bảo mật attestation
secure_storage=2
# Cấp keystore báo cho hệ thống
keystore_state=2
```

> **Lưu ý ROM này có sẵn `end.sh` đổi `ro.product.model=SM-N960N` toàn cục.** PIF chỉ spoof trong GMS, nên Play Integrity vẫn dựa trên fingerprint do PIF cung cấp. Tuy nhiên nếu thấy kết quả lạ (VD app đọc model khác fingerprint), hãy tắt `end.sh` không set model hoặc đổi cho khớp fingerprint PIF.

---

## 11. Cấu hình PIF fingerprint

### 11.1 Chọn fingerprint đúng Android version

ROM là Android 10 → chọn fingerprint của thiết bị Google chạy **Android 10/11** để khớp:

```json
// /data/adb/modules/playintegrityfix/custom.pif.json
{
  "PRODUCT": "redfin",
  "DEVICE": "redfin",
  "MANUFACTURER": "Google",
  "BRAND": "google",
  "MODEL": "Pixel 5",
  "FINGERPRINT": "google/redfin/redfin:11/RQ3A.211001.001/7474184:user/release-keys",
  "SECURITY_PATCH": "2021-10-05",
  "DEVICE_INITIAL_SDK_INT": "30"
}
```

### 11.2 Cập nhật fingerprint vào ROM

Với PIF-Next / PIF inject (bản bạn có), file fingerprint có thể nằm sẵn trong module. Ghi đè trước khi đóng ROM:

```bash
cp custom.pif.json ~/a10/prebake/pif/playintegrityfix/custom.pif.json
# hoặc nếu module đọc pif.json ở thư mục gốc module
```

> **Cập nhật định kỳ**: fingerprint bị Google "đốt" sau 1 thời gian → phải đổi. Xem mục 13.

### 11.3 Bật các tùy chọn PIF

Từ KernelSU-Next WebUI (hoặc chỉnh file config của PIF):
- **Use preview fingerprint**: ON (kéo dài tuổi thọ fingerprint)
- **Spoof Provider**: ON (chặn DroidGuard key attestation — kết hợp với TrickyStore)
- **Spoof Props**: ON nếu cần che `ro.build.type`

---

## 12. Kiểm tra & verify

### 12.1 Sau khi flash

```sh
# Root hoạt động (KernelSU built-in)
adb shell su -c id

# KernelSU-Next đã load?
adb shell su -c "ksud status"

# Module đã load?
adb shell su -c "ls /data/adb/modules/"

# TrickyStore keybox có không?
adb shell su -c "ls -la /data/adb/tricky_store/keybox.xml"

# PIF fingerprint đang dùng
adb shell su -c "getprop ro.build.fingerprint"
```

### 12.2 Clear GMS data (quan trọng — fingerprint cũ bị cache)

Lần boot đầu tiên, GMS còn cache fingerprint cũ của ROM (SM-N960N). Cần clear:

```sh
adb shell pm clear com.google.android.gms
adb shell pm clear com.google.android.gms.unstable 2>/dev/null
adb shell am force-stop com.android.vending
adb reboot
```

> Muốn tự động: thêm vào `playintegrity.sh` (chạy lúc flash trước khi boot) không có tác dụng vì GMS chưa chạy. Thay vào đó dùng **script first-boot** trong ROM (service chạy 1 lần) — ví dụ file ở `/system/system/etc/init/` hoặc `/system/addon.d/` chạy `pm clear` một lần rồi tự xóa.

### 12.3 Chạy Play Integrity API Checker

- Mở Play Store tải `Play Integrity API Checker` (gr.nikolasspyr.integritycheck)
- Chạy → mong đợi:

```text
MEETS_BASIC_INTEGRITY   ✅
MEETS_DEVICE_INTEGRITY  ✅
MEETS_STRONG_INTEGRITY  ✅
MEETS_VIRTUAL_INTEGRITY ❌ (đúng, chỉ cho emulator)
```

### 12.4 Kiểm tra thêm (tùy chọn)

```sh
# Kiểm tra kernel name không nằm trong blacklist Google
adb shell uname -r

# Kiểm tra SELinux (phải Enforcing)
adb shell getenforce

# Key Attestation Demo để xem cert chain do TrickyStore trả
# (tải từ vvb2060/KeyAttestation) — root attestation key phải khớp keybox
```

---

## 13. Auto-update keybox + fingerprint (khi có phiên bản mới)

### 13.1 Kiến trúc

```
Server riêng của bạn (GitHub repo private / API đơn giản)
├── pif.json            ← fingerprint mới nhất
├── keybox.xml          ← keybox mới nhất (chưa revoked)
└── version             ← số phiên bản để so sánh

Daemon trên máy (chạy mỗi 6h)
├── Tải version mới → so sánh
├── Cập nhật /data/adb/modules/playintegrityfix/custom.pif.json
├── Cập nhật /data/adb/tricky_store/keybox.xml
└── Clear GMS data + restart GMS
```

### 13.2 Script updater (`/data/adb/modules/playintegrityfix/update.sh` + cron/service)

```sh
#!/system/bin/sh
# update.sh — chạy định kỳ
SERVER="https://your-server.example.com"
DATA_DIR=/data/local/piupdater
MOD_PIF=/data/adb/modules/playintegrityfix

mkdir -p $DATA_DIR

# 1. Check version
CUR=$(cat $DATA_DIR/version 2>/dev/null)
NEW=$(curl -s "$SERVER/version")

if [ "$CUR" = "$NEW" ]; then
    echo "Up to date"
    exit 0
fi

# 2. Tải fingerprint mới
curl -s "$SERVER/pif.json" -o $DATA_DIR/pif.json
if [ -s $DATA_DIR/pif.json ]; then
    cp $DATA_DIR/pif.json $MOD_PIF/custom.pif.json
    chmod 644 $MOD_PIF/custom.pif.json
fi

# 3. Tải keybox mới
curl -s "$SERVER/keybox.xml" -o $DATA_DIR/keybox.xml
if grep -q "AndroidAttestation" $DATA_DIR/keybox.xml; then
    cp $DATA_DIR/keybox.xml /data/adb/tricky_store/keybox.xml
    chmod 644 /data/adb/tricky_store/keybox.xml
fi

# 4. Ghi version
echo "$NEW" > $DATA_DIR/version

# 5. Reset GMS để áp dụng
pm clear com.google.android.gms
am force-stop com.android.vending

echo "Updated to $NEW"
```

### 13.3 Lên lịch chạy

Thêm service trong ROM (first-boot không cần, đây là định kỳ):

```text
# /system/system/etc/init/piupdater.rc
service piupdater /system/bin/sh /data/adb/modules/playintegrityfix/update.sh
    class late_start
    user root
    group root
    oneshot
    disabled
    seclabel u:r:su:s0

# Trigger mỗi 6h qua AlarmManager app hoặc crontab (toybox crontab)
```

> Nếu chưa có daemon service: dùng app nhỏ hoặc chạy bằng tay `su -c sh update.sh` khi cần. Đơn giản nhất là một **app nhỏ** tự build bằng Tasker/Tasker-like chạy shell mỗi 6h.

### 13.4 Phát hiện keybox bị revoke (tự động cảnh báo)

```sh
# Trong update.sh — kiểm tra CRL Google
# 1. Lấy serial của keybox đang dùng
# 2. Tra /data/local/piupdater/crl (tải định kỳ từ android.googleapis.com/attestation/status)
# 3. Nếu serial xuất hiện → keybox chết → cảnh báo + fallback fingerprint-only (DEVICE giảm xuống BASIC)
```

---

## 14. Xử lý sự cố (troubleshooting)

| Triệu chứng | Nguyên nhân | Xử lý |
|---|---|---|
| **Bootloop sau khi build kernel** | kprobe hỏng | Comment `ksu_sucompat_init()` và `ksu_ksud_init()` trong `kernel/ksu.c` để test; hoặc chuyển manual integrate (mục 5) |
| **Root không hoạt động** | CONFIG_KSU chưa bật / toolchain sai | Check `make menuconfig` → Search KSU; build lại sạch (`make clean`) |
| **Module không load sau flash** | `/data/adb/modules` chưa dựng đúng lúc flash | Kiểm tra `playintegrity.sh` chạy chưa (log recovery); mount `/data` trước |
| **Pass BASIC, fail DEVICE** | Keybox revoke / TrickyStore chưa nhận keybox | Đặt lại keybox, reboot, clear GMS; check Key Attestation Demo |
| **Pass DEVICE, fail STRONG** | Keybox bị soft-ban (server-side) | Đổi keybox khác; bật "Spoof Provider" |
| **Fingerprint hết hạn** | Google đốt fingerprint | Cập nhật `custom.pif.json` (mục 13) |
| **GMS force stop liên tục** | Fingerprint không khớp device props | Đồng bộ toàn bộ props trong pif.json; clear GMS data |
| **SELinux deny / app crash** | Permissive bị phát hiện | Giữ Enforcing; không bật permissive toàn cục |
| **uname -r nằm trong blacklist** | Tên kernel kích hoạt detect | Đổi `LOCALVERSION` trong kernel config thành tên sạch |

---

## 15. Lưu ý & khuyến nghị cuối

1. **Keybox = mấu chốt** — giữ riêng, không up lên GitHub public/Telegram. Chia sẻ = revoke nhanh.
2. **Giữ kernel build sạch** — không patch lung tung; chỉ cần KernelSU-Next. Ít sửa đổi = ít bị detect.
3. **Kết hợp SUSFS** (tùy chọn): KernelSU-Next + SUSFS module tăng khả năng che giấu (fake kernel name, hide mount, etc.) nếu app kiểm tra gắt.
4. **Mọi app không nhất thiết cần STRONG** — hầu hết chỉ cần DEVICE (NFC pay, RCS, banking). STRONG thường để "flex" hoặc app bảo mật cao.
5. **Cập nhật ROM mới phải lặp lại** — kernel/`playintegrity.sh` nằm trong ROM zip, mỗi lần ROM port mới bạn phải gắn lại kernel + pre-bake.
6. **Kiểm tra trước khi đóng ROM**: boot kernel thử trên máy, chạy checker, pass hết rồi mới đóng zip bàn giao.

---

## Phụ lục: Danh sách liên kết

- KernelSU-Next: https://github.com/KernelSU-Next/KernelSU-Next
- Hướng dẫn integrate non-GKI (chính thức): https://kernelsu-next.github.io/webpage/pages/how-to-integrate-for-non-gki.html
- TrickyStore: https://github.com/5ec1cff/TrickyStore
- PlayIntegrityFix (fork còn cập nhật): https://github.com/KOWX712/PlayIntegrityFix
- ZygiskNext (nếu cần module riêng): https://github.com/Dr-TSNG/ZygiskNext/releases
- Key Attestation Demo: https://github.com/vvb2060/KeyAttestation
- XDA Play Integrity thread: https://xdaforums.com/t/info-play-integrity-api-replacement-for-safetynet.4479337/
- Samsung Open Source: https://opensource.samsung.com
