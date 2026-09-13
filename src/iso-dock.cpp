#include "iso-dock.hpp"

#include "iso-settings.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>

namespace iso {

static QString defaultBasePath()
{
	// A folder of its own under the platform's video location, so a fresh install
	// has somewhere sane to write before anyone has touched the setting.
	const QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	return (videos.isEmpty() ? QDir::homePath() : videos) + "/ISO Recorder";
}

static bool muxableEncoderId(const std::string &id)
{
	const std::string codec = codecForEncoderId(id);
	return codec == "h264" || codec == "hevc" || codec == "prores";
}

static bool encoderRegistered(const std::string &id)
{
	for (size_t i = 0;; ++i) {
		const char *candidate = nullptr;
		if (!obs_enum_encoder_types(i, &candidate))
			break;
		if (candidate && id == candidate)
			return true;
	}
	return false;
}

static std::string firstEncoderWithPrefix(const char *prefix)
{
	const size_t len = strlen(prefix);
	for (size_t i = 0;; ++i) {
		const char *candidate = nullptr;
		if (!obs_enum_encoder_types(i, &candidate))
			break;
		if (candidate && strncmp(candidate, prefix, len) == 0)
			return candidate;
	}
	return {};
}

// OBS's Simple Output uses logical names ("apple_h264"); libobs only knows the
// real encoder ids, which macOS derives from the system at runtime. Map the
// logical name to a registered one, or fall back to software x264.
static std::string realEncoderId(const std::string &id)
{
	if (id == "apple_h264") {
		std::string hw =
			firstEncoderWithPrefix("com.apple.videotoolbox.videoencoder.ave.avc");
		if (hw.empty())
			hw = firstEncoderWithPrefix("com.apple.videotoolbox.videoencoder.");
		return hw.empty() ? std::string("obs_x264") : hw;
	}
	if (id == "apple_hevc") {
		std::string hw =
			firstEncoderWithPrefix("com.apple.videotoolbox.videoencoder.ave.hevc");
		if (hw.empty())
			hw = firstEncoderWithPrefix("com.apple.videotoolbox.videoencoder.hevc");
		return hw.empty() ? std::string("obs_x264") : hw;
	}
	return id;
}

// Only offer encoders this OBS actually registered. Hardcoded ids rot: OBS 32
// replaced "jim_nvenc" with "obs_nvenc_h264_tex", so a fixed list would offer a
// dead choice on a box that does have NVENC.
static std::string firstRegisteredEncoder(std::initializer_list<const char *> ids)
{
	for (const char *id : ids) {
		const std::string real = realEncoderId(id);
		if (encoderRegistered(real) && muxableEncoderId(real))
			return id;
	}
	return {};
}

static void addEncoder(QComboBox *box, const char *label, const std::string &id)
{
	if (id.empty())
		return;
	box->addItem(QObject::tr(label), QString::fromStdString(id));
}

static void fillEncoders(QComboBox *box)
{
	box->addItem(QObject::tr("Same as the stream"), "");
#ifdef __APPLE__
	addEncoder(box, "Apple (VideoToolbox)",
		   realEncoderId("apple_h264") == "obs_x264" ? std::string() : std::string("apple_h264"));
	addEncoder(box, "Apple (VideoToolbox) HEVC",
		   realEncoderId("apple_hevc") == "obs_x264" ? std::string() : std::string("apple_hevc"));
#else
	addEncoder(box, "NVIDIA (NVENC)",
		   firstRegisteredEncoder({"jim_nvenc", "obs_nvenc_h264_tex", "ffmpeg_nvenc"}));
	addEncoder(box, "NVIDIA (NVENC) HEVC",
		   firstRegisteredEncoder({"jim_hevc_nvenc", "obs_nvenc_hevc_tex"}));
	addEncoder(box, "Intel (Quick Sync)",
		   firstRegisteredEncoder({"obs_qsv11_v2", "obs_qsv11"}));
	addEncoder(box, "Intel (Quick Sync) HEVC", firstRegisteredEncoder({"obs_qsv11_hevc"}));
	addEncoder(box, "AMD (AMF)", firstRegisteredEncoder({"h264_texture_amf", "h264_amf"}));
#endif
	if (encoderRegistered("obs_x264") && muxableEncoderId("obs_x264"))
		addEncoder(box, "CPU (x264)", "obs_x264");
}

static std::string streamingEncoderId()
{
	obs_output_t *output = obs_frontend_get_streaming_output();
	if (!output)
		return {};
	obs_encoder_t *encoder = obs_output_get_video_encoder(output);
	const char *id = encoder ? obs_encoder_get_id(encoder) : nullptr;
	std::string result = id ? id : "";
	obs_output_release(output);
	return result;
}

static std::string platformDefaultEncoderId()
{
#ifdef __APPLE__
	return realEncoderId("apple_h264") == "obs_x264" ? std::string("obs_x264") : std::string("apple_h264");
#else
	const std::string hw = firstRegisteredEncoder(
		{"jim_nvenc", "obs_nvenc_h264_tex", "ffmpeg_nvenc", "obs_qsv11_v2", "obs_qsv11",
		 "h264_texture_amf", "h264_amf"});
	return hw.empty() ? std::string("obs_x264") : hw;
#endif
}

static uint64_t streamingBitrateBitsPerSec()
{
	obs_output_t *output = obs_frontend_get_streaming_output();
	if (!output)
		return 0;
	uint64_t bitrate = 0;
	obs_encoder_t *encoder = obs_output_get_video_encoder(output);
	if (encoder) {
		obs_data_t *settings = obs_encoder_get_settings(encoder);
		if (settings) {
			const int64_t kbps = obs_data_get_int(settings, "bitrate");
			if (kbps > 0)
				bitrate = (uint64_t)kbps * 1000ull;
			obs_data_release(settings);
		}
	}
	obs_output_release(output);
	return bitrate;
}

static SessionConfig resolvedConfig(SessionConfig cfg)
{
	if (cfg.videoEncoderId.empty()) {
		const std::string stream = streamingEncoderId();
		cfg.videoEncoderId = (!stream.empty() && muxableEncoderId(stream))
					     ? stream
					     : platformDefaultEncoderId();
	}
	cfg.videoEncoderId = realEncoderId(cfg.videoEncoderId);
	if (!encoderRegistered(cfg.videoEncoderId) || !muxableEncoderId(cfg.videoEncoderId))
		cfg.videoEncoderId = "obs_x264";
	const uint64_t bitrate = streamingBitrateBitsPerSec();
	if (bitrate > 0)
		cfg.bitrateBitsPerSec = bitrate;
	return cfg;
}

static QString formatElapsed(double seconds)
{
	const int total = (int)seconds;
	const int hours = total / 3600;
	const int minutes = (total % 3600) / 60;
	const int secs = total % 60;
	if (hours > 0)
		return QString::asprintf("%d:%02d:%02d", hours, minutes, secs);
	return QString::asprintf("%d:%02d", minutes, secs);
}

IsoDock::IsoDock(IsoSession *session, QWidget *parent) : QWidget(parent), session_(session)
{
	auto *layout = new QVBoxLayout(this);
	auto *pictureLabel = new QLabel(tr("Picture"), this);
	picture_ = new QListWidget(this);
	auto *soundLabel = new QLabel(tr("Sound"), this);
	sound_ = new QListWidget(this);

	auto *pathRow = new QHBoxLayout();
	path_ = new QLineEdit(this);
	browse_ = new QPushButton(tr("Browse…"), this);
	pathRow->addWidget(new QLabel(tr("Save to"), this));
	pathRow->addWidget(path_);
	pathRow->addWidget(browse_);

	encoder_ = new QComboBox(this);
	fillEncoders(encoder_);
	withStream_ = new QCheckBox(tr("Start and stop with the stream"), this);
	withStream_->setChecked(true);
	recordScene_ = new QCheckBox(tr("Also record the live scene"), this);

	record_ = new QPushButton(tr("Record"), this);
	newGame_ = new QPushButton(tr("New game"), this);
	newGame_->setStyleSheet(QStringLiteral(
		"QPushButton { background-color: #1d6fd0; color: white; border: 1px solid #1657a5;"
		" border-radius: 3px; padding: 4px 10px; }"
		"QPushButton:disabled { background-color: #b9c6d4; color: #f4f4f4;"
		" border-color: #a8b6c4; }"));
	newGame_->setToolTip(tr("Start a new game folder inside this session."));
	recording_ = new QLabel(this);
	recording_->setStyleSheet(QStringLiteral("color: #c0392b; font-weight: bold;"));
	recording_->hide();
	status_ = new QLabel(tr("Tick a source to keep."), this);
	// An unwrapped label takes its size hint from the whole sentence, which widens the
	// dock whenever a source name lands in the status text.
	status_->setWordWrap(true);

	auto *encoderLabel = new QLabel(tr("Video encoder"), this);
	auto *audioNote = new QLabel(tr("Audio is saved as 24-bit WAV."), this);
	audioNote->setStyleSheet(QStringLiteral("color: #808080;"));

	layout->addWidget(pictureLabel);
	layout->addWidget(picture_);
	layout->addWidget(soundLabel);
	layout->addWidget(sound_);
	layout->addLayout(pathRow);
	layout->addWidget(encoderLabel);
	layout->addWidget(encoder_);
	layout->addWidget(audioNote);
	layout->addWidget(withStream_);
	layout->addWidget(recordScene_);
	layout->addWidget(record_);
	layout->addWidget(newGame_);
	layout->addWidget(recording_);
	layout->addWidget(status_);

	applyConfig(loadConfig());
	// Resolving "Same as the stream" asks the frontend for its streaming output. At module
	// load OBS has not built its output handler yet, so querying it here dereferences null.
	// Defer the seed to the event loop, which runs once OBS is fully up; this also covers a
	// plugin (re)loaded after startup, when the handler already exists.
	QTimer::singleShot(0, this, [this] { session_->setConfig(sessionConfigFromWidgets()); });

	connect(record_, &QPushButton::clicked, this, &IsoDock::onRecordClicked);
	connect(newGame_, &QPushButton::clicked, this, &IsoDock::onNewGameClicked);
	connect(picture_, &QListWidget::itemChanged, this, &IsoDock::onItemChanged);
	connect(sound_, &QListWidget::itemChanged, this, &IsoDock::onItemChanged);
	connect(browse_, &QPushButton::clicked, this, &IsoDock::onBrowse);
	connect(path_, &QLineEdit::textChanged, this, &IsoDock::onConfigChanged);
	connect(encoder_, &QComboBox::currentIndexChanged, this, &IsoDock::onConfigChanged);
	connect(withStream_, &QCheckBox::toggled, this, &IsoDock::onConfigChanged);
	connect(recordScene_, &QCheckBox::toggled, this, &IsoDock::onConfigChanged);

	timer_ = new QTimer(this);
	timer_->setInterval(1000);
	connect(timer_, &QTimer::timeout, this, &IsoDock::onTick);
	timer_->start();
	refreshSources();
}

IsoDock::~IsoDock()
{
	releaseHeld();
}

void IsoDock::releaseHeld()
{
	for (obs_source_t *source : held_)
		if (source)
			obs_source_release(source);
	held_.clear();
}

void IsoDock::addSource(QListWidget *list, obs_source_t *source, Kind kind)
{
	auto *item = new QListWidgetItem(obs_source_get_name(source), list);
	item->setData(Qt::UserRole, QVariant::fromValue(reinterpret_cast<quintptr>(source)));
	item->setData(Qt::UserRole + 1, kind == Kind::Video ? 0 : 1);
	item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
	held_.push_back(obs_source_get_ref(source));
	applySourceState(item, source, kind);
}

void IsoDock::restoreArms(const std::vector<obs_source_t *> &pictureSources,
			  const std::vector<obs_source_t *> &soundSources)
{
	auto tickIfRemembered = [this](obs_source_t *source) {
		if (session_->isArmed(source))
			return;
		const std::string name = obs_source_get_name(source);
		if (std::find(armed_.begin(), armed_.end(), name) == armed_.end())
			return;
		std::string err;
		if (!session_->arm(source, &err))
			blog(LOG_WARNING, "[iso-recorder] could not tick %s again: %s", name.c_str(),
			     err.c_str());
	};
	for (obs_source_t *source : pictureSources)
		tickIfRemembered(source);
	for (obs_source_t *source : soundSources)
		tickIfRemembered(source);
}

void IsoDock::rememberArmed(const std::string &name, bool armed)
{
	auto it = std::find(armed_.begin(), armed_.end(), name);
	if (armed) {
		if (it == armed_.end())
			armed_.push_back(name);
	} else if (it != armed_.end()) {
		armed_.erase(it);
	}
	dirty_ = true;
}

void IsoDock::applySourceState(QListWidgetItem *item, obs_source_t *source, Kind kind)
{
	item->setCheckState(session_->isArmed(source) ? Qt::Checked : Qt::Unchecked);

	const QString name = QString::fromUtf8(obs_source_get_name(source));
	switch (session_->rowStateFor(source)) {
	case RowState::Recording:
		item->setText(QStringLiteral("● ") + name);
		item->setForeground(QColor(0xc0, 0x39, 0x2b));
		break;
	case RowState::Failed:
		item->setText(name + tr("  —  not recorded"));
		item->setForeground(QColor(0xb7, 0x79, 0x1f));
		break;
	default:
		item->setText(name);
		item->setForeground(QBrush());
		break;
	}

	QString tip;
	if (kind == Kind::Video)
		tip = QString::number(obs_source_get_width(source)) + "×" +
		      QString::number(obs_source_get_height(source));
	const std::string status = session_->statusFor(source);
	if (!status.empty()) {
		if (!tip.isEmpty())
			tip += "\n";
		tip += QString::fromStdString(status);
	}
	item->setToolTip(tip);
}

void IsoDock::refreshSources()
{
	std::vector<obs_source_t *> pictureSources;
	std::vector<obs_source_t *> soundSources;
	struct EnumCtx {
		std::vector<obs_source_t *> *picture;
		std::vector<obs_source_t *> *sound;
	} ctx{&pictureSources, &soundSources};
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *self = static_cast<EnumCtx *>(param);
			const uint32_t flags = obs_source_get_output_flags(source);
			if (!(flags & (OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO)))
				return true;
			if (flags & OBS_SOURCE_VIDEO)
				self->picture->push_back(source);
			else
				self->sound->push_back(source);
			return true;
		},
		&ctx);

	// Ticked sources are remembered per scene collection, and ending a session
	// clears the session's own set — so put the ticks back whenever the session is
	// idle with nothing ticked. Someone who unticks everything has an empty
	// remembered set, so nothing comes back on its own. A source only exists once
	// its collection has loaded, hence the wait for a non-empty list.
	const char *collection = obs_frontend_get_current_scene_collection();
	const std::string collectionName = collection ? collection : "";
	if (collectionName != armedCollection_) {
		armedCollection_ = collectionName;
		armed_ = loadArmedSources(armedCollection_);
	}
	if (!session_->active() && !session_->isArmedAny() && !armed_.empty() &&
	    !(pictureSources.empty() && soundSources.empty())) {
		restoreArms(pictureSources, soundSources);
		if (session_->armedVisualCount() > kEncoderWarnThreshold)
			blog(LOG_WARNING,
			     "[iso-recorder] the remembered set is %zu video files at once, more than this machine is expected to keep up with",
			     session_->armedVisualCount());
	}

	updating_ = true;
	const bool unchanged = pictureSources == lastPicture_ && soundSources == lastSound_;	{
		const QSignalBlocker pictureBlocker(picture_);
		const QSignalBlocker soundBlocker(sound_);
		if (unchanged) {
			for (int i = 0; i < picture_->count(); ++i) {
				auto *item = picture_->item(i);
				auto *source = reinterpret_cast<obs_source_t *>(
					item->data(Qt::UserRole).value<quintptr>());
				if (source)
					applySourceState(item, source, Kind::Video);
			}
			for (int i = 0; i < sound_->count(); ++i) {
				auto *item = sound_->item(i);
				auto *source = reinterpret_cast<obs_source_t *>(
					item->data(Qt::UserRole).value<quintptr>());
				if (source)
					applySourceState(item, source, Kind::Audio);
			}
		} else {
			releaseHeld();
			picture_->clear();
			sound_->clear();
			for (obs_source_t *source : pictureSources)
				addSource(picture_, source, Kind::Video);
			for (obs_source_t *source : soundSources)
				addSource(sound_, source, Kind::Audio);
			lastPicture_ = pictureSources;
			lastSound_ = soundSources;
		}
	}
	updating_ = false;

	const bool active = session_->active();
	const bool armed = session_->isArmedAny();
	path_->setEnabled(!active);
	browse_->setEnabled(!active);
	encoder_->setEnabled(!active);
	withStream_->setEnabled(!active);
	recordScene_->setEnabled(!active);
	record_->setText(active ? tr("Stop recording") : tr("Record"));
	if (active) {
		recording_->setText(tr("● Recording · ") + formatElapsed(session_->elapsedSeconds()));
		recording_->show();
	} else {
		recording_->hide();
	}
	newGame_->setEnabled(active);
	if (!active && !armed) {
		record_->setEnabled(false);
		record_->setToolTip(tr("Tick at least one source to record."));
		if (!transientStatus_)
			status_->setText(tr("Tick a source to record."));
	} else {
		record_->setEnabled(true);
		record_->setToolTip(QString());
	}
}

