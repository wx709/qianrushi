#include "mainwindow.h"

#include <CamCmosOctUsb3.h>

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QMenuBar>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QProcess>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QSizePolicy>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QTransform>
#include <QtGlobal>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <fcntl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

constexpr int kCameraScanMaxAttempts = 8;
constexpr int kCameraScanRetryDelayMs = 250;
constexpr double kExposureDeadTimeUs = 0.70;
constexpr double kUiMinExposureUs = 0.01;
constexpr double kDefaultExposureUs = 40.0;
constexpr double kDefaultExternalLineFrequencyHz = 2100.0;
constexpr double kDefaultExternalLinePeriodUs = 1000000.0 / kDefaultExternalLineFrequencyHz;
constexpr double kUiMinExternalLinePeriodUs = 12.50;
constexpr double kUiMaxExternalLinePeriodUs = 655.35;
constexpr double kUiMinExternalLineFrequencyHz = 1000000.0 / kUiMaxExternalLinePeriodUs;
constexpr double kUiMaxExternalLineFrequencyHz = 1000000.0 / kUiMinExternalLinePeriodUs;
constexpr double kZynqFabricClockHz = 100000000.0;
constexpr quint32 kExpectedFpgaVersion = 0x20260619;
constexpr quint32 kFpgaRegVersion = 0x0C;
constexpr quint32 kFpgaRegProcCtrl = 0x3C;
constexpr quint32 kDefaultFpgaProcCtrl = 0x0000100A;
constexpr quint32 kLegacyFpgaProcCtrlNorm10 = 0x0000100A;
constexpr quint32 kLegacyFpgaProcCtrlOut8Norm10 = 0x0000108A;
constexpr quint32 kLegacyFpgaProcCtrlLogNorm0 = 0x00001000;
constexpr quint32 kLegacyFpgaProcCtrlLinearHalfScale = 0x00460E00;
constexpr quint32 kLegacyFpgaProcCtrlLinearCurrent = 0x00671C00;
constexpr int kDefaultMotorSweepIntervalMs = 100;
constexpr int kDefaultTriggerThresholdCode = 13000;
constexpr int kDefaultTriggerPulseCount = 0;
constexpr quint32 kMotorControlRun = 0x01;
constexpr quint32 kMotorControlClear = 0x02;
constexpr quint32 kMotorControlFastSweepStart = 0x20;
constexpr quint32 kMotorControlCameraTrigger = 0x40;
constexpr quint32 kMotorControlAbort = 0x80;
constexpr size_t kFpgaBarMapSize = 0x10000;
constexpr bool kUseDatasetCaptureMode = true;
constexpr int kDatasetAutoStopFrames = 0;
constexpr int kDatasetAnalysisMaxFrames = 0;
constexpr int kDatasetAutoTestFrames = 75;
constexpr int kDatasetDisplayIntervalMs = 100;
const char *kZynqSerialDefaultPort = "/dev/ttyUSB0";
const char *kSdkIncludeDir = "/usr/include/camcmosoctusb3";
const char *kSdkLibDir = "/usr/local/lib/camcmosoctusb3_1.2";
const char *kGenicamDir = "/usr/local/bin/camcmosoctusb3/genicam3_0_2";
const char *kUdevRuleFile = "/etc/udev/rules.d/99-octopus-usb.rules";
const char *kFpgaPcieReadyScript = "/usr/local/sbin/ensure_fpga_pcie_ready.sh";

QStringList analysisVideoFileNames() {
  return QStringList()
         << QStringLiteral("yolo_rknn_annotated_stream.mp4")
         << QStringLiteral("yolo_rknn_annotated_stream.avi");
}

QStringList processedImageNameFilters() {
  return QStringList()
         << QStringLiteral("*.pgm")
         << QStringLiteral("*.png")
         << QStringLiteral("*.jpg")
         << QStringLiteral("*.jpeg")
         << QStringLiteral("*.bmp")
         << QStringLiteral("*.tif")
         << QStringLiteral("*.tiff")
         << QStringLiteral("*.raw");
}

int processedFrameCount(const QString &capture_dir) {
  const QDir processed_dir(
      QDir(capture_dir).filePath(QStringLiteral("fpga_processed_images")));
  if (!processed_dir.exists()) {
    return 0;
  }

  int count = 0;
  const QFileInfoList files = processed_dir.entryInfoList(
      processedImageNameFilters(), QDir::Files, QDir::Name);
  for (const QFileInfo &file : files) {
    if (!file.fileName().startsWith(QStringLiteral("__"))) {
      ++count;
    }
  }
  return count;
}

bool containsOctCamera(const QString &text) {
  return text.contains(QStringLiteral("0fd3:0616"), Qt::CaseInsensitive) ||
         text.contains(QStringLiteral("octopus"), Qt::CaseInsensitive) ||
         text.contains(QStringLiteral("e2v"), Qt::CaseInsensitive);
}

QString sdkErrorText(int err) {
  char err_buf[512] = {0};
  size_t err_size = sizeof(err_buf);
  if (USB3_GetErrorText(err, err_buf, &err_size) == CAM_ERR_SUCCESS) {
    return QString::fromUtf8(err_buf);
  }
  return QStringLiteral("Unknown SDK error");
}

bool updateCameraListWithRetry(uint32_t *nb_cameras, int *last_err) {
  *nb_cameras = 0;
  *last_err = CAM_ERR_SUCCESS;

  for (int attempt = 1; attempt <= kCameraScanMaxAttempts; ++attempt) {
    *nb_cameras = 0;
    *last_err = USB3_UpdateCameraList(nb_cameras);
    if (*last_err == CAM_ERR_SUCCESS && *nb_cameras > 0) {
      return true;
    }
    if (attempt < kCameraScanMaxAttempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kCameraScanRetryDelayMs));
    }
  }

  return *last_err == CAM_ERR_SUCCESS;
}

bool configureSerialFd(int fd) {
  termios tty {};
  if (::tcgetattr(fd, &tty) != 0) {
    return false;
  }
  ::cfmakeraw(&tty);
  ::cfsetispeed(&tty, B115200);
  ::cfsetospeed(&tty, B115200);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 1;
  return ::tcsetattr(fd, TCSANOW, &tty) == 0;
}

QString serialReadLine(int fd, int timeout_ms) {
  QByteArray buffer;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    char ch = 0;
    const ssize_t n = ::read(fd, &ch, 1);
    if (n == 1) {
      if (ch == '\n') {
        break;
      }
      if (ch != '\r') {
        buffer.append(ch);
      }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      break;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  return QString::fromUtf8(buffer).trimmed();
}

quint32 parseHexOrDecU32(const QString &text, bool *ok) {
  QString value = text.trimmed();
  int base = 10;
  if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
    value = value.mid(2);
    base = 16;
  }
  return value.toUInt(ok, base);
}

qint32 parseHexOrDecS32(const QString &text, bool *ok) {
  QString value = text.trimmed();
  if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
    const quint32 raw = parseHexOrDecU32(value, ok);
    return static_cast<qint32>(raw);
  }
  return static_cast<qint32>(value.toLongLong(ok, 10));
}

QString readTextFileTrimmed(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  return QString::fromLatin1(file.readAll()).trimmed();
}

QString findFpgaResource0(QString *error) {
  QDir pci_dir(QStringLiteral("/sys/bus/pci/devices"));
  const QFileInfoList entries = pci_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QFileInfo &entry : entries) {
    const QString dir = entry.absoluteFilePath();
    if (readTextFileTrimmed(dir + QStringLiteral("/vendor")).compare(
            QStringLiteral("0x10ee"), Qt::CaseInsensitive) != 0) {
      continue;
    }
    const QString resource0 = dir + QStringLiteral("/resource0");
    if (QFileInfo::exists(resource0)) {
      return resource0;
    }
  }
  if (error) {
    *error = QStringLiteral("未检测到 FPGA 控制寄存器");
  }
  return QString();
}

class FpgaBarAccess {
 public:
  explicit FpgaBarAccess(QString *error) {
    path_ = findFpgaResource0(error);
    if (path_.isEmpty()) {
      return;
    }
    const QByteArray path_bytes = path_.toLocal8Bit();
    fd_ = ::open(path_bytes.constData(), O_RDWR | O_SYNC);
    if (fd_ < 0) {
      if (error) {
        *error = QStringLiteral("打开 FPGA resource0 失败：%1 (%2)")
                     .arg(path_)
                     .arg(QString::fromLocal8Bit(std::strerror(errno)));
      }
      return;
    }
    void *mapped = ::mmap(nullptr, kFpgaBarMapSize, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
      if (error) {
        *error = QStringLiteral("映射 FPGA resource0 失败：%1")
                     .arg(QString::fromLocal8Bit(std::strerror(errno)));
      }
      ::close(fd_);
      fd_ = -1;
      return;
    }
    base_ = static_cast<uint8_t *>(mapped);
  }

