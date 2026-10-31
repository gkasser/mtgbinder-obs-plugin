#pragma once

#include <QObject>
#include <QString>

#include <functional>

/* Receives QCefWidget::urlChanged without linking obs-browser headers. */
class TwitchUrlSink final : public QObject {
	Q_OBJECT
public:
	explicit TwitchUrlSink(QObject *parent = nullptr) : QObject(parent) {}

	void setHandler(std::function<void(const QString &)> handler) { handler_ = std::move(handler); }

public slots:
	void onUrlChanged(const QString &url)
	{
		if (handler_) {
			handler_(url);
		}
	}

private:
	std::function<void(const QString &)> handler_;
};
