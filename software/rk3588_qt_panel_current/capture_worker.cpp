#include "capture_worker.h"

#include <CamCmosOctUsb3.h>

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStringList>
#include <QTextStream>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kRegSynchroMode = 0x1210C;
constexpr uint32_t kRegLinePeriodMin = 0x12104;
constexpr uint32_t kRegExposureTime = 0x12108;
constexpr uint32_t kRegExposureTimeMin = 0x12114;
constexpr uint32_t kRegExposureTimeMax = 0x12118;
constexpr uint32_t kRegMissedTriggerReset = 0x12110;
constexpr uint32_t kRegGpiFormat = 0x12120;
constexpr uint32_t kRegGpiState = 0x12124;
constexpr uint32_t kRegLineCounterReset = 0x12288;
constexpr uint32_t kSynchroModeExtLineTimed = 2;
constexpr uint32_t kGpiFormatTtlSingleEnded = 0;
constexpr int kCameraScanMaxAttempts = 8;
constexpr int kCameraScanRetryDelayMs = 250;
constexpr uint32_t kStopPollTimeoutMs = 100;
constexpr double kExposureDeadTimeUs = 0.70;
constexpr size_t kFpgaBarMapSize = 0x10000;
constexpr off_t kFpgaRegCtrl = 0x00;
constexpr off_t kFpgaRegVersion = 0x0C;
constexpr off_t kFpgaRegLineCoreCycles = 0x30;
constexpr off_t kFpgaRegUserClkHz = 0x2C;
constexpr off_t kFpgaRegProcCtrl = 0x3C;
constexpr uint32_t kExpectedFpgaVersion = 0x20260619;
constexpr int kXdmaFrameTimeoutMs = 5000;
constexpr size_t kDatasetRawWidth = 2048;
constexpr size_t kDatasetRawHeight = 4096;
constexpr size_t kDatasetAverageRows = 4;
constexpr size_t kDatasetProcessedHeight = kDatasetRawHeight / kDatasetAverageRows;
constexpr size_t kDatasetRealtimePreviewHeight = kDatasetRawHeight;
constexpr uint16_t kDatasetAdcMask = 0x0FFF;
constexpr int kDatasetLogEveryFrames = 30;
constexpr int kDatasetAnalysisCacheFrames = 0;

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

class FpgaBar {
 public:
  explicit FpgaBar(QString *error) {
    path_ = findFpgaResource0(error);
    if (path_.isEmpty()) {
      return;
    }
    const QByteArray path_bytes = path_.toLocal8Bit();
    fd_ = ::open(path_bytes.constData(), O_RDWR | O_SYNC);
    if (fd_ < 0) {
      if (error) {
        *error = QStringLiteral("打开 FPGA resource0 失败: %1 (%2)")
                     .arg(path_)
                     .arg(QString::fromLocal8Bit(std::strerror(errno)));
      }
      return;
    }
    void *mapped = ::mmap(nullptr, kFpgaBarMapSize, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
      if (error) {
        *error = QStringLiteral("映射 FPGA resource0 失败: %1")
                     .arg(QString::fromLocal8Bit(std::strerror(errno)));
      }
      ::close(fd_);
      fd_ = -1;
      return;
    }
    base_ = static_cast<uint8_t *>(mapped);
  }

  ~FpgaBar() {
    if (base_) {
      ::munmap(base_, kFpgaBarMapSize);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  bool ok() const { return base_ != nullptr; }

  uint32_t read32(off_t offset) const {
    return *reinterpret_cast<volatile uint32_t *>(base_ + offset);
  }

  void write32(off_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base_ + offset) = value;
    const uintptr_t page_offset = static_cast<uintptr_t>(offset) & ~static_cast<uintptr_t>(0xFFF);
    ::msync(base_ + page_offset, 0x1000, MS_SYNC);
  }

 private:
  QString path_;
  int fd_ = -1;
  uint8_t *base_ = nullptr;
};

double cyclesToMs(uint32_t cycles, uint32_t hz) {
  if (cycles == 0 || hz == 0) {
    return -1.0;
  }
  return static_cast<double>(cycles) * 1000.0 / static_cast<double>(hz);
}

QString sdkErrorText(int err) {
  char err_buf[512] = {0};
  size_t err_size = sizeof(err_buf);
  if (USB3_GetErrorText(err, err_buf, &err_size) == CAM_ERR_SUCCESS) {
    return QString::fromUtf8(err_buf);
  }
  return QStringLiteral("Unknown SDK error");
}

void throwIfError(const char *api, int err) {
  if (err == CAM_ERR_SUCCESS) {
    return;
  }
  throw std::runtime_error(QString("%1 failed, err=%2 (%3)")
                               .arg(api)
                               .arg(err)
                               .arg(sdkErrorText(err))
                               .toStdString());
}

void writeRegU32(CAM_HANDLE camera, uint32_t addr, uint32_t value) {
  size_t size = sizeof(value);
  throwIfError("USB3_WriteRegister",
               USB3_WriteRegister(camera, addr, &value, &size));
}

uint32_t readRegU32(CAM_HANDLE camera, uint32_t addr) {
  uint32_t value = 0;
  size_t size = sizeof(value);
  throwIfError("USB3_ReadRegister",
               USB3_ReadRegister(camera, addr, &value, &size));
  return value;
}

QString pixelTypeName(tImagePixelType pt) {
  switch (pt) {
    case eMono8:
      return QStringLiteral("Mono8");
    case eMono10:
      return QStringLiteral("Mono10");
    case eMono11:
      return QStringLiteral("Mono11");
    case eMono12:
      return QStringLiteral("Mono12");
    default:
      return QStringLiteral("Unknown");
  }
}

int pixelBytes(tImagePixelType pt) {
  switch (pt) {
    case eMono8:
      return 1;
    case eMono10:
    case eMono11:
    case eMono12:
      return 2;
    default:
      return 0;
  }
}

uint16_t pixelMax(tImagePixelType pt) {
  switch (pt) {
    case eMono8:
      return 255;
    case eMono10:
      return 1023;
    case eMono11:
      return 2047;
    case eMono12:
      return 4095;
    default:
      return 65535;
  }
}

uint16_t readPixelValue(const std::vector<uint8_t> &data,
                        size_t offset,
                        int bytes_per_pixel,
                        uint16_t mask) {
  if (bytes_per_pixel == 1) {
    return data[offset];
  }
  uint16_t value = 0;
  std::memcpy(&value, data.data() + offset, sizeof(value));
  return value & mask;
}

QImage makeGrayscalePreview(const std::vector<uint8_t> &data,
                            size_t width,
                            size_t height,
                            size_t line_pitch,
                            int bytes_per_pixel,
                            uint16_t mask,
                            int max_w = 1024,
                            int max_h = 900) {
  if (data.empty() || width == 0 || height == 0 || bytes_per_pixel <= 0) {
    return QImage();
  }

  const int out_w = static_cast<int>(std::min(width, static_cast<size_t>(max_w)));
  const int out_h = static_cast<int>(std::min(height, static_cast<size_t>(max_h)));
  QImage image(out_w, out_h, QImage::Format_Grayscale8);
  if (image.isNull()) {
    return image;
  }

  std::vector<uint32_t> histogram(static_cast<size_t>(mask) + 1u, 0u);
  size_t sample_count = 0;
  for (int y = 0; y < out_h; ++y) {
    const size_t src_y = static_cast<size_t>(y) * height / static_cast<size_t>(out_h);
    const size_t row_base = src_y * line_pitch;
    for (int x = 0; x < out_w; ++x) {
      const size_t src_x = static_cast<size_t>(x) * width / static_cast<size_t>(out_w);
      const size_t off = row_base + src_x * static_cast<size_t>(bytes_per_pixel);
      if (off + static_cast<size_t>(bytes_per_pixel) > data.size()) {
        continue;
      }
      const uint16_t v = readPixelValue(data, off, bytes_per_pixel, mask);
      ++histogram[v];
      ++sample_count;
    }
  }
  if (sample_count == 0) {
    return image;
  }

  const size_t low_target = sample_count / 200;  // ~0.5 percentile
  const size_t high_target = sample_count - 1 - sample_count / 200;  // ~99.5 percentile
  size_t cumulative = 0;
  uint16_t min_v = 0;
  uint16_t max_v = mask;
  bool low_found = false;
  for (size_t i = 0; i < histogram.size(); ++i) {
    cumulative += histogram[i];
    if (!low_found && cumulative > low_target) {
      min_v = static_cast<uint16_t>(i);
      low_found = true;
    }
    if (cumulative > high_target) {
      max_v = static_cast<uint16_t>(i);
      break;
    }
  }
  if (max_v <= min_v) {
    max_v = static_cast<uint16_t>(min_v + 1);
  }

  const double scale = 255.0 / static_cast<double>(max_v - min_v);
  for (int y = 0; y < out_h; ++y) {
    uchar *dst = image.scanLine(y);
    const size_t src_y = static_cast<size_t>(y) * height / static_cast<size_t>(out_h);
    const size_t row_base = src_y * line_pitch;
    for (int x = 0; x < out_w; ++x) {
      const size_t src_x = static_cast<size_t>(x) * width / static_cast<size_t>(out_w);
      const size_t off = row_base + src_x * static_cast<size_t>(bytes_per_pixel);
      uint16_t v = min_v;
      if (off + static_cast<size_t>(bytes_per_pixel) <= data.size()) {
        v = readPixelValue(data, off, bytes_per_pixel, mask);
      }
      int g = static_cast<int>((static_cast<int>(v) - static_cast<int>(min_v)) * scale + 0.5);
      g = std::max(0, std::min(255, g));
      dst[x] = static_cast<uchar>(g);
    }
  }
  return image;
}

QImage makeFpgaVideoStreamPreview(const std::vector<uint8_t> &data,
                                  size_t width,
                                  size_t height,
                                  size_t line_pitch,
                                  int out_h) {
  if (data.empty() || width == 0 || height == 0 || out_h <= 0) {
    return QImage();
  }

  const int out_w = static_cast<int>(width);
  QImage image(out_w, out_h, QImage::Format_Grayscale8);
  if (image.isNull()) {
    return image;
  }

  std::vector<uint32_t> histogram(65536u, 0u);
  size_t sample_count = 0;
  for (size_t y = 0; y < height; ++y) {
    const size_t row_base = y * line_pitch;
    for (size_t x = 0; x < width; ++x) {
      const size_t off = row_base + x * sizeof(uint16_t);
      if (off + sizeof(uint16_t) > data.size()) {
        continue;
      }
      const uint16_t v = readPixelValue(data, off, 2, 65535);
      ++histogram[v];
      ++sample_count;
    }
  }
  if (sample_count == 0) {
    return image;
  }

  const size_t low_target = sample_count / 200;
  const size_t high_target = sample_count - 1 - sample_count / 200;
  size_t cumulative = 0;
  uint16_t min_v = 0;
  uint16_t max_v = 65535;
  bool low_found = false;
  for (size_t i = 0; i < histogram.size(); ++i) {
    cumulative += histogram[i];
    if (!low_found && cumulative > low_target) {
      min_v = static_cast<uint16_t>(i);
      low_found = true;
    }
    if (cumulative > high_target) {
      max_v = static_cast<uint16_t>(i);
      break;
    }
  }
  if (max_v <= min_v) {
    max_v = static_cast<uint16_t>(min_v + 1);
  }

  const double scale = 255.0 / static_cast<double>(max_v - min_v);
  auto toGray = [&](uint16_t v) {
    int g = static_cast<int>((static_cast<int>(v) - static_cast<int>(min_v)) * scale + 0.5);
    return std::max(0, std::min(255, g));
  };

  for (int y = 0; y < out_h; ++y) {
    uchar *dst = image.scanLine(y);
    const size_t y0 = static_cast<size_t>(y) * height / static_cast<size_t>(out_h);
    const size_t y1 = std::max(y0 + 1, (static_cast<size_t>(y) + 1) * height / static_cast<size_t>(out_h));
    const size_t rows = y1 - y0;
    for (size_t x = 0; x < width; ++x) {
      uint32_t sum = 0;
      for (size_t yy = y0; yy < y1; ++yy) {
        const size_t off = yy * line_pitch + x * sizeof(uint16_t);
        uint16_t v = min_v;
        if (off + sizeof(uint16_t) <= data.size()) {
          v = readPixelValue(data, off, 2, 65535);
        }
        sum += static_cast<uint32_t>(toGray(v));
      }
      dst[static_cast<int>(x)] = static_cast<uchar>((sum + rows / 2) / rows);
    }
  }
  return image;
}

QByteArray readPgmToken(QFile &file) {
  QByteArray token;
  char ch = 0;
  while (file.getChar(&ch)) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isspace(c)) {
      continue;
    }
    if (ch == '#') {
      file.readLine();
      continue;
    }
    token.append(ch);
    break;
  }
  while (file.getChar(&ch)) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isspace(c)) {
      break;
    }
    if (ch == '#') {
      file.readLine();
      break;
    }
    token.append(ch);
  }
  if (token.isEmpty()) {
    throw std::runtime_error("Invalid PGM header");
  }
  return token;
}