  ~FpgaBarAccess() {
    if (base_) {
      ::munmap(base_, kFpgaBarMapSize);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  bool ok() const { return base_ != nullptr; }

  quint32 read32(quint32 offset) const {
    return *reinterpret_cast<volatile quint32 *>(base_ + offset);
  }

  void write32(quint32 offset, quint32 value) {
    *reinterpret_cast<volatile quint32 *>(base_ + offset) = value;
    const uintptr_t page = static_cast<uintptr_t>(offset) & ~static_cast<uintptr_t>(0xFFF);
    ::msync(base_ + page, 0x1000, MS_SYNC);
  }

 private:
  QString path_;
  int fd_ = -1;
  uint8_t *base_ = nullptr;
};

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setupUi();
  if (QCoreApplication::arguments().contains(QStringLiteral("--capture-tab")) && main_tabs_) {
    main_tabs_->setCurrentIndex(1);
  }
  if (QCoreApplication::arguments().contains(QStringLiteral("--motor-dialog")) && motor_dialog_) {
    QTimer::singleShot(300, this, [this]() {
      motor_dialog_->show();
      motor_dialog_->raise();
      motor_dialog_->activateWindow();
    });
  }
  if (QCoreApplication::arguments().contains(QStringLiteral("--params-dialog"))) {
    QTimer::singleShot(300, this, [this]() {
      if (auto *params_dialog = findChild<QDialog *>(QStringLiteral("ParamsDialog"))) {
        params_dialog->show();
        params_dialog->raise();
        params_dialog->activateWindow();
      }
    });
  }
  const QStringList args = QCoreApplication::arguments();
  QString screenshot_path;
  auto_dataset_test_mode_ = args.contains(QStringLiteral("--auto-dataset-test"));
  auto_test_output_dir_ = QDir::tempPath();
  for (int i = 0; i < args.size(); ++i) {
    if (args.at(i) == QStringLiteral("--screenshot") && i + 1 < args.size()) {
      screenshot_path = args.at(i + 1);
      break;
    }
    if (args.at(i).startsWith(QStringLiteral("--screenshot="))) {
      screenshot_path = args.at(i).mid(QStringLiteral("--screenshot=").size());
      break;
    }
  }
  for (int i = 0; i < args.size(); ++i) {
    if (args.at(i) == QStringLiteral("--auto-test-output-dir") && i + 1 < args.size()) {
      auto_test_output_dir_ = args.at(i + 1);
      break;
    }
    if (args.at(i).startsWith(QStringLiteral("--auto-test-output-dir="))) {
      auto_test_output_dir_ =
          args.at(i).mid(QStringLiteral("--auto-test-output-dir=").size());
      break;
    }
  }
  if (auto_dataset_test_mode_) {
    QDir().mkpath(auto_test_output_dir_);
    if (main_tabs_) {
      main_tabs_->setCurrentIndex(1);
    }
    QTimer::singleShot(900, this, [this]() {
      if (continuous_capture_cb_) {
        continuous_capture_cb_->setChecked(false);
      }
      onNewDiagnosisClicked();
      onStartClicked();
    });
  }
  if (!screenshot_path.isEmpty()) {
    QTimer::singleShot(1200, this, [this, screenshot_path]() {
      QWidget *target = this;
      if (QCoreApplication::arguments().contains(QStringLiteral("--params-dialog"))) {
        if (auto *params_dialog = findChild<QDialog *>(QStringLiteral("ParamsDialog"))) {
          target = params_dialog;
        }
      } else if (QCoreApplication::arguments().contains(QStringLiteral("--motor-dialog")) &&
                 motor_dialog_) {
        target = motor_dialog_;
      }
      target->show();
      target->raise();
      target->activateWindow();
      target->grab().save(screenshot_path);
      QApplication::quit();
    });
  }
  appendLog(QStringLiteral("程序启动：当前诊断为空，首次采集会自动创建新的诊断和初诊目录。"));
  if (QCoreApplication::arguments().contains(QStringLiteral("--config-check"))) {
    QTimer::singleShot(200, this, [this]() {
      refreshConfigStatus();
    });
  }
}

MainWindow::~MainWindow() {
  savePanelSettings();
  if (worker_) {
    QMetaObject::invokeMethod(worker_, "requestStop", Qt::QueuedConnection);
  }
  if (worker_thread_) {
    worker_thread_->quit();
    worker_thread_->wait(3000);
  }
  if (analysis_process_) {
    analysis_process_->kill();
    analysis_process_->waitForFinished(1000);
  }
}

QWidget *MainWindow::createConfigTab(QWidget *parent) {
  auto *page = new QWidget(parent);
  auto *page_layout = new QVBoxLayout(page);
  page_layout->setContentsMargins(18, 18, 18, 18);
  page_layout->setSpacing(14);

  auto *header = new QWidget(page);
  auto *header_layout = new QHBoxLayout(header);
  header_layout->setContentsMargins(0, 0, 0, 0);
  auto *title_col = new QWidget(header);
  auto *title_layout = new QVBoxLayout(title_col);
  title_layout->setContentsMargins(0, 0, 0, 0);
  title_layout->setSpacing(4);
  auto *title = new QLabel(QStringLiteral("OCT 相机配置"), title_col);
  QFont title_font = title->font();
  title_font.setPointSize(title_font.pointSize() + 6);
  title_font.setBold(true);
  title->setFont(title_font);
  auto *subtitle = new QLabel(QStringLiteral("SDK、USB 权限、相机连接和面板文件状态"), title_col);
  subtitle->setStyleSheet(QStringLiteral("color:#5f6368;"));
  title_layout->addWidget(title);
  title_layout->addWidget(subtitle);
  header_layout->addWidget(title_col, 1);

  config_capture_tab_btn_ = new QPushButton(QStringLiteral("进入采集面板"), header);
  config_capture_tab_btn_->setMinimumHeight(36);
  header_layout->addWidget(config_capture_tab_btn_);
  page_layout->addWidget(header);

  auto *main_split = new QSplitter(Qt::Horizontal, page);
  page_layout->addWidget(main_split, 1);

  auto *left = new QWidget(main_split);
  auto *left_layout = new QVBoxLayout(left);
  left_layout->setContentsMargins(0, 0, 0, 0);
  left_layout->setSpacing(12);

  auto *status_box = new QGroupBox(QStringLiteral("运行环境"), left);
  auto *status_form = new QFormLayout(status_box);
  status_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  status_form->setHorizontalSpacing(14);
  status_form->setVerticalSpacing(10);

  config_sdk_include_status_ = new QLabel(status_box);
  config_sdk_lib_status_ = new QLabel(status_box);
  config_genicam_status_ = new QLabel(status_box);
  config_udev_status_ = new QLabel(status_box);
  config_plugdev_status_ = new QLabel(status_box);
  config_usb_camera_status_ = new QLabel(status_box);
  config_capture_binary_status_ = new QLabel(status_box);

  const QList<QLabel *> status_labels = {
      config_sdk_include_status_, config_sdk_lib_status_, config_genicam_status_,
      config_udev_status_, config_plugdev_status_, config_usb_camera_status_,
      config_capture_binary_status_};
  for (QLabel *label : status_labels) {
    label->setWordWrap(true);
    label->setMinimumWidth(300);
  }

  status_form->addRow(QStringLiteral("SDK 头文件"), config_sdk_include_status_);
  status_form->addRow(QStringLiteral("SDK 动态库"), config_sdk_lib_status_);
  status_form->addRow(QStringLiteral("GenICam 运行时"), config_genicam_status_);
  status_form->addRow(QStringLiteral("USB 权限规则"), config_udev_status_);
  status_form->addRow(QStringLiteral("plugdev 用户组"), config_plugdev_status_);
  status_form->addRow(QStringLiteral("OCT USB 相机"), config_usb_camera_status_);
  status_form->addRow(QStringLiteral("统一 Qt 面板"), config_capture_binary_status_);
  left_layout->addWidget(status_box);

  auto *actions_box = new QGroupBox(QStringLiteral("常用操作"), left);
  auto *actions_layout = new QGridLayout(actions_box);
  actions_layout->setHorizontalSpacing(10);
  actions_layout->setVerticalSpacing(10);
  config_refresh_btn_ = new QPushButton(QStringLiteral("刷新状态"), actions_box);
  config_apply_usb_btn_ = new QPushButton(QStringLiteral("安装/刷新 USB 权限"), actions_box);
  config_captures_btn_ = new QPushButton(QStringLiteral("打开保存目录"), actions_box);
  config_project_btn_ = new QPushButton(QStringLiteral("打开工程目录"), actions_box);
  actions_layout->addWidget(config_refresh_btn_, 0, 0);
  actions_layout->addWidget(config_apply_usb_btn_, 0, 1);
  actions_layout->addWidget(config_captures_btn_, 1, 0);
  actions_layout->addWidget(config_project_btn_, 1, 1);
  actions_layout->setColumnStretch(0, 1);
  actions_layout->setColumnStretch(1, 1);
  left_layout->addWidget(actions_box);
  left_layout->addStretch(1);

  config_log_edit_ = new QPlainTextEdit(main_split);
  config_log_edit_->setReadOnly(true);
  config_log_edit_->setMaximumBlockCount(1500);
  config_log_edit_->setPlaceholderText(QStringLiteral("状态刷新和系统命令输出会显示在这里。"));

  main_split->addWidget(left);
  main_split->addWidget(config_log_edit_);
  main_split->setStretchFactor(0, 0);
  main_split->setStretchFactor(1, 1);
  main_split->setSizes({360, 520});

  connect(config_refresh_btn_, &QPushButton::clicked, this, &MainWindow::refreshConfigStatus);
  connect(config_apply_usb_btn_, &QPushButton::clicked, this, &MainWindow::applyConfigUsbRule);
  connect(config_capture_tab_btn_, &QPushButton::clicked, this, &MainWindow::switchToCapturePanel);
  connect(config_captures_btn_, &QPushButton::clicked, this, &MainWindow::openConfigCapturesFolder);
  connect(config_project_btn_, &QPushButton::clicked, this, &MainWindow::openConfigProjectFolder);

  page->setStyleSheet(QStringLiteral(
      "QGroupBox { font-weight: 600; border: 1px solid #d0d7de; border-radius: 6px; "
      "margin-top: 10px; padding: 12px; background: #ffffff; }"
      "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
      "QPushButton { min-height: 32px; padding: 4px 12px; }"
      "QPlainTextEdit { border: 1px solid #d0d7de; border-radius: 6px; background: #fbfbfb; }"));

  return page;
}

void MainWindow::setupUi() {
  setWindowTitle(QStringLiteral("OCT 采集与 FPGA 实时处理"));
  resize(1024, 600);
  setMinimumSize(900, 520);

  auto *central = new QWidget(this);
  auto *root_layout = new QVBoxLayout(central);
  root_layout->setContentsMargins(18, 14, 18, 16);
  root_layout->setSpacing(12);
  setCentralWidget(central);

  auto *params_dialog = new QDialog(this);
  params_dialog->setObjectName(QStringLiteral("ParamsDialog"));
  params_dialog->setWindowTitle(QStringLiteral("采集参数"));
  params_dialog->resize(720, 680);
  auto *params_root = new QVBoxLayout(params_dialog);
  auto *params_scroll = new QScrollArea(params_dialog);
  params_scroll->setWidgetResizable(true);
  params_scroll->setFrameShape(QFrame::NoFrame);
  auto *params_page = new QWidget(params_scroll);
  auto *params_layout = new QVBoxLayout(params_page);
  params_layout->setContentsMargins(18, 18, 18, 18);
  params_layout->setSpacing(14);
  params_scroll->setWidget(params_page);
  params_root->addWidget(params_scroll);

  auto *path_box = new QGroupBox(QStringLiteral("文件与识别"), params_page);
  auto *path_form = new QFormLayout(path_box);
  path_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  output_dir_edit_ = new QLineEdit(defaultCaptureRootDir(), path_box);
  browse_btn_ = new QPushButton(QStringLiteral("浏览"), path_box);
  auto *out_row = new QWidget(path_box);
  auto *out_row_layout = new QHBoxLayout(out_row);
  out_row_layout->setContentsMargins(0, 0, 0, 0);
  out_row_layout->addWidget(output_dir_edit_, 1);
  out_row_layout->addWidget(browse_btn_);
  path_form->addRow(QStringLiteral("保存根目录"), out_row);
  python_dir_edit_ = new QLineEdit(defaultPythonDir(), path_box);
  browse_python_btn_ = new QPushButton(QStringLiteral("浏览"), path_box);
  auto *python_row = new QWidget(path_box);
  auto *python_row_layout = new QHBoxLayout(python_row);
  python_row_layout->setContentsMargins(0, 0, 0, 0);
  python_row_layout->addWidget(python_dir_edit_, 1);
  python_row_layout->addWidget(browse_python_btn_);
  path_form->addRow(QStringLiteral("Python识别目录"), python_row);
  prefix_edit_ = new QLineEdit(QStringLiteral("oct"), path_box);
  path_form->addRow(QStringLiteral("文件前缀"), prefix_edit_);
  params_layout->addWidget(path_box);

  auto *camera_box = new QGroupBox(QStringLiteral("相机模式二参数"), params_page);
  auto *form = new QFormLayout(camera_box);
  form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  exposure_spin_ = new QDoubleSpinBox(camera_box);
  exposure_spin_->setRange(kUiMinExposureUs, kDefaultExternalLinePeriodUs - kExposureDeadTimeUs);
  exposure_spin_->setDecimals(2);
  exposure_spin_->setValue(kDefaultExposureUs);
  exposure_spin_->setSuffix(QStringLiteral(" us"));
  exposure_spin_->setToolTip(QStringLiteral("模式2下曝光时间可设置；上限按外部触发周期参考值锁定为：周期 - 0.7 us。"));
  form->addRow(QStringLiteral("曝光时间"), exposure_spin_);
  external_trigger_freq_spin_ = new QDoubleSpinBox(camera_box);
  external_trigger_freq_spin_->setRange(kUiMinExternalLineFrequencyHz,
                                        kUiMaxExternalLineFrequencyHz);
  external_trigger_freq_spin_->setDecimals(2);
  external_trigger_freq_spin_->setValue(kDefaultExternalLineFrequencyHz);
  external_trigger_freq_spin_->setSuffix(QStringLiteral(" Hz"));
  external_trigger_freq_spin_->setToolTip(QStringLiteral("Zynq 第三路 DAC8830 产生 OCT 外触发；模式2要求外部周期 >= 曝光+0.7us，且不能小于相机读回的最小线周期。"));
  form->addRow(QStringLiteral("外触发频率"), external_trigger_freq_spin_);
  line_period_spin_ = new QDoubleSpinBox(camera_box);
  line_period_spin_->setRange(kUiMinExternalLinePeriodUs, 1000000.0);
  line_period_spin_->setDecimals(2);
  line_period_spin_->setValue(kDefaultExternalLinePeriodUs);
  line_period_spin_->setSuffix(QStringLiteral(" us"));
  line_period_spin_->setReadOnly(true);
  line_period_spin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
  form->addRow(QStringLiteral("外触发周期参考"), line_period_spin_);
  trigger_threshold_spin_ = new QSpinBox(camera_box);
  trigger_threshold_spin_->setRange(-32768, 32767);
  trigger_threshold_spin_->setValue(kDefaultTriggerThresholdCode);
  trigger_threshold_spin_->setToolTip(QStringLiteral("AD7616 A路原始code超过该阈值后，FPGA开始输出相机TTL方波。"));
  form->addRow(QStringLiteral("触发阈值code"), trigger_threshold_spin_);
  trigger_pulse_count_spin_ = new QSpinBox(camera_box);
  trigger_pulse_count_spin_->setRange(0, 65535);
  trigger_pulse_count_spin_->setValue(kDefaultTriggerPulseCount);
  trigger_pulse_count_spin_->setSpecialValueText(QStringLiteral("连续"));
  trigger_pulse_count_spin_->setToolTip(QStringLiteral("每次联动输出的TTL方波个数；0表示只要快扫有效就连续输出。"));
  form->addRow(QStringLiteral("方波个数"), trigger_pulse_count_spin_);
  auto *trigger_level_label = new QLabel(QStringLiteral("第三路 DAC8830，示波器确认后接 OCT GPI"), camera_box);
  trigger_level_label->setStyleSheet(QStringLiteral("color:#047857; font-weight:600;"));
  form->addRow(QStringLiteral("触发幅值"), trigger_level_label);
  frame_lines_spin_ = new QSpinBox(camera_box);
  frame_lines_spin_->setRange(1, 200000);
  frame_lines_spin_->setValue(4096);
  form->addRow(QStringLiteral("目标线数"), frame_lines_spin_);
  staging_lines_spin_ = new QSpinBox(camera_box);
  staging_lines_spin_->setRange(1, 1024);
  staging_lines_spin_->setValue(5);
  form->addRow(QStringLiteral("缓冲区线数"), staging_lines_spin_);
  sdk_buffers_spin_ = new QSpinBox(camera_box);
  sdk_buffers_spin_->setRange(2, 256);
  sdk_buffers_spin_->setValue(16);
  form->addRow(QStringLiteral("SDK缓冲个数"), sdk_buffers_spin_);
  timeout_spin_ = new QSpinBox(camera_box);
  timeout_spin_->setRange(10, 10000);
  timeout_spin_->setValue(200);
  timeout_spin_->setSuffix(QStringLiteral(" ms"));
  form->addRow(QStringLiteral("取帧超时"), timeout_spin_);
  timing_hint_label_ = new QLabel(camera_box);
  timing_hint_label_->setWordWrap(true);
  form->addRow(QStringLiteral("时序限制"), timing_hint_label_);
  params_layout->addWidget(camera_box);

  setFpgaProcCtrlUi(kDefaultFpgaProcCtrl);

  auto *save_box = new QGroupBox(QStringLiteral("保存与诊断"), params_page);
  auto *save_layout = new QGridLayout(save_box);
  auto *save_raw_cb = new QCheckBox(QStringLiteral("保存 RAW"), save_box);
  save_raw_cb->setChecked(true);
  save_raw_cb->setObjectName("save_raw");
  auto *save_pgm_cb = new QCheckBox(QStringLiteral("保存 PGM"), save_box);
  save_pgm_cb->setChecked(true);
  save_pgm_cb->setObjectName("save_pgm");
  continuous_capture_cb_ = new QCheckBox(QStringLiteral("连续采集"), save_box);
  continuous_capture_cb_->setChecked(true);
  new_diagnosis_btn_ = new QPushButton(QStringLiteral("新建诊断"), save_box);
  previous_diagnosis_btn_ = new QPushButton(QStringLiteral("以往诊断"), save_box);
  delete_current_diagnosis_btn_ = new QPushButton(QStringLiteral("删除当前诊断"), save_box);
  delete_current_diagnosis_btn_->setEnabled(false);
  analyze_last_btn_ = new QPushButton(QStringLiteral("分析当前诊断"), save_box);
  analyze_last_btn_->setEnabled(false);
  open_last_session_btn_ = new QPushButton(QStringLiteral("打开诊断目录"), save_box);
  open_last_session_btn_->setEnabled(false);
  save_layout->addWidget(save_raw_cb, 0, 0);
  save_layout->addWidget(save_pgm_cb, 0, 1);
  save_layout->addWidget(continuous_capture_cb_, 1, 0, 1, 2);
  save_layout->addWidget(new_diagnosis_btn_, 2, 0);
  save_layout->addWidget(previous_diagnosis_btn_, 2, 1);
  save_layout->addWidget(delete_current_diagnosis_btn_, 3, 0);
  save_layout->addWidget(open_last_session_btn_, 3, 1);
  save_layout->addWidget(analyze_last_btn_, 4, 0, 1, 2);
  params_layout->addWidget(save_box);
  params_layout->addStretch(1);

  auto *config_dialog = new QDialog(this);
  config_dialog->setWindowTitle(QStringLiteral("环境与相机配置"));
  config_dialog->resize(900, 620);
  auto *config_root = new QVBoxLayout(config_dialog);
  auto *config_scroll = new QScrollArea(config_dialog);
  config_scroll->setFrameShape(QFrame::NoFrame);
  config_scroll->setWidgetResizable(true);
  config_scroll->setWidget(createConfigTab(config_scroll));
  config_root->addWidget(config_scroll);

  auto *log_dialog = new QDialog(this);
  log_dialog->setWindowTitle(QStringLiteral("完整日志"));
  log_dialog->resize(900, 560);
  auto *log_root = new QVBoxLayout(log_dialog);
  log_edit_ = new QPlainTextEdit(log_dialog);
  log_edit_->setReadOnly(true);
  log_edit_->setMaximumBlockCount(800);
  clear_log_btn_ = new QPushButton(QStringLiteral("清空日志"), log_dialog);
  log_root->addWidget(log_edit_, 1);
  log_root->addWidget(clear_log_btn_);

  motor_dialog_ = createMotorDialog();

  auto *capture_menu = menuBar()->addMenu(QStringLiteral("采集"));
  QAction *scan_action = capture_menu->addAction(QStringLiteral("扫描相机"));
  QAction *start_action = capture_menu->addAction(QStringLiteral("开始采集"));
  QAction *stop_action = capture_menu->addAction(QStringLiteral("停止采集"));
  capture_menu->addSeparator();
  QAction *new_diag_action = capture_menu->addAction(QStringLiteral("新建诊断"));
  QAction *open_diag_action = capture_menu->addAction(QStringLiteral("打开诊断目录"));
  QAction *analyze_action = capture_menu->addAction(QStringLiteral("分析当前诊断"));
  QAction *params_action = menuBar()->addAction(QStringLiteral("参数设置"));
  QAction *motor_action = menuBar()->addAction(QStringLiteral("电机控制"));
  QAction *config_action = menuBar()->addAction(QStringLiteral("环境检查"));
  QAction *log_action = menuBar()->addAction(QStringLiteral("完整日志"));

  auto *header = new QFrame(central);
  header->setObjectName(QStringLiteral("HeaderBar"));
  auto *header_layout = new QHBoxLayout(header);
  header_layout->setContentsMargins(16, 12, 16, 12);
  header_layout->setSpacing(12);
  auto *title_col = new QWidget(header);
  auto *title_layout = new QVBoxLayout(title_col);
  title_layout->setContentsMargins(0, 0, 0, 0);
  title_layout->setSpacing(2);
  auto *title = new QLabel(QStringLiteral("OCT 实时采集"), title_col);
  QFont title_font = title->font();
  title_font.setPointSize(title_font.pointSize() + 7);
  title_font.setBold(true);
  title->setFont(title_font);
  auto *subtitle = new QLabel(QStringLiteral("原始图像与 FPGA 处理结果同步显示"), title_col);
  subtitle->setStyleSheet(QStringLiteral("color:#6b7280;"));
  title_layout->addWidget(title);
  title_layout->addWidget(subtitle);
  header_layout->addWidget(title_col, 1);
  camera_combo_ = new QComboBox(header);
  camera_combo_->setMinimumWidth(260);
  scan_btn_ = new QPushButton(QStringLiteral("扫描"), header);
  start_btn_ = new QPushButton(QStringLiteral("开始采集"), header);
  analyze_recognition_btn_ = new QPushButton(QStringLiteral("分析识别"), header);
  stop_btn_ = new QPushButton(QStringLiteral("停止"), header);
  stop_btn_->setEnabled(false);
  header_layout->addWidget(camera_combo_);
  header_layout->addWidget(scan_btn_);
  header_layout->addWidget(start_btn_);
  header_layout->addWidget(analyze_recognition_btn_);
  header_layout->addWidget(stop_btn_);
  root_layout->addWidget(header);

  auto *image_grid = new QGridLayout();
  image_grid->setHorizontalSpacing(14);
  image_grid->setVerticalSpacing(14);
  image_grid->addWidget(createImagePanel(QStringLiteral("实时原始图像"),
                                         &raw_image_label_, &raw_latency_label_), 0, 0);
  image_grid->addWidget(createImagePanel(QStringLiteral("FPGA处理图像"),
                                         &processed_image_label_, &processed_latency_label_), 0, 1);
  image_grid->setColumnStretch(0, 1);
  image_grid->setColumnStretch(1, 1);
  root_layout->addLayout(image_grid, 1);

  auto *status_frame = new QFrame(central);
  status_frame->setObjectName(QStringLiteral("StatusPanel"));
  auto *status_layout = new QHBoxLayout(status_frame);
  status_layout->setContentsMargins(14, 10, 14, 10);
  status_layout->setSpacing(12);
  status_label_ = new QLabel(status_frame);
  status_label_->setWordWrap(true);
  status_label_->setMinimumWidth(360);
  missed_trigger_label_ = new QLabel(QStringLiteral("未开始采集；漏触发 0，丢线 0"), status_frame);
  missed_trigger_label_->setWordWrap(true);
  missed_trigger_label_->setStyleSheet(QStringLiteral("color:#047857; font-weight:600;"));
  key_log_edit_ = new QPlainTextEdit(status_frame);
  key_log_edit_->setReadOnly(true);
  key_log_edit_->setMaximumBlockCount(10);
  key_log_edit_->setMaximumHeight(72);
  key_log_edit_->setPlaceholderText(QStringLiteral("关键状态"));
  status_layout->addWidget(status_label_, 1);
  status_layout->addWidget(missed_trigger_label_, 1);
  status_layout->addWidget(key_log_edit_, 2);
  root_layout->addWidget(status_frame);

  connect(scan_btn_, &QPushButton::clicked, this, &MainWindow::onScanClicked);
  connect(new_diagnosis_btn_, &QPushButton::clicked, this, &MainWindow::onNewDiagnosisClicked);
  connect(previous_diagnosis_btn_, &QPushButton::clicked,
          this, &MainWindow::onPreviousDiagnosisClicked);
  connect(delete_current_diagnosis_btn_, &QPushButton::clicked,
          this, &MainWindow::onDeleteCurrentDiagnosisClicked);
  connect(start_btn_, &QPushButton::clicked, this, &MainWindow::onStartClicked);
  connect(analyze_recognition_btn_, &QPushButton::clicked,
          this, &MainWindow::onAnalyzeLastClicked);
  connect(stop_btn_, &QPushButton::clicked, this, &MainWindow::onStopClicked);
  connect(analyze_last_btn_, &QPushButton::clicked, this, &MainWindow::onAnalyzeLastClicked);
  connect(open_last_session_btn_, &QPushButton::clicked, this, &MainWindow::onOpenLastSessionClicked);
  connect(clear_log_btn_, &QPushButton::clicked, this, [this]() {
    if (log_edit_) {
      log_edit_->clear();
    }
    if (key_log_edit_) {
      key_log_edit_->clear();
    }
  });
  connect(browse_btn_, &QPushButton::clicked, this, [this]() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择保存目录"), output_dir_edit_->text());
    if (!dir.isEmpty()) {
      output_dir_edit_->setText(dir);
    }
  });
  connect(browse_python_btn_, &QPushButton::clicked, this, [this]() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择Python识别目录"), python_dir_edit_->text());
    if (!dir.isEmpty()) {
      python_dir_edit_->setText(dir);
    }
  });
  connect(output_dir_edit_, &QLineEdit::textChanged, this, [this]() { updateStatusLabel(); });
  connect(external_trigger_freq_spin_, qOverload<double>(&QDoubleSpinBox::valueChanged),
          this, [this](double frequency_hz) {
            if (line_period_spin_ && frequency_hz > 0.0) {
              line_period_spin_->setValue(1000000.0 / frequency_hz);
            }
            updateTimingHint();
          });
  connect(line_period_spin_, qOverload<double>(&QDoubleSpinBox::valueChanged),
          this, [this]() { updateTimingHint(); });
  connect(exposure_spin_, qOverload<double>(&QDoubleSpinBox::valueChanged),
          this, [this]() {
            updateTimingHint();
            if (external_trigger_freq_spin_ && line_period_spin_ &&
                external_trigger_freq_spin_->value() > 0.0) {
              line_period_spin_->setValue(1000000.0 / external_trigger_freq_spin_->value());
            }
          });
  connect(scan_action, &QAction::triggered, scan_btn_, &QPushButton::click);
  connect(start_action, &QAction::triggered, start_btn_, &QPushButton::click);
  connect(stop_action, &QAction::triggered, stop_btn_, &QPushButton::click);
  connect(new_diag_action, &QAction::triggered, new_diagnosis_btn_, &QPushButton::click);
  connect(open_diag_action, &QAction::triggered, open_last_session_btn_, &QPushButton::click);
  connect(analyze_action, &QAction::triggered, analyze_last_btn_, &QPushButton::click);
  connect(params_action, &QAction::triggered, this, [params_dialog]() {
    params_dialog->show();
    params_dialog->raise();
    params_dialog->activateWindow();
  });
  connect(config_action, &QAction::triggered, this, [config_dialog]() {
    config_dialog->show();
    config_dialog->raise();
    config_dialog->activateWindow();
  });
  connect(motor_action, &QAction::triggered, this, [this]() {
    motor_dialog_->show();
    motor_dialog_->raise();
    motor_dialog_->activateWindow();
    if (motor_timer_ && !motor_timer_->isActive()) {
      motor_timer_->start();
    }
  });
  connect(log_action, &QAction::triggered, this, [log_dialog]() {
    log_dialog->show();
    log_dialog->raise();
    log_dialog->activateWindow();
  });

  setStyleSheet(QStringLiteral(
      "QMainWindow, QWidget { background:#f5f5f7; color:#111827; font-size:14px; }"
      "QMenuBar { background:#f5f5f7; padding:4px 8px; }"
      "QMenuBar::item { padding:6px 12px; border-radius:6px; }"
      "QMenuBar::item:selected { background:#e5e7eb; }"
      "QFrame#HeaderBar, QFrame#StatusPanel, QFrame#ImagePanel, QGroupBox {"
      " background:#ffffff; border:1px solid #d8dee9; border-radius:8px; }"
      "QGroupBox { margin-top:10px; padding:12px; font-weight:600; }"
      "QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 4px; }"
      "QPushButton { min-height:32px; padding:5px 14px; border:1px solid #c8d0da;"
      " border-radius:7px; background:#ffffff; }"
      "QPushButton:hover { background:#eef2f7; }"
      "QPushButton:pressed { background:#dbe3ee; }"
      "QPushButton:disabled { color:#9ca3af; background:#f3f4f6; }"
      "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit {"
      " background:#ffffff; border:1px solid #cfd7e3; border-radius:7px; padding:5px; }"
      "QPlainTextEdit { selection-background-color:#bfdbfe; }"));

  loadPanelSettings();
  updateStatusLabel();
  updateTimingHint();
  appendKeyStatus(QStringLiteral("等待采集"));
}

