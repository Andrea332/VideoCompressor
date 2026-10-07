#include "mainwindow.h"

#include "encodercheck.h"
#include "settings.h"
#include "sizeestimator.h"
#include "spinner.h"
#include "systeminfo.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

using ComboItems = QList<QPair<QString, QVariant>>;

constexpr int PREVIEW_WIDTH = 192;    // preview frame of the source video, 16:9
constexpr int PREVIEW_HEIGHT = 108;

const char *const VIDEO_EXTENSIONS =
    "mp4 m4v mkv mov qt avi webm wmv asf flv f4v ts mts m2ts m2t mpg mpeg mpe m1v m2v vob evo "
    "3gp 3g2 mxf ogv ogm dv divx xvid rm rmvb nut y4m h264 264 h265 265 hevc ivf obu gif apng "
    "amv mjpeg mjpg wtv dvr-ms";

QString videoFilter()
{
    QStringList patterns;
    for (const QString &ext : QString(VIDEO_EXTENSIONS).split(' '))
        patterns << "*." + ext;
    return "Videos (" + patterns.join(' ') + ");;All files (*)";
}

// Replaces the items of a combo box, keeping the current choice when it is still there (if keep).
void fillCombo(QComboBox *combo, const ComboItems &items, bool keep = true)
{
    const QString current = combo->currentText();
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const auto &[label, data] : items)
        combo->addItem(label, data);
    combo->setCurrentIndex(keep ? std::max(0, combo->findText(current)) : 0);
}

// Lightning bolt that marks codecs with hardware acceleration (transparent if color is invalid).
QIcon boltIcon(const QColor &color = QColor())
{
    QPixmap pix(16, 16);
    pix.fill(Qt::transparent);
    if (color.isValid()) {
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPolygon(QPolygonF({{9.5, 0.5}, {3, 9}, {7.5, 9}, {6, 15.5}, {13, 6.5}, {8.5, 6.5}, {11, 0.5}}));
    }
    return QIcon(pix);
}

// Round badge with a white check mark, exclamation mark or cross, for the result of a compression.
QPixmap badge(const QColor &color, char symbol, int size, qreal dpr)
{
    QPixmap pix(qRound(size * dpr), qRound(size * dpr));
    pix.setDevicePixelRatio(dpr);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(0, 0, size, size));
    p.setPen(QPen(Qt::white, size / 9.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const auto at = [size](double x, double y) { return QPointF(x * size, y * size); };
    if (symbol == 'v') {
        p.drawPolyline(QPolygonF({at(0.28, 0.52), at(0.44, 0.68), at(0.73, 0.36)}));
    } else if (symbol == '!') {
        p.drawLine(at(0.5, 0.25), at(0.5, 0.56));
        p.drawPoint(at(0.5, 0.74));
    } else {
        p.drawLine(at(0.34, 0.34), at(0.66, 0.66));
        p.drawLine(at(0.66, 0.34), at(0.34, 0.66));
    }
    return pix;
}

// Scrolls only vertically: it is as wide as its content needs, so the window can't get narrower than that.
class VerticalScrollArea : public QScrollArea
{
public:
    QSize minimumSizeHint() const override
    {
        const int content = widget() ? widget()->minimumSizeHint().width() : 0;
        return {content + verticalScrollBar()->sizeHint().width(), QScrollArea::minimumSizeHint().height()};
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == widget() && event->type() == QEvent::LayoutRequest)
            updateGeometry();   // the content changed: its width may have too
        return QScrollArea::eventFilter(watched, event);
    }
};

// Explanations of the rate control modes, in the order of the menu
const char *const RATE_TIPS[] = {
    "The encoder gives each scene the bits it needs to keep the chosen quality:\n"
    "the best quality for the size. The size is estimated by encoding a few samples.",
    "An average bitrate of your choice: complex scenes get more, simple ones less\n"
    "(up to twice the average). The size is about bitrate × duration, smaller for simple videos.",
    "The same bitrate all the time, for streaming or for devices that need it.\n"
    "The size is bitrate × duration; the quality changes from scene to scene.",
};

QString withSuffix(const QString &path, const QString &ext)
{
    const QString suffix = QFileInfo(path).suffix();
    return (suffix.isEmpty() ? path + "." : path.left(path.size() - suffix.size())) + ext;
}

bool samePath(const QString &a, const QString &b)
{
    return QFileInfo(a).absoluteFilePath().compare(QFileInfo(b).absoluteFilePath(), Qt::CaseInsensitive) == 0;
}

