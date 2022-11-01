/* Copyright (c) 2013-2022 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QFile>
#include <QObject>

#include "ForwarderGenerator.h"

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace QGBA {

class ForwarderController : public QObject {
Q_OBJECT

public:
	ForwarderController(QObject* parent = nullptr);

	void setGenerator(std::unique_ptr<ForwarderGenerator>&& generator);
	ForwarderGenerator* generator() { return m_generator.get(); }

	QString channel() const { return m_channel; }

public slots:
	void startBuild(const QString& outFilename);

signals:
	void buildComplete();
	void buildFailed();

private slots:
	void gotManifest(QNetworkReply*);
	void gotBuild(QNetworkReply*);
	void gotForwarderKit(QNetworkReply*);

private:
	void downloadForwarderKit();
	void downloadManifest();
	void downloadBuild(const QUrl&);
	bool toolInstalled(const QString& tool);
	void cleanup();

	void connectErrorFailure(QNetworkReply*);

	QString m_channel{"dev"};
	QString m_outFilename;
	QNetworkAccessManager* m_netman;
	std::unique_ptr<ForwarderGenerator> m_generator;
	QFile m_sourceFile;
	bool m_inProgress = false;
	QByteArray m_originalPath;
};

}
