#include "mtgbinder-client.hpp"

#include <curl/curl.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/config-file.h>

#include "stb_image_write.h"

#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("mtgbinder-obs-plugin", "en-US")

namespace {

constexpr const char *kConfigSection = "MTGBinderOBS";
constexpr const char *kDefaultBaseUrl = "https://mtg-trade.fr";
constexpr int kJpegQuality = 85;
constexpr uint32_t kCaptureWidth = 1280;
constexpr uint32_t kCaptureHeight = 720;
constexpr int kTwitchPollIntervalMs = 7000;

struct RuntimeState {
	std::mutex configMutex;
	std::string baseUrl = kDefaultBaseUrl;
	std::string token;
	PluginConfig config;

	std::atomic<bool> connected{false};
	std::atomic<bool> uploadInFlight{false};
	std::atomic<uint64_t> frameIndex{0};
	std::atomic<int64_t> nextCaptureMs{0};

	/* Persisted: user clicked Stop; do not auto-start until Start again. */
	bool userStopped = false;
	/* True when the active channel came from OBS Twitch account Name. */
	bool channelFromTwitchAccount = false;
	std::string lastDetectedChannel;

	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stage = nullptr;
	uint32_t stageWidth = 0;
	uint32_t stageHeight = 0;
};

RuntimeState gState;
QPointer<QTimer> gTwitchPollTimer;
class ConnectDialog;
QPointer<ConnectDialog> gOpenDialog;

int64_t epochMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string normalizeChannelName(std::string channel)
{
	while (!channel.empty() &&
	       std::isspace(static_cast<unsigned char>(channel.front()))) {
		channel.erase(channel.begin());
	}
	while (!channel.empty() &&
	       std::isspace(static_cast<unsigned char>(channel.back()))) {
		channel.pop_back();
	}
	if (!channel.empty() && channel.front() == '@') {
		channel.erase(channel.begin());
	}
	for (char &c : channel) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return channel;
}

/* Reads only Auth/Type and Twitch/Name. Never Token, RefreshToken, ExpireTime, or service key. */
std::string detectTwitchChannelFromObs()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg) {
		return {};
	}

	const char *authType = config_get_string(cfg, "Auth", "Type");
	if (!authType || std::strcmp(authType, "Twitch") != 0) {
		return {};
	}

	const char *name = config_get_string(cfg, "Twitch", "Name");
	if (!name || !*name) {
		return {};
	}

	obs_service_t *service = obs_frontend_get_streaming_service();
	if (service) {
		obs_data_t *settings = obs_service_get_settings(service);
		if (settings) {
			const char *serviceName = obs_data_get_string(settings, "service");
			/* Only the "service" field; never read "key". Reject a non-Twitch platform name. */
			const bool reject = serviceName && *serviceName && std::strcmp(serviceName, "Twitch") != 0;
			obs_data_release(settings);
			if (reject) {
				return {};
			}
		}
	}

	return normalizeChannelName(name);
}

void saveConfig()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg) {
		return;
	}

	std::lock_guard<std::mutex> lock(gState.configMutex);
	config_set_string(cfg, kConfigSection, "base_url", gState.baseUrl.c_str());
	config_set_string(cfg, kConfigSection, "token", gState.token.c_str());
	config_set_string(cfg, kConfigSection, "channel", gState.config.channel.c_str());
	config_set_string(cfg, kConfigSection, "plan", gState.config.plan.c_str());
	config_set_string(cfg, kConfigSection, "tcg_type", gState.config.tcgType.c_str());
	config_set_int(cfg, kConfigSection, "frame_interval_secs", gState.config.frameIntervalSecs);
	config_set_string(cfg, kConfigSection, "endpoint", gState.config.endpoint.c_str());
	config_set_bool(cfg, kConfigSection, "user_stopped", gState.userStopped);
	config_set_bool(cfg, kConfigSection, "channel_from_twitch", gState.channelFromTwitchAccount);
	config_save_safe(cfg, "tmp", nullptr);
}