QString capitalized(QString text)
{
    if (!text.isEmpty())
        text[0] = text[0].toUpper();
    return text;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    const QString version = QCoreApplication::applicationVersion();
    setWindowTitle(version.isEmpty() ? QString("Video Compressor") : "Video Compressor " + version);
    setAcceptDrops(true);

    ffmpeg_ = findTool("ffmpeg");
    ffprobe_ = findTool("ffprobe");
    gpus_ = gpuNames();
    cpu_ = cpuName();
    hwIcon_ = boltIcon(QColor("#f5a623"));
    noIcon_ = boltIcon();
    showMessage = [this](QMessageBox::Icon icon, const QString &title, const QString &text) {
        QMessageBox box(icon, title, text, QMessageBox::Ok, this);
        box.exec();
    };

    encoders_ = new EncoderCheck(ffmpeg_, this);
    connect(encoders_, &EncoderCheck::changed, this, &MainWindow::refreshCodecs);
    estimator_ = new SizeEstimator(ffmpeg_, ffprobe_, this);
    connect(estimator_, &SizeEstimator::done, this, &MainWindow::onEstimateDone);
    connect(estimator_, &SizeEstimator::failed, this, &MainWindow::onEstimateFailed);
    estTimer_ = new QTimer(this);
    estTimer_->setSingleShot(true);
    estTimer_->setInterval(600);
    connect(estTimer_, &QTimer::timeout, this, &MainWindow::runEstimate);

    // --- Update notice (hidden until a newer version is found) ---
    updater_ = new UpdateChecker(this);
    updateBar_ = new QFrame;
    updateBar_->setObjectName("updateBar");
    updateBar_->setStyleSheet("#updateBar { background: rgba(232, 137, 12, 0.14); "
                              "border: 1px solid rgba(232, 137, 12, 0.6); border-radius: 6px; }");
    updateText_ = new QLabel;
    updateText_->setWordWrap(true);
    notesBtn_ = new QPushButton("What's new");
    updateBtn_ = new QPushButton;
    laterBtn_ = new QPushButton("Later");
    auto *updateRowTop = new QHBoxLayout(updateBar_);
    updateRowTop->addWidget(updateText_, 1);
    updateRowTop->addWidget(notesBtn_);
    updateRowTop->addWidget(updateBtn_);
    updateRowTop->addWidget(laterBtn_);
    updateBar_->hide();
    connect(notesBtn_, &QPushButton::clicked, this, [this] {
        if (pendingUpdate_)
            QDesktopServices::openUrl(pendingUpdate_->page);
    });
    connect(updateBtn_, &QPushButton::clicked, this, &MainWindow::startUpdate);
    connect(laterBtn_, &QPushButton::clicked, this, [this] {
        updater_->cancelDownload();
        downloadingUpdate_ = false;
        updateBar_->hide();
    });
    connect(updater_, &UpdateChecker::updateAvailable, this, &MainWindow::onUpdateAvailable);
    connect(updater_, &UpdateChecker::upToDate, this, [this] {
        appSettings()->setValue("updates/lastCheck", QDateTime::currentDateTimeUtc());
        if (manualUpdateCheck_)
            updateStatus_->setText("✔ You have the latest version");
        manualUpdateCheck_ = false;
        checkNowBtn_->setEnabled(true);
    });
    connect(updater_, &UpdateChecker::failed, this, [this](const QString &message) {
        if (downloadingUpdate_) {
            downloadingUpdate_ = false;
            updateText_->setText("⚠ The update could not be downloaded: " + message.toHtmlEscaped());
            updateBtn_->setEnabled(true);
            return;
        }
        if (manualUpdateCheck_)   // automatic checks fail silently (e.g. no internet)
            updateStatus_->setText("⚠ Could not check for updates: " + message.toHtmlEscaped());
        manualUpdateCheck_ = false;
        checkNowBtn_->setEnabled(true);
    });
    connect(updater_, &UpdateChecker::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (pendingUpdate_ && total > 0)
            updateText_->setText(QString("Downloading Video Compressor %1… %2%")
                                     .arg(pendingUpdate_->version).arg(received * 100 / total));
    });
    connect(updater_, &UpdateChecker::downloaded, this, &MainWindow::installUpdate);

    // --- Source ---
    inEdit_ = new QLineEdit;
    inEdit_->setPlaceholderText("Drop a video here or click Browse…");
    connect(inEdit_, &QLineEdit::editingFinished, this, &MainWindow::onInputEdited);
    inBtn_ = new QPushButton("Browse…");
    connect(inBtn_, &QPushButton::clicked, this, &MainWindow::pickInput);
    auto *inRow = new QHBoxLayout;
    inRow->addWidget(inEdit_);
    inRow->addWidget(inBtn_);
    srcInfo_ = new QLabel("No video loaded.");
    srcInfo_->setWordWrap(true);
    srcInfo_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    preview_ = new QLabel;   // shown only when the path points to a video that can be read
    preview_->setFixedSize(PREVIEW_WIDTH, PREVIEW_HEIGHT);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setStyleSheet("background: black;");
    preview_->hide();
    auto *srcBox = new QGroupBox("Source video");
    auto *srcColumn = new QVBoxLayout;
    srcColumn->addLayout(inRow);
    srcColumn->addWidget(srcInfo_, 1);
    auto *srcLayout = new QHBoxLayout(srcBox);
    srcLayout->addWidget(preview_, 0, Qt::AlignTop);
    srcLayout->addLayout(srcColumn, 1);

    // --- Settings ---
    formatCombo_ = new QComboBox;
    for (const Format &f : formats())
        formatCombo_->addItem(f.label, f.ext);
    vcodecCombo_ = new QComboBox;
    vcodecCombo_->setToolTip("⚡ = this PC has a GPU that can encode the codec (hardware acceleration)");
    accelCombo_ = new QComboBox;
    accelCombo_->addItem("Automatic", "auto");
    accelCombo_->addItem("Manual", "manual");
    accelCombo_->addItem("Off (software, CPU)", "off");
    accelCombo_->setToolTip("Automatic: the best GPU that can encode the chosen codec, otherwise the CPU.\n"
                            "Manual: choose the GPU yourself.\n"
                            "Off: always encode on the CPU (slower, but smaller files at the same quality).");
    deviceCombo_ = new QComboBox;
    auto *accelRow = new QHBoxLayout;
    accelRow->addWidget(accelCombo_);
    accelRow->addWidget(deviceCombo_, 1);
    encoderLabel_ = new QLabel;   // always states codec, hardware acceleration and device
    // a table that needs no wrapping: always its own height, neither squeezed nor stretched by the window
    encoderLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // constant quality (the slider) or a bitrate, variable or constant, when the encoder has it
    rateCombo_ = new QComboBox;
    rateCombo_->addItem("Constant quality (recommended)", int(RateMode::Quality));
    rateCombo_->addItem("Variable bitrate (VBR)", int(RateMode::Vbr));
    rateCombo_->addItem("Constant bitrate (CBR)", int(RateMode::Cbr));
    for (int i = 0; i < rateCombo_->count(); ++i)
        rateCombo_->setItemData(i, QString(RATE_TIPS[i]), Qt::ToolTipRole);
    bitrateSpin_ = new QSpinBox;
    bitrateSpin_->setRange(MIN_VIDEO_KBPS, MAX_VIDEO_KBPS);
    bitrateSpin_->setSingleStep(100);
    bitrateSpin_->setValue(DEFAULT_VIDEO_KBPS);
    bitrateSpin_->setSuffix(" kbps");
    bitrateSpin_->setToolTip("Video bitrate. The audio bitrate is set below, next to the audio codec.");
    bitrateSpin_->hide();
    auto *rateRow = new QHBoxLayout;
    rateRow->addWidget(rateCombo_, 1);
    rateRow->addWidget(bitrateSpin_);

    qualitySlider_ = new QSlider(Qt::Horizontal);
    qualitySlider_->setInvertedAppearance(true);   // right = higher quality
    qualitySlider_->setInvertedControls(true);
    qualityLabel_ = new QLabel;
    sliderRow_ = new QHBoxLayout;
    sliderRow_->addWidget(new QLabel("Smaller file"));
    sliderRow_->addWidget(qualitySlider_, 1);
    sliderRow_->addWidget(new QLabel("Higher quality"));

    resCombo_ = new QComboBox;
    fpsCombo_ = new QComboBox;
    acodecCombo_ = new QComboBox;
    abitrateCombo_ = new QComboBox;
    for (int kbps : audioBitrates())
        abitrateCombo_->addItem(QString("%1 kbps").arg(kbps), kbps);
    abitrateCombo_->setCurrentIndex(abitrateCombo_->findData(DEFAULT_AUDIO_KBPS));
    auto *audioRow = new QHBoxLayout;
    audioRow->addWidget(acodecCombo_, 1);
    audioRow->addWidget(abitrateCombo_);
    speedCombo_ = new QComboBox;
    for (const SpeedLevel &s : speedLevels())
        speedCombo_->addItem(s.label, s.key);
    keyintCombo_ = new QComboBox;
    for (const KeyframeInterval &k : keyframeIntervals())
        keyintCombo_->addItem(k.label, k.seconds);
    keyintCombo_->setToolTip(
        "Keyframes are the points where playback can start when you jump in the video.\n"
        "More frequent keyframes give faster, more precise seeking (useful for editing\n"
        "and streaming) but a bigger file, especially for videos with little motion.");

    settingsForm_ = new QFormLayout;
    settingsForm_->addRow("Format:", formatCombo_);
    settingsForm_->addRow("Video codec:", vcodecCombo_);
    settingsForm_->addRow("Hardware acceleration:", accelRow);
    settingsForm_->addRow("", encoderLabel_);
    settingsForm_->addRow("Rate control:", rateRow);
    settingsForm_->addRow("Quality:", sliderRow_);
    settingsForm_->addRow("", qualityLabel_);
    settingsForm_->addRow("Resolution:", resCombo_);
    settingsForm_->addRow("Frame rate:", fpsCombo_);
    settingsForm_->addRow("Audio:", audioRow);
    settingsForm_->addRow("Speed:", speedCombo_);
    settingsForm_->addRow("Keyframes:", keyintCombo_);
    settingsBox_ = new QGroupBox("Settings");
    settingsBox_->setLayout(settingsForm_);
    settingsBox_->setEnabled(false);

    connect(formatCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onFormatChanged);
    for (QComboBox *combo : {vcodecCombo_, accelCombo_, deviceCombo_})
        connect(combo, &QComboBox::currentIndexChanged, this, &MainWindow::updateEncoder);
    connect(acodecCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onAudioCodecChanged);
    connect(rateCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onRateModeChanged);
    connect(bitrateSpin_, &QSpinBox::valueChanged, this, &MainWindow::onSettingsChanged);
    connect(qualitySlider_, &QSlider::valueChanged, this, &MainWindow::onSettingsChanged);
    for (QComboBox *combo : {resCombo_, fpsCombo_, abitrateCombo_, speedCombo_, keyintCombo_})
        connect(combo, &QComboBox::currentIndexChanged, this, &MainWindow::onSettingsChanged);

    // --- Size preview ---
    sizeLabel_ = new QLabel("—");
    QFont font = sizeLabel_->font();
    font.setPointSize(font.pointSize() + 10);
    font.setBold(true);
    sizeLabel_->setFont(font);
    spinner_ = new Spinner(QFontMetrics(font).height() * 3 / 4);
    auto *sizeRow = new QHBoxLayout;
    sizeRow->setSpacing(12);
    sizeRow->addWidget(sizeLabel_);
    sizeRow->addWidget(spinner_, 0, Qt::AlignVCenter);
    sizeRow->addStretch();
    sizeDetail_ = new QLabel("Load a video to see the estimate.");
    sizeDetail_->setWordWrap(true);

    limitSpin_ = new QDoubleSpinBox;
    limitSpin_->setRange(0.5, 4000);
    limitSpin_->setDecimals(1);
    limitSpin_->setValue(10);
    limitSpin_->setSuffix(" MB");
    connect(limitSpin_, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateLimitStatus);
    limitStatus_ = new QLabel;
    fitBtn_ = new QPushButton("Fit quality to limit");
    fitBtn_->setEnabled(false);
    connect(fitBtn_, &QPushButton::clicked, this, &MainWindow::fitToLimit);
    auto *limitRow = new QHBoxLayout;
    limitRow->addWidget(new QLabel("Limit:"));
    limitRow->addWidget(limitSpin_);
    limitRow->addWidget(limitStatus_, 1);
    limitRow->addWidget(fitBtn_);

    auto *previewBox = new QGroupBox("Estimated output size");
    auto *previewLayout = new QVBoxLayout(previewBox);
    previewLayout->addLayout(sizeRow);
    previewLayout->addWidget(sizeDetail_);
    previewLayout->addLayout(limitRow);

    // --- Output and start ---
    outEdit_ = new QLineEdit;
    outBtn_ = new QPushButton("Browse…");
    connect(outBtn_, &QPushButton::clicked, this, &MainWindow::pickOutput);
    auto *outRow = new QHBoxLayout;
    outRow->addWidget(new QLabel("Save as:"));
    outRow->addWidget(outEdit_);
    outRow->addWidget(outBtn_);

    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    startBtn_ = new QPushButton("Compress");
    startBtn_->setEnabled(false);
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::start);
    cancelBtn_ = new QPushButton("Cancel");
    cancelBtn_->setEnabled(false);
    connect(cancelBtn_, &QPushButton::clicked, this, &MainWindow::cancel);
    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(startBtn_);
    btnRow->addWidget(cancelBtn_);
    btnRow->addStretch();

    // --- Result of the last compression, hard to miss (hidden until one ends) ---
    outcomeBar_ = new QFrame;
    outcomeBar_->setObjectName("outcomeBar");
    outcomeIcon_ = new QLabel;
    outcomeTitle_ = new QLabel;
    QFont titleFont = outcomeTitle_->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    outcomeTitle_->setFont(titleFont);
    outcomeText_ = new QLabel;
    outcomeText_->setWordWrap(true);
    outcomeText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    playBtn_ = new QPushButton("Play");
    playBtn_->setToolTip("Open the compressed video with the default player");
    connect(playBtn_, &QPushButton::clicked, this, [this] {
        if (!currentOutput_.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(currentOutput_));
    });
    openBtn_ = new QPushButton("Open folder");
    openBtn_->setEnabled(false);
    connect(openBtn_, &QPushButton::clicked, this, &MainWindow::openFolder);
    auto *outcomeTexts = new QVBoxLayout;
    outcomeTexts->setSpacing(2);
    outcomeTexts->addWidget(outcomeTitle_);
    outcomeTexts->addWidget(outcomeText_);
    auto *outcomeRow = new QHBoxLayout(outcomeBar_);
    outcomeRow->setSpacing(12);
    outcomeRow->addWidget(outcomeIcon_, 0, Qt::AlignTop);
    outcomeRow->addLayout(outcomeTexts, 1);
    outcomeRow->addWidget(playBtn_, 0, Qt::AlignVCenter);
    outcomeRow->addWidget(openBtn_, 0, Qt::AlignVCenter);
    outcomeBar_->hide();

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(500);
    log_->setMaximumHeight(110);

    autoUpdateCheck_ = new QCheckBox("Check for updates automatically");
    autoUpdateCheck_->setChecked(appSettings()->value("updates/autoCheck", true).toBool());
    connect(autoUpdateCheck_, &QCheckBox::toggled, this,
            [](bool on) { appSettings()->setValue("updates/autoCheck", on); });
    checkNowBtn_ = new QPushButton("Check now");
    connect(checkNowBtn_, &QPushButton::clicked, this, [this] { checkForUpdates(true); });
    updateStatus_ = new QLabel;
    auto *updateRow = new QHBoxLayout;
    updateRow->addWidget(autoUpdateCheck_);
    updateRow->addWidget(checkNowBtn_);
    updateRow->addWidget(updateStatus_, 1);

    // everything in a scroll area: on short screens the window scrolls instead of squeezing what's in it
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->addWidget(updateBar_);
    layout->addWidget(srcBox);
    layout->addWidget(settingsBox_);
    layout->addWidget(previewBox);
    layout->addLayout(outRow);
    layout->addWidget(progress_);
    layout->addLayout(btnRow);
    layout->addWidget(outcomeBar_);
    layout->addWidget(log_);
    layout->addStretch();   // spare height, if any, at the bottom rather than inside the boxes
    layout->addLayout(updateRow);
    scroll_ = new VerticalScrollArea;
    scroll_->setWidget(content);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll_);
    resize(std::max(640, scroll_->minimumSizeHint().width()), 0);
    fitToContent();

    // automatic check: a few seconds after startup, at most once a day
    const QDateTime lastCheck = appSettings()->value("updates/lastCheck").toDateTime();
    if (autoUpdateCheck_->isChecked()
        && (!lastCheck.isValid() || lastCheck.secsTo(QDateTime::currentDateTimeUtc()) > 24 * 3600))
        QTimer::singleShot(3000, this, [this] { checkForUpdates(false); });

    refreshCodecs();
    if (ffmpeg_.isEmpty() || ffprobe_.isEmpty()) {
        srcInfo_->setText("⚠ ffmpeg/ffprobe not found. Put them in the 'ffmpeg' folder "
                          "or install them (e.g. 'winget install Gyan.FFmpeg') and restart the program.");
        inEdit_->setEnabled(false);
        inBtn_->setEnabled(false);
    }
}

