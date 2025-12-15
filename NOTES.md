# Minimal VMM Development Plan (Step1–Step3)

## 共通方針

- Host: Linux + KVM (/dev/kvm)
- Architecture: x86_64
- Boot method:
  - Step2 / Step3 ともに **direct kernel boot**
  - BIOS / UEFI / boot loader は使用しない
- vCPU: 1
- Console: COM1 serial (0x3F8)
- Interrupts: KVM in-kernel irqchip を使用
- 目的:
  - CPU 仮想化支援 (VMX/SVM)
  - KVM API
  - Linux boot protocol
  - virtio による仮想 I/O

---

## Step1: Minimal VM (Hello World)

### 目的
- /dev/kvm を直接操作して VM を作成
- vCPU を起動し、ゲストコードを実行できることを確認
- VM exit / I/O handling の理解

### ゲスト仕様
- CPU mode: **16bit real mode**
- メモリ: 16 MB
- ゲストコード:
  - COM1 (0x3F8) に文字列を出力
  - `hlt` で終了

### VMM 実装要件
- KVM API
  - `KVM_GET_API_VERSION`
  - `KVM_CREATE_VM`
  - `KVM_SET_USER_MEMORY_REGION`
  - `KVM_CREATE_VCPU`
  - `KVM_GET_VCPU_MMAP_SIZE`
  - `KVM_SET_SREGS`
  - `KVM_SET_REGS`
  - `KVM_RUN`
- VM exit handling
  - `KVM_EXIT_IO`（serial output）
  - `KVM_EXIT_HLT`

### 割り込み
- 使用しない（IF=0）

---

## Step2: Linux Kernel Boot (initramfs)

### 目的
- Linux kernel を direct boot で起動
- Linux boot protocol の理解
- 割り込み・タイマが動作する環境の構築

### ゲスト仕様
- メモリ: 256 MB
- CPU mode:
  - 起動時: 16bit real mode
  - kernel 内部で protected / long mode に遷移
- root filesystem:
  - initramfs（kernel に同梱）
- Console:
  - serial (ttyS0)

### Boot 方式
- bzImage を VMM がロード
- boot_params (zero page) を VMM が構築
- kernel entry に RIP を設定して実行

### Kernel cmdline (例)
```
console=ttyS0 earlyprintk=serial panic=1
```

### VMM 実装要件（追加分）
- `KVM_CREATE_IRQCHIP`
- カーネル・initramfs のロード処理
- boot_params 構築
- serial I/O handling の継続

---

## Step3: Full VM Boot (rootfs + virtio)

### 目的
- 仮想 I/O デバイスの実装
- 実用的な Linux VM の起動

### ゲスト仕様
- メモリ: 512 MB
- root filesystem:
  - virtio-mmio block device
  - raw disk image
- Console:
  - serial login (getty)

### Boot 方式
- Step2 と同じ direct kernel boot
- rootfs を block device からマウント

### Kernel cmdline (例)
```
console=ttyS0 root=/dev/vda rw panic=1
```

### VMM 実装要件（追加分）
- virtio-mmio block device
  - MMIO region 定義
  - virtio header 実装
  - virtqueue 管理
  - I/O request 処理
- 割り込み通知（irqchip 経由）

---

## 将来拡張（Optional）
- SMP (複数 vCPU)
- virtio-net
- PCI / ACPI
- 自作 irqchip
- 64bit long mode 直接起動