void loadConfig()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg) {
		return;
	}

	std::lock_guard<std::mutex> lock(gState.configMutex);
	const char *baseUrl = config_get_string(cfg, kConfigSection, "base_url");
	const char *token = config_get_string(cfg, kConfigSection, "token");
	const char *channel = config_get_string(cfg, kConfigSection, "channel");
	const char *plan = config_get_string(cfg, kConfigSection, "plan");
	const char *tcgType = config_get_string(cfg, kConfigSection, "tcg_type");
	const char *endpoint = config_get_string(cfg, kConfigSection, "endpoint");

	if (baseUrl && *baseUrl) {
		gState.baseUrl = baseUrl;
	}
	if (token) {
		gState.token = token;
	}
	if (channel) {
		gState.config.channel = normalizeChannelName(channel);
	}
	if (plan) {
		gState.config.plan = plan;
	}
	if (tcgType) {
		gState.config.tcgType = tcgType;
	}
	if (endpoint) {
		gState.config.endpoint = endpoint;
	}
	gState.config.frameIntervalSecs =
		static_cast<int>(config_get_int(cfg, kConfigSection, "frame_interval_secs"));
	if (gState.config.frameIntervalSecs <= 0) {
		gState.config.frameIntervalSecs = 25;
	}
	gState.userStopped = config_get_bool(cfg, kConfigSection, "user_stopped");
	gState.channelFromTwitchAccount = config_get_bool(cfg, kConfigSection, "channel_from_twitch");
	/* Capture only while OBS is streaming; resume via STREAMING_STARTED / tryResumeArmedCapture. */
	gState.connected = false;
}

void applyServerCadence(int intervalSecs, int retryAfterMs, int64_t ptsMs, bool accepted)
{
	if (intervalSecs > 0) {
		std::lock_guard<std::mutex> lock(gState.configMutex);
		gState.config.frameIntervalSecs = intervalSecs;
	}
	const int64_t now = epochMs();
	int64_t target = accepted && intervalSecs > 0 ? ptsMs + static_cast<int64_t>(intervalSecs) * 1000 : now;
	if (!accepted) {
		const int waitMs = retryAfterMs > 0 ? retryAfterMs : (intervalSecs > 0 ? intervalSecs * 1000 : 1000);
		target = now + waitMs;
	}
	const int64_t current = gState.nextCaptureMs.load();
	if (target > current) {
		gState.nextCaptureMs = target;
	}
}

void appendJpegChunk(void *context, void *data, int size)
{
	auto *out = static_cast<std::vector<uint8_t> *>(context);
	const auto *bytes = static_cast<const uint8_t *>(data);
	out->insert(out->end(), bytes, bytes + static_cast<size_t>(size));
}

std::vector<uint8_t> encodeJpegFromBgra(const std::vector<uint8_t> &bgra, uint32_t width, uint32_t height)
{
	const size_t pixels = static_cast<size_t>(width) * height;
	std::vector<uint8_t> rgb(pixels * 3);
	for (size_t i = 0; i < pixels; ++i) {
		rgb[i * 3 + 0] = bgra[i * 4 + 2];
		rgb[i * 3 + 1] = bgra[i * 4 + 1];
		rgb[i * 3 + 2] = bgra[i * 4 + 0];
	}

	std::vector<uint8_t> jpeg;
	stbi_write_jpg_to_func(
		appendJpegChunk,
		&jpeg,
		static_cast<int>(width),
		static_cast<int>(height),
		3,
		rgb.data(),
		kJpegQuality
	);
	return jpeg;
}

bool copyMainTexture(uint32_t width, uint32_t height, std::vector<uint8_t> &bgra)
{
	if (!gState.texrender) {
		gState.texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	}
	if (!gState.texrender) {
		return false;
	}

	/* Project the full OBS canvas into width x height (uniform scale, no crop). */
	obs_video_info ovi;
	float orthoWidth = static_cast<float>(width);
	float orthoHeight = static_cast<float>(height);
	if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
		orthoWidth = static_cast<float>(ovi.base_width);
		orthoHeight = static_cast<float>(ovi.base_height);
	}

	gs_texrender_reset(gState.texrender);
	if (!gs_texrender_begin(gState.texrender, width, height)) {
		return false;
	}
	gs_ortho(0.0f, orthoWidth, 0.0f, orthoHeight, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	obs_source_t *output = obs_get_output_source(0);
	if (output) {
		obs_source_video_render(output);
		obs_source_release(output);
	} else {
		obs_render_main_texture();
	}
	gs_blend_state_pop();

	gs_texrender_end(gState.texrender);

	gs_texture_t *texture = gs_texrender_get_texture(gState.texrender);
	if (!texture) {
		return false;
	}

	if (!gState.stage || gState.stageWidth != width || gState.stageHeight != height) {
		if (gState.stage) {
			gs_stagesurface_destroy(gState.stage);
		}
		gState.stage = gs_stagesurface_create(width, height, GS_BGRA);
		gState.stageWidth = width;
		gState.stageHeight = height;
	}
	if (!gState.stage) {
		return false;
	}

	gs_stage_texture(gState.stage, texture);
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(gState.stage, &data, &linesize)) {
		return false;
	}

	bgra.resize(static_cast<size_t>(width) * height * 4);
	for (uint32_t y = 0; y < height; ++y) {
		std::memcpy(
			bgra.data() + static_cast<size_t>(y) * width * 4,
			data + static_cast<size_t>(y) * linesize,
			static_cast<size_t>(width) * 4
		);
	}
	gs_stagesurface_unmap(gState.stage);
	return true;
}