MainWindow::~MainWindow()
{
    // After this destructor, QWidget's own one still closes the window and deletes the children, and they
    // keep emitting signals: the path field emits editingFinished when it loses the focus, a running
    // FFmpeg process emits finished. Disconnect them all from this window first, since its members are
    // about to be destroyed, then stop the processes.
    for (QObject *child : findChildren<QObject *>())
        child->disconnect(this);
    estimator_->stop();
    updater_->cancelDownload();
    for (QProcess *proc : findChildren<QProcess *>(Qt::FindDirectChildrenOnly)) {
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(3000);
        }
    }
}

// ------------------------------------------------ loading the video
void MainWindow::pickInput()
{
    const QString path = QFileDialog::getOpenFileName(this, "Choose a video", QString(), videoFilter());
    if (!path.isEmpty())
        setInput(QDir::toNativeSeparators(path));
}

void MainWindow::onInputEdited()
{
    QString path = inEdit_->text().trimmed();
    if (path.size() >= 2 && path.startsWith('"') && path.endsWith('"'))
        path = path.mid(1, path.size() - 2);
    if (!path.isEmpty() && (!media_ || !samePath(path, media_->path)))
        setInput(path);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() && !encoding_)
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty())
        setInput(QDir::toNativeSeparators(urls.first().toLocalFile()));
}