void MainWindow::appendLog(const QString &msg) {
  const QString line =
      QString("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), msg);
  if (log_edit_) {
    log_edit_->appendPlainText(line);
  }
}

void MainWindow::resetLiveViews() {
  displayed_roundtrip_ms_ = -1.0;
  displayed_line_ms_ = -1.0;
  last_latency_update_ms_ = 0;
  last_raw_preview_ = QImage();
  last_processed_preview_ = QImage();
  auto reset_image = [](QLabel *label) {
    if (!label) {
      return;
    }
    label->clear();
    label->setText(QStringLiteral("等待新图像"));
    label->setAlignment(Qt::AlignCenter);
  };
  reset_image(raw_image_label_);
  reset_image(processed_image_label_);
  if (raw_latency_label_) {
    raw_latency_label_->setText(QStringLiteral("-- ms"));
  }
  if (processed_latency_label_) {
    processed_latency_label_->setText(QStringLiteral("-- ms"));
  }
}

quint32 MainWindow::fpgaProcCtrlFromUi() const {
  const quint32 norm_shift = fpga_norm_shift_spin_
      ? static_cast<quint32>(fpga_norm_shift_spin_->value()) & 0xFu
      : (kDefaultFpgaProcCtrl & 0xFu);
  const quint32 out_shift = fpga_out_shift_spin_
      ? static_cast<quint32>(fpga_out_shift_spin_->value()) & 0xFu
      : ((kDefaultFpgaProcCtrl >> 4) & 0xFu);
  const quint32 log_gain = fpga_log_gain_spin_
      ? static_cast<quint32>(fpga_log_gain_spin_->value()) & 0xFFu
      : ((kDefaultFpgaProcCtrl >> 8) & 0xFFu);
  const qint16 log_offset = fpga_log_offset_spin_
      ? static_cast<qint16>(fpga_log_offset_spin_->value())
      : static_cast<qint16>((kDefaultFpgaProcCtrl >> 16) & 0xFFFFu);
  return (static_cast<quint32>(static_cast<quint16>(log_offset)) << 16) |
         (log_gain << 8) | (out_shift << 4) | norm_shift;
}

void MainWindow::setFpgaProcCtrlUi(quint32 value) {
  const bool old_norm = fpga_norm_shift_spin_ && fpga_norm_shift_spin_->blockSignals(true);
  const bool old_out = fpga_out_shift_spin_ && fpga_out_shift_spin_->blockSignals(true);
  const bool old_gain = fpga_log_gain_spin_ && fpga_log_gain_spin_->blockSignals(true);
  const bool old_offset = fpga_log_offset_spin_ && fpga_log_offset_spin_->blockSignals(true);

  if (fpga_norm_shift_spin_) {
    fpga_norm_shift_spin_->setValue(static_cast<int>(value & 0xFu));
    fpga_norm_shift_spin_->blockSignals(old_norm);
  }
  if (fpga_out_shift_spin_) {
    fpga_out_shift_spin_->setValue(static_cast<int>((value >> 4) & 0xFu));
    fpga_out_shift_spin_->blockSignals(old_out);
  }
  if (fpga_log_gain_spin_) {
    fpga_log_gain_spin_->setValue(static_cast<int>((value >> 8) & 0xFFu));
    fpga_log_gain_spin_->blockSignals(old_gain);
  }
  if (fpga_log_offset_spin_) {
    fpga_log_offset_spin_->setValue(static_cast<int>(static_cast<qint16>((value >> 16) & 0xFFFFu)));
    fpga_log_offset_spin_->blockSignals(old_offset);
  }

  updateFpgaProcSummary();
}

QString MainWindow::fpgaProcCtrlSummary(quint32 value) const {
  const int norm_shift = static_cast<int>(value & 0xFu);
  const int out_shift = static_cast<int>((value >> 4) & 0xFu);
  const int log_gain = static_cast<int>((value >> 8) & 0xFFu);
  const int log_offset = static_cast<int>(static_cast<qint16>((value >> 16) & 0xFFFFu));
  return QStringLiteral("0x%1  norm=%2, out=%3, gain=%4/16=%5, offset=%6")
      .arg(value, 8, 16, QChar('0'))
      .arg(norm_shift)
      .arg(out_shift)
      .arg(log_gain)
      .arg(static_cast<double>(log_gain) / 16.0, 0, 'f', 3)
      .arg(log_offset);
}

void MainWindow::updateFpgaProcSummary() {
  if (fpga_proc_ctrl_label_) {
    fpga_proc_ctrl_label_->setText(fpgaProcCtrlSummary(fpgaProcCtrlFromUi()));
  }
}

QFrame *MainWindow::createImagePanel(const QString &title_text,
                                     QLabel **image_label,
                                     QLabel **latency_label) {
  auto *panel = new QFrame();
  panel->setObjectName(QStringLiteral("ImagePanel"));
  auto *layout = new QVBoxLayout(panel);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(10);

  auto *top = new QWidget(panel);
  auto *top_layout = new QHBoxLayout(top);
  top_layout->setContentsMargins(0, 0, 0, 0);
  auto *caption = new QLabel(title_text, top);
  QFont caption_font = caption->font();
  caption_font.setBold(true);
  caption_font.setPointSize(caption_font.pointSize() + 1);
  caption->setFont(caption_font);
  auto *latency_name = new QLabel(
      title_text.contains(QStringLiteral("FPGA")) ? QStringLiteral("每一线延迟")
                                                  : QStringLiteral("链路延迟"),
      top);
  latency_name->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  latency_name->setStyleSheet(QStringLiteral("color:#6b7280; font-weight:600;"));
  *latency_label = new QLabel(QStringLiteral("-- ms"), top);
  (*latency_label)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  QFont latency_font = (*latency_label)->font();
  latency_font.setStyleHint(QFont::Monospace);
  (*latency_label)->setFont(latency_font);
  (*latency_label)->setMinimumWidth(104);
  (*latency_label)->setStyleSheet(QStringLiteral("color:#4b5563; font-weight:600; font-family:monospace;"));
  top_layout->addWidget(caption, 1);
  top_layout->addWidget(latency_name);
  top_layout->addWidget(*latency_label);

  *image_label = new QLabel(QStringLiteral("等待图像"), panel);
  (*image_label)->setAlignment(Qt::AlignCenter);
  (*image_label)->setMinimumSize(360, 280);
  (*image_label)->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  (*image_label)->setScaledContents(false);
  (*image_label)->setStyleSheet(QStringLiteral(
      "QLabel { background:#111827; color:#cbd5e1; border-radius:8px; }"));
  layout->addWidget(top);
  layout->addWidget(*image_label, 1);
  return panel;
}

QDialog *MainWindow::createMotorDialog() {
  auto *dialog = new QDialog(this);
  dialog->setWindowTitle(QStringLiteral("电机控制"));
  dialog->resize(900, 620);
  auto *root = new QVBoxLayout(dialog);
  root->setContentsMargins(18, 18, 18, 18);
  root->setSpacing(14);

  auto *header = new QFrame(dialog);
  header->setObjectName(QStringLiteral("ImagePanel"));
  auto *header_layout = new QHBoxLayout(header);
  header_layout->setContentsMargins(14, 12, 14, 12);
  auto *title_col = new QWidget(header);
  auto *title_layout = new QVBoxLayout(title_col);
  title_layout->setContentsMargins(0, 0, 0, 0);
  auto *title = new QLabel(QStringLiteral("电机联动测试"), title_col);
  QFont title_font = title->font();
  title_font.setBold(true);
  title_font.setPointSize(title_font.pointSize() + 4);
  title->setFont(title_font);
  auto *subtitle = new QLabel(QStringLiteral("PCIe BAR 控制 DAC/ADC，曲线显示 AD7616 A/B 双路反馈输入"), title_col);
  subtitle->setStyleSheet(QStringLiteral("color:#6b7280;"));
  title_layout->addWidget(title);
  title_layout->addWidget(subtitle);
  motor_state_label_ = new QLabel(QStringLiteral("控制链路待机 / 电机反馈未确认"), header);
  motor_state_label_->setStyleSheet(QStringLiteral("color:#6b7280; font-weight:700;"));
  header_layout->addWidget(title_col, 1);
  header_layout->addWidget(motor_state_label_);
  root->addWidget(header);

  motor_plot_label_ = new QLabel(dialog);
  motor_plot_label_->setMinimumSize(720, 250);
  motor_plot_label_->setAlignment(Qt::AlignCenter);
  motor_plot_label_->setScaledContents(true);
  motor_plot_label_->setStyleSheet(QStringLiteral(
      "QLabel { background:#ffffff; border:1px solid #d8dee9; border-radius:8px; }"));
  root->addWidget(motor_plot_label_, 1);

  auto *status = new QFrame(dialog);
  status->setObjectName(QStringLiteral("StatusPanel"));
  auto *status_layout = new QGridLayout(status);
  status_layout->setContentsMargins(14, 10, 14, 10);
  status_layout->setHorizontalSpacing(18);
  status_layout->setVerticalSpacing(8);
  motor_fpga_label_ = new QLabel(QStringLiteral("Zynq：正在检测"), status);
  motor_count_label_ = new QLabel(QStringLiteral("FPGA记录 0"), status);
  motor_status_label_ = new QLabel(QStringLiteral("状态 --"), status);
  motor_fpga_label_->setStyleSheet(QStringLiteral("font-weight:700;"));
  motor_count_label_->setStyleSheet(QStringLiteral("font-weight:700;"));
  status_layout->addWidget(motor_fpga_label_, 0, 0);
  status_layout->addWidget(motor_count_label_, 0, 1);
  status_layout->addWidget(motor_status_label_, 1, 0, 1, 2);
  root->addWidget(status);

  auto *control = new QFrame(dialog);
  control->setObjectName(QStringLiteral("StatusPanel"));
  auto *control_layout = new QGridLayout(control);
  control_layout->setContentsMargins(14, 10, 14, 10);
  control_layout->setHorizontalSpacing(12);
  control_layout->setVerticalSpacing(8);
  motor_position_label_ = new QLabel(QStringLiteral("AD反馈 A -- mV / B -- mV"), control);
  motor_position_label_->setStyleSheet(QStringLiteral("font-weight:700;"));
  motor_sweep_target_spin_ = new QSpinBox(control);
  motor_sweep_target_spin_->setRange(1, 65535);
  motor_sweep_target_spin_->setValue(65535);
  motor_sweep_target_spin_->setSuffix(QStringLiteral(" DAC"));
  motor_sweep_cycles_spin_ = new QSpinBox(control);
  motor_sweep_cycles_spin_->setRange(1, 100000);
  motor_sweep_cycles_spin_->setValue(1);
  motor_sweep_cycles_spin_->setSuffix(QStringLiteral(" 轮"));
  motor_sweep_interval_ms_spin_ = new QSpinBox(control);
  motor_sweep_interval_ms_spin_->setRange(0, 30000);
  motor_sweep_interval_ms_spin_->setValue(kDefaultMotorSweepIntervalMs);
  motor_sweep_interval_ms_spin_->setSuffix(QStringLiteral(" ms"));
  motor_start_btn_ = new QPushButton(QStringLiteral("启动快扫"), control);
  motor_stop_btn_ = new QPushButton(QStringLiteral("停止"), control);
  motor_clear_btn_ = new QPushButton(QStringLiteral("清零"), control);
  control_layout->addWidget(motor_position_label_, 0, 0, 1, 4);
  control_layout->addWidget(new QLabel(QStringLiteral("扫频上限"), control), 1, 0);
  control_layout->addWidget(motor_sweep_target_spin_, 1, 1);
  control_layout->addWidget(new QLabel(QStringLiteral("扫频轮数"), control), 1, 2);
  control_layout->addWidget(motor_sweep_cycles_spin_, 1, 3);
  control_layout->addWidget(new QLabel(QStringLiteral("轮间间隔"), control), 2, 0);
  control_layout->addWidget(motor_sweep_interval_ms_spin_, 2, 1);
  control_layout->addWidget(motor_start_btn_, 3, 1);
  control_layout->addWidget(motor_stop_btn_, 3, 2);
  control_layout->addWidget(motor_clear_btn_, 3, 3);
  root->addWidget(control);

  motor_timer_ = new QTimer(this);
  motor_timer_->setInterval(1000);
  connect(motor_timer_, &QTimer::timeout, this, [this]() { updateMotorPlot(motor_running_); });
  connect(motor_start_btn_, &QPushButton::clicked, this, [this]() {
    startMotorHardware(QStringLiteral("手动启动"));
  });
  connect(motor_stop_btn_, &QPushButton::clicked, this, [this]() {
    stopMotorHardware(QStringLiteral("手动停止"));
  });
  connect(motor_clear_btn_, &QPushButton::clicked, this, [this]() {
    clearMotorCounters(QStringLiteral("手动清零"));
  });
  if (motor_plot_label_) {
    motor_plot_label_->setPixmap(makeMotorPlot());
  }
  return dialog;
}

QPixmap MainWindow::makeMotorPlot() const {
  const int w = 800;
  const int h = 260;
  QPixmap pix(w, h);
  pix.fill(QColor("#ffffff"));
  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing, true);

  const int left = 58;
  const int right = w - 24;
  const int top = 26;
  const int bottom = h - 46;

  p.setPen(QPen(QColor("#d1d5db"), 1));
  for (int i = 0; i <= 6; ++i) {
    const int y = top + i * (bottom - top) / 6;
    p.drawLine(left, y, right, y);
  }
  for (int i = 0; i <= 10; ++i) {
    const int x = left + i * (right - left) / 10;
    p.drawLine(x, top, x, bottom);
  }

  auto include_range = [](const QVector<double> &values, double *min_v, double *max_v) {
    for (double value : values) {
      *min_v = qMin(*min_v, value);
      *max_v = qMax(*max_v, value);
    }
  };

  const bool has_a = !motor_position_history_.isEmpty();
  const bool has_b = !motor_position_b_history_.isEmpty();
  double min_v = -1.0;
  double max_v = 1.0;
  if (has_a || has_b) {
    min_v = 1.0e12;
    max_v = -1.0e12;
    include_range(motor_position_history_, &min_v, &max_v);
    include_range(motor_position_b_history_, &min_v, &max_v);
    const double pad = qMax(1.0, (max_v - min_v) * 0.08);
    min_v -= pad;
    max_v += pad;
  }
  if (max_v <= min_v) {
    max_v = min_v + 1.0;
  }

  auto draw_series = [&](const QVector<double> &values, const QColor &color) {
    if (values.isEmpty()) {
      return;
    }
    QPainterPath path;
    const int n = values.size();
    for (int i = 0; i < n; ++i) {
      const double x = n == 1 ? left :
          left + static_cast<double>(i) * (right - left) / static_cast<double>(n - 1);
      const double norm = (values.at(i) - min_v) / (max_v - min_v);
      const double y = bottom - norm * (bottom - top);
      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }
    p.setPen(QPen(color, 3));
    p.drawPath(path);
  };

  if (!has_a && !has_b) {
    p.setPen(QPen(QColor("#94a3b8"), 2));
    p.drawLine(left, (top + bottom) / 2, right, (top + bottom) / 2);
  } else {
    draw_series(motor_position_history_, QColor("#2563eb"));
    draw_series(motor_position_b_history_, QColor("#059669"));
  }

  p.setPen(QPen(QColor("#6b7280"), 1));
  p.drawText(8, top + 4, QStringLiteral("%1 mV").arg(max_v, 0, 'f', 0));
  p.drawText(8, bottom, QStringLiteral("%1 mV").arg(min_v, 0, 'f', 0));

  p.setPen(QPen(QColor("#111827"), 1));
  p.drawText(left, h - 24, QStringLiteral("AD7616 双路反馈电压趋势 (mV)"));
  p.setPen(QPen(QColor("#2563eb"), 3));
  p.drawLine(left + 260, h - 28, left + 300, h - 28);
  p.setPen(QPen(QColor("#111827"), 1));
  p.drawText(left + 308, h - 24, QStringLiteral("A路"));
  p.setPen(QPen(QColor("#059669"), 3));
  p.drawLine(left + 360, h - 28, left + 400, h - 28);
  p.setPen(QPen(QColor("#111827"), 1));
  p.drawText(left + 408, h - 24, QStringLiteral("B路"));
  p.setPen(QPen(QColor("#b45309"), 1));
  p.drawText(left + 475, h - 24,
             QStringLiteral("A-B=%1 mV").arg(motor_position_ - motor_position_b_, 0, 'f', 3));
  return pix;
}