void uploadFrameAsync(uint64_t frameIndex, int64_t ptsMs, uint32_t width, uint32_t height, std::vector<uint8_t> bgra)
{
	gState.uploadInFlight = true;
	std::thread([frameIndex, ptsMs, width, height, bgra = std::move(bgra)]() mutable {
		try {
			PluginConfig config;
			std::string token;
			std::string baseUrl;
			{
				std::lock_guard<std::mutex> lock(gState.configMutex);
				config = gState.config;
				token = gState.token;
				baseUrl = gState.baseUrl;
			}
			const auto jpeg = encodeJpegFromBgra(bgra, width, height);
			const auto result =
				MtgBinderClient(baseUrl).uploadFrame(config, token, frameIndex, ptsMs, width, height, jpeg);
			const int interval = jsonIntValue(result.body, "frame_interval_secs", 0);
			const int retryAfterMs = jsonIntValue(result.body, "retry_after_ms", 0);
			if (interval > 0 || result.httpStatus == 429) {
				applyServerCadence(interval, retryAfterMs, ptsMs, result.ok);
			}
			if (!result.ok && result.httpStatus != 429) {
				blog(LOG_WARNING, "[mtgbinder] upload failed: HTTP %ld %s", result.httpStatus,
				     result.body.c_str());
			} else if (!result.ok) {
				blog(LOG_INFO, "[mtgbinder] frame refused, next in %dms interval=%ds", retryAfterMs,
				     interval);
			} else {
				blog(LOG_INFO,
				     "[mtgbinder] upload ok frame=%llu pts=%lld size=%ux%u jpeg=%zuB",
				     static_cast<unsigned long long>(frameIndex),
				     static_cast<long long>(ptsMs),
				     width,
				     height,
				     jpeg.size());
			}
		} catch (const std::exception &e) {
			blog(LOG_WARNING, "[mtgbinder] upload error: %s", e.what());
		}
		gState.uploadInFlight = false;
	}).detach();
}

void onMainRender(void *, uint32_t, uint32_t)
{
	if (!gState.connected || gState.uploadInFlight) {
		return;
	}

	const int64_t now = epochMs();
	const int64_t next = gState.nextCaptureMs.load();
	if (now < next) {
		return;
	}

	int interval = 25;
	{
		std::lock_guard<std::mutex> lock(gState.configMutex);
		interval = gState.config.frameIntervalSecs > 0 ? gState.config.frameIntervalSecs : 25;
	}
	gState.nextCaptureMs = now + static_cast<int64_t>(interval) * 1000;

	std::vector<uint8_t> bgra;
	if (!copyMainTexture(kCaptureWidth, kCaptureHeight, bgra)) {
		return;
	}

	const uint64_t index = gState.frameIndex.fetch_add(1);
	uploadFrameAsync(index, now, kCaptureWidth, kCaptureHeight, std::move(bgra));
}

void startCaptureWithChannel(const std::string &channel, bool fromTwitchAccount)
{
	const bool live = obs_frontend_streaming_active();
	{
		std::lock_guard<std::mutex> lock(gState.configMutex);
		gState.token.clear();
		gState.config.channel = channel;
		gState.config.endpoint.clear();
		if (gState.config.frameIntervalSecs <= 0) {
			gState.config.frameIntervalSecs = 25;
		}
		gState.channelFromTwitchAccount = fromTwitchAccount;
		gState.userStopped = false;
		/* Frames are uploaded only while a stream is active. */
		gState.connected = live;
	}
	gState.nextCaptureMs = 0;
	saveConfig();
	if (live) {
		blog(LOG_INFO, "[mtgbinder] capture started for channel %s", channel.c_str());
	} else {
		blog(LOG_INFO, "[mtgbinder] channel %s armed; capture waits for stream start", channel.c_str());
	}
}