std::vector<uint8_t> readPgmU16LeFile(const QString &path,
                                      size_t *width,
                                      size_t *height) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    throw std::runtime_error(QString("Cannot open PGM file: %1")
                                 .arg(path)
                                 .toStdString());
  }

  const QByteArray magic = readPgmToken(file);
  if (magic != "P5") {
    throw std::runtime_error(QString("Unsupported PGM magic in %1: %2")
                                 .arg(path, QString::fromLatin1(magic))
                                 .toStdString());
  }
  bool ok_w = false;
  bool ok_h = false;
  bool ok_max = false;
  const int w = readPgmToken(file).toInt(&ok_w);
  const int h = readPgmToken(file).toInt(&ok_h);
  const int max_value = readPgmToken(file).toInt(&ok_max);
  if (!ok_w || !ok_h || !ok_max || w <= 0 || h <= 0 || max_value != 65535) {
    throw std::runtime_error(QString("Unsupported PGM header in %1")
                                 .arg(path)
                                 .toStdString());
  }

  const size_t samples = static_cast<size_t>(w) * static_cast<size_t>(h);
  const qint64 expected_bytes = static_cast<qint64>(samples * sizeof(uint16_t));
  const QByteArray be_data = file.read(expected_bytes);
  if (be_data.size() != expected_bytes) {
    throw std::runtime_error(QString("PGM data size mismatch in %1: got %2 expected %3")
                                 .arg(path)
                                 .arg(be_data.size())
                                 .arg(expected_bytes)
                                 .toStdString());
  }

  std::vector<uint8_t> le_data(samples * sizeof(uint16_t));
  const uchar *src = reinterpret_cast<const uchar *>(be_data.constData());
  for (size_t i = 0; i < samples; ++i) {
    le_data[i * 2] = src[i * 2 + 1];
    le_data[i * 2 + 1] = src[i * 2];
  }
  if (width) {
    *width = static_cast<size_t>(w);
  }
  if (height) {
    *height = static_cast<size_t>(h);
  }
  return le_data;
}

QImage makeFpgaVideoStreamPreviewFromPgm(const QString &path,
                                         size_t expected_width,
                                         size_t expected_height,
                                         int out_h) {
  size_t width = 0;
  size_t height = 0;
  std::vector<uint8_t> frame = readPgmU16LeFile(path, &width, &height);
  if (width != expected_width || height != expected_height) {
    throw std::runtime_error(QString("PGM frame size mismatch: %1 got %2x%3 expected %4x%5")
                                 .arg(path)
                                 .arg(width)
                                 .arg(height)
                                 .arg(expected_width)
                                 .arg(expected_height)
                                 .toStdString());
  }
  return makeFpgaVideoStreamPreview(frame, width, height,
                                    width * sizeof(uint16_t), out_h);
}

uint16_t histogramValueAt(const std::vector<uint32_t> &histogram, size_t target) {
  size_t cumulative = 0;
  for (size_t i = 0; i < histogram.size(); ++i) {
    cumulative += histogram[i];
    if (cumulative > target) {
      return static_cast<uint16_t>(i);
    }
  }
  return static_cast<uint16_t>(histogram.size() - 1);
}

double histogramPercentileLinear(const std::vector<uint32_t> &histogram,
                                 size_t sample_count,
                                 double percentile) {
  if (sample_count == 0) {
    return 0.0;
  }
  const double rank = (percentile / 100.0) * static_cast<double>(sample_count - 1);
  const size_t low_index = static_cast<size_t>(std::floor(rank));
  const size_t high_index = static_cast<size_t>(std::ceil(rank));
  const double fraction = rank - static_cast<double>(low_index);
  const double low_value = histogramValueAt(histogram, low_index);
  const double high_value = histogramValueAt(histogram, high_index);
  return low_value + (high_value - low_value) * fraction;
}

QImage makePredictBatchDisplayPreview(const std::vector<uint8_t> &data,
                                      size_t width,
                                      size_t height,
                                      size_t line_pitch) {
  if (data.empty() || width == 0 || height == 0) {
    return QImage();
  }

  QImage image(static_cast<int>(width), static_cast<int>(height),
               QImage::Format_Grayscale8);
  if (image.isNull()) {
    return image;
  }

  std::vector<uint32_t> histogram(65536u, 0u);
  size_t sample_count = 0;
  uint16_t min_seen = 65535;
  uint16_t max_seen = 0;
  for (size_t y = 0; y < height; ++y) {
    const size_t row_base = y * line_pitch;
    for (size_t x = 0; x < width; ++x) {
      const size_t off = row_base + x * sizeof(uint16_t);
      if (off + sizeof(uint16_t) > data.size()) {
        continue;
      }
      const uint16_t v = readPixelValue(data, off, 2, 65535);
      ++histogram[v];
      ++sample_count;
      min_seen = std::min(min_seen, v);
      max_seen = std::max(max_seen, v);
    }
  }
  if (sample_count == 0) {
    return image;
  }

  double low = histogramPercentileLinear(histogram, sample_count, 0.5);
  double high = histogramPercentileLinear(histogram, sample_count, 99.5);
  if (!std::isfinite(low) || !std::isfinite(high) || high <= low) {
    low = static_cast<double>(min_seen);
    high = static_cast<double>(max_seen);
  }
  const double denom = std::max(high - low, 1.0);
  const double scale = 255.0 / denom;

  for (size_t y = 0; y < height; ++y) {
    uchar *dst = image.scanLine(static_cast<int>(y));
    const size_t row_base = y * line_pitch;
    for (size_t x = 0; x < width; ++x) {
      const size_t off = row_base + x * sizeof(uint16_t);
      uint16_t v = 0;
      if (off + sizeof(uint16_t) <= data.size()) {
        v = readPixelValue(data, off, 2, 65535);
      }
      double g = (static_cast<double>(v) - low) * scale;
      if (g < 0.0) {
        g = 0.0;
      } else if (g > 255.0) {
        g = 255.0;
      }
      dst[static_cast<int>(x)] = static_cast<uchar>(static_cast<int>(g));
    }
  }
  return image;
}