void MainWindow::updateMotorPlot(bool running) {
  Q_UNUSED(running);
  ZynqMotorSnapshot snapshot;
  QString error;
  QString response;
  if (sendZynqCommand(QStringLiteral("STATUS"), &response, &error, 700) &&
      parseZynqStatusResponse(response, &snapshot, &error)) {
    const qint16 raw_a = static_cast<qint16>(snapshot.adc_raw & 0xffffu);
    const qint16 raw_b = static_cast<qint16>((snapshot.adc_raw >> 16) & 0xffffu);
    const double adc_a_mv = static_cast<double>(snapshot.adc_a_uv) / 1000.0;
    const double adc_b_mv = static_cast<double>(snapshot.adc_b_uv) / 1000.0;
    motor_running_ = snapshot.running;
    motor_position_ = adc_a_mv;
    motor_position_b_ = adc_b_mv;
    motor_position_history_.append(motor_position_);
    motor_position_b_history_.append(motor_position_b_);
    while (motor_position_history_.size() > 180) {
      motor_position_history_.removeFirst();
    }
    while (motor_position_b_history_.size() > 180) {
      motor_position_b_history_.removeFirst();
    }
    if (motor_fpga_label_) {
      motor_fpga_label_->setText(QStringLiteral("Zynq：已连接 %1").arg(snapshot.port));
      motor_fpga_label_->setStyleSheet(QStringLiteral("color:#047857; font-weight:700;"));
    }
    if (motor_count_label_) {
      motor_count_label_->setText(
          QStringLiteral("Zynq记录 %1    ADC raw A %2 / B %3")
              .arg(snapshot.count)
              .arg(raw_a)
              .arg(raw_b));
    }
    if (motor_status_label_) {
      motor_status_label_->setText(
          QStringLiteral("CONTROL 0x%1    STATUS 0x%2    INTR 0x%3    AD A %4 mV / B %5 mV")
              .arg(snapshot.control, 8, 16, QChar('0'))
              .arg(snapshot.status, 8, 16, QChar('0'))
              .arg(snapshot.intr_status, 8, 16, QChar('0'))
              .arg(adc_a_mv, 0, 'f', 3)
              .arg(adc_b_mv, 0, 'f', 3));
    }
    if (motor_state_label_) {
      motor_state_label_->setText(motor_running_
          ? QStringLiteral("Zynq电机/外触发运行中")
          : (snapshot.adc_ready ? QStringLiteral("Zynq待机 / AD已初始化 / 电机反馈待实物确认")
                                : QStringLiteral("Zynq待机 / AD未就绪 / 电机反馈未确认")));
      motor_state_label_->setStyleSheet(motor_running_
          ? QStringLiteral("color:#2563eb; font-weight:700;")
          : QStringLiteral("color:#6b7280; font-weight:700;"));
    }
  } else {
    if (motor_fpga_label_) {
      motor_fpga_label_->setText(QStringLiteral("Zynq：未连接"));
      motor_fpga_label_->setStyleSheet(QStringLiteral("color:#b45309; font-weight:700;"));
    }
    if (motor_status_label_) {
      motor_status_label_->setText(QStringLiteral("状态 --    %1").arg(error));
    }
    if (motor_state_label_) {
      motor_state_label_->setText(QStringLiteral("Zynq串口未连接 / 电机反馈未确认"));
      motor_state_label_->setStyleSheet(QStringLiteral("color:#6b7280; font-weight:700;"));
    }
  }
  if (motor_plot_label_) {
    motor_plot_label_->setPixmap(makeMotorPlot());
  }
  if (motor_position_label_) {
    if (error.isEmpty()) {
      motor_position_label_->setText(QStringLiteral("AD反馈 A %1 mV / B %2 mV，A-B %3 mV；需接入电机控制器后确认真实反馈")
          .arg(motor_position_, 0, 'f', 3)
          .arg(motor_position_b_, 0, 'f', 3)
          .arg(motor_position_ - motor_position_b_, 0, 'f', 3));
    } else {
      motor_position_label_->setText(QStringLiteral("AD反馈 A -- mV / B -- mV"));
    }
  }
}

bool MainWindow::writeFpgaReg(quint32 offset, quint32 value, QString *error) const {
  FpgaBarAccess bar(error);
  if (!bar.ok()) {
    return false;
  }
  const quint32 version = bar.read32(kFpgaRegVersion);
  if (version != kExpectedFpgaVersion) {
    if (error) {
      *error = QStringLiteral("FPGA版本不匹配：0x%1").arg(version, 8, 16, QChar('0'));
    }
    return false;
  }
  bar.write32(offset, value);
  return true;
}

void MainWindow::onApplyFpgaProcClicked() {
  QString error;
  if (!ensureFpgaPcieReady(&error)) {
    appendKeyStatus(QStringLiteral("FPGA PROC_CTRL apply failed"));
    appendLog(QString("FPGA PROC_CTRL apply failed: %1").arg(error));
    QMessageBox::warning(this, QStringLiteral("FPGA PROC_CTRL"), error);
    return;
  }

  const quint32 value = fpgaProcCtrlFromUi();
  if (!writeFpgaReg(kFpgaRegProcCtrl, value, &error)) {
    appendKeyStatus(QStringLiteral("FPGA PROC_CTRL apply failed"));
    appendLog(QString("FPGA PROC_CTRL apply failed: %1").arg(error));
    QMessageBox::warning(this, QStringLiteral("FPGA PROC_CTRL"), error);
    return;
  }

  appendKeyStatus(QStringLiteral("FPGA PROC_CTRL applied"));
  appendLog(QString("FPGA PROC_CTRL applied: %1").arg(fpgaProcCtrlSummary(value)));
}

void MainWindow::onReadFpgaProcClicked() {
  QString error;
  if (!ensureFpgaPcieReady(&error)) {
    appendKeyStatus(QStringLiteral("FPGA PROC_CTRL read failed"));
    appendLog(QString("FPGA PROC_CTRL read failed: %1").arg(error));
    QMessageBox::warning(this, QStringLiteral("FPGA PROC_CTRL"), error);
    return;
  }

  quint32 value = 0;
  if (!readFpgaReg(kFpgaRegProcCtrl, &value, &error)) {
    appendKeyStatus(QStringLiteral("FPGA PROC_CTRL read failed"));
    appendLog(QString("FPGA PROC_CTRL read failed: %1").arg(error));
    QMessageBox::warning(this, QStringLiteral("FPGA PROC_CTRL"), error);
    return;
  }

  setFpgaProcCtrlUi(value);
  appendKeyStatus(QStringLiteral("FPGA PROC_CTRL read"));
  appendLog(QString("FPGA PROC_CTRL read: %1").arg(fpgaProcCtrlSummary(value)));
}

bool MainWindow::readFpgaReg(quint32 offset, quint32 *value, QString *error) const {
  FpgaBarAccess bar(error);
  if (!bar.ok()) {
    return false;
  }
  const quint32 version = bar.read32(kFpgaRegVersion);
  if (version != kExpectedFpgaVersion) {
    if (error) {
      *error = QStringLiteral("FPGA版本不匹配：0x%1").arg(version, 8, 16, QChar('0'));
    }
    return false;
  }
  if (value) {
    *value = bar.read32(offset);
  }
  return true;
}

bool MainWindow::ensureFpgaPcieReady(QString *error) {
  QString probe_error;
  quint32 version = 0;
  if (readFpgaReg(kFpgaRegVersion, &version, &probe_error) &&
      version == kExpectedFpgaVersion) {
    return true;
  }

  if (!QFileInfo::exists(QString::fromUtf8(kFpgaPcieReadyScript))) {
    if (error) {
      *error = QStringLiteral("FPGA PCIe 自动准备脚本不存在");
    }
    return false;
  }

  appendKeyStatus(QStringLiteral("正在准备FPGA"));
  appendLog(QStringLiteral("正在检查并准备 FPGA PCIe/XDMA 链路..."));
  const CommandResult result = runCommand(
      QStringLiteral("sudo"),
      QStringList() << QStringLiteral("-n") << QString::fromUtf8(kFpgaPcieReadyScript),
      3000);
  if (result.exit_code != 0) {
    if (error) {
      *error = QStringLiteral("FPGA PCIe/XDMA 自动准备失败");
    }
    appendLog(QStringLiteral("FPGA PCIe/XDMA 自动准备失败：%1 %2")
                  .arg(result.stdout_text.trimmed(), result.stderr_text.trimmed()));
    return false;
  }

  probe_error.clear();
  version = 0;
  if (!readFpgaReg(kFpgaRegVersion, &version, &probe_error)) {
    if (error) {
      *error = QStringLiteral("FPGA 控制寄存器仍不可访问");
    }
    appendLog(QStringLiteral("FPGA 控制寄存器仍不可访问：%1").arg(probe_error));
    return false;
  }
  if (version != kExpectedFpgaVersion) {
    if (error) {
      *error = QStringLiteral("FPGA版本不匹配：0x%1").arg(version, 8, 16, QChar('0'));
    }
    appendLog(*error);
    return false;
  }

  appendLog(QStringLiteral("FPGA PCIe/XDMA 链路已准备。"));
  return true;
}

QString MainWindow::findZynqSerialPort(QString *error) const {
  const QString default_port = QString::fromUtf8(kZynqSerialDefaultPort);
  if (QFileInfo::exists(default_port)) {
    return default_port;
  }

  QDir dev_dir(QStringLiteral("/dev"));
  const QFileInfoList entries = dev_dir.entryInfoList(
      QStringList() << QStringLiteral("ttyUSB*") << QStringLiteral("ttyACM*"),
      QDir::System | QDir::Files | QDir::Readable, QDir::Name);
  for (const QFileInfo &entry : entries) {
    return entry.absoluteFilePath();
  }
  if (error) {
    *error = QStringLiteral("未找到 Zynq 串口设备；请确认 RK3588 已连接 Zynq 串口线");
  }
  return QString();
}

