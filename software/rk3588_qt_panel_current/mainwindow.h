#ifndef RK3568_CAPTURE_QT_PANEL_MAINWINDOW_H_
#define RK3568_CAPTURE_QT_PANEL_MAINWINDOW_H_

#include "capture_worker.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QVector>
#include <QSpinBox>
#include <QThread>
#include <QtGlobal>

class QLabel;
class QDialog;
class QResizeEvent;
class QTabWidget;
class QTimer;
class QWidget;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

 protected:
  void resizeEvent(QResizeEvent *event) override;

 private slots:
  void onScanClicked();
  void onNewDiagnosisClicked();
  void onPreviousDiagnosisClicked();
  void onDeleteCurrentDiagnosisClicked();
  void onStartClicked();
  void onStopClicked();
  void onAnalyzeLastClicked();
  void onOpenLastSessionClicked();
  void onApplyFpgaProcClicked();
  void onReadFpgaProcClicked();
  void onAnalysisReadyRead();
  void onAnalysisFinished(int exit_code, QProcess::ExitStatus exit_status);
  void onWorkerLog(const QString &msg);
  void onFrameReady(const QImage &raw_preview, const QImage &processed_preview,
                    int frame_index, double roundtrip_ms, double line_ms);
  void onCaptureStats(qulonglong missed_triggers, qulonglong line_lost, int saved_frames);
  void onWorkerFinished(bool ok, const QString &message);
  void refreshConfigStatus();
  void applyConfigUsbRule();
  void switchToCapturePanel();
  void openConfigCapturesFolder();
  void openConfigProjectFolder();

 private:
  struct CommandResult {
    int exit_code = -1;
    QString stdout_text;
    QString stderr_text;
  };

  struct ZynqMotorSnapshot {
    quint32 control = 0;
    quint32 status = 0;
    quint32 count = 0;
    quint32 intr_status = 0;
    quint32 adc_raw = 0;
    qint32 adc_a_uv = 0;
    qint32 adc_b_uv = 0;
    bool running = false;
    bool adc_ready = false;
    QString port;
    QString response;
  };

  void setupUi();
  QWidget *createConfigTab(QWidget *parent);
  void appendLog(const QString &msg);
  void appendKeyStatus(const QString &msg);
  QString keyStatusForLog(const QString &msg) const;
  void resetLiveViews();
  void updateLiveImage(QLabel *label, const QImage &image);
  void refreshLiveImages();
  QFrame *createImagePanel(const QString &title_text, QLabel **image_label,
                           QLabel **latency_label);
  QDialog *createMotorDialog();
  QPixmap makeMotorPlot() const;
  void updateMotorPlot(bool running);
  quint32 fpgaProcCtrlFromUi() const;
  void setFpgaProcCtrlUi(quint32 value);
  QString fpgaProcCtrlSummary(quint32 value) const;
  void updateFpgaProcSummary();
  bool writeFpgaReg(quint32 offset, quint32 value, QString *error) const;
  bool readFpgaReg(quint32 offset, quint32 *value, QString *error) const;
  bool ensureFpgaPcieReady(QString *error);
  QString findZynqSerialPort(QString *error) const;
  bool sendZynqCommand(const QString &command, QString *response, QString *error,
                       int timeout_ms = 1000) const;
  bool parseZynqStatusResponse(const QString &response, ZynqMotorSnapshot *snapshot,
                               QString *error) const;
  bool configureZynqTriggerFromUi(QString *error);
  bool startMotorHardware(const QString &reason);
  bool stopMotorHardware(const QString &reason);
  bool clearMotorCounters(const QString &reason);
  void loadPanelSettings();
  void savePanelSettings() const;
  void appendConfigLog(const QString &msg);
  void setConfigStatus(QLabel *label, bool ok, const QString &text);
  CommandResult runCommand(const QString &program, const QStringList &args,
                           int timeout_ms = 5000) const;
  bool pathExists(const QString &path) const;
  QString currentUser() const;
  void setRunningUi(bool running);
  bool scanSdkCameras(QStringList *out_list, QString *error);
  QString scanUsbPorts();
  QString projectRootDir() const;
  QString defaultCaptureRootDir() const;
  QString defaultPythonDir() const;
  QString makeDiagnosisDir(const QString &capture_root_dir, const QString &prefix, QString *diagnosis_id);
  QString makeVisitDir(const QString &diagnosis_dir, int visit_index, QString *visit_id);
  QString makeCaptureDir(const QString &parent_dir, int capture_index, QString *capture_id);
  bool ensureDiagnosis(const QString &capture_root_dir, const QString &prefix);
  bool startNewDiagnosis(const QString &capture_root_dir, const QString &prefix);
  bool startNextVisitForDiagnosis(const QString &diagnosis_dir);
  bool parseDiagnosisFolderName(const QString &folder_name, QString *date, int *index) const;
  int maxDailyDiagnosisIndex(const QString &capture_root_dir, const QString &date) const;
  int maxNumberedChildIndex(const QString &parent_dir, const QString &prefix) const;
  int maxCaptureFileIndex(const QString &capture_dir) const;
  void clearCurrentDiagnosis();
  void updateStatusLabel();
  void updateTimingHint();
  void writeDiagnosisManifest(const QString &diagnosis_dir, const QString &diagnosis_id,
                              const QString &capture_root_dir, const QString &prefix);
  void writeVisitManifest(const QString &visit_dir, const QString &visit_id, int visit_index);
  bool openFolderOnDesktop(const QString &dir, const QString &title);
  bool startPythonAnalysis(const QString &diagnosis_dir, bool open_when_finished);
  QString latestAnalysisInputDir(const QString &diagnosis_dir) const;
  bool analysisCacheMatchesLatestInput(const QString &diagnosis_dir) const;
  QString findAnalysisVideo(const QString &diagnosis_dir) const;
  bool openAnalysisVideo(const QString &diagnosis_dir);
  void saveAutoTestScreenshot(const QString &stem);
  void writeAutoTestResult(const QString &status, int analysis_exit_code = -999);

  QTabWidget *main_tabs_ = nullptr;
  QLabel *config_sdk_include_status_ = nullptr;
  QLabel *config_sdk_lib_status_ = nullptr;
  QLabel *config_genicam_status_ = nullptr;
  QLabel *config_udev_status_ = nullptr;
  QLabel *config_plugdev_status_ = nullptr;
  QLabel *config_usb_camera_status_ = nullptr;
  QLabel *config_capture_binary_status_ = nullptr;
  QPlainTextEdit *config_log_edit_ = nullptr;
  QPushButton *config_refresh_btn_ = nullptr;
  QPushButton *config_apply_usb_btn_ = nullptr;
  QPushButton *config_capture_tab_btn_ = nullptr;
  QPushButton *config_captures_btn_ = nullptr;
  QPushButton *config_project_btn_ = nullptr;

  QComboBox *camera_combo_ = nullptr;
  QLineEdit *output_dir_edit_ = nullptr;
  QLineEdit *prefix_edit_ = nullptr;
  QLineEdit *python_dir_edit_ = nullptr;
  QDoubleSpinBox *exposure_spin_ = nullptr;
  QDoubleSpinBox *external_trigger_freq_spin_ = nullptr;
  QDoubleSpinBox *line_period_spin_ = nullptr;
  QSpinBox *trigger_threshold_spin_ = nullptr;
  QSpinBox *trigger_pulse_count_spin_ = nullptr;
  QSpinBox *frame_lines_spin_ = nullptr;
  QSpinBox *staging_lines_spin_ = nullptr;
  QSpinBox *sdk_buffers_spin_ = nullptr;
  QSpinBox *timeout_spin_ = nullptr;
  QSpinBox *fpga_norm_shift_spin_ = nullptr;
  QSpinBox *fpga_out_shift_spin_ = nullptr;
  QSpinBox *fpga_log_gain_spin_ = nullptr;
  QSpinBox *fpga_log_offset_spin_ = nullptr;
  QLabel *fpga_proc_ctrl_label_ = nullptr;
  QPushButton *fpga_proc_apply_btn_ = nullptr;
  QPushButton *fpga_proc_read_btn_ = nullptr;
  QPushButton *scan_btn_ = nullptr;
  QPushButton *new_diagnosis_btn_ = nullptr;
  QPushButton *previous_diagnosis_btn_ = nullptr;
  QPushButton *delete_current_diagnosis_btn_ = nullptr;
  QPushButton *start_btn_ = nullptr;
  QPushButton *analyze_recognition_btn_ = nullptr;
  QPushButton *stop_btn_ = nullptr;
  QPushButton *browse_btn_ = nullptr;
  QPushButton *browse_python_btn_ = nullptr;
  QPushButton *analyze_last_btn_ = nullptr;
  QPushButton *open_last_session_btn_ = nullptr;
  QPushButton *clear_log_btn_ = nullptr;
  QCheckBox *continuous_capture_cb_ = nullptr;
  QLabel *timing_hint_label_ = nullptr;
  QLabel *missed_trigger_label_ = nullptr;
  QLabel *status_label_ = nullptr;
  QLabel *raw_image_label_ = nullptr;
  QLabel *processed_image_label_ = nullptr;
  QLabel *raw_latency_label_ = nullptr;
  QLabel *processed_latency_label_ = nullptr;
  QLabel *motor_position_label_ = nullptr;
  QLabel *motor_state_label_ = nullptr;
  QLabel *motor_fpga_label_ = nullptr;
  QLabel *motor_count_label_ = nullptr;
  QLabel *motor_status_label_ = nullptr;
  QLabel *motor_plot_label_ = nullptr;
  QDialog *motor_dialog_ = nullptr;
  QSpinBox *motor_sweep_target_spin_ = nullptr;
  QSpinBox *motor_sweep_cycles_spin_ = nullptr;
  QSpinBox *motor_sweep_interval_ms_spin_ = nullptr;
  QPushButton *motor_start_btn_ = nullptr;
  QPushButton *motor_stop_btn_ = nullptr;
  QPushButton *motor_clear_btn_ = nullptr;
  QPlainTextEdit *log_edit_ = nullptr;
  QPlainTextEdit *key_log_edit_ = nullptr;
  QTimer *motor_timer_ = nullptr;

  QThread *worker_thread_ = nullptr;
  CaptureWorker *worker_ = nullptr;
  QProcess *analysis_process_ = nullptr;
  QString current_diagnosis_id_;
  QString current_diagnosis_dir_;
  QString current_diagnosis_date_;
  QString current_visit_id_;
  QString current_visit_dir_;
  QString active_capture_dir_;
  QString last_diagnosis_dir_;
  QString analysis_target_dir_;
  QImage last_raw_preview_;
  QImage last_processed_preview_;
  QVector<double> motor_position_history_;
  QVector<double> motor_position_b_history_;
  int current_diagnosis_index_ = 0;
  int current_visit_index_ = 0;
  int visit_capture_index_ = 0;
  int active_capture_start_index_ = 0;
  double motor_position_ = 0.0;
  double motor_position_b_ = 0.0;
  double displayed_roundtrip_ms_ = -1.0;
  double displayed_line_ms_ = -1.0;
  qint64 last_latency_update_ms_ = 0;
  bool motor_running_ = false;
  bool running_ = false;
  bool continuous_capture_requested_ = false;
  bool analysis_open_when_ready_ = false;
  bool auto_dataset_test_mode_ = false;
  QString auto_test_output_dir_;
};

#endif  // RK3568_CAPTURE_QT_PANEL_MAINWINDOW_H_