/* End of stream / OBS exit: stop uploads but keep the armed channel for the next go-live. */
void pauseCapture()
{
	bool wasConnected = false;
	{
		std::lock_guard<std::mutex> lock(gState.configMutex);
		wasConnected = gState.connected;
		gState.connected = false;
	}
	if (wasConnected) {
		blog(LOG_INFO, "[mtgbinder] capture paused (stream stopped)");
	}
}

void stopCapture(bool userInitiated)
{
	{
		std::lock_guard<std::mutex> lock(gState.configMutex);
		gState.token.clear();
		gState.config = PluginConfig{};
		gState.channelFromTwitchAccount = false;
		gState.connected = false;
		if (userInitiated) {
			gState.userStopped = true;
		}
	}
	saveConfig();
}

class ConnectDialog final : public QDialog {
public:
	explicit ConnectDialog(QWidget *parent = nullptr) : QDialog(parent)
	{
		setWindowTitle("MTG Binder OBS");
		setMinimumWidth(560);

		setStyleSheet(
			"QDialog { background: #111820; color: #f8fafc; }"
			"QLabel { color: #cbd5e1; }"
			"QLineEdit { background: #0d131a; color: #f8fafc; border: 1px solid #334155; border-radius: 8px; padding: 9px 10px; }"
			"QLineEdit:focus { border-color: #e0b64d; }"
			"QPushButton { background: #e0b64d; color: #111820; border: 0; border-radius: 8px; padding: 10px 14px; font-weight: 700; }"
			"QPushButton:hover { background: #f2d27a; }"
			"QPushButton:disabled { background: #475569; color: #94a3b8; }"
			"QPushButton#secondaryButton { background: #243241; color: #f8fafc; }"
			"QPushButton#secondaryButton:hover { background: #334155; }"
		);

		auto *layout = new QVBoxLayout(this);
		layout->setSpacing(16);
		layout->setContentsMargins(22, 22, 22, 22);

		auto *title = new QLabel("MTG Binder OBS", this);
		title->setStyleSheet("color: #f8fafc; font-size: 22px; font-weight: 800;");
		layout->addWidget(title);

		status_ = new QLabel(this);
		status_->setWordWrap(true);
		layout->addWidget(status_);

		sourceLabel_ = new QLabel(this);
		sourceLabel_->setWordWrap(true);
		sourceLabel_->setStyleSheet("color: #94a3b8; font-size: 12px;");
		layout->addWidget(sourceLabel_);

		auto *form = new QFormLayout();
		form->setLabelAlignment(Qt::AlignLeft);
		siteLabel_ = new QLabel("Website", this);
		baseUrl_ = new QLineEdit(this);
		baseUrl_->setText(QString::fromStdString(gState.baseUrl));
		form->addRow(siteLabel_, baseUrl_);
		codeLabel_ = new QLabel("Twitch channel", this);
		linkCode_ = new QLineEdit(this);
		linkCode_->setPlaceholderText("channelname");
		form->addRow(codeLabel_, linkCode_);
		layout->addLayout(form);

		connectButton_ = new QPushButton("Start capture", this);
		disconnectButton_ = new QPushButton("Stop", this);
		disconnectButton_->setObjectName("secondaryButton");
		layout->addWidget(connectButton_);
		layout->addWidget(disconnectButton_);

		QObject::connect(connectButton_, &QPushButton::clicked, [this]() { connectPlugin(); });
		QObject::connect(disconnectButton_, &QPushButton::clicked, [this]() { disconnectPlugin(); });

		gOpenDialog = this;
		syncFromDetection(false);
		refreshStatus();
	}

	~ConnectDialog() override
	{
		if (gOpenDialog == this) {
			gOpenDialog.clear();
		}
	}

	void onExternalChannelUpdate()
	{
		syncFromDetection(true);
		refreshStatus();
	}

private:
	void syncFromDetection(bool preserveManualEdits)
	{
		const std::string detected = detectTwitchChannelFromObs();
		std::string currentChannel;
		bool fromTwitch = false;
		{
			std::lock_guard<std::mutex> lock(gState.configMutex);
			currentChannel = gState.config.channel;
			fromTwitch = gState.channelFromTwitchAccount;
		}

		if (!detected.empty()) {
			if (!preserveManualEdits || fromTwitch || currentChannel.empty() ||
			    currentChannel == detected) {
				linkCode_->setText(QString::fromStdString(detected));
			}
		} else if (!preserveManualEdits && !currentChannel.empty()) {
			linkCode_->setText(QString::fromStdString(currentChannel));
		}
	}