QImage makePredictBatchDisplayPreviewFromPgm(const QString &path,
                                             size_t expected_width,
                                             size_t expected_height) {
  size_t width = 0;
  size_t height = 0;
  std::vector<uint8_t> frame = readPgmU16LeFile(path, &width, &height);
  if (width != expected_width || height != expected_height) {
    throw std::runtime_error(QString("PGM frame size mismatch: %1 got %2x%3 expected %4x%5")
                                 .arg(path)
                                 .arg(width)
                                 .arg(height)
                                 .arg(expected_width)
                                 .arg(expected_height)
                                 .toStdString());
  }
  return makePredictBatchDisplayPreview(frame, width, height,
                                        width * sizeof(uint16_t));
}

QImage makeFpgaLinearFullPreview(const std::vector<uint8_t> &data,
                                 size_t width,
                                 size_t height,
                                 size_t line_pitch) {
  if (data.empty() || width == 0 || height == 0) {
    return QImage();
  }
  if (line_pitch < width * sizeof(uint16_t)) {
    return QImage();
  }

  QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_Grayscale8);
  if (image.isNull()) {
    return image;
  }

  for (size_t y = 0; y < height; ++y) {
    uchar *dst = image.scanLine(static_cast<int>(y));
    const size_t row_base = y * line_pitch;
    for (size_t x = 0; x < width; ++x) {
      const size_t off = row_base + x * sizeof(uint16_t);
      uint16_t v = 0;
      if (off + sizeof(uint16_t) <= data.size()) {
        v = readPixelValue(data, off, 2, 65535);
      }
      dst[static_cast<int>(x)] = static_cast<uchar>(std::min<uint16_t>(255, v >> 2));
    }
  }
  return image;
}

QImage invertGrayscaleImage(QImage image) {
  if (image.isNull()) {
    return image;
  }
  if (image.format() != QImage::Format_Grayscale8) {
    image = image.convertToFormat(QImage::Format_Grayscale8);
  }
  for (int y = 0; y < image.height(); ++y) {
    uchar *row = image.scanLine(y);
    for (int x = 0; x < image.width(); ++x) {
      row[x] = static_cast<uchar>(255 - row[x]);
    }
  }
  return image;
}

std::vector<uint8_t> compactToU16Frame(const std::vector<uint8_t> &data,
                                       size_t width,
                                       size_t height,
                                       size_t line_pitch,
                                       tImagePixelType pixel_type) {
  const int bpp = pixelBytes(pixel_type);
  if (bpp <= 0 || width == 0 || height == 0) {
    return {};
  }
  std::vector<uint8_t> frame(width * height * sizeof(uint16_t));
  const uint16_t mask = pixelMax(pixel_type);
  for (size_t y = 0; y < height; ++y) {
    const size_t src_row = y * line_pitch;
    const size_t dst_row = y * width * sizeof(uint16_t);
    if (bpp == 2 && line_pitch == width * sizeof(uint16_t)) {
      std::memcpy(frame.data() + dst_row, data.data() + src_row, width * sizeof(uint16_t));
      continue;
    }
    for (size_t x = 0; x < width; ++x) {
      const size_t src_off = src_row + x * static_cast<size_t>(bpp);
      uint16_t v = 0;
      if (src_off + static_cast<size_t>(bpp) <= data.size()) {
        if (bpp == 1) {
          v = static_cast<uint16_t>(data[src_off]) << 4;
        } else {
          std::memcpy(&v, data.data() + src_off, sizeof(v));
          v &= mask;
        }
      }
      std::memcpy(frame.data() + dst_row + x * sizeof(uint16_t), &v, sizeof(v));
    }
  }
  return frame;
}

void writeAll(int fd, const uint8_t *data, size_t size) {
  size_t done = 0;
  constexpr size_t kChunk = 4 * 1024 * 1024;
  while (done < size) {
    const size_t todo = std::min(kChunk, size - done);
    const ssize_t rc = ::write(fd, data + done, todo);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(QString("XDMA H2C write failed: %1")
                                   .arg(QString::fromLocal8Bit(std::strerror(errno)))
                                   .toStdString());
    }
    if (rc == 0) {
      throw std::runtime_error("XDMA H2C write returned 0");
    }
    done += static_cast<size_t>(rc);
  }
}

void waitFdReady(int fd, short events, int timeout_ms, const char *what) {
  pollfd pfd {};
  pfd.fd = fd;
  pfd.events = events;
  const int rc = ::poll(&pfd, 1, timeout_ms);
  if (rc == 0) {
    throw std::runtime_error(QString("%1 timeout after %2 ms")
                                 .arg(QString::fromLatin1(what))
                                 .arg(timeout_ms)
                                 .toStdString());
  }
  if (rc < 0) {
    if (errno == EINTR) {
      return;
    }
    throw std::runtime_error(QString("%1 poll failed: %2")
                                 .arg(QString::fromLatin1(what),
                                      QString::fromLocal8Bit(std::strerror(errno)))
                                 .toStdString());
  }
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
    throw std::runtime_error(QString("%1 device error, revents=0x%2")
                                 .arg(QString::fromLatin1(what))
                                 .arg(static_cast<int>(pfd.revents), 0, 16)
                                 .toStdString());
  }
}

void writeAllWithTimeout(int fd, const uint8_t *data, size_t size, int timeout_ms) {
  size_t done = 0;
  constexpr size_t kChunk = 1 * 1024 * 1024;
  while (done < size) {
    waitFdReady(fd, POLLOUT, timeout_ms, "XDMA H2C write");
    const size_t todo = std::min(kChunk, size - done);
    const ssize_t rc = ::write(fd, data + done, todo);
    if (rc < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      throw std::runtime_error(QString("XDMA H2C write failed: %1")
                                   .arg(QString::fromLocal8Bit(std::strerror(errno)))
                                   .toStdString());
    }
    if (rc == 0) {
      throw std::runtime_error("XDMA H2C write returned 0");
    }
    done += static_cast<size_t>(rc);
  }
}

void readAll(int fd, uint8_t *data, size_t size) {
  size_t done = 0;
  constexpr size_t kChunk = 4 * 1024 * 1024;
  while (done < size) {
    const size_t todo = std::min(kChunk, size - done);
    const ssize_t rc = ::read(fd, data + done, todo);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(QString("XDMA C2H read failed: %1")
                                   .arg(QString::fromLocal8Bit(std::strerror(errno)))
                                   .toStdString());
    }
    if (rc == 0) {
      throw std::runtime_error("XDMA C2H read returned 0");
    }
    done += static_cast<size_t>(rc);
  }
}

void readAllWithTimeout(int fd, uint8_t *data, size_t size, int timeout_ms) {
  size_t done = 0;
  constexpr size_t kChunk = 1 * 1024 * 1024;
  while (done < size) {
    waitFdReady(fd, POLLIN, timeout_ms, "XDMA C2H read");
    const size_t todo = std::min(kChunk, size - done);
    const ssize_t rc = ::read(fd, data + done, todo);
    if (rc < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      throw std::runtime_error(QString("XDMA C2H read failed: %1")
                                   .arg(QString::fromLocal8Bit(std::strerror(errno)))
                                   .toStdString());
    }
    if (rc == 0) {
      throw std::runtime_error("XDMA C2H read returned 0");
    }
    done += static_cast<size_t>(rc);
  }
}

bool processFrameWithXdma(const std::vector<uint8_t> &input,
                          std::vector<uint8_t> *output,
                          QString *error,
                          double *elapsed_ms,
                          double *fpga_line_ms,
                          uint32_t proc_ctrl) {
  if (elapsed_ms) {
    *elapsed_ms = -1.0;
  }
  if (fpga_line_ms) {
    *fpga_line_ms = -1.0;
  }
  output->assign(input.size(), 0);
  int h2c_fd = -1;
  int c2h_fd = -1;
  try {
    QString bar_error;
    FpgaBar bar(&bar_error);
    if (!bar.ok()) {
      throw std::runtime_error(bar_error.toStdString());
    }
    const uint32_t version = bar.read32(kFpgaRegVersion);
    if (version != kExpectedFpgaVersion) {
      throw std::runtime_error(QString("FPGA bitstream版本不匹配: 读到0x%1，期望0x%2")
                                   .arg(version, 8, 16, QChar('0'))
                                   .arg(kExpectedFpgaVersion, 8, 16, QChar('0'))
                                   .toStdString());
    }
    bar.write32(kFpgaRegProcCtrl, proc_ctrl);
    bar.write32(kFpgaRegCtrl, 0x00);
    usleep(1000);
    bar.write32(kFpgaRegCtrl, 0x02);
    usleep(1000);
    bar.write32(kFpgaRegCtrl, 0x10);
    usleep(1000);
    bar.write32(kFpgaRegCtrl, 0x01);
    usleep(10000);

    h2c_fd = ::open("/dev/xdma0_h2c_0", O_WRONLY | O_NONBLOCK);
    if (h2c_fd < 0) {
      throw std::runtime_error(QString("打开 /dev/xdma0_h2c_0 失败: %1")
                                   .arg(QString::fromLocal8Bit(std::strerror(errno)))
                                   .toStdString());
    }
    c2h_fd = ::open("/dev/xdma0_c2h_0", O_RDONLY | O_NONBLOCK);
    if (c2h_fd < 0) {
      throw std::runtime_error(QString("打开 /dev/xdma0_c2h_0 失败: %1")
                                   .arg(QString::fromLocal8Bit(std::strerror(errno)))
                                   .toStdString());
    }

    QElapsedTimer timer;
    timer.start();
    std::exception_ptr read_error;
    std::exception_ptr write_error;
    std::thread reader([&] {
      try {
        readAllWithTimeout(c2h_fd, output->data(), output->size(), kXdmaFrameTimeoutMs);
      } catch (...) {
        read_error = std::current_exception();
      }
    });
    std::thread writer([&] {
      try {
        writeAllWithTimeout(h2c_fd, input.data(), input.size(), kXdmaFrameTimeoutMs);
      } catch (...) {
        write_error = std::current_exception();
      }
    });
    writer.join();
    reader.join();
    if (write_error) {
      std::rethrow_exception(write_error);
    }
    if (read_error) {
      std::rethrow_exception(read_error);
    }
    if (elapsed_ms) {
      *elapsed_ms = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
    }
    if (fpga_line_ms) {
      *fpga_line_ms = cyclesToMs(bar.read32(kFpgaRegLineCoreCycles),
                                 bar.read32(kFpgaRegUserClkHz));
    }

    ::close(h2c_fd);
    ::close(c2h_fd);
    return true;
  } catch (const std::exception &e) {
    if (h2c_fd >= 0) {
      ::close(h2c_fd);
    }
    if (c2h_fd >= 0) {
      ::close(c2h_fd);
    }
    *error = QString::fromUtf8(e.what());
    return false;
  }
}