void IsoDock::onNewGameClicked()
{
	std::string error;
	if (!session_->newGame(&error)) {
		if (!error.empty())
			setStatus(tr("Could not start a new game: ") + QString::fromStdString(error));
		return;
	}
	setStatus(tr("Started ") + QString::fromStdString(session_->gameFolder()) + ".");
	refreshSources();
}

void IsoDock::onItemChanged(QListWidgetItem *item)
{
	if (updating_)
		return;
	auto *source = reinterpret_cast<obs_source_t *>(item->data(Qt::UserRole).value<quintptr>());
	if (!source)
		return;
	if (item->checkState() == Qt::Checked) {
		std::string err;
		if (!session_->arm(source, &err)) {
			setStatus(QString::fromStdString(err));
		} else {
			rememberArmed(obs_source_get_name(source), true);
			if (session_->armedVisualCount() > kEncoderWarnThreshold) {
				const auto choice = QMessageBox::warning(
					this, tr("ISO Recorder"),
					tr("That is more video recordings than this machine is expected to keep up with. Record anyway?"),
					QMessageBox::Yes | QMessageBox::No);
				if (choice != QMessageBox::Yes) {
					session_->disarm(source);
					rememberArmed(obs_source_get_name(source), false);
				}
			}
		}
	} else {
		session_->disarm(source);
		rememberArmed(obs_source_get_name(source), false);
	}
	QTimer::singleShot(0, this, &IsoDock::refreshSources);
}

