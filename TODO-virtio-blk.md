## virtio-blk Future Work
### 機能追加
- **Write サポート** - VIRTIO_BLK_T_OUT 実装、VIRTIO_BLK_F_RO を外す
- **Flush サポート** - VIRTIO_BLK_T_FLUSH + fsync()
- **Discard/TRIM** - VIRTIO_BLK_T_DISCARD (SSD 向け)

### パフォーマンス
- **非同期 I/O** - io_uring でブロッキング回避
- **KVM_IRQFD + KVM_IOEVENTFD** - NOTIFY と IRQ を eventfd 化して VM exit 削減
- **バッチ処理** - 複数 request をまとめて処理

### 堅牢性
- **エラーハンドリング** - lseek/read/write 失敗時に VIRTIO_BLK_S_IOERR を正しく返す
- **境界チェック** - sector + len が capacity を超えないか検証
- **queue サイズ検証** - negotiated size が MAX 以下か確認
