#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PluginConfig {
	std::string channel;
	std::string plan;
	std::string tcgType;
	int frameIntervalSecs = 25;
	std::string endpoint;
};

struct ConnectResponse {
	std::string accessToken;
	PluginConfig config;
};

struct UploadResult {
	long httpStatus = 0;
	std::string body;
	bool ok = false;
};

class MtgBinderClient {
public:
	explicit MtgBinderClient(std::string baseUrl);

	ConnectResponse connectWithLinkCode(const std::string &linkCode, const std::string &deviceName) const;
	UploadResult uploadFrame(
		const PluginConfig &config,
		const std::string &accessToken,
		uint64_t frameIndex,
		int64_t ptsMs,
		uint32_t width,
		uint32_t height,
		const std::vector<uint8_t> &jpeg
	) const;

	const std::string &baseUrl() const { return baseUrl_; }

private:
	std::string baseUrl_;

	std::string apiUrl(const char *path) const;
};

std::string jsonStringValue(const std::string &json, const char *key);
int jsonIntValue(const std::string &json, const char *key, int fallback);