	void updateSourceLabel(bool fromTwitch, const QString &channel)
	{
		if (channel.isEmpty()) {
			sourceLabel_->setText(
				"No Twitch account channel detected. Enter the channel name manually if you use a stream key only."
			);
		} else if (fromTwitch) {
			sourceLabel_->setText("Channel source: detected from the connected Twitch account.");
		} else {
			sourceLabel_->setText("Channel source: entered manually.");
		}
	}

	void refreshStatus()
	{
		std::string channel;
		std::string plan;
		bool fromTwitch = false;
		bool connected = false;
		bool userStopped = false;
		{
			std::lock_guard<std::mutex> lock(gState.configMutex);
			channel = gState.config.channel;
			plan = gState.config.plan;
			fromTwitch = gState.channelFromTwitchAccount;
			connected = gState.connected;
			userStopped = gState.userStopped;
		}

		const QString channelQt = QString::fromStdString(channel).toHtmlEscaped();
		updateSourceLabel(fromTwitch, QString::fromStdString(channel));

		const bool armed = !channel.empty() && !userStopped;
		if (connected) {
			const QString planQt = QString::fromStdString(plan).toUpper().toHtmlEscaped();
			status_->setText(QString(
				"<div style='color:#94a3b8; font-size:13px; font-weight:700; letter-spacing:0.08em; text-transform:uppercase;'>Capturing channel</div>"
				"<div style='color:#f8fafc; font-size:28px; font-weight:900; margin-top:6px;'>%1</div>"
				"<div style='color:#cbd5e1; font-size:13px; margin-top:8px;'>Tier %2 — uploads only while streaming</div>"
			)
						     .arg(channelQt)
						     .arg(planQt));
		} else if (armed) {
			status_->setText(QString(
				"<div style='color:#94a3b8; font-size:13px; font-weight:700; letter-spacing:0.08em; text-transform:uppercase;'>Ready</div>"
				"<div style='color:#f8fafc; font-size:28px; font-weight:900; margin-top:6px;'>%1</div>"
				"<div style='color:#cbd5e1; font-size:13px; margin-top:8px;'>Capture starts automatically when you go live.</div>"
			)
						     .arg(channelQt));
		} else {
			status_->setText(
				"Connect your Twitch account in OBS (Settings > Stream > Connect Account) for autodetection, or enter the channel name manually."
			);
		}

		const bool showForm = !connected && !armed;
		siteLabel_->setVisible(showForm);
		baseUrl_->setVisible(showForm);
		codeLabel_->setVisible(showForm);
		linkCode_->setVisible(showForm);
		connectButton_->setVisible(showForm);
		disconnectButton_->setVisible(connected || armed);
	}

	void connectPlugin()
	{
		try {
			const std::string baseUrl = baseUrl_->text().toStdString();
			std::string channel = normalizeChannelName(linkCode_->text().toStdString());
			if (channel.size() < 3) {
				status_->setText("Enter the Twitch channel name.");
				return;
			}

			const std::string detected = detectTwitchChannelFromObs();
			const bool fromTwitch = !detected.empty() && detected == channel;

			{
				std::lock_guard<std::mutex> lock(gState.configMutex);
				gState.baseUrl = baseUrl;
			}
			startCaptureWithChannel(channel, fromTwitch);
			refreshStatus();
		} catch (const std::exception &e) {
			status_->setText(QString("Connection error: %1").arg(e.what()));
		}
	}

	void disconnectPlugin()
	{
		stopCapture(true);
		refreshStatus();
	}

	QLabel *status_ = nullptr;
	QLabel *sourceLabel_ = nullptr;
	QLabel *siteLabel_ = nullptr;
	QLabel *codeLabel_ = nullptr;
	QLineEdit *baseUrl_ = nullptr;
	QLineEdit *linkCode_ = nullptr;
	QPushButton *connectButton_ = nullptr;
	QPushButton *disconnectButton_ = nullptr;
};

void notifyOpenDialog()
{
	if (gOpenDialog) {
		gOpenDialog->onExternalChannelUpdate();
	}
}

/* Resume a previously armed (auto or manual) channel when streaming becomes active. */
void tryResumeArmedCapture()
{
	if (!obs_frontend_streaming_active()) {
		return;
	}

	std::string channel;
	bool fromTwitch = false;
	bool userStopped = false;
	bool connected = false;
	{
		std::lock_guard<std::mutex> lock(gState.configMutex);
		channel = gState.config.channel;
		fromTwitch = gState.channelFromTwitchAccount;
		userStopped = gState.userStopped;
		connected = gState.connected;
	}

	if (connected || userStopped || channel.empty()) {
		return;
	}

	startCaptureWithChannel(channel, fromTwitch);
	notifyOpenDialog();
}