void resetFpgaDatapathBestEffort() {
  QString bar_error;
  FpgaBar bar(&bar_error);
  if (!bar.ok()) {
    return;
  }
  bar.write32(kFpgaRegCtrl, 0x00);
  usleep(1000);
  bar.write32(kFpgaRegCtrl, 0x02);
  usleep(1000);
  bar.write32(kFpgaRegCtrl, 0x00);
}

QStringList listDatasetRawFiles(const QString &dataset_root) {
  QStringList files;
  QDir root(dataset_root);
  if (!root.exists()) {
    return files;
  }
  QDirIterator it(dataset_root,
                  QStringList() << QStringLiteral("*_2048x4096_low12.raw")
                                << QStringLiteral("*.raw"),
                  QDir::Files,
                  QDirIterator::Subdirectories);
  const qint64 expected_size =
      static_cast<qint64>(kDatasetRawWidth * kDatasetRawHeight * sizeof(uint16_t));
  while (it.hasNext()) {
    const QString path = it.next();
    if (QFileInfo(path).size() == expected_size) {
      files.append(path);
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

QStringList listDatasetStripePreviewFiles(const QString &dataset_root) {
  QStringList files;
  QDir root(dataset_root);
  if (!root.exists()) {
    return files;
  }
  QDirIterator it(dataset_root,
                  QStringList() << QStringLiteral("*_stripes.png")
                                << QStringLiteral("*.png")
                                << QStringLiteral("*.pgm")
                                << QStringLiteral("*.jpg")
                                << QStringLiteral("*.jpeg")
                                << QStringLiteral("*.bmp")
                                << QStringLiteral("*.tif")
                                << QStringLiteral("*.tiff"),
                  QDir::Files,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    files.append(it.next());
  }
  files.removeDuplicates();
  std::sort(files.begin(), files.end());
  return files;
}

QString datasetRawRootForStripePreviewRoot(const QString &preview_root) {
  QString normalized = QDir::cleanPath(preview_root);
  const QString marker = QStringLiteral("/stripe_preview/");
  const int marker_pos = normalized.indexOf(marker);
  if (marker_pos >= 0) {
    normalized.replace(marker_pos, marker.size(), QStringLiteral("/raw/"));
    return normalized;
  }
  const QString input_marker = QStringLiteral("/input_preview/");
  const int input_marker_pos = normalized.indexOf(input_marker);
  if (input_marker_pos >= 0) {
    normalized.replace(input_marker_pos, input_marker.size(),
                       QStringLiteral("/raw_fpga_current_names/"));
    return normalized;
  }

  if (normalized.endsWith(QStringLiteral("/stripe_preview"))) {
    normalized.chop(QStringLiteral("/stripe_preview").size());
    return QDir(normalized).filePath(QStringLiteral("raw"));
  }
  if (normalized.endsWith(QStringLiteral("/input_preview"))) {
    normalized.chop(QStringLiteral("/input_preview").size());
    return QDir(normalized).filePath(QStringLiteral("raw_fpga_current_names"));
  }

  QDir dir(normalized);
  if (dir.dirName() == QStringLiteral("stripe_preview") ||
      dir.dirName() == QStringLiteral("input_preview")) {
    const bool is_input_preview = dir.dirName() == QStringLiteral("input_preview");
    dir.cdUp();
    return dir.filePath(is_input_preview
                            ? QStringLiteral("raw_fpga_current_names")
                            : QStringLiteral("raw"));
  }
  return normalized;
}

QString datasetBaseStemFromStripePreview(const QString &preview_path) {
  QString stem = QFileInfo(preview_path).completeBaseName();
  const QStringList suffixes = {
      QStringLiteral("_visual_stripes"),
      QStringLiteral("_stripes"),
      QStringLiteral("_stripe_preview"),
      QStringLiteral("_oct_lines"),
  };
  for (const QString &suffix : suffixes) {
    if (stem.endsWith(suffix)) {
      stem.chop(suffix.size());
      break;
    }
  }
  return stem;
}

QString findDatasetRawForStripePreview(const QString &preview_path,
                                       const QString &preview_root) {
  const QString raw_root = datasetRawRootForStripePreviewRoot(preview_root);
  const QString stem = datasetBaseStemFromStripePreview(preview_path);
  const QStringList names = {
      stem + QStringLiteral("_2048x4096_u16le_fpga_current.raw"),
      stem + QStringLiteral("_2048x4096_low12.raw"),
      stem + QStringLiteral(".raw"),
  };

  for (const QString &name : names) {
    const QString candidate = QDir(raw_root).filePath(name);
    if (QFileInfo(candidate).isFile()) {
      return candidate;
    }
  }

  QDirIterator it(raw_root,
                  QStringList() << (stem + QStringLiteral("*_2048x4096_u16le_fpga_current.raw"))
                                << (stem + QStringLiteral("*_2048x4096_low12.raw"))
                                << (stem + QStringLiteral("*.raw")),
                  QDir::Files,
                  QDirIterator::Subdirectories);
  const qint64 expected_size =
      static_cast<qint64>(kDatasetRawWidth * kDatasetRawHeight * sizeof(uint16_t));
  while (it.hasNext()) {
    const QString path = it.next();
    if (QFileInfo(path).size() == expected_size) {
      return path;
    }
  }

  throw std::runtime_error(
      QString("No matching RAW file for stripe preview: %1 under %2")
          .arg(preview_path, raw_root)
          .toStdString());
}

std::vector<uint8_t> readDatasetRawFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    throw std::runtime_error(QString("Cannot open dataset RAW: %1").arg(path).toStdString());
  }
  const QByteArray data = file.readAll();
  const qsizetype expected =
      static_cast<qsizetype>(kDatasetRawWidth * kDatasetRawHeight * sizeof(uint16_t));
  if (data.size() != expected) {
    throw std::runtime_error(QString("Dataset RAW size mismatch: %1 got %2 expected %3")
                                 .arg(path)
                                 .arg(data.size())
                                 .arg(expected)
                                 .toStdString());
  }
  return std::vector<uint8_t>(data.constData(), data.constData() + data.size());
}

std::vector<uint8_t> readDatasetStripePreviewFile(const QString &path,
                                                  QImage *preview_image) {
  QImage image(path);
  if (image.isNull()) {
    throw std::runtime_error(QString("Cannot open dataset stripe preview image: %1")
                                 .arg(path)
                                 .toStdString());
  }
  QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
  if (gray.width() != static_cast<int>(kDatasetRawWidth) ||
      gray.height() != static_cast<int>(kDatasetRawHeight)) {
    throw std::runtime_error(QString("Dataset stripe preview size mismatch: %1 got %2x%3 expected %4x%5")
                                 .arg(path)
                                 .arg(gray.width())
                                 .arg(gray.height())
                                 .arg(kDatasetRawWidth)
                                 .arg(kDatasetRawHeight)
                                 .toStdString());
  }
  if (preview_image) {
    *preview_image = gray;
  }

  std::vector<uint8_t> frame(kDatasetRawWidth * kDatasetRawHeight * sizeof(uint16_t));
  uint16_t *dst = reinterpret_cast<uint16_t *>(frame.data());
  for (size_t y = 0; y < kDatasetRawHeight; ++y) {
    const uchar *src = gray.constScanLine(static_cast<int>(y));
    for (size_t x = 0; x < kDatasetRawWidth; ++x) {
      dst[y * kDatasetRawWidth + x] = static_cast<uint16_t>(src[x]) << 4;
    }
  }
  return frame;
}

std::vector<uint8_t> makeDatasetProcessedFallback(const std::vector<uint8_t> &raw) {
  std::vector<uint8_t> out(kDatasetRawWidth * kDatasetProcessedHeight * sizeof(uint16_t));
  const uint16_t *src = reinterpret_cast<const uint16_t *>(raw.data());
  uint16_t *dst = reinterpret_cast<uint16_t *>(out.data());
  for (size_t y = 0; y < kDatasetProcessedHeight; ++y) {
    for (size_t x = 0; x < kDatasetRawWidth; ++x) {
      uint32_t sum = 0;
      for (size_t k = 0; k < kDatasetAverageRows; ++k) {
        sum += src[(y * kDatasetAverageRows + k) * kDatasetRawWidth + x] & kDatasetAdcMask;
      }
      dst[y * kDatasetRawWidth + x] = static_cast<uint16_t>((sum / kDatasetAverageRows) << 4);
    }
  }
  return out;
}

std::vector<uint8_t> reduceU16FrameToDatasetDisplay(
    const std::vector<uint8_t> &frame,
    size_t input_height,
    uint16_t mask,
    unsigned left_shift) {
  const size_t expected = kDatasetRawWidth * input_height * sizeof(uint16_t);
  if (frame.size() != expected || input_height == 0) {
    return std::vector<uint8_t>();
  }
  if (input_height == kDatasetProcessedHeight && mask == 0xFFFF && left_shift == 0) {
    return frame;
  }

  std::vector<uint8_t> out(kDatasetRawWidth * kDatasetProcessedHeight * sizeof(uint16_t));
  const uint16_t *src = reinterpret_cast<const uint16_t *>(frame.data());
  uint16_t *dst = reinterpret_cast<uint16_t *>(out.data());
  for (size_t y = 0; y < kDatasetProcessedHeight; ++y) {
    const size_t y0 = y * input_height / kDatasetProcessedHeight;
    const size_t y1 = std::max(y0 + 1, (y + 1) * input_height / kDatasetProcessedHeight);
    for (size_t x = 0; x < kDatasetRawWidth; ++x) {
      uint64_t sum = 0;
      for (size_t yy = y0; yy < y1; ++yy) {
        sum += src[yy * kDatasetRawWidth + x] & mask;
      }
      uint32_t value = static_cast<uint32_t>(sum / std::max<size_t>(1, y1 - y0));
      value = std::min<uint32_t>(65535u, value << left_shift);
      dst[y * kDatasetRawWidth + x] = static_cast<uint16_t>(value);
    }
  }
  return out;
}

QString datasetCaptureStem(const QString &raw_path, int index) {
  QString stem = QFileInfo(raw_path).completeBaseName();
  stem.replace(QStringLiteral("_2048x4096_low12"), QString());
  if (stem.isEmpty()) {
    stem = QStringLiteral("dataset_%1").arg(index, 3, 10, QChar('0'));
  }
  return stem;
}

void saveRawFile(const QString &path, const std::vector<uint8_t> &data) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    throw std::runtime_error(QString("Cannot write raw file: %1").arg(path).toStdString());
  }
  if (f.write(reinterpret_cast<const char *>(data.data()),
              static_cast<qint64>(data.size())) != static_cast<qint64>(data.size())) {
    throw std::runtime_error(QString("Failed writing raw file: %1").arg(path).toStdString());
  }
}