void MainWindow::setInput(const QString &path)
{
    if (path.isEmpty() || ffprobe_.isEmpty() || encoding_)
        return;
    inEdit_->setText(path);
    estimator_->stop();
    QString error;
    const std::optional<MediaInfo> media = probe(ffprobe_, path, &error);
    if (!media) {
        media_.reset();
        spinner_->stop();
        outcomeBar_->hide();
        loadPreview();
        settingsBox_->setEnabled(false);
        startBtn_->setEnabled(false);
        fitBtn_->setEnabled(false);
        srcInfo_->setText("⚠ Could not read the video: " + error);
        return;
    }

    media_ = media;
    estBps_.reset();
    estCache_.clear();
    fit_.reset();
    const MediaInfo &m = *media_;
    const QFileInfo file(path);
    outEdit_->setText(QDir::toNativeSeparators(file.path() + "/" + file.completeBaseName() + "_compressed." + fmt().ext));
    srcInfo_->setText(QString("Duration %1 · %2×%3 · %4 fps · audio %5 · %6")
                          .arg(fmtTime(m.duration))
                          .arg(m.width)
                          .arg(m.height)
                          .arg(m.fps, 0, 'f', 0)
                          .arg(m.hasAudio ? "yes" : "no")
                          .arg(fmtMb(double(m.size))));

    // The options offered depend on the video: no upscaling and no higher fps
    const int shortSide = std::min(m.width, m.height);
    programmatic_ = true;
    resCombo_->clear();
    resCombo_->addItem(QString("Original (%1p)").arg(shortSide), 0);
    for (int r : resolutionSteps())
        if (r < shortSide)
            resCombo_->addItem(QString("%1p").arg(r), r);
    fpsCombo_->clear();
    fpsCombo_->addItem(QString("Original (%1 fps)").arg(m.fps, 0, 'f', 0), 0);
    for (int f : fpsSteps())
        if (f < m.fps - 0.5)
            fpsCombo_->addItem(QString("%1 fps").arg(f), f);
    programmatic_ = false;
    updateAudioWidgets();

    settingsBox_->setEnabled(true);
    openBtn_->setEnabled(false);
    outcomeBar_->hide();
    loadPreview();
    onSettingsChanged();
}

// Shows a frame of the source video (at 10% of its length, so it is rarely a black intro frame).
void MainWindow::loadPreview()
{
    ++previewGen_;
    if (previewProc_) {
        previewProc_->disconnect(this);
        previewProc_->kill();
        previewProc_->waitForFinished(1000);
        previewProc_->deleteLater();
        previewProc_ = nullptr;
    }
    preview_->hide();
    preview_->clear();
    if (!media_ || ffmpeg_.isEmpty())
        return;

    // rendered at the screen's pixel density, so it stays sharp on scaled displays
    const qreal dpr = devicePixelRatioF();
    const int width = qRound(PREVIEW_WIDTH * dpr), height = qRound(PREVIEW_HEIGHT * dpr);
    const double at = std::min(media_->duration * 0.1, 10.0);
    const int gen = previewGen_;
    auto *proc = new QProcess(this);
    previewProc_ = proc;
    connect(proc, &QProcess::finished, this, [this, proc, gen, dpr, at](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (proc == previewProc_)
            previewProc_ = nullptr;
        if (gen != previewGen_ || status != QProcess::NormalExit || code != 0)
            return;
        QPixmap frame;
        if (!frame.loadFromData(proc->readAllStandardOutput(), "PNG"))
            return;
        frame.setDevicePixelRatio(dpr);
        preview_->setPixmap(frame);
        preview_->setToolTip(QString("Frame at %1").arg(fmtTime(at)));
        preview_->show();
    });
    proc->start(ffmpeg_, {"-v", "error", "-ss", QString::number(at, 'f', 3), "-i", media_->path,
                          "-map", QString("0:%1").arg(media_->videoIndex), "-frames:v", "1",
                          "-vf", QString("scale=w=%1:h=%2:force_original_aspect_ratio=decrease").arg(width).arg(height),
                          "-f", "image2pipe", "-c:v", "png", "-"});
}

// ------------------------------------------------ settings
const Format &MainWindow::fmt() const
{
    return formatByExt(formatCombo_->currentData().toString());
}

int MainWindow::quality() const
{
    return qualitySlider_->value();
}

RateMode MainWindow::rateMode() const
{
    return RateMode(rateCombo_->currentData().toInt());
}

RateControl MainWindow::rate() const
{
    return {rateMode(), rateMode() == RateMode::Quality ? quality() : bitrateSpin_->value()};
}

int MainWindow::keyint() const
{
    return keyintCombo_->currentData().toInt();
}

const AudioCodec *MainWindow::audioCodec() const
{
    if (!media_ || !media_->hasAudio)
        return nullptr;
    return audioCodecByName(acodecCombo_->currentData().toString());
}

int MainWindow::audioKbps() const
{
    return audioCodec() ? abitrateCombo_->currentData().toInt() : 0;
}

// GPU encoders for a codec that work on this PC, best first.
std::vector<const VideoCodec *> MainWindow::hwEncoders(const QString &family) const
{
    std::vector<const VideoCodec *> list;
    for (const VideoCodec &c : videoCodecs())
        if (c.family == family && c.hardware() && encoders_->available().contains(c.encoder))
            list.push_back(&c);
    std::stable_sort(list.begin(), list.end(),
                     [](const VideoCodec *a, const VideoCodec *b) { return vendorRank(a->vendor) < vendorRank(b->vendor); });
    return list;
}

QString MainWindow::deviceName(const QString &vendor) const
{
    const Vendor *v = vendorByKey(vendor);
    return gpus_.value(vendor, v ? v->genericName : vendor);
}

// Offers only the codecs that the chosen format accepts and this PC can encode.
void MainWindow::refreshCodecs()
{
    const Format &f = fmt();
    const QSet<QString> &available = encoders_->available();
    const QString prevAudio = acodecCombo_->currentData().toString();
    programmatic_ = true;

    ComboItems familyItems;
    QStringList familyKeys;
    for (const Family &family : families()) {
        const bool usable = std::any_of(videoCodecs().begin(), videoCodecs().end(), [&](const VideoCodec &c) {
            return c.family == family.key && available.contains(c.encoder);
        });
        if (!f.video.contains(family.key) || !usable)
            continue;
        familyItems.append({family.note.isEmpty() ? family.name : family.name + " (" + family.note + ")", family.key});
        familyKeys << family.key;
    }
    fillCombo(vcodecCombo_, familyItems);
    for (int i = 0; i < familyKeys.size(); ++i) {
        QStringList gpus;
        for (const VideoCodec *c : hwEncoders(familyKeys[i]))
            gpus << deviceName(c->vendor);
        vcodecCombo_->setItemIcon(i, gpus.isEmpty() ? noIcon_ : hwIcon_);
        vcodecCombo_->setItemData(i, gpus.isEmpty() ? QString("Software encoding only (CPU)")
                                                    : "Hardware acceleration available on: " + gpus.join(", "),
                                  Qt::ToolTipRole);
    }

    ComboItems audioItems;
    for (const AudioCodec &a : audioCodecs())
        if (f.audio.contains(a.name) && available.contains(a.encoder))
            audioItems.append({a.label, a.name});
    audioItems.append({"No audio", QString()});
    fillCombo(acodecCombo_, audioItems);

    updateEncoder();
    if (acodecCombo_->currentData().toString() != prevAudio)
        onAudioCodecChanged();
    programmatic_ = false;
}

