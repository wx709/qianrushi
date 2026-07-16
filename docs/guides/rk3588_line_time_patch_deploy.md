# RK3588 单线成像时间补丁部署命令

本补丁包已生成在当前目录：

```text
rk3588_line_time_qt_patch_20260513.tgz
rk3588_line_time_qt_patch_20260513.b64
```

SHA256：

```text
D244FDDCBC53EF1C2A395093D5CE2BF8B2A909DD4AD962876A9B3CEA337A43EC
```

## 如果 SSH 可用

在 Windows PowerShell：

```powershell
scp .\rk3588_line_time_qt_patch_20260513.tgz elf@192.168.0.232:/tmp/
```

在 RK3588：

```bash
cd /home/elf/rk3588_capture
sha256sum /tmp/rk3588_line_time_qt_patch_20260513.tgz
tar -tzf /tmp/rk3588_line_time_qt_patch_20260513.tgz
tar -xzf /tmp/rk3588_line_time_qt_patch_20260513.tgz -C /home/elf/rk3588_capture
./build_qt_panel.sh
pkill -f oct_qt_panel || true
DISPLAY=:0 ./run_qt_panel.sh >/tmp/oct_panel.log 2>&1 &
pgrep -af oct_qt_panel
```

## 如果只有串口可用

在 RK3588 串口 shell：

```bash
cd /home/elf/rk3588_capture
cat > /tmp/rk3588_line_time_qt_patch_20260513.b64 <<'B64'
```

然后粘贴 `rk3588_line_time_qt_patch_20260513.b64` 的全部内容，最后再粘贴：

```bash
B64
base64 -d /tmp/rk3588_line_time_qt_patch_20260513.b64 > /tmp/rk3588_line_time_qt_patch_20260513.tgz
sha256sum /tmp/rk3588_line_time_qt_patch_20260513.tgz
gzip -t /tmp/rk3588_line_time_qt_patch_20260513.tgz
tar -tzf /tmp/rk3588_line_time_qt_patch_20260513.tgz
tar -xzf /tmp/rk3588_line_time_qt_patch_20260513.tgz -C /home/elf/rk3588_capture
./build_qt_panel.sh
pkill -f oct_qt_panel || true
DISPLAY=:0 ./run_qt_panel.sh >/tmp/oct_panel.log 2>&1 &
pgrep -af oct_qt_panel
```

如果 `sha256sum` 输出不是上面的 hash，不要解压，重新传一次。