void copyFileOrThrow(const QString &source_path, const QString &dest_path) {
  QFile::remove(dest_path);
  if (!QFile::copy(source_path, dest_path)) {
    throw std::runtime_error(QString("Cannot copy file: %1 -> %2")
                                 .arg(source_path, dest_path)
                                 .toStdString());
  }
}

void savePgmFile(const QString &path,
                 const std::vector<uint8_t> &data,
                 size_t width,
                 size_t height,
                 size_t line_pitch,
                 tImagePixelType pixel_type) {
  const int bpp = pixelBytes(pixel_type);
  if (bpp <= 0) {
    throw std::runtime_error("Unsupported pixel type for PGM");
  }
  if (line_pitch < width * static_cast<size_t>(bpp)) {
    throw std::runtime_error("Invalid line pitch for PGM conversion");
  }

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    throw std::runtime_error(QString("Cannot write pgm file: %1").arg(path).toStdString());
  }

  const QByteArray header =
      QString("P5\n%1 %2\n%3\n").arg(width).arg(height).arg(pixelMax(pixel_type)).toUtf8();
  if (f.write(header) != header.size()) {
    throw std::runtime_error("Failed writing PGM header");
  }

  if (bpp == 1) {
    for (size_t y = 0; y < height; ++y) {
      const char *row = reinterpret_cast<const char *>(data.data() + y * line_pitch);
      if (f.write(row, static_cast<qint64>(width)) != static_cast<qint64>(width)) {
        throw std::runtime_error("Failed writing PGM row");
      }
    }
    return;
  }

  QByteArray row_out;
  row_out.resize(static_cast<int>(width * 2));
  const uint16_t max_v = pixelMax(pixel_type);
  for (size_t y = 0; y < height; ++y) {
    const uint8_t *row = data.data() + y * line_pitch;
    for (size_t x = 0; x < width; ++x) {
      uint16_t v = 0;
      std::memcpy(&v, row + x * 2, sizeof(v));
      v &= max_v;
      row_out[static_cast<int>(2 * x)] = static_cast<char>((v >> 8) & 0xFF);
      row_out[static_cast<int>(2 * x + 1)] = static_cast<char>(v & 0xFF);
    }
    if (f.write(row_out) != row_out.size()) {
      throw std::runtime_error("Failed writing PGM row");
    }
  }
}

void savePgmU16File(const QString &path,
                    const std::vector<uint8_t> &data,
                    size_t width,
                    size_t height,
                    size_t line_pitch) {
  if (line_pitch < width * sizeof(uint16_t)) {
    throw std::runtime_error("Invalid line pitch for 16-bit PGM conversion");
  }
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    throw std::runtime_error(QString("Cannot write pgm file: %1").arg(path).toStdString());
  }
  const QByteArray header = QString("P5\n%1 %2\n65535\n").arg(width).arg(height).toUtf8();
  if (f.write(header) != header.size()) {
    throw std::runtime_error("Failed writing PGM header");
  }
  QByteArray row_out;
  row_out.resize(static_cast<int>(width * 2));
  for (size_t y = 0; y < height; ++y) {
    const uint8_t *row = data.data() + y * line_pitch;
    for (size_t x = 0; x < width; ++x) {
      uint16_t v = 0;
      std::memcpy(&v, row + x * 2, sizeof(v));
      row_out[static_cast<int>(2 * x)] = static_cast<char>((v >> 8) & 0xFF);
      row_out[static_cast<int>(2 * x + 1)] = static_cast<char>(v & 0xFF);
    }
    if (f.write(row_out) != row_out.size()) {
      throw std::runtime_error("Failed writing PGM row");
    }
  }
}

void saveMetaFile(const QString &path,
                  const CaptureSettings &settings,
                  const QString &capture_id,
                  size_t width,
                  size_t height,
                  size_t line_pitch,
                  tImagePixelType pixel_type,
                  uint64_t missed_triggers,
                  uint64_t line_lost) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
    throw std::runtime_error(QString("Cannot write meta file: %1").arg(path).toStdString());
  }

  QTextStream ts(&f);
  ts << "diagnosis_id=" << settings.session_id << "\n";
  ts << "visit_id=" << settings.visit_id << "\n";
  ts << "capture_id=" << capture_id << "\n";
  ts << "capture_root_dir=" << settings.capture_root_dir << "\n";
  ts << "capture_dir=" << settings.output_dir << "\n";
  ts << "continuous_capture=" << (settings.continuous ? "true" : "false") << "\n";
  ts << "mode=External Line Trigger Timed (SynchroMode=2)\n";
  ts << "gpi_format=TTL single-ended (0x12120=0; pin5=trigger, pin4=GND, pin6=open)\n";
  ts << "frame_lines_target=" << settings.frame_lines << "\n";
  ts << "external_line_period_source=external TTL rising edges; panel value must match Zynq camera-trigger DAC period\n";
  ts << "external_line_period_us=" << settings.external_line_period_us << "\n";
  ts << "external_line_frequency_hz=" << settings.external_line_frequency_hz << "\n";
  ts << "zynq_camera_trigger_source=third DAC8830 output; verify on oscilloscope before connecting OCT GPI\n";
  ts << "zynq_camera_trigger_nominal_level=3.3V TTL target\n";
  ts << "zynq_camera_trigger_threshold_code=" << settings.trigger_threshold_code << "\n";
  ts << "zynq_camera_trigger_pulse_count=" << settings.trigger_pulse_count << "\n";
  ts << "expected_frame_time_ms="
     << (settings.external_line_period_us * static_cast<double>(settings.frame_lines) / 1000.0)
     << "\n";
  ts << "exposure_dead_time_margin_us=" << kExposureDeadTimeUs << "\n";
  ts << "staging_lines=" << settings.staging_lines << "\n";
  ts << "sdk_buffers=" << settings.sdk_buffers << "\n";
  ts << "getbuffer_timeout_ms=" << settings.getbuffer_timeout_ms << "\n";
  ts << "exposure_us=" << settings.exposure_us << "\n";
  ts << "pixel_type=" << pixelTypeName(pixel_type) << "\n";
  ts << "width=" << width << "\n";
  ts << "height=" << height << "\n";
  ts << "line_pitch=" << line_pitch << "\n";
  ts << "fpga_proc_ctrl=0x"
     << QString("%1").arg(settings.fpga_proc_ctrl, 8, 16, QChar('0')) << "\n";
  ts << "fpga_norm_shift=" << (settings.fpga_proc_ctrl & 0xFu) << "\n";
  ts << "fpga_out_shift=" << ((settings.fpga_proc_ctrl >> 4) & 0xFu) << "\n";
  ts << "fpga_log_gain_q4_4=" << ((settings.fpga_proc_ctrl >> 8) & 0xFFu) << "\n";
  ts << "fpga_log_offset="
     << static_cast<int>(static_cast<int16_t>((settings.fpga_proc_ctrl >> 16) & 0xFFFFu))
     << "\n";
  ts << "missed_triggers=" << missed_triggers << "\n";
  ts << "line_lost=" << line_lost << "\n";
}

}  // namespace

CaptureWorker::CaptureWorker(const CaptureSettings &settings, QObject *parent)
    : QObject(parent), settings_(settings) {}

void CaptureWorker::requestStop() {
  stop_requested_.store(true);
}