// Picks the encoder from codec, acceleration mode and device, and states clearly what will be used.
void MainWindow::updateEncoder()
{
    const QString family = vcodecCombo_->currentData().toString();
    const QString mode = accelCombo_->currentData().toString();
    std::vector<const VideoCodec *> hardware;
    if (mode != "off")
        hardware = hwEncoders(family);
    const VideoCodec *software = nullptr;
    for (const VideoCodec &c : videoCodecs())
        if (c.family == family && !c.hardware() && encoders_->available().contains(c.encoder)) {
            software = &c;
            break;
        }

    // the device menu always shows what will be used, but can be changed only in manual mode
    ComboItems devices;
    for (const VideoCodec *c : hardware)
        devices.append({deviceName(c->vendor) + " (" + c->label + ")", c->vendor});
    if (devices.isEmpty())
        devices.append({"CPU: " + cpu_, QString()});
    fillCombo(deviceCombo_, devices, mode == "manual");
    deviceCombo_->setEnabled(mode == "manual" && !hardware.empty());
    const QString vendor = deviceCombo_->currentData().toString();
    const VideoCodec *codec = software;
    for (const VideoCodec *c : hardware)
        if (c->vendor == vendor) {
            codec = c;
            break;
        }

    const QString name = familyName(family).toHtmlEscaped();
    QString text;
    if (!codec) {
        text = QString("⚠ No %1encoder for <b>%2</b> on this PC.").arg(mode == "off" ? "software " : "", name);
    } else {
        QString accel, device;
        if (codec->hardware()) {
            accel = "<b>yes</b>";
            device = "<b>" + deviceName(codec->vendor).toHtmlEscaped() + "</b>";
        } else {
            const QString why = mode == "off" ? QString() : " (no GPU in this PC can encode " + name + ")";
            accel = "<b>no</b>" + why;
            device = "<b>CPU</b> " + cpu_.toHtmlEscaped();
        }
        const QList<QPair<QString, QString>> rows = {{"Codec:", "<b>" + name + "</b>"},
                                                     {"Hardware acceleration:", accel},
                                                     {"Device:", device},
                                                     {"Encoder:", codec->label.toHtmlEscaped()}};
        text = "<table cellspacing='0' cellpadding='1'>";
        for (const auto &[label, value] : rows)
            text += "<tr><td>" + label + "&nbsp;&nbsp;</td><td>" + value + "</td></tr>";
        text += "</table>";
    }
    encoderLabel_->setText(text);
    setCodec(codec);
}

// Plain-text version of the encoder summary, for the log.
QString MainWindow::encoderSummary() const
{
    const QString device = codec_->hardware() ? deviceName(codec_->vendor) + ", hardware" : QString("CPU, software");
    return QString("%1 (%2 on %3)").arg(familyName(codec_->family), codec_->label, device);
}

void MainWindow::onFormatChanged()
{
    const QString out = outEdit_->text().trimmed();
    if (!out.isEmpty()) {
        const QString suffix = QFileInfo(out).suffix().toLower();
        if (std::any_of(formats().begin(), formats().end(), [&](const Format &f) { return f.ext == suffix; }))
            outEdit_->setText(withSuffix(out, fmt().ext));
    }
    refreshCodecs();
    onSettingsChanged();   // the format can change the video args (e.g. the HEVC tag)
}

void MainWindow::setCodec(const VideoCodec *codec)
{
    const bool changed = codec != codec_;
    codec_ = codec;
    if (changed && codec) {
        const QSignalBlocker blocker(qualitySlider_);
        qualitySlider_->setRange(codec->quality.best, codec->quality.worst);
        qualitySlider_->setValue(codec->quality.def);
        speedCombo_->setEnabled(!codec->speeds.isEmpty());
    }
    updateRateModes();   // also when the codec is the same: a GPU test may have just finished
    if (changed)
        onSettingsChanged();
}

// Offers the rate control modes the encoder has; a GPU's ones only if they passed the test on this PC.
void MainWindow::updateRateModes()
{
    auto *model = qobject_cast<QStandardItemModel *>(rateCombo_->model());
    for (int i = 0; i < rateCombo_->count(); ++i) {
        const bool ok = !codec_ || encoders_->supports(*codec_, RateMode(rateCombo_->itemData(i).toInt()));
        model->item(i)->setEnabled(ok);
        const QString tip = RATE_TIPS[i];
        rateCombo_->setItemData(i, ok ? tip : tip + "\n\nNot available with " + codec_->label
                                                   + (codec_->hardware() ? " on this PC." : "."),
                                Qt::ToolTipRole);
    }
    if (codec_ && !encoders_->supports(*codec_, rateMode())) {
        // e.g. CBR, then a codec whose encoder hasn't it: the closest mode it has (this updates the estimate)
        const bool vbr = rateMode() == RateMode::Cbr && encoders_->supports(*codec_, RateMode::Vbr);
        rateCombo_->setCurrentIndex(rateCombo_->findData(int(vbr ? RateMode::Vbr : RateMode::Quality)));
    }
}

void MainWindow::onRateModeChanged()
{
    const bool quality = rateMode() == RateMode::Quality;
    // the bitrate starts from the one of the current estimate, so that the size stays about the same
    if (!quality && estBps_) {
        const QSignalBlocker blocker(bitrateSpin_);
        bitrateSpin_->setValue(int(std::lround(*estBps_ * 8 / 1000 / 10)) * 10);
    }
    bitrateSpin_->setVisible(!quality);
    settingsForm_->setRowVisible(sliderRow_, quality);
    settingsForm_->setRowVisible(qualityLabel_, quality);
    fitBtn_->setText(quality ? "Fit quality to limit" : "Fit bitrate to limit");
    onSettingsChanged();
}

void MainWindow::onAudioCodecChanged()
{
    updateAudioWidgets();
    onSettingsChanged();
}

void MainWindow::updateAudioWidgets()
{
    const bool hasAudio = media_ && media_->hasAudio;
    acodecCombo_->setEnabled(hasAudio);
    abitrateCombo_->setEnabled(hasAudio && !acodecCombo_->currentData().toString().isEmpty());
}

