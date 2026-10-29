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
#include <QPushButton>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
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

struct RuntimeState {
	std::mutex configMutex;
	std::string baseUrl = kDefaultBaseUrl;
	std::string token;
	PluginConfig config;

	std::atomic<bool> connected{false};
	std::atomic<bool> uploadInFlight{false};
	std::atomic<uint64_t> frameIndex{0};
	std::atomic<int64_t> nextCaptureMs{0};

	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stage = nullptr;
	uint32_t stageWidth = 0;
	uint32_t stageHeight = 0;
};

RuntimeState gState;

int64_t epochMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void saveConfig()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg) return;

	std::lock_guard<std::mutex> lock(gState.configMutex);
	config_set_string(cfg, kConfigSection, "base_url", gState.baseUrl.c_str());
	config_set_string(cfg, kConfigSection, "token", gState.token.c_str());
	config_set_string(cfg, kConfigSection, "channel", gState.config.channel.c_str());
	config_set_string(cfg, kConfigSection, "plan", gState.config.plan.c_str());
	config_set_string(cfg, kConfigSection, "tcg_type", gState.config.tcgType.c_str());
	config_set_int(cfg, kConfigSection, "frame_interval_secs", gState.config.frameIntervalSecs);
	config_set_string(cfg, kConfigSection, "endpoint", gState.config.endpoint.c_str());
	config_save_safe(cfg, "tmp", nullptr);
}

void loadConfig()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg) return;

	std::lock_guard<std::mutex> lock(gState.configMutex);
	const char *baseUrl = config_get_string(cfg, kConfigSection, "base_url");
	const char *token = config_get_string(cfg, kConfigSection, "token");
	const char *channel = config_get_string(cfg, kConfigSection, "channel");
	const char *plan = config_get_string(cfg, kConfigSection, "plan");
	const char *tcgType = config_get_string(cfg, kConfigSection, "tcg_type");
	const char *endpoint = config_get_string(cfg, kConfigSection, "endpoint");

	if (baseUrl && *baseUrl) gState.baseUrl = baseUrl;
	if (token) gState.token = token;
	if (channel) gState.config.channel = channel;
	if (plan) gState.config.plan = plan;
	if (tcgType) gState.config.tcgType = tcgType;
	if (endpoint) gState.config.endpoint = endpoint;
	gState.config.frameIntervalSecs = static_cast<int>(config_get_int(cfg, kConfigSection, "frame_interval_secs"));
	if (gState.config.frameIntervalSecs <= 0) gState.config.frameIntervalSecs = 25;
	gState.connected = !gState.config.channel.empty();
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
	if (target > current) gState.nextCaptureMs = target;
}

void appendJpegChunk(void *context, void *data, int size)
{
	auto *out = static_cast<std::vector<uint8_t> *>(context);
	const auto *bytes = static_cast<const uint8_t *>(data);
	out->insert(out->end(), bytes, bytes + static_cast<size_t>(size));
}

std::vector<uint8_t> encodeJpegFromBgra(
	const std::vector<uint8_t> &bgra,
	uint32_t width,
	uint32_t height
)
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
	if (!gState.texrender) gState.texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!gState.texrender) return false;

	// Le canvas OBS (base resolution) peut différer de la cible de capture.
	// On projette tout le canvas dans la cible width×height : le frame complet est
	// réduit uniformément (pas de crop), donc les bbox YOLO restent alignées sur la
	// vidéo affichée côté viewer.
	obs_video_info ovi;
	float orthoWidth = static_cast<float>(width);
	float orthoHeight = static_cast<float>(height);
	if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
		orthoWidth = static_cast<float>(ovi.base_width);
		orthoHeight = static_cast<float>(ovi.base_height);
	}

	gs_texrender_reset(gState.texrender);
	if (!gs_texrender_begin(gState.texrender, width, height)) return false;
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
	if (!texture) return false;

	if (!gState.stage || gState.stageWidth != width || gState.stageHeight != height) {
		if (gState.stage) gs_stagesurface_destroy(gState.stage);
		gState.stage = gs_stagesurface_create(width, height, GS_BGRA);
		gState.stageWidth = width;
		gState.stageHeight = height;
	}
	if (!gState.stage) return false;

	gs_stage_texture(gState.stage, texture);
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(gState.stage, &data, &linesize)) return false;

	bgra.resize(static_cast<size_t>(width) * height * 4);
	for (uint32_t y = 0; y < height; ++y) {
		std::memcpy(bgra.data() + static_cast<size_t>(y) * width * 4, data + static_cast<size_t>(y) * linesize, width * 4);
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
			const auto result = MtgBinderClient(baseUrl).uploadFrame(config, token, frameIndex, ptsMs, width, height, jpeg);
			const int interval = jsonIntValue(result.body, "frame_interval_secs", 0);
			const int retryAfterMs = jsonIntValue(result.body, "retry_after_ms", 0);
			if (interval > 0 || result.httpStatus == 429) {
				applyServerCadence(interval, retryAfterMs, ptsMs, result.ok);
			}
			if (!result.ok && result.httpStatus != 429) {
				blog(LOG_WARNING, "[mtgbinder] upload failed: HTTP %ld %s", result.httpStatus, result.body.c_str());
			} else if (!result.ok) {
				blog(LOG_INFO, "[mtgbinder] frame refused, next in %dms interval=%ds", retryAfterMs, interval);
			} else {
				blog(
					LOG_INFO,
					"[mtgbinder] upload ok frame=%llu pts=%lld size=%ux%u jpeg=%zuB",
					static_cast<unsigned long long>(frameIndex),
					static_cast<long long>(ptsMs),
					width,
					height,
					jpeg.size()
				);
			}
		} catch (const std::exception &e) {
			blog(LOG_WARNING, "[mtgbinder] upload error: %s", e.what());
		}
		gState.uploadInFlight = false;
	}).detach();
}

