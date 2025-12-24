## virtio-blk Future Work
### パフォーマンス
- **非同期 I/O** - io_uring でブロッキング回避
- **KVM_IRQFD + KVM_IOEVENTFD** - NOTIFY と IRQ を eventfd 化して VM exit 削減
- **バッチ処理** - 複数 request をまとめて処理