void IsoDock::onBrowse()
{
	const QString dir = QFileDialog::getExistingDirectory(this, tr("Save recordings in"),
							      path_->text());
	if (!dir.isEmpty())
		path_->setText(dir);
}

void IsoDock::onRecordClicked()
{
	QString message;
	if (session_->active()) {
		session_->stop();
		message = tr("Stopped.");
	} else {
		SessionConfig cfg = sessionConfigFromWidgets();
		session_->setConfig(cfg);
		std::string why;
		std::string err;
		if (!session_->preflight(cfg, &why))
			message = QString::fromStdString(why);
		else if (!session_->start(cfg, &err)) {
			if (session_->active())
				message = tr("Recording, but some sources failed: ") +
					  QString::fromStdString(err);
			else
				message = QString::fromStdString(err);
		} else
			message = tr("Recording.");
	}
	refreshSources();
	setStatus(message);
}

void IsoDock::onTick()
{
	refreshSources();
	const std::string failure = session_->refreshManifest();
	transientStatus_ = false;
	if (!failure.empty())
		setStatus(tr("Source failed: ") + QString::fromStdString(failure));
	if (dirty_) {
		dirty_ = false;
		saveConfig(configFromWidgets());
		saveArmedSources(armedCollection_, armed_);
		obs_frontend_save();
	}
}