QStringList MainWindow::videoArgs() const
{
    const MediaInfo &m = *media_;
    const VideoCodec &codec = *codec_;
    const Format &f = fmt();
    QStringList filters;
    const int target = resCombo_->currentData().toInt();
    if (target)   // scale the short side, so it also works with vertical videos
        filters << (m.width >= m.height ? QString("scale=-2:%1").arg(target) : QString("scale=%1:-2").arg(target));
    else if (m.width % 2 || m.height % 2)
        filters << "scale=trunc(iw/2)*2:trunc(ih/2)*2";   // 4:2:0 video needs even dimensions
    const int fps = fpsCombo_->currentData().toInt();
    if (fps)
        filters << QString("fps=%1").arg(fps);
    QStringList args{"-map", QString("0:%1").arg(m.videoIndex)};
    if (!filters.isEmpty())
        args << "-vf" << filters.join(',');
    // same keyframe spacing in seconds for every encoder (their defaults go from 0.4 to 10 s)
    const double outFps = fps ? fps : (m.fps > 0 ? m.fps : 30.0);
    const int gopFrames = std::max(1, int(std::nearbyint(outFps * keyint())));
    args += codec.args(rate(), speedCombo_->currentData().toString(), std::pair{gopFrames, keyint()});
    if (codec.family == "hevc" && (f.ext == "mp4" || f.ext == "mov"))
        args << "-tag:v" << "hvc1";   // needed by Apple players
    else if (codec.family == "mpeg4" && f.ext == "avi")
        args << "-tag:v" << "XVID";   // recognized by old players
    return args;
}

void MainWindow::updateQualityLabel()
{
    if (!codec_) {
        qualityLabel_->clear();
        return;
    }
    qualityLabel_->setText(QString("%1 quality (%2 %3)")
                               .arg(capitalized(qualityName(quality(), codec_->quality)), codec_->qName)
                               .arg(codec_->shownQuality(quality())));
}

void MainWindow::onSettingsChanged()
{
    if (!programmatic_)
        fit_.reset();   // a manual change interrupts the search
    updateQualityLabel();
    if (!media_ || encoding_)
        return;
    estimator_->stop();
    estTimer_->stop();
    fitBtn_->setEnabled(false);
    startBtn_->setEnabled(codec_ != nullptr);
    if (!codec_) {
        estBps_.reset();
        spinner_->stop();
        sizeLabel_->setText("—");
        sizeDetail_->setText("⚠ No video encoder available for this format.");
        updateLimitStatus();
        return;
    }
    if (rateMode() != RateMode::Quality) {
        // the size follows from the bitrate: nothing to encode
        pendingKey_.reset();
        onEstimateDone(bitrateSpin_->value() * 1000.0 / 8, false);
        return;
    }
    sizeLabel_->setStyleSheet("color: gray;");
    sizeDetail_->setText("Updating estimate…");
    spinner_->start();
    estTimer_->start();
}

// ------------------------------------------------ size estimate
void MainWindow::runEstimate()
{
    if (!media_ || encoding_ || !codec_ || rateMode() != RateMode::Quality)
        return;
    const double duration = media_->duration;
    QList<QPair<double, double>> segments;
    if (duration <= WHOLE_IF_SHORTER) {
        segments.append({0.0, duration});
    } else {
        for (int i = 0; i < SAMPLE_COUNT; ++i)
            segments.append({std::max(0.0, duration * (i + 0.5) / SAMPLE_COUNT - SAMPLE_LEN / 2), SAMPLE_LEN});
    }
    const QStringList args = videoArgs();
    const QString key = args.join(QChar(0x1F));
    if (estCache_.contains(key)) {
        pendingKey_.reset();
        const auto [bps, exact] = estCache_.value(key);
        onEstimateDone(bps, exact);
        return;
    }
    pendingKey_ = key;
    sizeDetail_->setText(fit_ ? "Searching for the best quality under the limit…"
                              : "Estimating (encoding a few short samples)…");
    estimator_->start(media_->path, segments, args, fmt().ext, keyint());
}

double MainWindow::estimatedBytes() const
{
    return (estBps_.value_or(0) + audioKbps() * 1000.0 / 8) * media_->duration * CONTAINER_OVERHEAD;
}

void MainWindow::onEstimateDone(double bps, bool exact)
{
    if (pendingKey_) {
        estCache_.insert(*pendingKey_, {bps, exact});
        pendingKey_.reset();
    }
    estBps_ = bps;
    estExact_ = exact;
    spinner_->stop();
    const double total = estimatedBytes();
    const QString share = QString::number(total / double(media_->size) * 100, 'f', 0);
    sizeLabel_->setStyleSheet(QString());
    sizeLabel_->setText((exact ? "" : "≈ ") + fmtMb(total));
    if (rateMode() == RateMode::Quality) {
        const QString accuracy = exact ? "exact estimate" : "sample-based estimate, roughly ±10%";
        sizeDetail_->setText(QString("Video ≈ %1 kbps · audio %2 kbps · %3% of the original (%4)")
                                 .arg(bps * 8 / 1000, 0, 'f', 0)
                                 .arg(audioKbps())
                                 .arg(share, accuracy));
    } else {
        const bool variable = rateMode() == RateMode::Vbr;
        sizeDetail_->setText(QString("Video %1 kbps %2 · audio %3 kbps · %4% of the original (from the bitrate%5)")
                                 .arg(bitrateSpin_->value())
                                 .arg(variable ? "on average" : "constant")
                                 .arg(audioKbps())
                                 .arg(share, variable ? "; simple videos come out smaller" : ""));
    }
    updateLimitStatus();
    if (fit_)
        fitStep();
    fitBtn_->setEnabled(!fit_);
}

void MainWindow::onEstimateFailed(const QString &message)
{
    fit_.reset();
    pendingKey_.reset();
    estBps_.reset();
    spinner_->stop();
    sizeLabel_->setStyleSheet(QString());
    sizeLabel_->setText("—");
    sizeDetail_->setText("⚠ Estimate failed: " + message);
    updateLimitStatus();
}

void MainWindow::updateLimitStatus()
{
    if (!media_ || !estBps_) {
        limitStatus_->clear();
        return;
    }
    if (estimatedBytes() <= limitSpin_->value() * MB) {
        limitStatus_->setText("✔ Under the limit");
        limitStatus_->setStyleSheet("color: #2e7d32; font-weight: bold;");
    } else {
        limitStatus_->setText("✖ Over the limit");
        limitStatus_->setStyleSheet("color: #c62828; font-weight: bold;");
    }
}

// ------------------------------------------------ fit to limit
// Searches for the best quality value that stays under the limit; with a bitrate, computes it.
void MainWindow::fitToLimit()
{
    if (!media_ || !estBps_ || !codec_)
        return;
    // GPU encoders can end up to ~10% above an average bitrate (CBR and software encoders: a few %)
    const double margin = rateMode() == RateMode::Vbr && codec_->hardware() ? GPU_VBR_LIMIT_MARGIN : LIMIT_MARGIN;
    const double target = limitSpin_->value() * MB * margin;
    const double audioBytes = audioKbps() * 1000.0 / 8 * media_->duration * CONTAINER_OVERHEAD;
    if (audioBytes >= target) {
        showMessage(QMessageBox::Information, "Limit too low",
                    "The audio alone already exceeds the limit: lower or remove it.");
        return;
    }
    if (rateMode() != RateMode::Quality) {
        const double videoBytesPerSecond = (target - audioBytes) / (media_->duration * CONTAINER_OVERHEAD);
        const int kbps = int(videoBytesPerSecond * 8 / 1000 / 10) * 10;
        if (kbps < MIN_VIDEO_KBPS) {
            showMessage(QMessageBox::Information, "Limit not reachable",
                        QString("The video would need less than %1 kbps.\n"
                                "Try reducing the resolution, frame rate or audio.").arg(MIN_VIDEO_KBPS));
            return;
        }
        setBitrate(std::min(kbps, MAX_VIDEO_KBPS));
        return;
    }
    fit_ = Fit{codec_->quality.best, codec_->quality.worst, std::nullopt, {}, 8, target};
    fitBtn_->setEnabled(false);
    fitStep();
}