void onMainRender(void *, uint32_t, uint32_t)
{
	if (!gState.connected || gState.uploadInFlight) return;

	const int64_t now = epochMs();
	const int64_t next = gState.nextCaptureMs.load();
	if (now < next) return;

	int interval = 25;
	{
		std::lock_guard<std::mutex> lock(gState.configMutex);
		interval = gState.config.frameIntervalSecs > 0 ? gState.config.frameIntervalSecs : 25;
	}
	gState.nextCaptureMs = now + static_cast<int64_t>(interval) * 1000;

	std::vector<uint8_t> bgra;
	if (!copyMainTexture(kCaptureWidth, kCaptureHeight, bgra)) return;

	const uint64_t index = gState.frameIndex.fetch_add(1);
	uploadFrameAsync(index, now, kCaptureWidth, kCaptureHeight, std::move(bgra));
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

		refreshStatus();
	}

private:
	void refreshStatus()
	{
		if (gState.connected) {
			const QString channel = QString::fromStdString(gState.config.channel).toHtmlEscaped();
			const QString plan = QString::fromStdString(gState.config.plan).toUpper().toHtmlEscaped();
			status_->setText(QString(
				"<div style='color:#94a3b8; font-size:13px; font-weight:700; letter-spacing:0.08em; text-transform:uppercase;'>Connected to channel</div>"
				"<div style='color:#f8fafc; font-size:28px; font-weight:900; margin-top:6px;'>%1</div>"
				"<div style='color:#cbd5e1; font-size:13px; margin-top:8px;'>Tier %2</div>"
			)
				.arg(channel)
				.arg(plan));
		} else {
			status_->setText("Enter your Twitch channel. The server accepts or rejects each frame and tells the plugin the allowed interval.");
		}
		const bool connected = gState.connected;
		siteLabel_->setVisible(!connected);
		baseUrl_->setVisible(!connected);
		codeLabel_->setVisible(!connected);
		linkCode_->setVisible(!connected);
		connectButton_->setVisible(!connected);
		disconnectButton_->setVisible(connected);
	}

	void connectPlugin()
	{
		try {
			const std::string baseUrl = baseUrl_->text().toStdString();
			std::string channel = linkCode_->text().trimmed().toLower().toStdString();
			if (!channel.empty() && channel[0] == '@') channel.erase(channel.begin());
			if (channel.size() < 3) {
				status_->setText("Enter the Twitch channel name.");
				return;
			}
			{
				std::lock_guard<std::mutex> lock(gState.configMutex);
				gState.baseUrl = baseUrl;
				gState.token.clear();
				gState.config.channel = channel;
				gState.config.endpoint.clear();
				if (gState.config.frameIntervalSecs <= 0) gState.config.frameIntervalSecs = 25;
				gState.connected = true;
			}
			gState.nextCaptureMs = 0;
			saveConfig();
			refreshStatus();
		} catch (const std::exception &e) {
			status_->setText(QString("Connection error: %1").arg(e.what()));
		}
	}

	void disconnectPlugin()
	{
		{
			std::lock_guard<std::mutex> lock(gState.configMutex);
			gState.token.clear();
			gState.config = PluginConfig {};
			gState.connected = false;
		}
		saveConfig();
		refreshStatus();
	}

	QLabel *status_ = nullptr;
	QLabel *siteLabel_ = nullptr;
	QLabel *codeLabel_ = nullptr;
	QLineEdit *baseUrl_ = nullptr;
	QLineEdit *linkCode_ = nullptr;
	QPushButton *connectButton_ = nullptr;
	QPushButton *disconnectButton_ = nullptr;
};

void openConnectDialog(void *)
{
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
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
	obs_frontend_add_tools_menu_item("MTG Binder OBS", openConnectDialog, nullptr);
	blog(LOG_INFO, "[mtgbinder] plugin loaded");
	return true;
}

void obs_module_unload(void)
{
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