bool MainWindow::sendZynqCommand(const QString &command, QString *response, QString *error,
                                 int timeout_ms) const {
  const QString port = findZynqSerialPort(error);
  if (port.isEmpty()) {
    return false;
  }
  const QByteArray port_bytes = port.toLocal8Bit();
  const int fd = ::open(port_bytes.constData(), O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
  if (fd < 0) {
    if (error) {
      *error = QStringLiteral("打开 Zynq 串口失败：%1 (%2)")
                   .arg(port, QString::fromLocal8Bit(std::strerror(errno)));
    }
    return false;
  }

  auto close_fd = [&fd]() {
    if (fd >= 0) {
      ::close(fd);
    }
  };
  if (!configureSerialFd(fd)) {
    if (error) {
      *error = QStringLiteral("配置 Zynq 串口失败：%1").arg(port);
    }
    close_fd();
    return false;
  }
  ::tcflush(fd, TCIOFLUSH);

  QByteArray payload = command.trimmed().toUtf8();
  payload.append('\n');
  const ssize_t written = ::write(fd, payload.constData(), payload.size());
  if (written != payload.size()) {
    if (error) {
      *error = QStringLiteral("发送 Zynq 命令失败：%1").arg(command);
    }
    close_fd();
    return false;
  }
  const QString line = serialReadLine(fd, timeout_ms);
  close_fd();
  if (line.isEmpty()) {
    if (error) {
      *error = QStringLiteral("Zynq 无响应：%1").arg(command);
    }
    return false;
  }
  if (response) {
    *response = line;
  }
  if (!line.startsWith(QStringLiteral("OK"))) {
    if (error) {
      *error = QStringLiteral("Zynq 返回异常：%1").arg(line);
    }
    return false;
  }
  return true;
}

bool MainWindow::parseZynqStatusResponse(const QString &response,
                                         ZynqMotorSnapshot *snapshot,
                                         QString *error) const {
  if (!snapshot) {
    return false;
  }
  const QStringList tokens = response.split(QRegularExpression(QStringLiteral("\\s+")),
                                            Qt::SkipEmptyParts);
  if (tokens.size() < 2 || tokens.at(0) != QStringLiteral("OK") ||
      tokens.at(1) != QStringLiteral("STATUS")) {
    if (error) {
      *error = QStringLiteral("Zynq STATUS 格式错误：%1").arg(response);
    }
    return false;
  }

  snapshot->response = response;
  snapshot->port = findZynqSerialPort(nullptr);
  for (int i = 2; i < tokens.size(); ++i) {
    const QString token = tokens.at(i);
    const int eq = token.indexOf('=');
    if (eq <= 0) {
      continue;
    }
    const QString key = token.left(eq).toUpper();
    const QString value = token.mid(eq + 1);
    if (key == QStringLiteral("CTRL")) {
      bool ok = false;
      const quint32 u32 = parseHexOrDecU32(value, &ok);
      if (!ok) continue;
      snapshot->control = u32;
    } else if (key == QStringLiteral("STATUS")) {
      bool ok = false;
      const quint32 u32 = parseHexOrDecU32(value, &ok);
      if (!ok) continue;
      snapshot->status = u32;
    } else if (key == QStringLiteral("COUNT")) {
      bool ok = false;
      const quint32 u32 = parseHexOrDecU32(value, &ok);
      if (!ok) continue;
      snapshot->count = u32;
    } else if (key == QStringLiteral("INTR")) {
      bool ok = false;
      const quint32 u32 = parseHexOrDecU32(value, &ok);
      if (!ok) continue;
      snapshot->intr_status = u32;
    } else if (key == QStringLiteral("ADC_RAW")) {
      bool ok = false;
      const quint32 u32 = parseHexOrDecU32(value, &ok);
      if (!ok) continue;
      snapshot->adc_raw = u32;
    } else if (key == QStringLiteral("ADC_A_UV")) {
      bool ok = false;
      const qint32 s32 = parseHexOrDecS32(value, &ok);
      if (!ok) continue;
      snapshot->adc_a_uv = s32;
    } else if (key == QStringLiteral("ADC_B_UV")) {
      bool ok = false;
      const qint32 s32 = parseHexOrDecS32(value, &ok);
      if (!ok) continue;
      snapshot->adc_b_uv = s32;
    }
  }

  const bool fast_active = (snapshot->control & kMotorControlFastSweepStart) != 0;
  const bool camera_active = (snapshot->control & kMotorControlCameraTrigger) != 0;
  const bool hw_busy = (snapshot->status & ((1u << 1) | (1u << 3) |
                                            (1u << 8) | (1u << 9))) != 0;
  snapshot->running = (snapshot->control & kMotorControlRun) || fast_active ||
                      camera_active || hw_busy;
  snapshot->adc_ready = (snapshot->status & (1u << 4)) != 0;
  return true;
}

bool MainWindow::configureZynqTriggerFromUi(QString *error) {
  const double frequency_hz = external_trigger_freq_spin_
      ? external_trigger_freq_spin_->value()
      : kDefaultExternalLineFrequencyHz;
  if (frequency_hz <= 0.0) {
    if (error) {
      *error = QStringLiteral("外触发频率必须大于0");
    }
    return false;
  }

  const double min_period_us = qMax(kUiMinExternalLinePeriodUs,
                                    exposure_spin_ ? exposure_spin_->value() + kExposureDeadTimeUs
                                                   : kDefaultExposureUs + kExposureDeadTimeUs);
  const double max_frequency_hz = 1000000.0 / min_period_us;
  if (frequency_hz < kUiMinExternalLineFrequencyHz || frequency_hz > max_frequency_hz) {
    if (error) {
      *error = QStringLiteral("外触发频率 %1 Hz 超出模式二安全范围 %2 - %3 Hz")
                   .arg(frequency_hz, 0, 'f', 2)
                   .arg(kUiMinExternalLineFrequencyHz, 0, 'f', 2)
                   .arg(max_frequency_hz, 0, 'f', 2);
    }
    return false;
  }

  const quint32 total_cycles = qMax<quint32>(
      2u, static_cast<quint32>(std::llround(kZynqFabricClockHz / frequency_hz)));
  const quint32 high_cycles = qMax<quint32>(1u, total_cycles / 2u);
  const quint32 low_cycles = qMax<quint32>(1u, total_cycles - high_cycles);
  const int threshold_code = trigger_threshold_spin_
      ? trigger_threshold_spin_->value()
      : kDefaultTriggerThresholdCode;
  const quint32 pulse_count = trigger_pulse_count_spin_
      ? static_cast<quint32>(trigger_pulse_count_spin_->value())
      : static_cast<quint32>(kDefaultTriggerPulseCount);

  const QString command = QStringLiteral(
      "CFG FREQ_HZ=%1 HIGH=%2 LOW=%3 THRESH=%4 PULSES=%5")
      .arg(frequency_hz, 0, 'f', 2)
      .arg(high_cycles)
      .arg(low_cycles)
      .arg(threshold_code)
      .arg(pulse_count);
  QString response;
  if (!sendZynqCommand(command, &response, error, 1200)) {
    return false;
  }

  appendLog(QStringLiteral("Zynq外触发已配置：%1 Hz，high=%2 cycles，low=%3 cycles，阈值=%4，方波个数=%5，第三路DAC8830输出，需示波器确认后接OCT")
                .arg(frequency_hz, 0, 'f', 2)
                .arg(high_cycles)
                .arg(low_cycles)
                .arg(threshold_code)
                .arg(pulse_count == 0 ? QStringLiteral("连续")
                                      : QString::number(pulse_count)));
  return true;
}

bool MainWindow::startMotorHardware(const QString &reason) {
  QString error;

  if (!configureZynqTriggerFromUi(&error)) {
    appendLog(QString("Zynq trigger configuration failed(%1): %2").arg(reason, error));
    if (motor_state_label_) {
      motor_state_label_->setText(QStringLiteral("外触发配置失败"));
      motor_state_label_->setStyleSheet(QStringLiteral("color:#b91c1c; font-weight:700;"));
    }
    return false;
  }

  const quint32 target = motor_sweep_target_spin_
      ? static_cast<quint32>(motor_sweep_target_spin_->value())
      : 65535u;
  const quint32 cycles = motor_sweep_cycles_spin_
      ? static_cast<quint32>(motor_sweep_cycles_spin_->value())
      : 1u;
  const quint32 interval_ms = motor_sweep_interval_ms_spin_
      ? static_cast<quint32>(motor_sweep_interval_ms_spin_->value())
      : static_cast<quint32>(kDefaultMotorSweepIntervalMs);
  const quint32 hold_cycles = static_cast<quint32>(
      std::llround(static_cast<double>(interval_ms) * kZynqFabricClockHz / 1000.0));

  const QString command = QStringLiteral("START TARGET=%1 CYCLES=%2 HOLD_MS=%3 HOLD=%4")
      .arg(target)
      .arg(cycles)
      .arg(interval_ms)
      .arg(hold_cycles);
  QString response;
  if (!sendZynqCommand(command, &response, &error, 1200)) {
    appendLog(QString("电机启动失败(%1): %2").arg(reason, error));
    if (motor_state_label_) {
      motor_state_label_->setText(QStringLiteral("启动失败"));
      motor_state_label_->setStyleSheet(QStringLiteral("color:#b91c1c; font-weight:700;"));
    }
    return false;
  }
  motor_running_ = true;
  if (motor_state_label_) {
    motor_state_label_->setText(reason);
    motor_state_label_->setStyleSheet(QStringLiteral("color:#2563eb; font-weight:700;"));
  }
  appendLog(QString("电机控制链路已启动：%1，扫频上限=%2，轮数=%3，轮间间隔=%4 ms")
                .arg(reason)
                .arg(target)
                .arg(cycles)
                .arg(interval_ms));
  if (motor_timer_ && !motor_timer_->isActive()) {
    motor_timer_->start();
  }
  updateMotorPlot(true);
  return true;
}

bool MainWindow::stopMotorHardware(const QString &reason) {
  if (kUseDatasetCaptureMode) {
    Q_UNUSED(reason);
    motor_running_ = false;
    return true;
  }
  QString error;
  QString response;
  if (!sendZynqCommand(QStringLiteral("STOP"), &response, &error, 1000)) {
    appendLog(QString("电机停止失败(%1): %2").arg(reason, error));
    return false;
  }

  bool stopped = false;
  for (int i = 0; i < 25; ++i) {
    ZynqMotorSnapshot snapshot;
    QString status_response;
    if (!sendZynqCommand(QStringLiteral("STATUS"), &status_response, &error, 700) ||
        !parseZynqStatusResponse(status_response, &snapshot, &error)) {
      break;
    }
    if (!snapshot.running) {
      stopped = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  motor_running_ = !stopped;
  if (motor_state_label_) {
    motor_state_label_->setText(stopped
        ? QStringLiteral("控制链路待机 / 电机反馈未确认")
        : QStringLiteral("停止命令已发送 / 硬件仍忙"));
    motor_state_label_->setStyleSheet(stopped
        ? QStringLiteral("color:#6b7280; font-weight:700;")
        : QStringLiteral("color:#b45309; font-weight:700;"));
  }
  appendLog(stopped
      ? QString("电机控制链路已停止：%1").arg(reason)
      : QString("电机停止命令已发送但硬件仍忙：%1").arg(reason));
  updateMotorPlot(false);
  return stopped;
}

bool MainWindow::clearMotorCounters(const QString &reason) {
  QString error;
  QString response;
  if (!sendZynqCommand(QStringLiteral("CLEAR"), &response, &error, 1000)) {
    appendLog(QString("电机清零失败(%1): %2").arg(reason, error));
    return false;
  }
  motor_running_ = false;
  motor_position_ = 0.0;
  motor_position_b_ = 0.0;
  motor_position_history_.clear();
  motor_position_b_history_.clear();
  appendLog(QString("Zynq电机记录计数已清零：%1").arg(reason));
  updateMotorPlot(false);
  return true;
}

void MainWindow::appendKeyStatus(const QString &msg) {
  if (!key_log_edit_ || msg.trimmed().isEmpty()) {
    return;
  }
  key_log_edit_->appendPlainText(
      QString("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), msg));
}

QString MainWindow::keyStatusForLog(const QString &msg) const {
  if (msg.contains(QStringLiteral("Initializing SDK"), Qt::CaseInsensitive)) {
    return QStringLiteral("正在初始化相机");
  }
  if (msg.contains(QStringLiteral("Camera scan complete"), Qt::CaseInsensitive)) {
    return QStringLiteral("相机扫描完成");
  }
  if (msg.contains(QStringLiteral("Opening camera"), Qt::CaseInsensitive)) {
    return QStringLiteral("正在打开相机");
  }
  if (msg.contains(QStringLiteral("Configuring camera"), Qt::CaseInsensitive)) {
    return QStringLiteral("正在配置相机");
  }
  if (msg.contains(QStringLiteral("Acquisition started"), Qt::CaseInsensitive)) {
    return QStringLiteral("正在采集");
  }
  if (msg.contains(QStringLiteral("收到完整SDK图像缓冲"))) {
    return QStringLiteral("收到完整图像");
  }
  if (msg.contains(QStringLiteral("已合成并保存第"))) {
    return msg.section(QStringLiteral(":"), 0, 0);
  }
  if (msg.contains(QStringLiteral("FPGA处理暂不可用"))) {
    return QStringLiteral("FPGA处理暂不可用");
  }
  if (msg.contains(QStringLiteral("failed"), Qt::CaseInsensitive) ||
      msg.contains(QStringLiteral("失败")) ||
      msg.contains(QStringLiteral("异常"))) {
    return QStringLiteral("采集异常：") + msg.left(80);
  }
  return QString();
}

void MainWindow::appendConfigLog(const QString &msg) {
  if (!config_log_edit_) {
    return;
  }
  config_log_edit_->appendPlainText(
      QString("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), msg));
}

void MainWindow::setConfigStatus(QLabel *label, bool ok, const QString &text) {
  if (!label) {
    return;
  }
  label->setText((ok ? QStringLiteral("正常  ") : QStringLiteral("异常  ")) + text);
  label->setStyleSheet(ok ? QStringLiteral("color:#188038; font-weight:600;")
                          : QStringLiteral("color:#b3261e; font-weight:600;"));
}

MainWindow::CommandResult MainWindow::runCommand(
    const QString &program,
    const QStringList &args,
    int timeout_ms) const {
  QProcess process;
  process.start(program, args);
  CommandResult result;
  if (!process.waitForStarted(2000)) {
    result.stderr_text = QStringLiteral("Failed to start: ") + program;
    return result;
  }
  if (!process.waitForFinished(timeout_ms)) {
    process.kill();
    process.waitForFinished(1000);
    result.stderr_text = QStringLiteral("Command timeout: ") + program;
    return result;
  }
  result.exit_code = process.exitCode();
  result.stdout_text = QString::fromUtf8(process.readAllStandardOutput());
  result.stderr_text = QString::fromUtf8(process.readAllStandardError());
  return result;
}

bool MainWindow::pathExists(const QString &path) const {
  return QFileInfo::exists(path);
}

QString MainWindow::currentUser() const {
  const QByteArray user = qgetenv("USER");
  if (!user.isEmpty()) {
    return QString::fromUtf8(user);
  }
  const auto result = runCommand(QStringLiteral("id"), QStringList() << QStringLiteral("-un"), 2000);
  return result.stdout_text.trimmed();
}

void MainWindow::refreshConfigStatus() {
  appendConfigLog(QStringLiteral("刷新相机配置状态..."));

  setConfigStatus(config_sdk_include_status_, pathExists(kSdkIncludeDir),
                  QString::fromUtf8(kSdkIncludeDir));
  setConfigStatus(config_sdk_lib_status_, pathExists(kSdkLibDir),
                  QString::fromUtf8(kSdkLibDir));
  setConfigStatus(config_genicam_status_, pathExists(kGenicamDir),
                  QString::fromUtf8(kGenicamDir));
  setConfigStatus(config_udev_status_, pathExists(kUdevRuleFile),
                  QString::fromUtf8(kUdevRuleFile));

  const auto groups = runCommand(QStringLiteral("id"), QStringList() << QStringLiteral("-nG"), 2000);
  const bool in_plugdev = groups.stdout_text.split(QRegularExpression(QStringLiteral("\\s+")),
                                                   Qt::SkipEmptyParts)
                              .contains(QStringLiteral("plugdev"));
  setConfigStatus(config_plugdev_status_, in_plugdev,
                  in_plugdev ? QStringLiteral("当前用户已加入 plugdev")
                             : QStringLiteral("当前用户未加入 plugdev，USB 权限可能不足"));

  const auto lsusb = runCommand(QStringLiteral("lsusb"), QStringList(), 3000);
  const QString usb_text = (lsusb.stdout_text + QStringLiteral("\n") + lsusb.stderr_text).trimmed();
  const bool camera_found = containsOctCamera(usb_text);
  setConfigStatus(config_usb_camera_status_, camera_found,
                  camera_found ? QStringLiteral("已检测到 OCT / e2v USB 相机")
                               : QStringLiteral("未检测到 OCT 相机，请检查 USB3 连接"));
  if (!usb_text.isEmpty()) {
    appendConfigLog(QStringLiteral("lsusb:\n") + usb_text);
  }

  const QString capture_binary = projectRootDir() + QStringLiteral("/qt_panel/build/oct_qt_panel");
  setConfigStatus(config_capture_binary_status_, QFileInfo(capture_binary).isExecutable(),
                  capture_binary);
  appendConfigLog(QStringLiteral("状态刷新完成。"));
}

void MainWindow::applyConfigUsbRule() {
  const QString script = projectRootDir() + QStringLiteral("/run_camera_config_apply.sh");
  if (!QFileInfo(script).isExecutable()) {
    QMessageBox::warning(this, QStringLiteral("缺少脚本"),
                         QStringLiteral("找不到可执行脚本：\n%1").arg(script));
    return;
  }

  appendConfigLog(QStringLiteral("启动 USB 权限配置脚本..."));
  if (!QProcess::startDetached(script, QStringList() << currentUser(), projectRootDir())) {
    QMessageBox::warning(this, QStringLiteral("启动失败"),
                         QStringLiteral("无法启动 USB 权限配置脚本。"));
    return;
  }
  QMessageBox::information(
      this,
      QStringLiteral("已启动"),
      QStringLiteral("如系统弹出授权窗口，请输入当前用户密码。完成后点击“刷新状态”。"));
}

void MainWindow::switchToCapturePanel() {
  showNormal();
  raise();
  activateWindow();
}

void MainWindow::openConfigCapturesFolder() {
  const QString dir = defaultCaptureRootDir();
  QDir().mkpath(dir);
  QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::openConfigProjectFolder() {
  QDesktopServices::openUrl(QUrl::fromLocalFile(projectRootDir()));
}

void MainWindow::setRunningUi(bool running) {
  running_ = running;
  start_btn_->setEnabled(!running);
  stop_btn_->setEnabled(running);
  scan_btn_->setEnabled(!running);
  new_diagnosis_btn_->setEnabled(!running);
  previous_diagnosis_btn_->setEnabled(!running);
  delete_current_diagnosis_btn_->setEnabled(
      !running && analysis_process_ == nullptr && !current_diagnosis_dir_.isEmpty());
  camera_combo_->setEnabled(!running);
  const bool has_diagnosis = !current_diagnosis_dir_.isEmpty() || !last_diagnosis_dir_.isEmpty();
  const bool has_current = !current_diagnosis_dir_.isEmpty();
  if (analyze_recognition_btn_) {
    analyze_recognition_btn_->setEnabled(!running && has_diagnosis);
  }
  analyze_last_btn_->setText(has_current ? QStringLiteral("分析当前诊断")
                                         : QStringLiteral("分析最近诊断"));
  open_last_session_btn_->setText(has_current ? QStringLiteral("打开诊断目录")
                                              : QStringLiteral("打开最近诊断"));
  analyze_last_btn_->setEnabled(!running && has_diagnosis);
  open_last_session_btn_->setEnabled(has_diagnosis);
  updateStatusLabel();
}

QString MainWindow::projectRootDir() const {
  QDir dir(QCoreApplication::applicationDirPath());
  dir.cdUp();
  dir.cdUp();
  return dir.absolutePath();
}

QString MainWindow::defaultCaptureRootDir() const {
  return QDir(projectRootDir()).filePath(QStringLiteral("captures"));
}

QString MainWindow::defaultPythonDir() const {
  const QString yolo_python =
      QDir(QDir::homePath()).filePath(QStringLiteral("Desktop/yolo/03_rk3588_model"));
  if (QDir(yolo_python).exists()) {
    return yolo_python;
  }
  const QString project_python = QDir(projectRootDir()).filePath(QStringLiteral("python"));
  if (QDir(project_python).exists()) {
    return project_python;
  }
  const QString home_python = QDir(QDir::homePath()).filePath(QStringLiteral("python"));
  if (QDir(home_python).exists()) {
    return home_python;
  }
  return project_python;
}

bool MainWindow::parseDiagnosisFolderName(
    const QString &folder_name,
    QString *date,
    int *index) const {
  const QRegularExpression re(QStringLiteral("^(\\d{8})_第(\\d+)次诊断(?:_.*)?$"));
  const QRegularExpressionMatch match = re.match(folder_name);
  if (!match.hasMatch()) {
    return false;
  }
  if (date) {
    *date = match.captured(1);
  }
  if (index) {
    *index = match.captured(2).toInt();
  }
  return true;
}

int MainWindow::maxDailyDiagnosisIndex(const QString &capture_root_dir, const QString &date) const {
  QDir root(capture_root_dir);
  if (!root.exists()) {
    return 0;
  }

  int max_index = 0;
  const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QString &entry : entries) {
    QString parsed_date;
    int parsed_index = 0;
    if (parseDiagnosisFolderName(entry, &parsed_date, &parsed_index) && parsed_date == date) {
      max_index = qMax(max_index, parsed_index);
    }
  }
  return max_index;
}

QString MainWindow::makeDiagnosisDir(
    const QString &capture_root_dir,
    const QString &prefix,
    QString *diagnosis_id) {
  QDir root(capture_root_dir);
  if (!root.exists() && !root.mkpath(".")) {
    return QString();
  }

  Q_UNUSED(prefix);
  const QString date = QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
  int diagnosis_index = maxDailyDiagnosisIndex(root.absolutePath(), date) + 1;
  QString id;
  do {
    id = QStringLiteral("%1_第%2次诊断")
             .arg(date)
             .arg(diagnosis_index, 3, 10, QLatin1Char('0'));
    if (!root.exists(id)) {
      break;
    }
    ++diagnosis_index;
  } while (true);

  if (!root.mkpath(id)) {
    return QString();
  }
  if (diagnosis_id) {
    *diagnosis_id = id;
  }
  return root.filePath(id);
}

QString MainWindow::makeCaptureDir(
    const QString &parent_dir,
    int capture_index,
    QString *capture_id) {
  QDir dir(parent_dir);
  if (!dir.exists()) {
    return QString();
  }
  const QString id = QStringLiteral("capture_%1").arg(capture_index, 3, 10, QLatin1Char('0'));
  if (!dir.mkpath(id)) {
    return QString();
  }
  if (capture_id) {
    *capture_id = id;
  }
  return dir.filePath(id);
}

QString MainWindow::makeVisitDir(
    const QString &diagnosis_dir,
    int visit_index,
    QString *visit_id) {
  QDir diagnosis(diagnosis_dir);
  if (!diagnosis.exists()) {
    return QString();
  }
  const QString id = QStringLiteral("visit_%1").arg(visit_index, 3, 10, QLatin1Char('0'));
  if (!diagnosis.mkpath(id)) {
    return QString();
  }
  if (visit_id) {
    *visit_id = id;
  }
  return diagnosis.filePath(id);
}

bool MainWindow::ensureDiagnosis(const QString &capture_root_dir, const QString &prefix) {
  if (!current_diagnosis_dir_.isEmpty() && QDir(current_diagnosis_dir_).exists() &&
      !current_visit_dir_.isEmpty() && QDir(current_visit_dir_).exists()) {
    return true;
  }
  return startNewDiagnosis(capture_root_dir, prefix);
}

bool MainWindow::startNewDiagnosis(const QString &capture_root_dir, const QString &prefix) {
  QString diagnosis_id;
  const QString dir = makeDiagnosisDir(capture_root_dir, prefix, &diagnosis_id);
  if (dir.isEmpty()) {
    return false;
  }
  current_diagnosis_id_ = diagnosis_id;
  current_diagnosis_dir_ = dir;
  current_diagnosis_date_.clear();
  current_diagnosis_index_ = 0;
  parseDiagnosisFolderName(current_diagnosis_id_, &current_diagnosis_date_, &current_diagnosis_index_);
  last_diagnosis_dir_ = dir;
  current_visit_index_ = 1;
  visit_capture_index_ = 0;
  current_visit_dir_ = makeVisitDir(current_diagnosis_dir_, current_visit_index_, &current_visit_id_);
  if (current_visit_dir_.isEmpty()) {
    return false;
  }
  writeDiagnosisManifest(dir, diagnosis_id, capture_root_dir, prefix);
  writeVisitManifest(current_visit_dir_, current_visit_id_, current_visit_index_);

  appendLog(QString("新建诊断: %1").arg(current_diagnosis_id_));
  appendLog(QString("诊断目录: %1").arg(current_diagnosis_dir_));
  appendLog(QString("当前就诊: %1 (初诊)").arg(current_visit_id_));
  setRunningUi(running_);
  return true;
}

bool MainWindow::startNextVisitForDiagnosis(const QString &diagnosis_dir) {
  QFileInfo selected_info(diagnosis_dir);
  QString normalized_dir = selected_info.absoluteFilePath();
  if (selected_info.fileName().startsWith(QStringLiteral("capture_"))) {
    QFileInfo parent_info(selected_info.absoluteDir().absolutePath());
    normalized_dir = parent_info.fileName().startsWith(QStringLiteral("visit_"))
                         ? parent_info.absoluteDir().absolutePath()
                         : parent_info.absoluteFilePath();
  } else if (selected_info.fileName().startsWith(QStringLiteral("visit_"))) {
    normalized_dir = selected_info.absoluteDir().absolutePath();
  }

  QDir diagnosis(normalized_dir);
  if (!diagnosis.exists()) {
    return false;
  }

  current_diagnosis_dir_ = diagnosis.absolutePath();
  current_diagnosis_id_ = QFileInfo(current_diagnosis_dir_).fileName();
  current_diagnosis_date_.clear();
  current_diagnosis_index_ = 0;
  parseDiagnosisFolderName(current_diagnosis_id_, &current_diagnosis_date_, &current_diagnosis_index_);
  last_diagnosis_dir_ = current_diagnosis_dir_;
  const int max_visit_index =
      maxNumberedChildIndex(current_diagnosis_dir_, QStringLiteral("visit_"));
  const int legacy_capture_index =
      maxNumberedChildIndex(current_diagnosis_dir_, QStringLiteral("capture_"));
  if (max_visit_index > 0) {
    current_visit_index_ = max_visit_index + 1;
  } else if (legacy_capture_index > 0) {
    current_visit_index_ = 2;
  } else {
    current_visit_index_ = 1;
  }
  visit_capture_index_ = 0;
  current_visit_dir_ = makeVisitDir(current_diagnosis_dir_, current_visit_index_, &current_visit_id_);
  if (current_visit_dir_.isEmpty()) {
    return false;
  }
  writeVisitManifest(current_visit_dir_, current_visit_id_, current_visit_index_);

  appendLog(QString("以往诊断: %1").arg(current_diagnosis_id_));
  appendLog(QString("诊断目录: %1").arg(current_diagnosis_dir_));
  appendLog(QString("新复诊目录: %1").arg(current_visit_id_));
  setRunningUi(running_);
  return true;
}

int MainWindow::maxNumberedChildIndex(const QString &parent_dir, const QString &prefix) const {
  QDir dir(parent_dir);
  int max_index = 0;
  const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  const QRegularExpression re(QStringLiteral("^%1(\\d+)$").arg(QRegularExpression::escape(prefix)));
  for (const QString &entry : entries) {
    const QRegularExpressionMatch match = re.match(entry);
    if (match.hasMatch()) {
      max_index = qMax(max_index, match.captured(1).toInt());
    }
  }
  return max_index;
}

int MainWindow::maxCaptureFileIndex(const QString &capture_dir) const {
  QDir dir(capture_dir);
  int max_index = 0;
  const QStringList files = dir.entryList(QDir::Files, QDir::Name);
  const QRegularExpression re(QStringLiteral("capture_(\\d+)"));
  for (const QString &file : files) {
    const QRegularExpressionMatch match = re.match(file);
    if (match.hasMatch()) {
      max_index = qMax(max_index, match.captured(1).toInt());
    }
  }
  return max_index;
}

void MainWindow::clearCurrentDiagnosis() {
  current_diagnosis_id_.clear();
  current_diagnosis_dir_.clear();
  current_diagnosis_date_.clear();
  current_visit_id_.clear();
  current_visit_dir_.clear();
  active_capture_dir_.clear();
  current_diagnosis_index_ = 0;
  current_visit_index_ = 0;
  visit_capture_index_ = 0;
}

void MainWindow::updateStatusLabel() {
  if (!status_label_) {
    return;
  }

  if (analysis_process_) {
    status_label_->setText(QStringLiteral("正在分析识别"));
    return;
  }

  if (!current_diagnosis_dir_.isEmpty()) {
    QString diagnosis_text = current_diagnosis_id_;
    if (!current_diagnosis_date_.isEmpty() && current_diagnosis_index_ > 0) {
      diagnosis_text = QStringLiteral("%1 第%2次诊断")
                           .arg(current_diagnosis_date_)
                           .arg(current_diagnosis_index_);
    }

    QString visit_text = QStringLiteral("未创建就诊目录");
    if (!current_visit_id_.isEmpty()) {
      visit_text = current_visit_index_ <= 1
                       ? QStringLiteral("%1 初诊").arg(current_visit_id_)
                       : QStringLiteral("%1 第%2次复诊")
                             .arg(current_visit_id_)
                             .arg(current_visit_index_ - 1);
    }

    status_label_->setText(
        QStringLiteral("当前诊断：%1\n当前就诊：%2；本就诊已采集 %3 次")
            .arg(diagnosis_text)
            .arg(visit_text)
            .arg(visit_capture_index_));
    return;
  }

  const QString capture_root_dir = output_dir_edit_ && !output_dir_edit_->text().trimmed().isEmpty()
                                       ? output_dir_edit_->text().trimmed()
                                       : defaultCaptureRootDir();
  const QString today = QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
  const int next_index = maxDailyDiagnosisIndex(capture_root_dir, today) + 1;
  status_label_->setText(
      QStringLiteral("当前诊断：未创建\n下一次开始采集时，将自动创建 %1 第%2次诊断")
          .arg(today)
          .arg(next_index));
}

void MainWindow::updateTimingHint() {
  if (!timing_hint_label_ || !line_period_spin_ || !exposure_spin_) {
    return;
  }

  const double exposure_us_for_limit = exposure_spin_->value();
  const double min_period_for_exposure_us =
      qMax(kUiMinExternalLinePeriodUs, exposure_us_for_limit + kExposureDeadTimeUs);
  const double max_frequency_hz = 1000000.0 / min_period_for_exposure_us;
  if (external_trigger_freq_spin_) {
    external_trigger_freq_spin_->setRange(kUiMinExternalLineFrequencyHz, max_frequency_hz);
    if (external_trigger_freq_spin_->value() > max_frequency_hz) {
      external_trigger_freq_spin_->setValue(max_frequency_hz);
      return;
    }
    if (external_trigger_freq_spin_->value() < kUiMinExternalLineFrequencyHz) {
      external_trigger_freq_spin_->setValue(kUiMinExternalLineFrequencyHz);
      return;
    }
  }

  const double line_period_us = line_period_spin_->value();
  const double max_exposure_us = qMax(kUiMinExposureUs, line_period_us - kExposureDeadTimeUs);
  exposure_spin_->setMaximum(max_exposure_us);
  const double exposure_us = exposure_spin_->value();
  const double frequency_hz = 1000000.0 / line_period_us;

  timing_hint_label_->setText(
      QStringLiteral("外触发频率参考：%1 Hz，周期：%2 us。\n"
                     "模式2安全范围：%3-%4 Hz；当前曝光：%5 us，最大曝光：%6 us。\n"
                     "上限依据：外部周期 >= 曝光 + 0.7 us；下限按 LinePeriod 655.35 us 做软件保护。")
          .arg(frequency_hz, 0, 'f', 2)
          .arg(line_period_us, 0, 'f', 2)
          .arg(kUiMinExternalLineFrequencyHz, 0, 'f', 2)
          .arg(max_frequency_hz, 0, 'f', 2)
          .arg(exposure_us, 0, 'f', 2)
          .arg(max_exposure_us, 0, 'f', 2));
  timing_hint_label_->setStyleSheet(
      QStringLiteral("color:#1f7a5a; background:#f3fbf7; border:1px solid #cce8d8; "
                     "border-radius:4px; padding:5px;"));
}

void MainWindow::loadPanelSettings() {
  QSettings settings(QStringLiteral("Qianrushi"), QStringLiteral("OCTCapturePanel"));

  if (output_dir_edit_) {
    output_dir_edit_->setText(settings.value(QStringLiteral("paths/output_dir"),
                                             output_dir_edit_->text()).toString());
  }
  if (prefix_edit_) {
    prefix_edit_->setText(settings.value(QStringLiteral("paths/prefix"),
                                         prefix_edit_->text()).toString());
  }
  if (python_dir_edit_) {
    python_dir_edit_->setText(settings.value(QStringLiteral("paths/python_dir"),
                                             python_dir_edit_->text()).toString());
    const QString current_script =
        QDir(python_dir_edit_->text()).filePath(QStringLiteral("predict_batch.py"));
    const QString yolo_python =
        QDir(QDir::homePath()).filePath(QStringLiteral("Desktop/yolo/03_rk3588_model"));
    const QString yolo_script = QDir(yolo_python).filePath(QStringLiteral("predict_batch.py"));
    if (!QFileInfo::exists(current_script) && QFileInfo::exists(yolo_script)) {
      python_dir_edit_->setText(yolo_python);
    }
  }
  if (exposure_spin_) {
    exposure_spin_->setValue(settings.value(QStringLiteral("camera/exposure_us"),
                                            kDefaultExposureUs).toDouble());
  }
  if (external_trigger_freq_spin_) {
    external_trigger_freq_spin_->setValue(settings.value(
        QStringLiteral("camera/external_trigger_frequency_hz"),
        kDefaultExternalLineFrequencyHz).toDouble());
  }
  if (line_period_spin_ && external_trigger_freq_spin_ &&
      external_trigger_freq_spin_->value() > 0.0) {
    line_period_spin_->setValue(1000000.0 / external_trigger_freq_spin_->value());
  }
  if (trigger_threshold_spin_) {
    trigger_threshold_spin_->setValue(settings.value(QStringLiteral("camera/trigger_threshold_code"),
                                                     kDefaultTriggerThresholdCode).toInt());
  }
  if (trigger_pulse_count_spin_) {
    trigger_pulse_count_spin_->setValue(settings.value(QStringLiteral("camera/trigger_pulse_count"),
                                                       kDefaultTriggerPulseCount).toInt());
  }
  if (frame_lines_spin_) {
    frame_lines_spin_->setValue(settings.value(QStringLiteral("camera/frame_lines"), 4096).toInt());
  }
  if (staging_lines_spin_) {
    staging_lines_spin_->setValue(settings.value(QStringLiteral("camera/staging_lines"), 5).toInt());
  }
  if (sdk_buffers_spin_) {
    sdk_buffers_spin_->setValue(settings.value(QStringLiteral("camera/sdk_buffers"), 16).toInt());
  }
  if (timeout_spin_) {
    timeout_spin_->setValue(settings.value(QStringLiteral("camera/getbuffer_timeout_ms"), 200).toInt());
  }
  quint32 fpga_proc_ctrl =
      settings.value(QStringLiteral("fpga/proc_ctrl"), kDefaultFpgaProcCtrl).toUInt();
  if (fpga_proc_ctrl == kLegacyFpgaProcCtrlNorm10 ||
      fpga_proc_ctrl == kLegacyFpgaProcCtrlOut8Norm10 ||
      fpga_proc_ctrl == kLegacyFpgaProcCtrlLogNorm0 ||
      fpga_proc_ctrl == kLegacyFpgaProcCtrlLinearHalfScale ||
      fpga_proc_ctrl == kLegacyFpgaProcCtrlLinearCurrent) {
    fpga_proc_ctrl = kDefaultFpgaProcCtrl;
    settings.setValue(QStringLiteral("fpga/proc_ctrl"), fpga_proc_ctrl);
  }
  setFpgaProcCtrlUi(fpga_proc_ctrl);

  if (auto *save_raw = findChild<QCheckBox *>(QStringLiteral("save_raw"))) {
    save_raw->setChecked(kUseDatasetCaptureMode
                             ? false
                             : settings.value(QStringLiteral("save/save_raw"), true).toBool());
  }
  if (auto *save_pgm = findChild<QCheckBox *>(QStringLiteral("save_pgm"))) {
    save_pgm->setChecked(kUseDatasetCaptureMode
                             ? false
                             : settings.value(QStringLiteral("save/save_pgm"), true).toBool());
  }
  settings.remove(QStringLiteral("save/auto_analyze"));
  if (continuous_capture_cb_) {
    continuous_capture_cb_->setChecked(settings.value(QStringLiteral("save/continuous"),
                                                      continuous_capture_cb_->isChecked()).toBool());
  }
  if (motor_sweep_target_spin_) {
    motor_sweep_target_spin_->setValue(settings.value(QStringLiteral("motor/sweep_target"),
                                                      motor_sweep_target_spin_->value()).toInt());
  }
  if (motor_sweep_cycles_spin_) {
    motor_sweep_cycles_spin_->setValue(settings.value(QStringLiteral("motor/sweep_cycles"),
                                                      motor_sweep_cycles_spin_->value()).toInt());
  }
  if (motor_sweep_interval_ms_spin_) {
    motor_sweep_interval_ms_spin_->setValue(settings.value(QStringLiteral("motor/sweep_interval_ms"),
                                                           kDefaultMotorSweepIntervalMs).toInt());
  }
}

void MainWindow::savePanelSettings() const {
  QSettings settings(QStringLiteral("Qianrushi"), QStringLiteral("OCTCapturePanel"));

  if (output_dir_edit_) settings.setValue(QStringLiteral("paths/output_dir"), output_dir_edit_->text());
  if (prefix_edit_) settings.setValue(QStringLiteral("paths/prefix"), prefix_edit_->text());
  if (python_dir_edit_) settings.setValue(QStringLiteral("paths/python_dir"), python_dir_edit_->text());
  if (camera_combo_ && camera_combo_->currentIndex() >= 0) {
    settings.setValue(QStringLiteral("camera/selected_index"), camera_combo_->currentData().toInt());
  }
  if (exposure_spin_) settings.setValue(QStringLiteral("camera/exposure_us"), exposure_spin_->value());
  if (external_trigger_freq_spin_) {
    settings.setValue(QStringLiteral("camera/external_trigger_frequency_hz"),
                      external_trigger_freq_spin_->value());
  }
  if (trigger_threshold_spin_) {
    settings.setValue(QStringLiteral("camera/trigger_threshold_code"),
                      trigger_threshold_spin_->value());
  }
  if (trigger_pulse_count_spin_) {
    settings.setValue(QStringLiteral("camera/trigger_pulse_count"),
                      trigger_pulse_count_spin_->value());
  }
  if (frame_lines_spin_) settings.setValue(QStringLiteral("camera/frame_lines"), frame_lines_spin_->value());
  if (staging_lines_spin_) settings.setValue(QStringLiteral("camera/staging_lines"), staging_lines_spin_->value());
  if (sdk_buffers_spin_) settings.setValue(QStringLiteral("camera/sdk_buffers"), sdk_buffers_spin_->value());
  if (timeout_spin_) settings.setValue(QStringLiteral("camera/getbuffer_timeout_ms"), timeout_spin_->value());
  settings.setValue(QStringLiteral("fpga/proc_ctrl"), fpgaProcCtrlFromUi());

  if (auto *save_raw = findChild<QCheckBox *>(QStringLiteral("save_raw"))) {
    settings.setValue(QStringLiteral("save/save_raw"), save_raw->isChecked());
  }
  if (auto *save_pgm = findChild<QCheckBox *>(QStringLiteral("save_pgm"))) {
    settings.setValue(QStringLiteral("save/save_pgm"), save_pgm->isChecked());
  }
  settings.remove(QStringLiteral("save/auto_analyze"));
  if (continuous_capture_cb_) settings.setValue(QStringLiteral("save/continuous"), continuous_capture_cb_->isChecked());
  if (motor_sweep_target_spin_) settings.setValue(QStringLiteral("motor/sweep_target"), motor_sweep_target_spin_->value());
  if (motor_sweep_cycles_spin_) settings.setValue(QStringLiteral("motor/sweep_cycles"), motor_sweep_cycles_spin_->value());
  if (motor_sweep_interval_ms_spin_) {
    settings.setValue(QStringLiteral("motor/sweep_interval_ms"),
                      motor_sweep_interval_ms_spin_->value());
  }
}

void MainWindow::writeDiagnosisManifest(
    const QString &diagnosis_dir,
    const QString &diagnosis_id,
    const QString &capture_root_dir,
    const QString &prefix) {
  QFile manifest(QDir(diagnosis_dir).filePath(QStringLiteral("diagnosis_manifest.txt")));
  if (manifest.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QTextStream ts(&manifest);
    ts << "diagnosis_id=" << diagnosis_id << "\n";
    ts << "created_at=" << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ts << "capture_root_dir=" << capture_root_dir << "\n";
    ts << "file_prefix=" << prefix << "\n";
  }
}

void MainWindow::writeVisitManifest(
    const QString &visit_dir,
    const QString &visit_id,
    int visit_index) {
  QFile manifest(QDir(visit_dir).filePath(QStringLiteral("visit_manifest.txt")));
  if (manifest.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QTextStream ts(&manifest);
    ts << "diagnosis_id=" << current_diagnosis_id_ << "\n";
    ts << "visit_id=" << visit_id << "\n";
    ts << "visit_index=" << visit_index << "\n";
    ts << "followup_index=" << qMax(0, visit_index - 1) << "\n";
    ts << "visit_type=" << (visit_index == 1 ? "initial" : "follow_up") << "\n";
    ts << "created_at=" << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ts << "diagnosis_dir=" << current_diagnosis_dir_ << "\n";
  }
}

QString MainWindow::scanUsbPorts() {
  QProcess p;
  p.start("bash", QStringList() << "-lc"
                                << "which lsusb >/dev/null 2>&1 && { lsusb; echo '---'; lsusb -t; }");
  if (!p.waitForFinished(3000)) {
    p.kill();
    return QStringLiteral("lsusb scan timeout");
  }
  QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
  if (out.isEmpty()) {
    out = QString::fromUtf8(p.readAllStandardError()).trimmed();
  }
  return out.isEmpty() ? QStringLiteral("lsusb not available") : out;
}

bool MainWindow::scanSdkCameras(QStringList *out_list, QString *error) {
  out_list->clear();
  error->clear();

  int err = USB3_InitializeLibrary();
  if (err != CAM_ERR_SUCCESS) {
    *error = QString("USB3_InitializeLibrary failed: %1 (%2)")
                 .arg(err)
                 .arg(sdkErrorText(err));
    return false;
  }

  uint32_t nb_cameras = 0;
  int last_err = CAM_ERR_SUCCESS;
  if (!updateCameraListWithRetry(&nb_cameras, &last_err)) {
    *error = QString("USB3_UpdateCameraList failed: %1 (%2)")
                 .arg(last_err)
                 .arg(sdkErrorText(last_err));
    USB3_TerminateLibrary();
    return false;
  }

  for (uint32_t i = 0; i < nb_cameras; ++i) {
    tCameraInfo info {};
    err = USB3_GetCameraInfo(i, &info);
    if (err == CAM_ERR_SUCCESS) {
      out_list->push_back(QString("[%1] %2").arg(i).arg(info.pcID));
    } else {
      out_list->push_back(QString("[%1] <GetCameraInfo error %2>").arg(i).arg(err));
    }
  }

  USB3_TerminateLibrary();
  return true;
}

void MainWindow::onScanClicked() {
  const bool manual_scan = (sender() == scan_btn_);
  appendLog("Scanning USB ports...");
  appendLog(scanUsbPorts());

  QStringList cameras;
  QString error;
  if (!scanSdkCameras(&cameras, &error)) {
    appendLog(error);
    if (manual_scan) {
      QMessageBox::warning(this, QStringLiteral("扫描失败"), error);
    }
    camera_combo_->clear();
    return;
  }

  camera_combo_->clear();
  for (int i = 0; i < cameras.size(); ++i) {
    camera_combo_->addItem(cameras[i], i);
  }
  QSettings panel_settings(QStringLiteral("Qianrushi"), QStringLiteral("OCTCapturePanel"));
  const int preferred_camera =
      panel_settings.value(QStringLiteral("camera/selected_index"), -1).toInt();
  if (preferred_camera >= 0) {
    const int preferred_combo_index = camera_combo_->findData(preferred_camera);
    if (preferred_combo_index >= 0) {
      camera_combo_->setCurrentIndex(preferred_combo_index);
    }
  }

  appendLog(QString("SDK scan complete, camera count=%1").arg(cameras.size()));
  if (manual_scan && cameras.isEmpty()) {
    QMessageBox::information(this, QStringLiteral("扫描结果"), QStringLiteral("未发现相机。"));
  }
}

void MainWindow::onNewDiagnosisClicked() {
  if (running_) {
    return;
  }

  const QString capture_root_dir = output_dir_edit_->text().trimmed();
  if (capture_root_dir.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("保存根目录不能为空。"));
    return;
  }

  clearCurrentDiagnosis();
  appendLog(QStringLiteral("已准备新诊断：首次开始采集时才会创建诊断文件夹。"));
  setRunningUi(running_);
}

void MainWindow::onPreviousDiagnosisClicked() {
  if (running_) {
    return;
  }

  const QString root_dir = output_dir_edit_->text().trimmed().isEmpty()
                               ? defaultCaptureRootDir()
                               : output_dir_edit_->text().trimmed();
  const QString dir = QFileDialog::getExistingDirectory(
      this, QStringLiteral("选择以往诊断目录"), root_dir);
  if (dir.isEmpty()) {
    return;
  }

  if (!startNextVisitForDiagnosis(dir)) {
    QMessageBox::warning(this, QStringLiteral("提示"),
                         QStringLiteral("无法在所选以往诊断目录中创建新的复诊目录。"));
  }
}

void MainWindow::onDeleteCurrentDiagnosisClicked() {
  if (running_) {
    return;
  }
  if (analysis_process_) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("Python识别正在运行，暂不能删除。"));
    return;
  }
  if (current_diagnosis_dir_.isEmpty()) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("当前没有已创建的诊断。"));
    return;
  }

  const QString dir = current_diagnosis_dir_;
  const int ret = QMessageBox::question(
      this,
      QStringLiteral("删除当前诊断"),
      QStringLiteral("确定删除当前诊断吗？\n%1\n\n此操作会删除其中全部采集图像和识别结果，且不可恢复。").arg(dir),
      QMessageBox::Yes | QMessageBox::No,
      QMessageBox::No);
  if (ret != QMessageBox::Yes) {
    return;
  }

  QDir diagnosis(dir);
  if (!diagnosis.exists()) {
    clearCurrentDiagnosis();
    setRunningUi(running_);
    return;
  }
  if (!diagnosis.removeRecursively()) {
    QMessageBox::warning(this, QStringLiteral("删除失败"),
                         QStringLiteral("无法删除当前诊断目录：\n%1").arg(dir));
    return;
  }

  appendLog(QString("已删除当前诊断: %1").arg(dir));
  if (last_diagnosis_dir_ == dir) {
    last_diagnosis_dir_.clear();
  }
  clearCurrentDiagnosis();
  setRunningUi(running_);
}

void MainWindow::onStartClicked() {
  if (running_) {
    return;
  }
  const bool dataset_mode = kUseDatasetCaptureMode;
  if (!dataset_mode && camera_combo_->count() == 0) {
    appendKeyStatus(QStringLiteral("正在扫描相机"));
    QStringList cameras;
    QString scan_error;
    if (!scanSdkCameras(&cameras, &scan_error)) {
      QMessageBox::warning(this, QStringLiteral("相机未就绪"), scan_error);
      appendKeyStatus(QStringLiteral("相机未就绪"));
      return;
    }
    camera_combo_->clear();
    for (int i = 0; i < cameras.size(); ++i) {
      camera_combo_->addItem(cameras.at(i), i);
    }
    if (camera_combo_->count() == 0) {
      QMessageBox::warning(this, QStringLiteral("相机未就绪"),
                           QStringLiteral("未发现 OCT 相机，请检查 USB3 连接。"));
      appendKeyStatus(QStringLiteral("相机未就绪"));
      return;
    }
    camera_combo_->setCurrentIndex(0);
    appendLog(QStringLiteral("开始采集前自动扫描到 %1 台相机，已选择第一台。")
                  .arg(camera_combo_->count()));
  }

  CaptureSettings settings;
  settings.camera_index = dataset_mode ? 0 : camera_combo_->currentData().toInt();
  settings.capture_root_dir = output_dir_edit_->text().trimmed();
  settings.file_prefix = prefix_edit_->text().trimmed();
  settings.frame_lines = frame_lines_spin_->value();
  settings.staging_lines = staging_lines_spin_->value();
  settings.sdk_buffers = sdk_buffers_spin_->value();
  settings.getbuffer_timeout_ms = timeout_spin_->value();
  settings.external_line_period_us =
      (external_trigger_freq_spin_ && external_trigger_freq_spin_->value() > 0.0)
          ? (1000000.0 / external_trigger_freq_spin_->value())
          : line_period_spin_->value();
  settings.external_line_frequency_hz =
      settings.external_line_period_us > 0.0 ? 1000000.0 / settings.external_line_period_us : 0.0;
  settings.trigger_threshold_code = trigger_threshold_spin_
      ? trigger_threshold_spin_->value()
      : kDefaultTriggerThresholdCode;
  settings.trigger_pulse_count = trigger_pulse_count_spin_
      ? trigger_pulse_count_spin_->value()
      : kDefaultTriggerPulseCount;
  settings.fpga_proc_ctrl = fpgaProcCtrlFromUi();
  if (line_period_spin_) {
    line_period_spin_->setValue(settings.external_line_period_us);
  }
  settings.exposure_us = exposure_spin_->value();
  settings.save_raw = findChild<QCheckBox *>("save_raw")->isChecked();
  settings.save_pgm = findChild<QCheckBox *>("save_pgm")->isChecked();
  settings.continuous = continuous_capture_cb_->isChecked();
  settings.use_dataset_source = dataset_mode;
	  if (dataset_mode) {
	    settings.dataset_root_dir =
	        QDir(QDir::homePath()).filePath(
            QStringLiteral("Desktop/input_preview/test"));
	    settings.dataset_preview_frames =
	        auto_dataset_test_mode_ ? kDatasetAutoTestFrames : kDatasetAutoStopFrames;
    settings.dataset_frame_interval_ms = kDatasetDisplayIntervalMs;
    settings.save_raw = false;
    settings.save_pgm = false;
  }
  savePanelSettings();

  const double exposure_limit_us = settings.external_line_period_us - kExposureDeadTimeUs;
  if (exposure_limit_us <= 0.0) {
    QMessageBox::warning(this, QStringLiteral("提示"),
                         QStringLiteral("单线成像时间太短，必须大于 0.7 us。"));
    return;
  }
  if (settings.exposure_us > exposure_limit_us) {
    QMessageBox::warning(
        this,
        QStringLiteral("参数不合法"),
        QStringLiteral("曝光时间 %1 us 大于当前单线成像时间允许的上限 %2 us。\n\n"
                       "面板不会自动修改该参数，请手动调整曝光时间或单线成像时间。")
            .arg(settings.exposure_us, 0, 'f', 2)
            .arg(exposure_limit_us, 0, 'f', 2));
    return;
  }

  if (settings.capture_root_dir.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("保存根目录不能为空。"));
    return;
  }
  if (settings.file_prefix.isEmpty()) {
    settings.file_prefix = QStringLiteral("oct");
  }
  if (!dataset_mode && !settings.save_raw && !settings.save_pgm) {
    QMessageBox::warning(this, QStringLiteral("提示"),
                         QStringLiteral("RAW 和 PGM 至少选择一种保存格式。"));
    return;
  }

  QString fpga_ready_error;
  if (!ensureFpgaPcieReady(&fpga_ready_error)) {
    QMessageBox::warning(this, QStringLiteral("FPGA not ready"),
                         QStringLiteral("FPGA PCIe/XDMA link is not ready: %1")
                             .arg(fpga_ready_error));
    appendKeyStatus(QStringLiteral("FPGA not ready"));
    return;
  }

  continuous_capture_requested_ = continuous_capture_cb_->isChecked();
  appendLog(QString("Using FPGA PROC_CTRL: %1").arg(fpgaProcCtrlSummary(settings.fpga_proc_ctrl)));

  if (!ensureDiagnosis(settings.capture_root_dir, settings.file_prefix)) {
    QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("无法创建诊断目录。"));
    return;
  }

  const QString capture_dir = QDir(current_visit_dir_).filePath(QStringLiteral("captures"));
  QDir capture_qdir(capture_dir);
  if (!capture_qdir.exists() && !capture_qdir.mkpath(".")) {
    QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("无法创建本次采集目录。"));
    return;
  }

  int next_capture_index = visit_capture_index_ + 1;
  if (!dataset_mode) {
    const int legacy_dir_index = maxNumberedChildIndex(current_visit_dir_, QStringLiteral("capture_"));
    const int file_index = maxCaptureFileIndex(capture_dir);
    next_capture_index = qMax(qMax(visit_capture_index_, legacy_dir_index), file_index) + 1;
  }
  const QString capture_id =
      QStringLiteral("capture_%1").arg(next_capture_index, 3, 10, QChar('0'));
  settings.output_dir = capture_dir;
  settings.session_id = current_diagnosis_id_;
  settings.visit_id = current_visit_id_;
  settings.capture_id = capture_id;
  settings.capture_index_start = next_capture_index;
  visit_capture_index_ = next_capture_index - 1;
  active_capture_start_index_ = next_capture_index;

  active_capture_dir_ = settings.output_dir;
  resetLiveViews();
  if (!dataset_mode && !clearMotorCounters(QStringLiteral("采集开始前"))) {
    appendLog(QStringLiteral("Zynq 清零失败，采集仍可继续等待相机，但电机/外触发联动不会启动。"));
  } else if (dataset_mode) {
    appendLog(QStringLiteral("数据集模拟采集：跳过Zynq串口清零，避免界面等待串口响应。"));
  }
  motor_running_ = false;
  motor_position_ = 0.0;
  motor_position_b_ = 0.0;
  motor_position_history_.clear();
  motor_position_b_history_.clear();
  updateMotorPlot(false);
  if (motor_state_label_) {
    motor_state_label_->setText(QStringLiteral("等待相机进入采集"));
  }
  if (missed_trigger_label_) {
    missed_trigger_label_->setText(QStringLiteral("本次采集：漏触发 0，丢线 0"));
    missed_trigger_label_->setStyleSheet(QStringLiteral("color:#1f7a5a;"));
  }
  appendLog(QString("Starting capture worker. Diagnosis: %1, Visit: %2, Start capture: %3")
                .arg(settings.session_id, settings.visit_id, settings.capture_id));
  appendLog(QString("Capture output folder: %1").arg(settings.output_dir));
  appendLog(QString("外触发周期参考(模式2单线时间): %1 us, 预计单帧时间: %2 ms")
                .arg(settings.external_line_period_us, 0, 'f', 2)
                .arg(settings.external_line_period_us *
                         static_cast<double>(settings.frame_lines) / 1000.0,
                     0, 'f', 2));
  if (settings.continuous) {
    appendLog(QString("连续采集已启用：相机保持打开，每组 %1 个触发保存一张图。")
                  .arg(settings.frame_lines));
  }
  appendKeyStatus(QStringLiteral("正在采集"));
  setRunningUi(true);

  worker_thread_ = new QThread(this);
  worker_ = new CaptureWorker(settings);
  worker_->moveToThread(worker_thread_);

  connect(worker_thread_, &QThread::started, worker_, &CaptureWorker::process);
  connect(worker_, &CaptureWorker::logMessage, this, &MainWindow::onWorkerLog);
  connect(worker_, &CaptureWorker::frameReady, this, &MainWindow::onFrameReady,
          Qt::BlockingQueuedConnection);
  connect(worker_, &CaptureWorker::captureStats, this, &MainWindow::onCaptureStats);
  connect(worker_, &CaptureWorker::finished, this, &MainWindow::onWorkerFinished);

  connect(worker_, &CaptureWorker::finished, worker_thread_, &QThread::quit);
  connect(worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(worker_thread_, &QThread::finished, worker_thread_, &QObject::deleteLater);

  worker_thread_->start();
}