void MainWindow::setQuality(int value)
{
    programmatic_ = true;
    qualitySlider_->setValue(value);
    programmatic_ = false;
}

void MainWindow::setBitrate(int kbps)
{
    programmatic_ = true;
    bitrateSpin_->setValue(kbps);
    programmatic_ = false;
}

void MainWindow::fitStep()
{
    Fit &f = *fit_;
    const int worst = codec_->quality.worst;
    const int q = quality();
    const double size = estimatedBytes();
    f.points.emplace_back(q, size);
    if (size <= f.target) {
        f.best = f.best ? std::min(*f.best, q) : q;
        f.hi = std::min(f.hi, q - 1);   // try a higher quality
    } else {
        f.lo = std::max(f.lo, q + 1);   // needs more compression
    }
    --f.left;

    if (f.lo > f.hi || f.left <= 0) {
        const Fit result = f;
        fit_.reset();
        if (result.best) {
            if (*result.best != q)
                setQuality(*result.best);
        } else if (result.lo > worst) {
            showMessage(QMessageBox::Information, "Limit not reachable",
                        "Even at the lowest quality the file exceeds the limit.\n"
                        "Try reducing the resolution, frame rate or audio.");
        } else {
            setQuality(std::min(result.lo, worst));
        }
        return;
    }

    // guess the next value: size drops roughly exponentially as the quality value grows
    double slope = -std::log(2.0) / codec_->halving;
    if (f.points.size() >= 2) {
        const auto [q1, s1] = f.points[f.points.size() - 2];
        const auto [q2, s2] = f.points.back();
        if (q1 != q2 && s1 > 0 && s2 > 0) {
            const double measured = (std::log(s2) - std::log(s1)) / (q2 - q1);
            if (measured < 0)
                slope = measured;
        }
    }
    const int next = int(std::nearbyint(q + (std::log(f.target) - std::log(size)) / slope));
    setQuality(std::max(f.lo, std::min(f.hi, next)));
}

// ------------------------------------------------ output
void MainWindow::pickOutput()
{
    const Format &f = fmt();
    QString path = QFileDialog::getSaveFileName(this, "Save as", outEdit_->text(), QString("%1 (*.%2)").arg(f.label, f.ext));
    if (path.isEmpty())
        return;
    if (!path.endsWith("." + f.ext, Qt::CaseInsensitive))
        path += "." + f.ext;
    outEdit_->setText(QDir::toNativeSeparators(path));
}

void MainWindow::openFolder()
{
    if (!currentOutput_.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(currentOutput_).absolutePath()));
}

// ------------------------------------------------ compression
void MainWindow::start()
{
    if (!media_ || !codec_)
        return;
    const Format &f = fmt();
    const QString input = media_->path;
    QString out = outEdit_->text().trimmed();
    if (out.size() >= 2 && out.startsWith('"') && out.endsWith('"'))
        out = out.mid(1, out.size() - 2);
    if (out.isEmpty()) {
        showMessage(QMessageBox::Warning, "Error", "Choose where to save the file.");
        return;
    }
    if (QFileInfo(out).suffix().toLower() != f.ext) {
        out = withSuffix(out, f.ext);
        outEdit_->setText(out);
    }
    if (samePath(input, out)) {
        showMessage(QMessageBox::Warning, "Error", "The output file must be different from the input file.");
        return;
    }

    resumeEstimate_ = spinner_->isSpinning();   // estimated again when the compression ends
    estTimer_->stop();
    estimator_->stop();
    spinner_->stop();
    const AudioCodec *acodec = audioCodec();
    const int kbps = audioKbps();
    // put the seek index at the start of the file, so players can jump anywhere right away
    // (also when the file is streamed or opened over the network)
    QStringList index;
    if (f.ext == "mp4" || f.ext == "mov")
        index = {"-movflags", "+faststart"};
    else if (f.ext == "mkv" || f.ext == "webm")
        index = {"-cues_to_front", "1"};
    QStringList args{"-y", "-hide_banner", "-nostats", "-progress", "pipe:1", "-i", input};
    args += videoArgs();
    args += acodec ? QStringList{"-map", "0:a:0"} + acodec->args(kbps) : QStringList{"-an"};
    args += index;
    args << out;

    cancelled_ = false;
    currentOutput_ = out;
    outBuf_.clear();
    stderrTail_.clear();
    progress_->setValue(0);
    log_->clear();
    outcomeBar_->hide();
    const QString audio = acodec ? QString("%1 %2 kbps").arg(acodec->label).arg(kbps) : QString("no audio");
    const QString speed = codec_->speeds.isEmpty() ? QString() : " · " + speedCombo_->currentText().toLower();
    const QString rateText = rateMode() == RateMode::Quality
                                 ? QString("%1 %2").arg(codec_->qName).arg(codec_->shownQuality(quality()))
                                 : QString("%1 %2 kbps").arg(rateModeName(rateMode())).arg(bitrateSpin_->value());
    log_->appendPlainText(QString("Compressing: %1 · %2 · %3 · %4 · %5 · %6%7 · keyframes every %8 s")
                              .arg(f.label, encoderSummary(), rateText, resCombo_->currentText(),
                                   fpsCombo_->currentText(), audio, speed)
                              .arg(keyint()));
    encodeClock_.start();
    setRunning(true);
    if (proc_)
        proc_->deleteLater();
    proc_ = new QProcess(this);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &MainWindow::onStdout);
    connect(proc_, &QProcess::readyReadStandardError, this, &MainWindow::onStderr);
    connect(proc_, &QProcess::finished, this, &MainWindow::onFinished);
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onFinished(-1, QProcess::CrashExit);
    });
    proc_->start(ffmpeg_, args);
}

void MainWindow::onStdout()
{
    outBuf_ += QString::fromUtf8(proc_->readAllStandardOutput());
    QStringList lines = outBuf_.split('\n');
    outBuf_ = lines.takeLast();
    for (QString line : lines) {
        line = line.trimmed();
        if (!line.startsWith("out_time_us=") && !line.startsWith("out_time_ms="))
            continue;
        bool ok = false;
        const qint64 value = line.section('=', 1).toLongLong(&ok);
        if (ok && value >= 0) {
            const double frac = std::min(value / 1e6 / media_->duration, 1.0);
            progress_->setValue(int(frac * 100));
        }
    }
}

void MainWindow::onStderr()
{
    stderrTail_ += QString::fromUtf8(proc_->readAllStandardError()).split(QRegularExpression("\r?\n|\r"),
                                                                          Qt::SkipEmptyParts);
    if (stderrTail_.size() > 30)
        stderrTail_ = stderrTail_.mid(stderrTail_.size() - 30);
}

