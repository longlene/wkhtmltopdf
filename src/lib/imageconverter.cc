// -*- mode: c++; tab-width: 4; indent-tabs-mode: t; eval: (progn (c-set-style "stroustrup") (c-set-offset 'innamespace 0)); -*-
// vi:set ts=4 sts=4 sw=4 noet :
//
// Copyright 2010-2020 wkhtmltopdf authors
//
// This file is part of wkhtmltopdf.
//
// wkhtmltopdf is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// wkhtmltopdf is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with wkhtmltopdf.  If not, see <http://www.gnu.org/licenses/>.


#include "imageconverter_p.hh"
#include "imagesettings.hh"
#include <QBuffer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QSvgGenerator>
#include <QUrl>
#include <QWebEngineView>
#include <qapplication.h>

#ifdef Q_OS_WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace wkhtmltopdf {

ImageConverterPrivate::ImageConverterPrivate(ImageConverter & o, wkhtmltopdf::settings::ImageGlobal & s, const QString * data):
	settings(s),
	loader(s.loadGlobal, 96, true),
	out(o) {
	out.emitCheckboxSvgs(s.loadPage);
	if (data) inputData = *data;

	phaseDescriptions.push_back("Loading page");
	phaseDescriptions.push_back("Rendering");
	phaseDescriptions.push_back("Done");

	connect(&loader, SIGNAL(loadProgress(int)), this, SLOT(loadProgress(int)));
	connect(&loader, SIGNAL(loadFinished(bool)), this, SLOT(pagesLoaded(bool)));
	connect(&loader, SIGNAL(error(QString)), this, SLOT(forwardError(QString)));
	connect(&loader, SIGNAL(warning(QString)), this, SLOT(forwardWarning(QString)));
	connect(&loader, SIGNAL(info(QString)), this, SLOT(forwardInfo(QString)));
	connect(&loader, SIGNAL(debug(QString)), this, SLOT(forwardDebug(QString)));
}

void ImageConverterPrivate::beginConvert() {
	error = false;
	conversionDone = false;
	errorCode = 0;
	progressString = "0%";
	loaderObject = loader.addResource(settings.in, settings.loadPage, &inputData);
	updateWebSettings(&loaderObject->page, settings.web);
	currentPhase=0;
	emit out.phaseChanged();
	loadProgress(0);
	loader.load();
}


void ImageConverterPrivate::clearResources() {
	loader.clearResources();
}

void ImageConverterPrivate::pagesLoaded(bool ok) {
	if (errorCode == 0) errorCode = loader.httpErrorCode();
	if (!ok) {
		fail();
		return;
	}
	if (settings.fmt == "") {
		if (settings.out == "-")
			settings.fmt = "jpg";
		else {
			QFileInfo fi(settings.out);
			settings.fmt = fi.suffix();
		}
	}

	currentPhase = 1;
	emit out.phaseChanged();
	loadProgress(0);

	QWebEnginePage & page = loaderObject->page;

	// Determine initial width
	int screenWidth = settings.screenWidth > 0 ? settings.screenWidth : 1024;

	// Use JavaScript to get content dimensions asynchronously
	QEventLoop dimLoop;
	int contentWidth = screenWidth;
	int contentHeight = 768;

	page.runJavaScript("document.documentElement.scrollWidth", [&](const QVariant &v) {
		contentWidth = v.toInt();
		dimLoop.quit();
	});
	dimLoop.exec();

	if (settings.smartWidth && contentWidth > screenWidth)
		screenWidth = qMin(contentWidth, 32000);

	page.runJavaScript("document.documentElement.scrollHeight", [&](const QVariant &v) {
		contentHeight = v.toInt();
		dimLoop.quit();
	});
	dimLoop.exec();

	int screenHeight = settings.screenHeight > 0 ? settings.screenHeight : contentHeight;
	if (screenHeight < 1) screenHeight = 1;

	loadProgress(25);

	// Set background colour
	if (settings.transparent && (settings.fmt == "png" || settings.fmt == "svg"))
		page.setBackgroundColor(Qt::transparent);
	else
		page.setBackgroundColor(Qt::white);

	// Render via QWebEngineView (offscreen)
	QWebEngineView view;
	view.setAttribute(Qt::WA_DontShowOnScreen);
	view.setPage(&page);
	view.resize(screenWidth, screenHeight);
	view.show();
	QApplication::processEvents();

	QPixmap fullPixmap = view.grab();
	view.setPage(nullptr);  // Don't let the view take ownership of the page

	loadProgress(75);

	// Apply crop
	if (settings.crop.left < 0) settings.crop.left = 0;
	if (settings.crop.top < 0) settings.crop.top = 0;
	QRect fullRect(0, 0, fullPixmap.width(), fullPixmap.height());
	QRect cropRect = fullRect.intersected(QRect(
		settings.crop.left,
		settings.crop.top,
		settings.crop.width > 0 ? settings.crop.width : fullPixmap.width(),
		settings.crop.height > 0 ? settings.crop.height : fullPixmap.height()));

	if (cropRect.width() == 0 || cropRect.height() == 0) {
		emit out.error("Will not output an empty image");
		fail();
		return;
	}

	QPixmap pixmap = fullPixmap.copy(cropRect);

	// Open output device
	QFile file;
	QBuffer buffer(&outputData);
	QIODevice * dev = nullptr;

	if (settings.out.isEmpty()) {
		buffer.open(QIODevice::WriteOnly);
		dev = &buffer;
	} else if (settings.out != "-") {
		file.setFileName(settings.out);
		if (!file.open(QIODevice::WriteOnly)) {
			emit out.error("Could not write to output file");
			fail();
			return;
		}
		dev = &file;
	} else {
#ifdef Q_OS_WIN32
		_setmode(_fileno(stdout), _O_BINARY);
#endif
		if (!file.open(stdout, QIODevice::WriteOnly)) {
			emit out.error("Could not write to stdout");
			fail();
			return;
		}
		dev = &file;
	}

	if (settings.fmt == "svg") {
		// SVG: render via QSvgGenerator (basic, no WebEngine integration)
		QSvgGenerator generator;
		generator.setOutputDevice(dev);
		generator.setSize(pixmap.size());
		generator.setViewBox(QRect(QPoint(0,0), pixmap.size()));
		QPainter painter(&generator);
		painter.drawPixmap(0, 0, pixmap);
		painter.end();
	} else {
		QByteArray fmt = settings.fmt.toLatin1();
		if (!pixmap.save(dev, fmt.data(), settings.quality)) {
			emit out.error("Could not save image");
			fail();
			return;
		}
	}

	loadProgress(100);

	currentPhase = 2;
	clearResources();
	emit out.phaseChanged();
	conversionDone = true;
	emit out.finished(true);

	qApp->exit(0);
}

Converter & ImageConverterPrivate::outer() {
	return out;
}

ImageConverter::~ImageConverter() {
	delete d;
}

ConverterPrivate & ImageConverter::priv() {
	return *d;
}


ImageConverter::ImageConverter(wkhtmltopdf::settings::ImageGlobal & s, const QString * data) {
	d = new ImageConverterPrivate(*this, s, data);
}

const QByteArray & ImageConverter::output() {
	return d->outputData;
}

}