void CaptureWorker::process() {
  if (settings_.use_dataset_source) {
    try {
      const QString dataset_root = settings_.dataset_root_dir.isEmpty()
          ? QDir(QDir::homePath()).filePath(
                QStringLiteral("Desktop/competition_same_patient_lcoct_yolo_20260701/stripe_preview/test"))
          : settings_.dataset_root_dir;
      const QString dataset_raw_root = datasetRawRootForStripePreviewRoot(dataset_root);
      const QStringList source_files = listDatasetStripePreviewFiles(dataset_root);
      if (source_files.isEmpty()) {
        throw std::runtime_error(QString("No dataset stripe preview images found: %1")
                                     .arg(dataset_root)
	                                     .toStdString());
	      }

      QDir out_dir(settings_.output_dir);
      if (!out_dir.exists() && !out_dir.mkpath(".")) {
        throw std::runtime_error(QString("Cannot create output dir: %1")
                                     .arg(settings_.output_dir)
                                     .toStdString());
      }
      const bool cache_dataset_analysis = settings_.use_dataset_source;
      const bool save_dataset_files = settings_.save_raw || settings_.save_pgm ||
                                      cache_dataset_analysis;
      const QString raw_dir_path = out_dir.filePath(QStringLiteral("raw_images"));
      const QString processed_dir_path = out_dir.filePath(QStringLiteral("fpga_processed_images"));
      QDir raw_dir(raw_dir_path);
      QDir processed_dir(processed_dir_path);
      if (save_dataset_files && !raw_dir.exists() && !raw_dir.mkpath(".")) {
        throw std::runtime_error(QString("Cannot create raw image dir: %1")
                                     .arg(raw_dir_path)
                                     .toStdString());
      }
      if (save_dataset_files && !processed_dir.exists() && !processed_dir.mkpath(".")) {
        throw std::runtime_error(QString("Cannot create FPGA processed image dir: %1")
                                     .arg(processed_dir_path)
                                     .toStdString());
      }

      emit logMessage(QString("Initializing dataset source: %1 stripe preview frames from %2")
                          .arg(source_files.size())
                          .arg(dataset_root));
      emit logMessage(QString("FPGA dataset input uses matching low12 RAW frames from %1")
                          .arg(dataset_raw_root));
      emit logMessage(QString("Acquisition started. Dataset realtime playback: %1 fps, %2, save=%3.")
                          .arg(1000.0 / qMax(1, settings_.dataset_frame_interval_ms), 0, 'f', 1)
                          .arg(settings_.dataset_preview_frames > 0
                                   ? QString("auto-stop at %1 frames").arg(settings_.dataset_preview_frames)
                                   : QStringLiteral("manual stop"))
                          .arg((settings_.save_raw || settings_.save_pgm)
                                   ? QStringLiteral("on")
                                   : QStringLiteral("analysis-cache-only")));

      const int max_preview_frames =
          settings_.dataset_preview_frames > 0 ? settings_.dataset_preview_frames : 0;
      const int frame_interval_ms = qMax(1, settings_.dataset_frame_interval_ms);
      int saved_frames = 0;
      int file_pos = 0;
      int next_capture_index =
          settings_.capture_index_start > 0 ? settings_.capture_index_start : 1;
      QString last_base;
      QElapsedTimer playback_timer;
      playback_timer.start();

      while (!stop_requested_.load()) {
        QElapsedTimer frame_timer;
        frame_timer.start();
        const qint64 frame_start_ms = playback_timer.elapsed();
        const QString preview_path = source_files.at(file_pos % source_files.size());
        const QString fpga_raw_path =
            findDatasetRawForStripePreview(preview_path, dataset_root);
        file_pos++;

        QImage raw_preview;
        std::vector<uint8_t> raw_display_frame =
            readDatasetStripePreviewFile(preview_path, &raw_preview);
        if (raw_preview.isNull()) {
          raw_preview = makeFpgaVideoStreamPreview(
              raw_display_frame, kDatasetRawWidth, kDatasetRawHeight,
              kDatasetRawWidth * sizeof(uint16_t),
              static_cast<int>(kDatasetRealtimePreviewHeight));
        }
        std::vector<uint8_t> fpga_input_frame = readDatasetRawFile(fpga_raw_path);
        if (stop_requested_.load()) {
          break;
        }
        std::vector<uint8_t> processed_frame;
        double roundtrip_ms = -1.0;
        double line_ms = -1.0;
        QString fpga_error;
        bool used_fpga = processFrameWithXdma(fpga_input_frame, &processed_frame, &fpga_error,
                                              &roundtrip_ms, &line_ms,
                                              settings_.fpga_proc_ctrl);
        if (stop_requested_.load()) {
          break;
        }
        const bool cache_this_frame = cache_dataset_analysis && used_fpga &&
                                      (kDatasetAnalysisCacheFrames <= 0 ||
                                       saved_frames < kDatasetAnalysisCacheFrames);
        const bool write_this_frame = settings_.save_raw || settings_.save_pgm || cache_this_frame;
        QString raw_base = preview_path;
        QString processed_base;
        QString capture_id;
        const size_t processed_height = kDatasetRawHeight;
        if (!used_fpga || processed_frame.empty()) {
          const QString message =
              QString("FPGA processing failed for dataset stream: %1").arg(fpga_error);
          emit logMessage(message);
          throw std::runtime_error(message.toStdString());
        }
        const size_t expected_fpga_output =
            kDatasetRawWidth * kDatasetRawHeight * sizeof(uint16_t);
        if (processed_frame.size() != expected_fpga_output) {
          const QString message =
              QString("FPGA output size mismatch: got %1 bytes, expected %2 bytes for uncropped %3x%4 frame")
                  .arg(static_cast<qulonglong>(processed_frame.size()))
                  .arg(static_cast<qulonglong>(expected_fpga_output))
                  .arg(kDatasetRawWidth)
                  .arg(kDatasetRawHeight);
          emit logMessage(message);
          throw std::runtime_error(message.toStdString());
        }

        capture_id = QString("capture_%1")
                         .arg(next_capture_index, 3, 10, QChar('0'));
        const QString stamp =
            QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
        const QString stem = datasetCaptureStem(fpga_raw_path, saved_frames + 1);
        raw_base = raw_dir.filePath(
            QString("%1_%2_%3_%4_L%5")
                .arg(settings_.file_prefix)
                .arg(capture_id)
                .arg(stamp)
                .arg(stem)
                .arg(kDatasetRawHeight));
        processed_base = processed_dir.filePath(
            QString("%1_%2_%3_%4_fpga")
                .arg(settings_.file_prefix)
                .arg(capture_id)
                .arg(stamp)
                .arg(stem));
        const QString processed_pgm_path = write_this_frame
            ? processed_base + QStringLiteral(".pgm")
            : processed_dir.filePath(QStringLiteral("__live_fpga_preview.pgm"));
        savePgmU16File(processed_pgm_path, processed_frame, kDatasetRawWidth,
                       processed_height, kDatasetRawWidth * sizeof(uint16_t));

        QImage processed_preview = makePredictBatchDisplayPreviewFromPgm(
            processed_pgm_path, kDatasetRawWidth, processed_height);
        if (processed_preview.isNull()) {
          throw std::runtime_error(QString("Failed to build FPGA realtime preview from cached PGM: %1")
                                       .arg(processed_pgm_path)
                                       .toStdString());
        }

        emit frameReady(raw_preview, processed_preview, saved_frames + 1,
                        roundtrip_ms, line_ms);
        if (stop_requested_.load()) {
          break;
        }

        if (save_dataset_files && write_this_frame) {
          if (settings_.save_raw && !stop_requested_.load()) {
            saveRawFile(raw_base + ".raw", fpga_input_frame);
            saveRawFile(processed_base + ".raw", processed_frame);
          }
          if ((settings_.save_pgm || cache_this_frame) && !stop_requested_.load()) {
	            if (settings_.save_pgm) {
              savePgmU16File(raw_base + ".pgm", raw_display_frame, kDatasetRawWidth,
                             kDatasetRawHeight, kDatasetRawWidth * sizeof(uint16_t));
            } else if (cache_this_frame) {
              const QString suffix = QFileInfo(preview_path).suffix().isEmpty()
                                         ? QStringLiteral("png")
                                         : QFileInfo(preview_path).suffix();
              copyFileOrThrow(preview_path, raw_base + QStringLiteral(".") + suffix);
            }
          }
          if (stop_requested_.load()) {
            break;
          }
          saveMetaFile(raw_base + ".txt", settings_, capture_id, kDatasetRawWidth,
                       kDatasetRawHeight, kDatasetRawWidth * sizeof(uint16_t),
                       eMono12, 0, 0);
          QFile meta(processed_base + ".txt");
          if (meta.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&meta);
            ts << "source_capture_id=" << capture_id << "\n";
            ts << "source_dataset_stripe_preview=" << preview_path << "\n";
            ts << "source_dataset_raw=" << fpga_raw_path << "\n";
            ts << "raw_dir=" << raw_dir_path << "\n";
            ts << "processed_dir=" << processed_dir_path << "\n";
            ts << "width=" << kDatasetRawWidth << "\n";
            ts << "height=" << processed_height << "\n";
            ts << "line_pitch=" << (kDatasetRawWidth * sizeof(uint16_t)) << "\n";
            ts << "pixel_type=Mono16_from_fpga_low12_raw\n";
            ts << "fpga_input_source=raw_low12\n";
            ts << "fpga_input_width=" << kDatasetRawWidth << "\n";
            ts << "fpga_input_height=" << kDatasetRawHeight << "\n";
	            ts << "fpga_output_uncropped=true\n";
	            ts << "pcie_roundtrip_ms=" << roundtrip_ms << "\n";
            ts << "fpga_line_core_ms=" << line_ms << "\n";
            ts << "fpga_proc_ctrl=0x"
               << QString("%1").arg(settings_.fpga_proc_ctrl, 8, 16, QChar('0')) << "\n";
            ts << "fpga_norm_shift=" << (settings_.fpga_proc_ctrl & 0xFu) << "\n";
            ts << "fpga_out_shift=" << ((settings_.fpga_proc_ctrl >> 4) & 0xFu) << "\n";
            ts << "fpga_log_gain_q4_4=" << ((settings_.fpga_proc_ctrl >> 8) & 0xFFu) << "\n";
            ts << "fpga_log_offset="
               << static_cast<int>(static_cast<int16_t>((settings_.fpga_proc_ctrl >> 16) & 0xFFFFu))
               << "\n";
            ts << "fpga_linear_gain_q4_4=" << ((settings_.fpga_proc_ctrl >> 8) & 0xFFu) << "\n";
            ts << "fpga_linear_offset="
               << static_cast<int>(static_cast<int16_t>((settings_.fpga_proc_ctrl >> 16) & 0xFFFFu))
               << "\n";
            ts << "fpga_used=" << (used_fpga ? "true" : "false") << "\n";
          }
        }

        saved_frames++;
        next_capture_index++;
        last_base = raw_base;
        emit captureStats(0, 0, saved_frames);
        if (saved_frames == 1 || (saved_frames % kDatasetLogEveryFrames) == 0) {
          emit logMessage(QString("DATASET_FRAME frame=%1 fpga=%2 roundtrip_ms=%3 line_ms=%4 total_ms=%5 source=%6")
                              .arg(saved_frames)
                              .arg(used_fpga ? 1 : 0)
                              .arg(roundtrip_ms, 0, 'f', 2)
                              .arg(line_ms, 0, 'f', 4)
                              .arg(static_cast<double>(frame_timer.nsecsElapsed()) / 1000000.0,
                                   0, 'f', 2)
                              .arg(QString("%1 raw=%2").arg(preview_path, fpga_raw_path)));
        }

        if (max_preview_frames > 0 && saved_frames >= max_preview_frames) {
          break;
        }
        const qint64 target_next_ms =
            static_cast<qint64>(saved_frames) * static_cast<qint64>(frame_interval_ms);
        const qint64 delay_ms = target_next_ms - playback_timer.elapsed();
        if (delay_ms > 0) {
          qint64 remaining_ms = delay_ms;
          while (remaining_ms > 0 && !stop_requested_.load()) {
            const qint64 slice_ms = std::min<qint64>(remaining_ms, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice_ms));
            remaining_ms -= slice_ms;
          }
        } else if (playback_timer.elapsed() - frame_start_ms < 1) {
          std::this_thread::yield();
        }
      }

      const bool stopped = stop_requested_.load();
      if (stopped) {
        resetFpgaDatapathBestEffort();
      }
      emit finished(!stopped, QString("Dataset stream %1, processed %2 frame(s) at %3 fps. Last: %4")
                               .arg(stopped ? QStringLiteral("stopped by user")
                                            : QStringLiteral("finished"))
                               .arg(saved_frames)
                               .arg(1000.0 / qMax(1, settings_.dataset_frame_interval_ms), 0, 'f', 1)
                               .arg(last_base));
      return;
    } catch (const std::exception &e) {
      emit finished(false, QString::fromUtf8(e.what()));
      return;
    }
  }

  CAM_HANDLE camera = nullptr;
  bool lib_ready = false;
  bool cam_opened = false;
  bool acq_started = false;

  try {
    emit logMessage("Initializing SDK...");
    throwIfError("USB3_InitializeLibrary", USB3_InitializeLibrary());
    lib_ready = true;

    uint32_t nb_cameras = 0;
    int scan_err = CAM_ERR_SUCCESS;
    for (int attempt = 1; attempt <= kCameraScanMaxAttempts; ++attempt) {
      nb_cameras = 0;
      scan_err = USB3_UpdateCameraList(&nb_cameras);
      if (scan_err == CAM_ERR_SUCCESS && nb_cameras > 0) {
        break;
      }
      emit logMessage(QString("Camera scan attempt %1/%2: count=%3, err=%4")
                          .arg(attempt)
                          .arg(kCameraScanMaxAttempts)
                          .arg(nb_cameras)
                          .arg(scan_err));
      if (attempt < kCameraScanMaxAttempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kCameraScanRetryDelayMs));
      }
    }
    throwIfError("USB3_UpdateCameraList", scan_err);
    emit logMessage(QString("Camera scan complete, found: %1").arg(nb_cameras));
    if (nb_cameras == 0) {
      throw std::runtime_error("No camera found after retry.");
    }
    if (settings_.camera_index < 0 ||
        static_cast<uint32_t>(settings_.camera_index) >= nb_cameras) {
      throw std::runtime_error("Selected camera index is out of range.");
    }

    tCameraInfo info {};
    throwIfError("USB3_GetCameraInfo",
                 USB3_GetCameraInfo(static_cast<uint32_t>(settings_.camera_index), &info));
    emit logMessage(QString("Opening camera [%1]: %2")
                        .arg(settings_.camera_index)
                        .arg(info.pcID));

    throwIfError("USB3_OpenCamera", USB3_OpenCamera(&info, &camera));
    cam_opened = true;

    emit logMessage("Configuring camera to external line trigger mode 2...");
    writeRegU32(camera, kRegGpiFormat, kGpiFormatTtlSingleEnded);
    writeRegU32(camera, kRegSynchroMode, kSynchroModeExtLineTimed);

    const uint32_t mode_readback = readRegU32(camera, kRegSynchroMode);
    const uint32_t gpi_format_readback = readRegU32(camera, kRegGpiFormat);
    const uint32_t exposure_min = readRegU32(camera, kRegExposureTimeMin);
    const uint32_t exposure_max = readRegU32(camera, kRegExposureTimeMax);
    const uint32_t line_period_min = readRegU32(camera, kRegLinePeriodMin);
    const uint32_t gpi_state = readRegU32(camera, kRegGpiState);
    const double line_period_min_us = line_period_min / 100.0;
    const double exposure_min_us = exposure_min / 100.0;
    const double max_exposure_by_period_us =
        settings_.external_line_period_us - kExposureDeadTimeUs;

    if (settings_.external_line_period_us < line_period_min_us) {
      throw std::runtime_error(
          QString("External line period too short: selected %1 us, camera minimum %2 us")
              .arg(settings_.external_line_period_us, 0, 'f', 2)
              .arg(line_period_min_us, 0, 'f', 2)
              .toStdString());
    }
    if (settings_.exposure_us < exposure_min_us) {
      throw std::runtime_error(
          QString("Exposure too short: selected %1 us, camera minimum %2 us")
              .arg(settings_.exposure_us, 0, 'f', 2)
              .arg(exposure_min_us, 0, 'f', 2)
              .toStdString());
    }
    if (settings_.exposure_us > max_exposure_by_period_us) {
      throw std::runtime_error(
          QString("Exposure too long for selected line time: exposure %1 us, line time %2 us, limit %3 us")
              .arg(settings_.exposure_us, 0, 'f', 2)
              .arg(settings_.external_line_period_us, 0, 'f', 2)
              .arg(max_exposure_by_period_us, 0, 'f', 2)
              .toStdString());
    }

    writeRegU32(camera, kRegExposureTime,
                static_cast<uint32_t>(settings_.exposure_us * 100.0 + 0.5));
    writeRegU32(camera, kRegMissedTriggerReset, 1);
    writeRegU32(camera, kRegLineCounterReset, 1);

    const uint32_t exposure_readback = readRegU32(camera, kRegExposureTime);

    emit logMessage(QString("Trigger mode: SynchroMode=%1, GPI format=%2, GPI state=%3")
                        .arg(mode_readback)
                        .arg(gpi_format_readback)
                        .arg(gpi_state));
    emit logMessage(QString("Mode 2 expects %1 valid TTL rising edges for one image; "
                            "the external trigger source should output %1 complete cycles.")
                        .arg(settings_.frame_lines));
    emit logMessage(QString("External trigger period reference: %1 us (%2 Hz), expected frame time=%3 ms")
                        .arg(settings_.external_line_period_us, 0, 'f', 2)
                        .arg(settings_.external_line_frequency_hz, 0, 'f', 2)
                        .arg(settings_.external_line_period_us *
                                 static_cast<double>(settings_.frame_lines) / 1000.0,
                             0, 'f', 2));
    emit logMessage(QString("Timing readback: exposure=%1 us, exposure range=%2..%3 us, "
                            "line_period_min=%4 us")
                        .arg(exposure_readback / 100.0, 0, 'f', 2)
                        .arg(exposure_min / 100.0, 0, 'f', 2)
                        .arg(exposure_max / 100.0, 0, 'f', 2)
                        .arg(line_period_min / 100.0, 0, 'f', 2));

    emit logMessage(QString("SetImageParameters(height=%1, sdk_buffers=%2)")
                        .arg(settings_.frame_lines)
                        .arg(settings_.sdk_buffers));
    throwIfError("USB3_SetImageParameters",
                 USB3_SetImageParameters(camera, static_cast<size_t>(settings_.frame_lines),
                                         static_cast<size_t>(settings_.sdk_buffers)));

    throwIfError("USB3_StartAcquisition", USB3_StartAcquisition(camera));
    acq_started = true;
    emit logMessage("Acquisition started. Waiting for external TTL line triggers...");

    QDir out_dir(settings_.output_dir);
    if (!out_dir.exists() && !out_dir.mkpath(".")) {
      throw std::runtime_error(QString("Cannot create output dir: %1")
                                   .arg(settings_.output_dir)
                                   .toStdString());
    }
    const QString raw_dir_path = out_dir.filePath(QStringLiteral("raw_images"));
    const QString processed_dir_path = out_dir.filePath(QStringLiteral("fpga_processed_images"));
    QDir raw_dir(raw_dir_path);
    QDir processed_dir(processed_dir_path);
    if (!raw_dir.exists() && !raw_dir.mkpath(".")) {
      throw std::runtime_error(QString("Cannot create raw image dir: %1")
                                   .arg(raw_dir_path)
                                   .toStdString());
    }
    if (!processed_dir.exists() && !processed_dir.mkpath(".")) {
      throw std::runtime_error(QString("Cannot create FPGA processed image dir: %1")
                                   .arg(processed_dir_path)
                                   .toStdString());
    }

    int next_capture_index =
        settings_.capture_index_start > 0 ? settings_.capture_index_start : 1;
    int saved_frames = 0;
    QString last_base;
    bool fpga_warning_emitted = false;

    while (true) {
      std::vector<uint8_t> full_frame;
      size_t captured_lines = 0;
      size_t width = 0;
      size_t line_pitch = 0;
      tImagePixelType pixel_type = eUnknown;
      uint64_t missed_triggers = 0;
      uint64_t line_lost = 0;

      while (true) {
        if (stop_requested_.load()) {
          throw std::runtime_error(QString("Capture stopped by user, saved_frames=%1")
                                       .arg(saved_frames)
                                       .toStdString());
        }

        tImageInfos image {};
        // Poll with short timeout so stop requests react quickly even when no trigger arrives.
        const uint32_t poll_timeout_ms = static_cast<uint32_t>(settings_.getbuffer_timeout_ms);
        const uint32_t effective_timeout_ms =
            poll_timeout_ms < kStopPollTimeoutMs ? poll_timeout_ms : kStopPollTimeoutMs;
        const int err = USB3_GetBuffer(camera, &image, effective_timeout_ms);
        if (err == CAM_ERR_TIMEOUT) {
          continue;
        }
        throwIfError("USB3_GetBuffer", err);

        width = image.iImageWidth;
        line_pitch = image.iLinePitch;
        pixel_type = image.eImagePixelType;
        captured_lines = image.iImageHeight;
        full_frame.resize(captured_lines * line_pitch);
        const uint8_t *base = static_cast<const uint8_t *>(image.pDatas);
        for (size_t row = 0; row < captured_lines; ++row) {
          const uint8_t *row_ptr = base + row * image.iLinePitch;
          std::memcpy(full_frame.data() + row * line_pitch, row_ptr, line_pitch);
        }
        missed_triggers = image.iNbMissedTriggers;
        line_lost = image.iNbLineLost;
        throwIfError("USB3_RequeueBuffer", USB3_RequeueBuffer(camera, image.hBuffer));

        emit logMessage(QString("收到完整SDK图像缓冲: width=%1 height=%2 line_pitch=%3 pixel=%4")
                            .arg(width)
                            .arg(captured_lines)
                            .arg(line_pitch)
                            .arg(pixelTypeName(pixel_type)));
        break;
      }

      if (captured_lines != static_cast<size_t>(settings_.frame_lines)) {
        throw std::runtime_error(QString("Capture incomplete, got %1/%2 lines")
                                     .arg(captured_lines)
                                     .arg(settings_.frame_lines)
                                     .toStdString());
      }
      if (stop_requested_.load()) {
        throw std::runtime_error(QString("Capture stopped by user, saved_frames=%1")
                                     .arg(saved_frames)
                                     .toStdString());
      }

      const int bpp = pixelBytes(pixel_type);
      QImage raw_preview = makeGrayscalePreview(full_frame, width, captured_lines,
                                                line_pitch, bpp, pixelMax(pixel_type));
      QImage processed_preview = raw_preview;
      std::vector<uint8_t> processed_frame;
      double roundtrip_ms = -1.0;
      double line_ms = -1.0;
      if (width == 2048 && bpp > 0) {
        std::vector<uint8_t> compact_frame =
            compactToU16Frame(full_frame, width, captured_lines, line_pitch, pixel_type);
        QString fpga_error;
        if (!compact_frame.empty() &&
            processFrameWithXdma(compact_frame, &processed_frame, &fpga_error,
                                 &roundtrip_ms, &line_ms,
                                 settings_.fpga_proc_ctrl)) {
          processed_preview = makeGrayscalePreview(processed_frame, width, captured_lines,
                                                   width * sizeof(uint16_t), 2, 65535);
        } else if (!fpga_warning_emitted) {
          emit logMessage(QStringLiteral("FPGA处理暂不可用，右侧图像先显示原始预览：%1")
                              .arg(fpga_error));
          fpga_warning_emitted = true;
        }
      } else if (!fpga_warning_emitted) {
        emit logMessage(QStringLiteral("FPGA处理暂跳过：当前图像宽度=%1，像素格式=%2")
                            .arg(width)
                            .arg(pixelTypeName(pixel_type)));
        fpga_warning_emitted = true;
      }
      emit frameReady(raw_preview, processed_preview, saved_frames + 1,
                      roundtrip_ms, line_ms);
      if (stop_requested_.load()) {
        throw std::runtime_error(QString("Capture stopped by user, saved_frames=%1")
                                     .arg(saved_frames)
                                     .toStdString());
      }

      const QString capture_id = QString("capture_%1")
                                     .arg(next_capture_index, 3, 10, QChar('0'));
      const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
      const QString raw_base = raw_dir.filePath(
          QString("%1_%2_%3_L%4")
              .arg(settings_.file_prefix)
              .arg(capture_id)
              .arg(stamp)
              .arg(settings_.frame_lines));
      const QString processed_base = processed_dir.filePath(
          QString("%1_%2_%3_L%4_fpga")
              .arg(settings_.file_prefix)
              .arg(capture_id)
              .arg(stamp)
              .arg(settings_.frame_lines));

      if (settings_.save_raw && !stop_requested_.load()) {
        saveRawFile(raw_base + ".raw", full_frame);
        if (!processed_frame.empty()) {
          saveRawFile(processed_base + ".raw", processed_frame);
        }
      }
      if (settings_.save_pgm && !stop_requested_.load()) {
        savePgmFile(raw_base + ".pgm", full_frame, width,
                    captured_lines, line_pitch, pixel_type);
        if (!processed_frame.empty()) {
          savePgmU16File(processed_base + ".pgm", processed_frame, width,
                         captured_lines, width * sizeof(uint16_t));
        }
      }
      if (stop_requested_.load()) {
        throw std::runtime_error(QString("Capture stopped by user, saved_frames=%1")
                                     .arg(saved_frames)
                                     .toStdString());
      }
      saveMetaFile(raw_base + ".txt", settings_, capture_id, width, captured_lines,
                   line_pitch, pixel_type, missed_triggers, line_lost);
      if (!processed_frame.empty()) {
        QFile meta(processed_base + ".txt");
        if (meta.open(QIODevice::WriteOnly | QIODevice::Text)) {
          QTextStream ts(&meta);
          ts << "source_capture_id=" << capture_id << "\n";
          ts << "raw_dir=" << raw_dir_path << "\n";
          ts << "processed_dir=" << processed_dir_path << "\n";
          ts << "width=" << width << "\n";
          ts << "height=" << captured_lines << "\n";
          ts << "line_pitch=" << (width * sizeof(uint16_t)) << "\n";
          ts << "pixel_type=Mono16_from_fpga\n";
          ts << "pcie_roundtrip_ms=" << roundtrip_ms << "\n";
          ts << "fpga_line_core_ms=" << line_ms << "\n";
          ts << "fpga_proc_ctrl=0x"
             << QString("%1").arg(settings_.fpga_proc_ctrl, 8, 16, QChar('0')) << "\n";
          ts << "fpga_norm_shift=" << (settings_.fpga_proc_ctrl & 0xFu) << "\n";
          ts << "fpga_out_shift=" << ((settings_.fpga_proc_ctrl >> 4) & 0xFu) << "\n";
          ts << "fpga_log_gain_q4_4=" << ((settings_.fpga_proc_ctrl >> 8) & 0xFFu) << "\n";
          ts << "fpga_log_offset="
             << static_cast<int>(static_cast<int16_t>((settings_.fpga_proc_ctrl >> 16) & 0xFFFFu))
             << "\n";
          ts << "fpga_linear_gain_q4_4=" << ((settings_.fpga_proc_ctrl >> 8) & 0xFFu) << "\n";
          ts << "fpga_linear_offset="
             << static_cast<int>(static_cast<int16_t>((settings_.fpga_proc_ctrl >> 16) & 0xFFFFu))
             << "\n";
        }
      }

      saved_frames++;
      next_capture_index++;
      last_base = raw_base;
      emit captureStats(static_cast<qulonglong>(missed_triggers),
                        static_cast<qulonglong>(line_lost),
                        saved_frames);
      emit logMessage(QString("已合成并保存第 %1 张图像: %2")
                          .arg(saved_frames)
                          .arg(raw_base));
      emit logMessage(QString("本张统计: height=%1, missed_triggers=%2, line_lost=%3")
                          .arg(captured_lines)
                          .arg(missed_triggers)
                          .arg(line_lost));
      if (!settings_.continuous) {
        break;
      }
      if (missed_triggers > 0) {
        emit logMessage(QString("警告：本张图像期间相机报告漏触发 %1 次。请检查外部触发源是否输出 %2 个完整周期/%2 个上升沿。")
                            .arg(missed_triggers)
                            .arg(settings_.frame_lines));
      }
      emit logMessage(QString("等待下一组外部线触发脉冲..."));
    }

    if (acq_started) {
      USB3_StopAcquisition(camera);
      USB3_FlushBuffers(camera);
      acq_started = false;
    }
    if (cam_opened) {
      USB3_CloseCamera(camera);
      cam_opened = false;
    }
    if (lib_ready) {
      USB3_TerminateLibrary();
      lib_ready = false;
    }

    emit finished(true, settings_.continuous
                            ? QString("Continuous capture finished, saved %1 frame(s). Last: %2")
                                  .arg(saved_frames)
                                  .arg(last_base)
                            : QString("Saved frame: %1").arg(last_base));
    return;

  } catch (const std::exception &e) {
    if (acq_started) {
      USB3_StopAcquisition(camera);
      USB3_FlushBuffers(camera);
    }
    if (cam_opened) {
      USB3_CloseCamera(camera);
    }
    if (lib_ready) {
      USB3_TerminateLibrary();
    }
    emit finished(false, QString::fromUtf8(e.what()));
  }
}