void MainWindow::onFinished(int code, QProcess::ExitStatus status)
{
    if (cancelled_)
        return;
    setRunning(false);
    if (status != QProcess::NormalExit || code != 0) {
        log_->appendPlainText(stderrTail_.join('\n'));
        const QString reason = stderrTail_.isEmpty() ? QString("FFmpeg could not be started.")
                                                     : stderrTail_.last().trimmed();
        showOutcome(Outcome::Failed, reason.toHtmlEscaped() + "<br>The log below has the details.");
        return;
    }
    progress_->setValue(100);
    const double size = double(QFileInfo(currentOutput_).size());
    QString msg = QString("Done! %1 → %2").arg(fmtMb(size), currentOutput_);
    if (estBps_)
        msg += QString("\n(estimate was %1)").arg(fmtMb(estimatedBytes()));
    log_->appendPlainText(msg);
    openBtn_->setEnabled(true);

    const double original = double(media_->size);
    const QString change = size <= original
                               ? QString("%1% smaller than the original").arg((1 - size / original) * 100, 0, 'f', 0)
                               : QString("%1% bigger than the original").arg((size / original - 1) * 100, 0, 'f', 0);
    // (non-breaking spaces: "3.5 MB" and "took 0:12" are never split across two lines)
    const QString what = QString("<b>%1</b> · %2, %3 · took&nbsp;%4")
                             .arg(QFileInfo(currentOutput_).fileName().toHtmlEscaped(),
                                  fmtMb(size).replace(' ', "&nbsp;"), change,
                                  fmtTime(encodeClock_.elapsed() / 1000.0));
    if (size > limitSpin_->value() * MB) {
        const bool quality = rateMode() == RateMode::Quality;
        showOutcome(Outcome::OverLimit,
                    what + QString("<br>It is over the %1 MB limit: use “%2” or lower the %3 a bit and try again.")
                               .arg(limitSpin_->value(), 0, 'f', 1)
                               .arg(fitBtn_->text(), quality ? "quality" : "bitrate"));
    } else {
        showOutcome(Outcome::Done, what);
    }
}

// Makes the window as tall as its content, if the screen has room (it never shrinks it): otherwise the
// content scrolls.
void MainWindow::fitToContent()
{
    QWidget *content = scroll_->widget();
    content->layout()->activate();
    int needed = content->heightForWidth(width());   // texts that wrap need more lines in a narrow window
    if (needed < 0)
        needed = content->sizeHint().height();
    const QRect room = screen()->availableGeometry();
    const int titleBar = isVisible() ? frameGeometry().height() - height() : 40;
    const int wanted = std::min(needed, room.height() - titleBar);
    if (wanted <= height())
        return;
    resize(width(), wanted);
    if (isVisible() && frameGeometry().bottom() > room.bottom())   // keep it all on the screen
        move(x(), std::max(room.top(), room.bottom() - frameGeometry().height()));
}

// Shows how the compression ended in a colored box under the buttons, and draws attention to the window
// (taskbar button on Windows, Dock icon on macOS) if it is in the background.
void MainWindow::showOutcome(Outcome outcome, const QString &text)
{
    static const struct {
        const char *title;
        const char *color;
        const char *rgb;
        char symbol;
    } looks[] = {
        {"Compression complete", "#2e7d32", "46, 125, 50", 'v'},
        {"Compression complete, but over the limit", "#d07a00", "232, 137, 12", '!'},
        {"Compression failed", "#c62828", "198, 40, 40", 'x'},
    };
    const auto &look = looks[int(outcome)];
    outcomeBar_->setStyleSheet(QString("#outcomeBar { background: rgba(%1, 0.13); border: 1px solid rgba(%1, 0.7); "
                                       "border-radius: 6px; }")
                                   .arg(look.rgb));
    const int size = outcomeTitle_->fontMetrics().height() * 2;
    outcomeIcon_->setPixmap(badge(QColor(look.color), look.symbol, size, devicePixelRatioF()));
    outcomeTitle_->setText(look.title);
    outcomeText_->setText(text);
    playBtn_->setVisible(outcome != Outcome::Failed);
    openBtn_->setVisible(outcome != Outcome::Failed);
    outcomeBar_->show();
    QTimer::singleShot(0, this, [this] {   // once the box has its size
        fitToContent();
        scroll_->ensureWidgetVisible(outcomeBar_);
    });
    QApplication::alert(this);
}

void MainWindow::cancel()
{
    cancelled_ = true;
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        proc_->kill();
        proc_->waitForFinished(3000);
    }
    if (!currentOutput_.isEmpty() && QFileInfo::exists(currentOutput_))
        QFile::remove(currentOutput_);
    progress_->setValue(0);
    log_->appendPlainText("Cancelled.");
    setRunning(false);
}

void MainWindow::setRunning(bool running)
{
    encoding_ = running;
    startBtn_->setEnabled(!running);
    cancelBtn_->setEnabled(running);
    fitBtn_->setEnabled(!running && estBps_ && !fit_);
    if (running)
        openBtn_->setEnabled(false);
    for (QWidget *w : std::initializer_list<QWidget *>{inEdit_, inBtn_, outEdit_, outBtn_, settingsBox_, limitSpin_})
        w->setEnabled(!running);
    if (!running && resumeEstimate_) {   // the compression interrupted an estimate: finish it
        resumeEstimate_ = false;
        onSettingsChanged();
    }
}

// ------------------------------------------------ updates
void MainWindow::checkForUpdates(bool manual)
{
    manualUpdateCheck_ = manual;
    if (manual) {
        checkNowBtn_->setEnabled(false);
        updateStatus_->setText("Checking…");
    }
    updater_->check();
}

void MainWindow::onUpdateAvailable(const UpdateInfo &info)
{
    appSettings()->setValue("updates/lastCheck", QDateTime::currentDateTimeUtc());
    pendingUpdate_ = info;
    updateText_->setText(QString("<b>Video Compressor %1</b> is available (you have %2).")
                             .arg(info.version.toHtmlEscaped(), QCoreApplication::applicationVersion()));
    updateBtn_->setText(canSelfUpdate() ? "Update now" : "Download");
    updateBtn_->setEnabled(true);
    updateBar_->show();
    QTimer::singleShot(0, this, [this] {
        fitToContent();
        scroll_->ensureWidgetVisible(updateBar_);
    });
    if (manualUpdateCheck_)
        updateStatus_->clear();
    manualUpdateCheck_ = false;
    checkNowBtn_->setEnabled(true);
}

// The app updates itself only when it was installed with the Windows installer; otherwise
// (portable zip, macOS, Linux) "Download" opens the release page.
bool MainWindow::canSelfUpdate() const
{
#ifdef Q_OS_WIN
    return pendingUpdate_ && !pendingUpdate_->installerUrl.isEmpty()
           && QFileInfo::exists(QCoreApplication::applicationDirPath() + "/unins000.exe");
#else
    return false;
#endif
}

void MainWindow::startUpdate()
{
    if (!pendingUpdate_)
        return;
    if (!canSelfUpdate()) {
        QDesktopServices::openUrl(pendingUpdate_->page);
        return;
    }
    if (encoding_) {
        showMessage(QMessageBox::Information, "Update", "Wait for the compression to finish, then update.");
        return;
    }
    downloadingUpdate_ = true;
    updateBtn_->setEnabled(false);
    updateText_->setText(QString("Downloading Video Compressor %1…").arg(pendingUpdate_->version));
    updater_->download(*pendingUpdate_);
}

// Runs the new installer without its wizard (for the same user, or for all users, as before) and closes
// the app so that its files can be replaced; the installer starts the new version when it's done.
void MainWindow::installUpdate(const QString &installer)
{
    downloadingUpdate_ = false;
    const QString appDir = QDir::cleanPath(QCoreApplication::applicationDirPath());
    const QString userData = QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    const bool forCurrentUser = appDir.startsWith(userData, Qt::CaseInsensitive);
    const QStringList args{"/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS",
                           forCurrentUser ? "/CURRENTUSER" : "/ALLUSERS"};
    if (!QProcess::startDetached(installer, args)) {
        updateText_->setText("⚠ The installer could not be started.");
        updateBtn_->setEnabled(true);
        return;
    }
    close();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    estimator_->stop();
    updater_->cancelDownload();
    if (previewProc_)
        previewProc_->kill();
    if (proc_ && proc_->state() != QProcess::NotRunning)
        cancel();
    event->accept();
}