void MainWindow::onStopClicked() {
  if (!running_ || !worker_) {
    continuous_capture_requested_ = false;
    return;
  }
  appendLog("Stop requested...");
  appendKeyStatus(QStringLiteral("正在停止"));
  continuous_capture_requested_ = false;
  stopMotorHardware(QStringLiteral("采集停止"));
  // requestStop() only flips an atomic flag, so direct call is thread-safe
  // and more responsive than queued delivery when worker thread is busy.
  worker_->requestStop();
  stop_btn_->setEnabled(false);
  stop_btn_->setText(QStringLiteral("停止中..."));
}

void MainWindow::onWorkerLog(const QString &msg) {
  appendLog(msg);
  if (msg.contains(QStringLiteral("Acquisition started"), Qt::CaseInsensitive) &&
      running_ && !motor_running_ && !kUseDatasetCaptureMode) {
    startMotorHardware(QStringLiteral("Zynq外触发与电机同步启动"));
    appendKeyStatus(QStringLiteral("Zynq电机/外触发已联动启动"));
  }
  const QString key = keyStatusForLog(msg);
  if (!key.isEmpty()) {
    appendKeyStatus(key);
  }
}

void MainWindow::updateLiveImage(QLabel *label, const QImage &image) {
  if (!label || image.isNull()) {
    return;
  }
  const QSize target = label->contentsRect().size();
  if (target.isEmpty()) {
    return;
  }
  const QImage display_image =
      image.transformed(QTransform().rotate(90), Qt::FastTransformation);
  const QImage scaled = display_image.scaled(target, Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation);
  QPixmap canvas(target);
  canvas.fill(QColor("#111827"));
  QPainter painter(&canvas);
  const QPoint top_left((target.width() - scaled.width()) / 2,
                        (target.height() - scaled.height()) / 2);
  painter.drawImage(top_left, scaled);
  label->setPixmap(canvas);
}

