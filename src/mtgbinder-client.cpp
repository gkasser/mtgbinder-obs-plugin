#include "mtgbinder-client.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {

struct HttpResponse {
	long status = 0;
	std::string body;
};

size_t writeBody(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *body = static_cast<std::string *>(userdata);
	body->append(ptr, size * nmemb);
	return size * nmemb;
}

std::string escapeJson(const std::string &value)
{
	std::string out;
	out.reserve(value.size() + 8);
	for (char c : value) {
		switch (c) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default: out += c; break;
		}
	}
	return out;
}

HttpResponse postJson(const std::string &url, const std::string &json)
{
	CURL *curl = curl_easy_init();
	if (!curl) throw std::runtime_error("curl_easy_init failed");

	std::string body;
	curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "mtgbinder-obs-plugin/" MTGBINDER_PLUGIN_VERSION);

	const CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (code != CURLE_OK) throw std::runtime_error(curl_easy_strerror(code));
	return {status, body};
}

} // namespace

std::string jsonStringValue(const std::string &json, const char *key)
{
	const std::string marker = std::string("\"") + key + "\":";
	size_t pos = json.find(marker);
	if (pos == std::string::npos) return {};
	pos = json.find('"', pos + marker.size());
	if (pos == std::string::npos) return {};
	size_t end = pos + 1;
	std::string value;
	bool escaped = false;
	for (; end < json.size(); ++end) {
		const char c = json[end];
		if (escaped) {
			value.push_back(c);
			escaped = false;
			continue;
		}
		if (c == '\\') {
			escaped = true;
			continue;
		}
		if (c == '"') break;
		value.push_back(c);
	}
	return value;
}

int jsonIntValue(const std::string &json, const char *key, int fallback)
{
	const std::string marker = std::string("\"") + key + "\":";
	size_t pos = json.find(marker);
	if (pos == std::string::npos) return fallback;
	pos += marker.size();
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
	int value = fallback;
	if (std::sscanf(json.c_str() + pos, "%d", &value) != 1) return fallback;
	return value;
}

MtgBinderClient::MtgBinderClient(std::string baseUrl) : baseUrl_(std::move(baseUrl))
{
	while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
}

std::string MtgBinderClient::apiUrl(const char *path) const
{
	return baseUrl_ + path;
}

ConnectResponse MtgBinderClient::connectWithLinkCode(const std::string &linkCode, const std::string &deviceName) const
{
	const std::string payload =
		"{\"link_code\":\"" + escapeJson(linkCode) + "\",\"device_name\":\"" + escapeJson(deviceName) + "\"}";
	const HttpResponse response = postJson(apiUrl("/api/obs/connect"), payload);
	if (response.status < 200 || response.status >= 300) {
		const std::string error = jsonStringValue(response.body, "error");
		throw std::runtime_error("OBS connect failed: HTTP " + std::to_string(response.status) + (error.empty() ? "" : " " + error));
	}

	ConnectResponse out;
	out.accessToken = jsonStringValue(response.body, "access_token");
	out.config.channel = jsonStringValue(response.body, "channel");
	out.config.plan = jsonStringValue(response.body, "plan");
	out.config.tcgType = jsonStringValue(response.body, "tcg_type");
	out.config.frameIntervalSecs = jsonIntValue(response.body, "frame_interval_secs", 25);
	out.config.endpoint = jsonStringValue(response.body, "endpoint");
	if (out.accessToken.empty() || out.config.channel.empty()) {
		throw std::runtime_error("OBS connect response is incomplete");
	}
	return out;
}

UploadResult MtgBinderClient::uploadFrame(
	const PluginConfig &config,
	const std::string &accessToken,
	uint64_t frameIndex,
	int64_t ptsMs,
	uint32_t width,
	uint32_t height,
	const std::vector<uint8_t> &jpeg
) const
{
	CURL *curl = curl_easy_init();
	if (!curl) throw std::runtime_error("curl_easy_init failed");

	std::string body;
	curl_slist *headers = nullptr;
	const std::string auth = "Authorization: Bearer " + accessToken;
	const std::string frame = "x-obs-frame-index: " + std::to_string(frameIndex);
	const std::string pts = "x-obs-pts-ms: " + std::to_string(ptsMs);
	const std::string sourceWidth = "x-obs-source-width: " + std::to_string(width);
	const std::string sourceHeight = "x-obs-source-height: " + std::to_string(height);
	headers = curl_slist_append(headers, auth.c_str());
	headers = curl_slist_append(headers, "Content-Type: image/jpeg");
	headers = curl_slist_append(headers, frame.c_str());
	headers = curl_slist_append(headers, pts.c_str());
	headers = curl_slist_append(headers, sourceWidth.c_str());
	headers = curl_slist_append(headers, sourceHeight.c_str());

	const std::string &endpoint = config.endpoint.empty() ? apiUrl("/api/obs/frames") : config.endpoint;
	curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, reinterpret_cast<const char *>(jpeg.data()));
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jpeg.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "mtgbinder-obs-plugin/" MTGBINDER_PLUGIN_VERSION);

	const CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (code != CURLE_OK) throw std::runtime_error(curl_easy_strerror(code));
	return {status, body, status >= 200 && status < 300};
}
