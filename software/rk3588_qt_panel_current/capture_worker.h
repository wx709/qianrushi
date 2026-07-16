#ifndef RK3568_CAPTURE_QT_PANEL_CAPTURE_WORKER_H_
#define RK3568_CAPTURE_QT_PANEL_CAPTURE_WORKER_H_

#include <QImage>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <atomic>

struct CaptureSettings {
  int camera_index = 0;
  QString capture_root_dir;
  QString session_id;
  QString visit_id;
  QString capture_id;
  QString output_dir;
  QString file_prefix = "oct";
  int capture_index_start = 1;
  int frame_lines = 4096;
  int staging_lines = 5;
  int sdk_buffers = 16;
  int getbuffer_timeout_ms = 200;
  double external_line_frequency_hz = 2100.0;
  double external_line_period_us = 80.0;
  int trigger_threshold_code = 13000;
  int trigger_pulse_count = 0;
  double exposure_us = 40.0;
  bool save_raw = true;
  bool save_pgm = true;
  bool continuous = false;
  bool use_dataset_source = false;
  QString dataset_root_dir;
  int dataset_preview_frames = 60;
  int dataset_frame_interval_ms = 33;
  quint32 fpga_proc_ctrl = 0x0000108A;
};

class CaptureWorker : public QObject {
  Q_OBJECT

 public:
  explicit CaptureWorker(const CaptureSettings &settings, QObject *parent = nullptr);

 public slots:
  void process();
  void requestStop();

signals:
  void logMessage(const QString &msg);
  void frameReady(const QImage &raw_preview, const QImage &processed_preview,
                  int frame_index, double roundtrip_ms, double line_ms);
  void captureStats(qulonglong missed_triggers, qulonglong line_lost, int saved_frames);
  void finished(bool ok, const QString &message);

 private:
  CaptureSettings settings_;
  std::atomic_bool stop_requested_ {false};
};

#endif  // RK3568_CAPTURE_QT_PANEL_CAPTURE_WORKER_H_