void MainWindow::refreshLiveImages() {
  updateLiveImage(raw_image_label_, last_raw_preview_);
  updateLiveImage(processed_image_label_, last_processed_preview_);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
  QMainWindow::resizeEvent(event);
  refreshLiveImages();
}

void MainWindow::onFrameReady(const QImage &raw_preview,
                              const QImage &processed_preview,
                              int frame_index,
                              double roundtrip_ms,
                              double line_ms) {
  last_raw_preview_ = raw_preview;
  last_processed_preview_ = processed_preview;
  refreshLiveImages();

  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  const bool should_update_latency =
      last_latency_update_ms_ == 0 || now_ms - last_latency_update_ms_ >= 200;
  auto smooth_value = [](double previous, double current) {
    if (current < 0.0) {
      return previous;
    }
    if (previous < 0.0) {
      return current;
    }
    return previous * 0.75 + current * 0.25;
  };
  if (should_update_latency) {
    bool changed = false;
    const double next_roundtrip = smooth_value(displayed_roundtrip_ms_, roundtrip_ms);
    if (next_roundtrip >= 0.0 && next_roundtrip != displayed_roundtrip_ms_) {
      displayed_roundtrip_ms_ = next_roundtrip;
      changed = true;
    }
    const double next_line = smooth_value(displayed_line_ms_, line_ms);
    if (next_line >= 0.0 && next_line != displayed_line_ms_) {
      displayed_line_ms_ = next_line;
      changed = true;
    }
    if (changed) {
      last_latency_update_ms_ = now_ms;
    }
    if (raw_latency_label_ && displayed_roundtrip_ms_ >= 0.0) {
      const QString text =
          QStringLiteral("%1 ms").arg(displayed_roundtrip_ms_, 7, 'f', 2);
      if (raw_latency_label_->text() != text) {
        raw_latency_label_->setText(text);
      }
    }
    if (processed_latency_label_ && displayed_line_ms_ >= 0.0) {
      const QString text = QStringLiteral("%1 ms").arg(displayed_line_ms_, 8, 'f', 4);
      if (processed_latency_label_->text() != text) {
        processed_latency_label_->setText(text);
      }
    }
  }
  if (frame_index == 1 && !motor_running_) {
    appendLog(QStringLiteral("Warning: first frame arrived before FPGA motor/trigger link was marked running."));
  }
  appendKeyStatus(QStringLiteral("第 %1 张图像").arg(frame_index));
}

