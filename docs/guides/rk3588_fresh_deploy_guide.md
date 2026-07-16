# RK3588 全新系统部署指南

部署包: `rk3588_slim_deploy_20260515.tgz`
SHA256: `1d9aa0e6663c7e6699bf477d8ab41ce333ce465b4cf101989443bdd5e92d4a59`

## 部署包内容

```
rk3568_capture/          -- Qt采集面板+配置面板源码、编译/运行脚本
  qt_panel/             -- Qt采集面板 (oct_qt_panel)
  qt_config_panel/      -- Qt相机配置面板 (oct_camera_config_panel)
  build_qt_panel.sh
  build_camera_config_panel.sh
  install_desktop_shortcuts.sh
  oct_camera_apply_usb_rule.sh
  run_*.sh
usr/                     -- OCT相机ARM64 SDK
  include/camcmosoctusb3/
  local/lib/camcmosoctusb3_1.2/
  local/bin/camcmosoctusb3/genicam3_0_2/
python/                  -- Python识别脚本 (best_resnet18.pth需单独传输)
```

---

## 第一步：传输部署包到RK3588

### 方式A：串口传输 (最可靠)

在RK3588串口shell执行：

```bash
cd /home/elf
cat > /tmp/rk3588_slim_deploy_20260515.b64 <<'B64'
```

然后粘贴 `rk3588_slim_deploy_20260515.b64` 的全部内容 (约31453行，2.4MB)，最后粘贴：

```bash
B64
```

解码并验证：

```bash
base64 -d /tmp/rk3588_slim_deploy_20260515.b64 > /tmp/rk3588_slim_deploy_20260515.tgz
sha256sum /tmp/rk3588_slim_deploy_20260515.tgz
# 应对齐: 95bcdabb26ffdd9592ed37864781ce60eb364265e3b450b4df95d48dd4ec2048
gzip -t /tmp/rk3588_slim_deploy_20260515.tgz
tar -tzf /tmp/rk3588_slim_deploy_20260515.tgz | head -30
```

### 方式B：SSH传输 (如果网络可用)

```powershell
scp .\rk3588_slim_deploy_20260515.tgz elf@192.168.0.232:/tmp/
```

---

## 第二步：解压部署包

```bash
cd /home/elf
tar -xzf /tmp/rk3588_slim_deploy_20260515.tgz -C /home/elf
ls -la /home/elf/rk3568_capture/
ls -la /home/elf/python/
```

---

## 第三步：安装系统依赖

```bash
# 更新软件源
sudo apt update

# 安装编译工具和Qt5开发包
sudo apt install -y g++ make cmake qtbase5-dev qt5-qmake qtchooser

# 安装Python依赖 (如需要识别功能)
sudo apt install -y python3-pip
cd /home/elf/python && pip3 install -r requirements.txt
```

> 注意：RK3588使用系统Qt5，不要使用SDK自带的老版本Qt5.9.5库（有ICU兼容性问题）。

---

## 第四步：安装OCT相机SDK

```bash
# 安装SDK头文件
sudo cp -r /home/elf/usr/include/camcmosoctusb3 /usr/include/
sudo chmod -R 755 /usr/include/camcmosoctusb3

# 安装SDK库文件
sudo mkdir -p /usr/local/lib/camcmosoctusb3_1.2
sudo cp /home/elf/usr/local/lib/camcmosoctusb3_1.2/* /usr/local/lib/camcmosoctusb3_1.2/
sudo chmod 755 /usr/local/lib/camcmosoctusb3_1.2/*.so*
sudo ldconfig /usr/local/lib/camcmosoctusb3_1.2

# 安装GenICam运行时
sudo mkdir -p /usr/local/bin/camcmosoctusb3/genicam3_0_2
sudo cp -r /home/elf/usr/local/bin/camcmosoctusb3/genicam3_0_2/* /usr/local/bin/camcmosoctusb3/genicam3_0_2/
sudo chmod -R 755 /usr/local/bin/camcmosoctusb3

# 验证SDK安装
ls /usr/include/camcmosoctusb3/CamCmosOctUsb3.h
ls /usr/local/lib/camcmosoctusb3_1.2/libcamcmosoctusb3.so*
ls /usr/local/bin/camcmosoctusb3/genicam3_0_2/bin/Linux64_ARM/
```

---

## 第五步：编译Qt面板

```bash
cd /home/elf/rk3568_capture

# 编译相机配置面板
./build_camera_config_panel.sh

# 编译相机采集面板
./build_qt_panel.sh
```

预期结果：
```
Build complete: .../qt_config_panel/build/oct_camera_config_panel
Build complete: .../qt_panel/build/oct_qt_panel
```

---

## 第六步：安装桌面快捷方式

```bash
cd /home/elf/rk3568_capture
chmod +x install_desktop_shortcuts.sh
./install_desktop_shortcuts.sh
```

预期桌面出现两个图标：
- `OCT Qt采集面板`
- `OCT Camera Config`

---

## 第七步：设置USB权限

```bash
cd /home/elf/rk3568_capture
sudo ./oct_camera_apply_usb_rule.sh elf
```

然后**注销重新登录**或重启：
```bash
sudo reboot
```

---

## 第八步：验证整体功能

```bash
# 检查相机USB连接
lsusb | grep -i "0fd3\|octopus\|e2v"
# 预期: ID 0fd3:0616 e2v OCTOPUS1_USB

# 检查elf用户在plugdev组
groups

# 验证SDK库能找到
ldconfig -p | grep camcmosoctusb3

# 打开配置面板
cd /home/elf/rk3568_capture
DISPLAY=:0 ./run_camera_config_panel.sh

# 或打开采集面板
DISPLAY=:0 ./run_qt_panel.sh >/tmp/oct_panel.log 2>&1 &
```

---

## 可选：传输ML模型文件

`python/best_resnet18.pth` (44MB) 未包含在部署包中，需要时单独传输：

```bash
# 在RK3588上
mkdir -p /home/elf/python
cat > /tmp/rk3588_model.b64 <<'B64'
# 粘贴 best_resnet18.pth 的base64编码
B64
base64 -d /tmp/rk3588_model.b64 > /home/elf/python/best_resnet18.pth
```

---

## 已知重要设置

- 采集模式固定为外部线触发模式2 (SynchroMode=2)
- 默认曝光时间: 40 us
- 默认目标线数(成帧): 300
- 默认SDK缓冲个数: 16
- MCU key0 按下应输出300个TTL上升沿，生成1张300线图像

详细硬件接线和调试见: `ttl_external_trigger_guide.md` 和 `RK3588_OCT_PROJECT_HANDOVER.md`