void IsoDock::setStatus(const QString &text)
{
	transientStatus_ = true;
	status_->setText(text);
}

void IsoDock::saveSettings()
{
	dirty_ = false;
	saveConfig(configFromWidgets());
	saveArmedSources(armedCollection_, armed_);
	obs_frontend_save();
}

void IsoDock::onConfigChanged()
{
	if (updating_)
		return;
	dirty_ = true;
	session_->setConfig(sessionConfigFromWidgets());
}

void IsoDock::applyConfig(const SessionConfig &cfg)
{
	updating_ = true;
	path_->setText(cfg.basePath.empty() ? defaultBasePath()
					    : QString::fromStdString(cfg.basePath));
	const int encoderIndex = encoder_->findData(QString::fromStdString(cfg.videoEncoderId));
	encoder_->setCurrentIndex(encoderIndex < 0 ? 0 : encoderIndex);
	withStream_->setChecked(cfg.withStream);
	recordScene_->setChecked(cfg.recordComposite);
	updating_ = false;
}

SessionConfig IsoDock::configFromWidgets() const
{
	SessionConfig cfg;
	cfg.basePath = path_->text().toStdString();
	cfg.videoEncoderId = encoder_->currentData().toString().toStdString();
	cfg.withStream = withStream_->isChecked();
	cfg.recordComposite = recordScene_->isChecked();
	return cfg;
}

SessionConfig IsoDock::sessionConfigFromWidgets() const
{
	return resolvedConfig(configFromWidgets());
}

} // namespace iso