void MainWindow::onCaptureStats(qulonglong missed_triggers,
                                qulonglong line_lost,
                                int saved_frames) {
  if (!missed_trigger_label_) {
    return;
  }

  missed_trigger_label_->setText(
      QStringLiteral("已保存 %1 张；本张漏触发 %2，丢线 %3")
          .arg(saved_frames)
          .arg(missed_triggers)
          .arg(line_lost));
  missed_trigger_label_->setStyleSheet(
      (missed_triggers == 0 && line_lost == 0)
          ? QStringLiteral("color:#1f7a5a;")
          : QStringLiteral("color:#9f2d2d; font-weight:bold;"));
}

void MainWindow::saveAutoTestScreenshot(const QString &stem) {
  if (!auto_dataset_test_mode_) {
    return;
  }
  QDir dir(auto_test_output_dir_.isEmpty() ? QDir::tempPath() : auto_test_output_dir_);
  if (!dir.exists()) {
    dir.mkpath(QStringLiteral("."));
  }
  const QString path = dir.filePath(stem + QStringLiteral(".png"));
  show();
  raise();
  activateWindow();
  grab().save(path);
  appendLog(QString("Auto test screenshot: %1").arg(path));
}

void MainWindow::writeAutoTestResult(const QString &status, int analysis_exit_code) {
  if (!auto_dataset_test_mode_) {
    return;
  }
  QDir dir(auto_test_output_dir_.isEmpty() ? QDir::tempPath() : auto_test_output_dir_);
  if (!dir.exists()) {
    dir.mkpath(QStringLiteral("."));
  }
  QFile file(dir.filePath(QStringLiteral("auto_dataset_test_result.txt")));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    return;
  }
  QTextStream ts(&file);
  ts << "status=" << status << "\n";
  ts << "updated_at=" << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
  ts << "diagnosis_dir=" << current_diagnosis_dir_ << "\n";
  ts << "visit_dir=" << current_visit_dir_ << "\n";
  ts << "analysis_dir=" << QDir(current_diagnosis_dir_).filePath(QStringLiteral("analysis")) << "\n";
  ts << "analysis_exit_code=" << analysis_exit_code << "\n";
}

void MainWindow::onWorkerFinished(bool ok, const QString &message) {
  const bool stopped_by_user =
      message.contains("stopped by user", Qt::CaseInsensitive) ||
      message.contains(QStringLiteral("停止"), Qt::CaseInsensitive);
  if (stopped_by_user) {
    ok = false;
    last_diagnosis_dir_ = current_diagnosis_dir_;
  }
  if (ok) {
    appendLog(QString("Capture done: %1").arg(message));
    appendKeyStatus(QStringLiteral("采集完成"));
    last_diagnosis_dir_ = current_diagnosis_dir_;
    appendLog(QString("Current diagnosis directory: %1").arg(current_diagnosis_dir_));
    appendLog(QString("Current visit directory: %1").arg(current_visit_dir_));
  } else if (stopped_by_user) {
    appendLog(QString("Capture stopped: %1").arg(message));
    appendKeyStatus(QStringLiteral("采集已停止"));
  } else {
    appendLog(QString("Capture failed: %1").arg(message));
    appendKeyStatus(QStringLiteral("采集异常"));
  }
  setRunningUi(false);
  stop_btn_->setText(QStringLiteral("停止"));

  if (motor_running_) {
    stopMotorHardware(QStringLiteral("采集结束"));
  }

  worker_ = nullptr;
  worker_thread_ = nullptr;
  active_capture_dir_.clear();

  if (active_capture_start_index_ > 0) {
    const QRegularExpression saved_re(QStringLiteral("saved (\\d+) frame"));
    const QRegularExpressionMatch saved_match = saved_re.match(message);
    if (saved_match.hasMatch()) {
      visit_capture_index_ = qMax(visit_capture_index_,
                                  active_capture_start_index_ + saved_match.captured(1).toInt() - 1);
    } else if (ok) {
      visit_capture_index_ = qMax(visit_capture_index_, active_capture_start_index_);
    }
  }
  active_capture_start_index_ = 0;

  if (auto_dataset_test_mode_) {
    saveAutoTestScreenshot(ok ? QStringLiteral("capture_panel_after_capture")
                              : QStringLiteral("capture_panel_capture_failed"));
    writeAutoTestResult(ok ? QStringLiteral("capture_done") : QStringLiteral("capture_failed"));
    if (ok) {
      startPythonAnalysis(current_diagnosis_dir_, false);
    } else {
      QTimer::singleShot(500, qApp, &QApplication::quit);
    }
    return;
  }

  if (!ok && !stopped_by_user) {
    QMessageBox::warning(this, QStringLiteral("采集失败"), message);
  }

  setRunningUi(false);
  const QString current_capture_dir =
      QDir(current_visit_dir_).filePath(QStringLiteral("captures"));
  if ((ok || stopped_by_user) && processedFrameCount(current_capture_dir) > 0) {
    startPythonAnalysis(current_diagnosis_dir_, false);
  }
}

void MainWindow::onAnalyzeLastClicked() {
  const QString diagnosis_dir =
      !current_diagnosis_dir_.isEmpty() ? current_diagnosis_dir_ : last_diagnosis_dir_;
  if (diagnosis_dir.isEmpty()) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("还没有诊断目录。"));
    return;
  }
  if (analysis_process_) {
    analysis_open_when_ready_ = true;
    appendKeyStatus(QStringLiteral("正在分析识别"));
    updateStatusLabel();
    return;
  }
  if (openAnalysisVideo(diagnosis_dir)) {
    return;
  }
  startPythonAnalysis(diagnosis_dir, true);
}

void MainWindow::onOpenLastSessionClicked() {
  const QString diagnosis_dir =
      !current_diagnosis_dir_.isEmpty() ? current_diagnosis_dir_ : last_diagnosis_dir_;
  if (diagnosis_dir.isEmpty()) {
    return;
  }
  QDesktopServices::openUrl(QUrl::fromLocalFile(diagnosis_dir));
}

QString MainWindow::latestAnalysisInputDir(const QString &diagnosis_dir) const {
  const QDir diagnosis(diagnosis_dir);
  if (!diagnosis.exists()) {
    return QString();
  }

  QString latest_capture_dir;
  QDateTime latest_modified;
  const QFileInfoList visits = diagnosis.entryInfoList(
      QStringList() << QStringLiteral("visit_*"), QDir::Dirs | QDir::NoDotAndDotDot,
      QDir::Time);
  for (const QFileInfo &visit : visits) {
    const QString capture_dir = QDir(visit.absoluteFilePath()).filePath(QStringLiteral("captures"));
    if (processedFrameCount(capture_dir) == 0) {
      continue;
    }
    const QFileInfo processed_dir(
        QDir(capture_dir).filePath(QStringLiteral("fpga_processed_images")));
    if (latest_capture_dir.isEmpty() || processed_dir.lastModified() > latest_modified) {
      latest_capture_dir = capture_dir;
      latest_modified = processed_dir.lastModified();
    }
  }

  if (!latest_capture_dir.isEmpty()) {
    return latest_capture_dir;
  }
  return processedFrameCount(diagnosis_dir) > 0 ? diagnosis_dir : QString();
}

bool MainWindow::analysisCacheMatchesLatestInput(const QString &diagnosis_dir) const {
  const QString input_dir = latestAnalysisInputDir(diagnosis_dir);
  const int input_frames = processedFrameCount(input_dir);
  if (input_dir.isEmpty() || input_frames <= 0) {
    return false;
  }

  QFile summary(QDir(diagnosis_dir).filePath(
      QStringLiteral("analysis/analysis_summary.json")));
  if (!summary.open(QIODevice::ReadOnly)) {
    return false;
  }
  const QJsonDocument document = QJsonDocument::fromJson(summary.readAll());
  if (!document.isObject()) {
    return false;
  }
  const QJsonObject object = document.object();
  const QString cached_input = QDir::cleanPath(object.value(QStringLiteral("input_dir")).toString());
  const int cached_frames = object.value(QStringLiteral("records")).toInt(-1);
  return cached_input == QDir::cleanPath(input_dir) && cached_frames == input_frames;
}

QString MainWindow::findAnalysisVideo(const QString &diagnosis_dir) const {
  if (diagnosis_dir.isEmpty()) {
    return QString();
  }
  if (!analysisCacheMatchesLatestInput(diagnosis_dir)) {
    return QString();
  }
  const QDir analysis_dir(QDir(diagnosis_dir).filePath(QStringLiteral("analysis")));
  if (!analysis_dir.exists()) {
    return QString();
  }

  for (const QString &name : analysisVideoFileNames()) {
    const QString path = analysis_dir.filePath(name);
    if (QFileInfo(path).isFile() && QFileInfo(path).size() > 0) {
      return path;
    }
  }
  return QString();
}

bool MainWindow::openAnalysisVideo(const QString &diagnosis_dir) {
  const QString video_path = findAnalysisVideo(diagnosis_dir);
  if (video_path.isEmpty()) {
    return false;
  }

  appendLog(QString("Opening analysis video: %1").arg(video_path));
  QProcess::execute(QStringLiteral("pkill"),
                    QStringList() << QStringLiteral("-x") << QStringLiteral("mpv"));
  QProcess::execute(QStringLiteral("pkill"),
                    QStringList() << QStringLiteral("-x") << QStringLiteral("ffplay"));
  QString player = QStringLiteral("/usr/bin/mpv");
  QStringList args;
  if (QFileInfo(player).isFile()) {
    args << QStringLiteral("--force-window=yes")
         << QStringLiteral("--keep-open=yes")
         << QStringLiteral("--loop-file=no")
         << QStringLiteral("--fps=10")
         << QStringLiteral("--speed=1.0")
         << QStringLiteral("--cache=yes")
         << QStringLiteral("--demuxer-readahead-secs=8")
         << QStringLiteral("--vd-lavc-threads=4")
         << video_path;
  } else if (QFileInfo(QStringLiteral("/usr/bin/ffplay")).isFile()) {
    player = QStringLiteral("/usr/bin/ffplay");
    args << QStringLiteral("-loop") << QStringLiteral("0") << video_path;
  } else {
    QDesktopServices::openUrl(QUrl::fromLocalFile(video_path));
    appendKeyStatus(QStringLiteral("已打开识别视频"));
    return true;
  }

  QProcess::startDetached(player, args);
  appendKeyStatus(QStringLiteral("已打开识别视频"));
  return true;
}

bool MainWindow::startPythonAnalysis(const QString &diagnosis_dir, bool open_when_finished) {
  if (diagnosis_dir.isEmpty()) {
    if (open_when_finished) {
      QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("诊断目录为空。"));
    }
    return false;
  }
  if (analysis_process_) {
    if (open_when_finished) {
      analysis_open_when_ready_ = true;
      appendKeyStatus(QStringLiteral("正在分析识别"));
      updateStatusLabel();
    }
    return true;
  }

  const QString python_dir = python_dir_edit_->text().trimmed();
  const QString script_path = QDir(python_dir).filePath(QStringLiteral("predict_batch.py"));
  if (!QFileInfo(script_path).isFile()) {
    appendLog(QString("Python analysis script not found: %1").arg(script_path));
    if (open_when_finished) {
      QMessageBox::warning(
          this,
          QStringLiteral("找不到Python脚本"),
          QStringLiteral("请确认Python识别目录正确，并包含 predict_batch.py：\n%1").arg(script_path));
    } else {
      appendKeyStatus(QStringLiteral("识别脚本未找到"));
    }
    return false;
  }

  QDir diagnosis(diagnosis_dir);
  if (!diagnosis.exists()) {
    if (open_when_finished) {
      QMessageBox::warning(this, QStringLiteral("目录不存在"),
                           QStringLiteral("诊断目录不存在：\n%1").arg(diagnosis_dir));
    }
    return false;
  }

  const QString input_dir = latestAnalysisInputDir(diagnosis_dir);
  const int input_frames = processedFrameCount(input_dir);
  if (input_frames <= 0) {
    appendLog(QString("Python analysis blocked: no FPGA processed images under %1")
                  .arg(diagnosis_dir));
    if (open_when_finished) {
      QMessageBox::warning(
          this,
          QStringLiteral("No FPGA processed images"),
          QStringLiteral("No fpga_processed_images were found in this diagnosis. "
                         "Please run acquisition again after FPGA/XDMA is ready."));
    } else {
      appendKeyStatus(QStringLiteral("未找到FPGA处理图像"));
    }
    return false;
  }

  const QString analysis_dir = diagnosis.filePath(QStringLiteral("analysis"));
  QDir().mkpath(analysis_dir);

  const QString output_csv = QDir(analysis_dir).filePath(QStringLiteral("batch_predict_result.csv"));
  const QString output_jsonl = QDir(analysis_dir).filePath(QStringLiteral("batch_predict_result.jsonl"));
  for (const QString &name : analysisVideoFileNames()) {
    QFile::remove(QDir(analysis_dir).filePath(name));
  }
  QFile::remove(output_csv);
  QFile::remove(output_jsonl);

  analysis_process_ = new QProcess(this);
  analysis_target_dir_ = diagnosis_dir;
  analysis_open_when_ready_ = open_when_finished;
  analysis_process_->setWorkingDirectory(python_dir);
  analysis_process_->setProcessChannelMode(QProcess::MergedChannels);

  connect(analysis_process_, &QProcess::readyReadStandardOutput,
          this, &MainWindow::onAnalysisReadyRead);
  connect(analysis_process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, &MainWindow::onAnalysisFinished);

  QStringList args;
  args << script_path
       << QStringLiteral("--input-dir") << input_dir
       << QStringLiteral("--output-csv") << output_csv
       << QStringLiteral("--output-jsonl") << output_jsonl
       << QStringLiteral("--display") << QStringLiteral("never")
       << QStringLiteral("--require-fpga-processed")
       << QStringLiteral("--recursive");
  if (kDatasetAnalysisMaxFrames > 0) {
    args << QStringLiteral("--max-frames") << QString::number(kDatasetAnalysisMaxFrames);
  }

  appendLog(QString("Starting Python analysis: python3 %1").arg(args.join(' ')));
  appendKeyStatus(QStringLiteral("正在分析识别"));
  updateStatusLabel();
  analysis_process_->start(QStringLiteral("python3"), args);
  if (!analysis_process_->waitForStarted(3000)) {
    const QString err = analysis_process_->errorString();
    analysis_process_->deleteLater();
    analysis_process_ = nullptr;
    analysis_target_dir_.clear();
    analysis_open_when_ready_ = false;
    setRunningUi(running_);
    if (open_when_finished) {
      QMessageBox::warning(this, QStringLiteral("Python启动失败"), err);
    } else {
      appendKeyStatus(QStringLiteral("分析识别启动失败"));
    }
    appendLog(QString("Python analysis failed to start: %1").arg(err));
    return false;
  }
  setRunningUi(running_);
  return true;
}

void MainWindow::onAnalysisReadyRead() {
  if (!analysis_process_) {
    return;
  }
  const QString text = QString::fromUtf8(analysis_process_->readAllStandardOutput()).trimmed();
  if (!text.isEmpty()) {
    appendLog(QString("Python: %1").arg(text));
  }
}

void MainWindow::onAnalysisFinished(int exit_code, QProcess::ExitStatus exit_status) {
  Q_UNUSED(exit_status);
  const bool open_when_ready = analysis_open_when_ready_;
  analysis_open_when_ready_ = false;
  if (analysis_process_) {
    const QString text = QString::fromUtf8(analysis_process_->readAllStandardOutput()).trimmed();
    if (!text.isEmpty()) {
      appendLog(QString("Python: %1").arg(text));
    }
    analysis_process_->deleteLater();
    analysis_process_ = nullptr;
  }

  if (auto_dataset_test_mode_) {
    appendLog(exit_code == 0
                  ? QString("Python analysis complete. Results: %1")
                        .arg(QDir(analysis_target_dir_).filePath(QStringLiteral("analysis")))
                  : QString("Python analysis failed, exit code=%1").arg(exit_code));
    saveAutoTestScreenshot(exit_code == 0
                               ? QStringLiteral("capture_panel_after_analysis")
                               : QStringLiteral("capture_panel_analysis_failed"));
    writeAutoTestResult(exit_code == 0 ? QStringLiteral("analysis_done")
                                       : QStringLiteral("analysis_failed"),
                        exit_code);
    analysis_target_dir_.clear();
    setRunningUi(running_);
    QTimer::singleShot(500, qApp, &QApplication::quit);
    return;
  }

  if (exit_code == 0) {
    appendLog(QString("Python analysis complete. Results: %1")
                  .arg(QDir(analysis_target_dir_).filePath(QStringLiteral("analysis"))));
    const QString target_dir = analysis_target_dir_;
    analysis_target_dir_.clear();
    setRunningUi(running_);
    appendKeyStatus(QStringLiteral("分析识别完成"));
    if (open_when_ready) {
      appendKeyStatus(QStringLiteral("识别完成，正在打开视频流"));
      QTimer::singleShot(700, this, [this, target_dir]() {
        if (!openAnalysisVideo(target_dir)) {
          appendLog(QString("Analysis finished but YOLO video was not found under %1")
                        .arg(QDir(target_dir).filePath(QStringLiteral("analysis"))));
          appendKeyStatus(QStringLiteral("识别完成，未找到视频流"));
        }
      });
    }
    return;
  } else {
    appendLog(QString("Python analysis failed, exit code=%1").arg(exit_code));
    appendKeyStatus(QStringLiteral("分析识别失败"));
    if (open_when_ready) {
      QMessageBox::warning(this, QStringLiteral("识别失败"),
                           QStringLiteral("Python识别失败，请查看日志。"));
    }
  }
  analysis_target_dir_.clear();
  setRunningUi(running_);
}