void applyTwitchAutodetect()
{
	const std::string detected = detectTwitchChannelFromObs();

	std::string previousDetected;
	std::string currentChannel;
	bool userStopped = false;
	bool connected = false;
	bool fromTwitch = false;
	{
		std::lock_guard<std::mutex> lock(gState.configMutex);
		previousDetected = gState.lastDetectedChannel;
		currentChannel = gState.config.channel;
		userStopped = gState.userStopped;
		connected = gState.connected;
		fromTwitch = gState.channelFromTwitchAccount;
	}

	if (detected != previousDetected) {
		if (!detected.empty()) {
			blog(LOG_INFO, "[mtgbinder] Twitch channel detected: %s", detected.c_str());
		}
		std::lock_guard<std::mutex> lock(gState.configMutex);
		gState.lastDetectedChannel = detected;
	}

	if (detected.empty()) {
		notifyOpenDialog();
		return;
	}

	/* Do not override a deliberate manual channel that differs from OBS Twitch login. */
	const bool followDetection = fromTwitch || currentChannel.empty() || currentChannel == previousDetected ||
				     currentChannel == detected;
	if (!followDetection) {
		notifyOpenDialog();
		return;
	}

	if (currentChannel != detected) {
		if (!userStopped) {
			/* Follow rename: updates armed channel; uploads only if live. */
			startCaptureWithChannel(detected, true);
		} else {
			{
				std::lock_guard<std::mutex> lock(gState.configMutex);
				gState.config.channel = detected;
				gState.channelFromTwitchAccount = true;
			}
			saveConfig();
		}
	} else if (!connected && !userStopped && obs_frontend_streaming_active()) {
		startCaptureWithChannel(detected, true);
	}

	notifyOpenDialog();
}

void ensureTwitchPollTimer()
{
	if (gTwitchPollTimer) {
		return;
	}
	QWidget *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *timer = new QTimer(mainWindow);
	timer->setInterval(kTwitchPollIntervalMs);
	QObject::connect(timer, &QTimer::timeout, []() { applyTwitchAutodetect(); });
	timer->start();
	gTwitchPollTimer = timer;
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		loadConfig();
		ensureTwitchPollTimer();
		/* Re-read Auth/Type + Twitch/Name; start only if already live. */
		applyTwitchAutodetect();
		tryResumeArmedCapture();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		loadConfig();
		/* Profile switch: re-read basic.ini Twitch login and follow rename. */
		applyTwitchAutodetect();
		tryResumeArmedCapture();
		notifyOpenDialog();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		/* Early re-read of Auth/Type + Twitch/Name before go-live. */
		applyTwitchAutodetect();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		/* Re-read channel (rename while OBS was open) then start capture. */
		applyTwitchAutodetect();
		tryResumeArmedCapture();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		pauseCapture();
		notifyOpenDialog();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		pauseCapture();
		if (gTwitchPollTimer) {
			gTwitchPollTimer->stop();
			gTwitchPollTimer->deleteLater();
			gTwitchPollTimer.clear();
		}
		break;
	default:
		break;
	}
}

void openConnectDialog(void *)
{
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	applyTwitchAutodetect();
	ConnectDialog dialog(parent);
	dialog.exec();
}

} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
	return "MTG Binder OBS frame uploader";
}

bool obs_module_load(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	loadConfig();
	obs_add_main_render_callback(onMainRender, nullptr);
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	obs_frontend_add_tools_menu_item(obs_module_text("ToolsMenu.MTGBinderOBS"), openConnectDialog, nullptr);
	blog(LOG_INFO, "[mtgbinder] plugin loaded (capture only while streaming)");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	pauseCapture();
	if (gTwitchPollTimer) {
		gTwitchPollTimer->stop();
		gTwitchPollTimer->deleteLater();
		gTwitchPollTimer.clear();
	}
	obs_remove_main_render_callback(onMainRender, nullptr);
	if (gState.stage) {
		gs_stagesurface_destroy(gState.stage);
		gState.stage = nullptr;
	}
	if (gState.texrender) {
		gs_texrender_destroy(gState.texrender);
		gState.texrender = nullptr;
	}
	curl_global_cleanup();
	blog(LOG_INFO, "[mtgbinder] plugin unloaded");
}
