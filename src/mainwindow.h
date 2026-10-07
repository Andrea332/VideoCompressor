#pragma once

#include "codecs.h"
#include "media.h"
#include "updatechecker.h"

#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QWidget>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

class EncoderCheck;
class SizeEstimator;
class Spinner;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QFrame;
class QDoubleSpinBox;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QTimer;

class MainWindow : public QWidget
{
    Q_OBJECT
    friend class TestVideoCompressor;

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void setInput(const QString &path);

    // Shows a message box; tests replace it so that no dialog blocks them.
    std::function<void(QMessageBox::Icon, const QString &title, const QString &text)> showMessage;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    // settings
    const Format &fmt() const;
    int quality() const;
    RateMode rateMode() const;
    RateControl rate() const;
    int keyint() const;
    const AudioCodec *audioCodec() const;
    int audioKbps() const;
    std::vector<const VideoCodec *> hwEncoders(const QString &family) const;
    QString deviceName(const QString &vendor) const;
    void refreshCodecs();
    void updateEncoder();
    QString encoderSummary() const;
    void onFormatChanged();
    void setCodec(const VideoCodec *codec);
    void updateRateModes();
    void onRateModeChanged();
    void onAudioCodecChanged();
    void updateAudioWidgets();
    QStringList videoArgs() const;
    void updateQualityLabel();
    void onSettingsChanged();

    // size estimate
    void runEstimate();
    double estimatedBytes() const;
    void onEstimateDone(double bps, bool exact);
    void onEstimateFailed(const QString &message);
    void updateLimitStatus();

    // fit to limit
    void fitToLimit();
    void setQuality(int value);
    void setBitrate(int kbps);
    void fitStep();

    // input and output
    void loadPreview();
    void pickInput();
    void onInputEdited();
    void pickOutput();
    void openFolder();

    // compression
    void start();
    void onStdout();
    void onStderr();
    void onFinished(int code, QProcess::ExitStatus status);
    void cancel();
    void setRunning(bool running);
    void fitToContent();
    enum class Outcome { Done, OverLimit, Failed };
    void showOutcome(Outcome outcome, const QString &text);

    // updates
    void checkForUpdates(bool manual);
    void saveLastUpdateCheck();
    void onUpdateAvailable(const UpdateInfo &info);
    bool canSelfUpdate() const;
    void startUpdate();
    void installUpdate(const QString &installer);

    struct Fit {
        int lo;
        int hi;
        std::optional<int> best;
        std::vector<std::pair<int, double>> points;
        int left;
        double target;
    };

    QString ffmpeg_;
    QString ffprobe_;
    QMap<QString, QString> gpus_;
    QString cpu_;
    QIcon hwIcon_;
    QIcon noIcon_;
    std::optional<MediaInfo> media_;
    const VideoCodec *codec_ = nullptr;   // encoder that will be used (from codec, acceleration and device)
    std::optional<double> estBps_;        // estimated bytes/s of the video track
    bool estExact_ = false;
    std::optional<Fit> fit_;              // state of the "Fit to limit" search
    QHash<QString, std::pair<double, bool>> estCache_;   // video settings -> bytes/s already estimated
    std::optional<QString> pendingKey_;
    bool programmatic_ = false;
    bool encoding_ = false;
    bool cancelled_ = false;
    bool resumeEstimate_ = false;         // an estimate was running when the compression started
    QProcess *proc_ = nullptr;
    QProcess *previewProc_ = nullptr;   // extracts the preview frame of the source video
    int previewGen_ = 0;                // discards frames of videos that are no longer loaded
    QString currentOutput_;
    QElapsedTimer encodeClock_;
    QString outBuf_;
    QStringList stderrTail_;

    EncoderCheck *encoders_;
    SizeEstimator *estimator_;
    QTimer *estTimer_;
    UpdateChecker *updater_;
    std::optional<UpdateInfo> pendingUpdate_;
    bool manualUpdateCheck_ = false;
    bool downloadingUpdate_ = false;

    QScrollArea *scroll_;   // holds everything else
    QFrame *updateBar_;
    QLabel *updateText_;
    QPushButton *updateBtn_;
    QPushButton *notesBtn_;
    QPushButton *laterBtn_;
    QCheckBox *autoUpdateCheck_;
    QPushButton *checkNowBtn_;
    QLabel *updateStatus_;

    QLineEdit *inEdit_;
    QPushButton *inBtn_;
    QLabel *preview_;
    QLabel *srcInfo_;
    QComboBox *formatCombo_;
    QComboBox *vcodecCombo_;
    QComboBox *accelCombo_;
    QComboBox *deviceCombo_;
    QLabel *encoderLabel_;
    QComboBox *rateCombo_;
    QSpinBox *bitrateSpin_;
    QHBoxLayout *sliderRow_;
    QSlider *qualitySlider_;
    QLabel *qualityLabel_;
    QComboBox *resCombo_;
    QComboBox *fpsCombo_;
    QComboBox *acodecCombo_;
    QComboBox *abitrateCombo_;
    QComboBox *speedCombo_;
    QComboBox *keyintCombo_;
    QFormLayout *settingsForm_;
    QGroupBox *settingsBox_;
    QLabel *sizeLabel_;
    Spinner *spinner_;   // turns while the size is being estimated
    QLabel *sizeDetail_;
    QDoubleSpinBox *limitSpin_;
    QLabel *limitStatus_;
    QPushButton *fitBtn_;
    QLineEdit *outEdit_;
    QPushButton *outBtn_;
    QProgressBar *progress_;
    QPushButton *startBtn_;
    QPushButton *cancelBtn_;
    QFrame *outcomeBar_;   // result of the last compression: done, over the limit or failed
    QLabel *outcomeIcon_;
    QLabel *outcomeTitle_;
    QLabel *outcomeText_;
    QPushButton *playBtn_;
    QPushButton *openBtn_;
    QPlainTextEdit *log_;
};
