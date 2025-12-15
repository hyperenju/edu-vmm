# Helloworld (step1)
KVM APIを用いて16ビットのリアルモードで動作し、I/O処理（シリアル出力）と`hlt`による停止を行う最小限の仮想マシン（Minimal VM / Hello World）を構築するためのTODOリスト（コーディングエージェント向け）を作成します。

このタスクは、KVMの主要な概念であるシステム/VM/VCPUファイルディスクリプタの操作、メモリマッピング（`KVM_SET_USER_MEMORY_REGION`）、レジスタの初期化（`KVM_SET_SREGS`, `KVM_SET_REGS`）、およびI/O処理を伴うVM Exitの処理（`KVM_RUN`）を網羅します。

---

# Step 1: Minimal VM (Hello World) - TODOリスト

## 1. 環境構築と初期定義

### 1.1. 必要なヘッダと定数の定義
*   KVM APIに必要な構造体 (`kvm_run`, `kvm_sregs`, `kvm_regs`, `kvm_userspace_memory_region`など) の定義を確認し、インクルードする。
*   使用するioctl定数 (`KVM_GET_API_VERSION`, `KVM_CREATE_VM`, `KVM_SET_USER_MEMORY_REGION`, `KVM_CREATE_VCPU`, `KVM_RUN`, `KVM_GET_VCPU_MMAP_SIZE`, `KVM_EXIT_IO`, `KVM_EXIT_HLT`など) を定義する。

### 1.2. ゲストコードの準備
*   **16bitリアルモード**で動作するゲストコード（バイナリ形式）を作成する。
    *   目的：I/Oポート **`0x3F8` (COM1)** に文字列を出力し、最後に**`hlt`**命令で実行を停止させる。
    *   例: 1文字を出力し、HALTするアセンブリコードを、メモリの先頭（例: ゲスト物理アドレス 0x0000）に配置できるよう、バイト列として定義する。
        *   例: `OUT DX, AL` (DX=0x3F8, AL='H' など) を使用する。

## 2. KVM システムおよび VM のセットアップ

| No. | アクション | ioctl/API | 詳細/目的 |
| :--- | :--- | :--- | :--- |
| **2.1** | KVM サブシステムへのアクセス | `open("/dev/kvm")` | KVM サブシステムにアクセスするためのシステムファイルディスクリプタ（`dev_fd`）を取得する。 |
| **2.2** | API バージョンの確認 | `KVM_GET_API_VERSION` | KVM APIのバージョンが安定版（12）であることを確認する。 |
| **2.3** | 仮想マシンの作成 | `KVM_CREATE_VM` | VM ファイルディスクリプタ（`vm_fd`）を取得する。 |
| **2.4** | ホストメモリの確保 | `mmap` または `malloc` | ゲストOSに割り当てるメモリ領域（16 MB）をホストユーザー空間に確保する。このアドレスを `userspace_addr`とする。 |
| **2.5** | ゲスト物理メモリ領域の設定 | `KVM_SET_USER_MEMORY_REGION` | 確保したホストメモリをゲストの物理アドレス空間 (GPA 0x00000000) にマッピングする。`slot=0`、`guest_phys_addr=0x0`、`memory_size=16MB` を設定する。 |
| **2.6** | ゲストコードの配置 | `memcpy` | ステップ1.2で作成したゲストコードのバイト列を、ステップ2.4で確保したホストメモリの対応するオフセット（GPA 0x0000）にコピーする。 |

## 3. VCPU のセットアップ (16bit Real Mode の初期化)

| No. | アクション | ioctl/API | 詳細/目的 |
| :--- | :--- | :--- | :--- |
| **3.1** | VCPU の作成 | `KVM_CREATE_VCPU` | VCPU ファイルディスクリプタ（`vcpu_fd`）を ID 0 で取得する。 |
| **3.2** | `kvm_run` 構造体のマッピング | `KVM_GET_VCPU_MMAP_SIZE` + `mmap` | `KVM_RUN` ioctlとの通信に使用する共有メモリ領域（`kvm_run`構造体）をVCPUファイルディスクリプタにマップする,,。 |
| **3.3** | 特殊レジスタの初期化 (SREGS) | `KVM_SET_SREGS` | 16ビットリアルモードで実行するために、セグメントレジスタ（CS, DS, ES, SSなど）とコントロールレジスタ（CR0, EFERなど）を初期化する。<br> - **CR0**: プロテクトイネーブルビット (PE, ビット0) を**クリア**する（リアルモード）<br> - **CS (コードセグメント)**: ベースアドレスをゲストコードの開始位置に合わせる（例: 0x0000）。他のセグメント（DS, ES, SS）も同様に設定する。 |
| **3.4** | 汎用レジスタの初期化 (REGS) | `KVM_SET_REGS` | ゲストコードの実行開始点（エントリポイント）を設定する。<br> - **`rip`**: ゲストコードのオフセット (例: 0x0000) を設定する。<br> - `rflags` (フラグ): 必要なフラグ（IF=0, デフォルトなど）を設定する。 |

## 4. 実行ループと I/O 処理

| No. | アクション | ioctl/API | 詳細/目的 |
| :--- | :--- | :--- | :--- |
| **4.1** | 実行ループの開始 | `KVM_RUN` | VCPUの実行を開始する。 |
| **4.2** | VM Exit 理由の確認 | `kvm_run->exit_reason` | `KVM_RUN`から戻った際、`kvm_run`構造体の`exit_reason`フィールドを確認する。 |
| **4.3** | I/O Exit の処理 | `KVM_EXIT_IO` | `exit_reason`が`KVM_EXIT_IO`の場合、I/Oポートアクセスが発生したと判断する。<br> - `kvm_run->io` のフィールド（`port`, `direction`, `size`, `count`, `data_offset`）からアクセス情報を抽出する。<br> - **`port`が`0x3F8` (COM1)** で、**`direction`が`KVM_EXIT_IO_OUT`** の場合、出力する文字データ（`kvm_run`内の`data_offset`が指す場所にある）を取得し、ホスト側（コンソールなど）に出力する。<br> - I/Oエミュレーションが完了したら、ループの先頭に戻り、再度`KVM_RUN`を実行する。 |
| **4.4** | HLT Exit の処理 | `KVM_EXIT_HLT` | ゲストが`hlt`命令を実行した場合、VM Exitが発生する。これを正常終了として扱う。実行ループを終了する。 |
| **4.5** | その他の Exit 処理 | 例: `KVM_EXIT_INTERNAL_ERROR` | その他の予期せぬExitが発生した場合、エラーを出力してプログラムを終了する。 |
| **4.6** | リソースの解放 | `close()` | 全てのファイルディスクリプタ（`dev_fd`, `vm_fd`, `vcpu_fd`）とマップされたメモリ領域（`mmap`）を解放し、プログラムを終了する。 |

---

### Key Concept: I/O エミュレーション

この「Hello World」の核心は、ゲストコードがシリアルポート (`0x3F8`) へ出力する際に発生する**`KVM_EXIT_IO`**をユーザー空間（ホストプログラム）で捕らえ、そのデータを取り出してホストの標準出力に出力する処理（**I/Oエミュレーション**）を実行することです。このプロセスにより、ゲストコードは物理的なデバイス（COM1）が存在すると信じたまま、実際の出力をホストに依存させることができます,。
