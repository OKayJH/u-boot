# DLCVCAM boot-try（用户态配套）

与 `arch/arm/mach-rockchip/fit.c` 中的 **boot-try** 记录布局必须一致。

## 协议（misc @ 48KiB）

| 字段 | 说明 |
|------|------|
| magic | `DCBT` = `0x54424344` LE |
| version | 1 |
| try_count | 已尝试从 boot 启动次数 |
| limit | 上限，默认 **2** |
| flags | bit0 = sticky（锁 recovery） |
| reserved[3] | 共 32 字节 |

偏移避开 Android BCB（0 / 16KiB）与 virtual-AB（32KiB）。

## U-Boot 行为摘要

- 已是 recovery 模式（BCB/key/env）：不改计数，直接 recovery  
- sticky 或 `try_count >= limit`：强制 recovery，写 sticky + BCB `boot-recovery`  
- 否则：`try_count++`，尝试 boot；boot FIT 不可用时再即时读 recovery  
- `fit_get_blob()` 多次调用时选分区结果缓存一次，避免重复 ++

## 本目录文件

| 文件 | 用途 |
|------|------|
| `dlcvcam_boot_try.py` | status / confirm / reset / set |
| `dlcvcam-boot-try-confirm.service` | multi-user 后 oneshot confirm |

**产品安装路径**：CamOS Admin 包（`camos_admin`）在升级时拷贝到  
`/usr/local/sbin/dlcvcam_boot_try.py` 并 enable 本 unit。  
开发机也可手工：

```bash
install -m755 tools/dlcvcam/dlcvcam_boot_try.py /usr/local/sbin/dlcvcam_boot_try.py
install -m644 tools/dlcvcam/dlcvcam-boot-try-confirm.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now dlcvcam-boot-try-confirm.service
```

刷试验内核前：`dlcvcam_boot_try.py reset`。  
调试累加时先 `systemctl disable --now dlcvcam-boot-try-confirm.service`。

## 不在本目录

| 组件 | 位置 |
|------|------|
| FIT fallback + boot-try 实现 | `arch/arm/mach-rockchip/fit.c`（本 PR） |
| recovery 金镜像 sync/restore | **CamOS Admin**（`dlcvcam_sync_boot_to_recovery.sh`） |
| 内核 boot.img 一键打包 / sidecar | **dl-cv/kernel**（`build_bootimg.sh`） |
| 硬件看门狗 conf | Admin（`10-dlcvcam-watchdog.conf`） |

硬挂不复位时 try 不会涨，需 systemd `RuntimeWatchdogSec`（见 Admin / 内核手册交叉说明）。

## 相关 PR

板端完整运维（金镜像 sync、confirm、看门狗、部署检查）：CamOS `docs/admin/DLCVCAM内核防砖与金镜像维护.md`（随 Admin 仓）。

- U-Boot：本分支 `feat/rk3576-fit-boot-recovery-fallback`  
- Kernel 编包：https://github.com/dl-cv/kernel/pull/15  
- Admin 内核升级 / seed / 安装：CamOS `feat/admin-kernel-update`
