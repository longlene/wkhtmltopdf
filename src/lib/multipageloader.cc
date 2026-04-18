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

#include "multipageloader_p.hh"
#include <QFile>
#include <QFileInfo>
#include <QNetworkCookie>
#include <QNetworkDiskCache>
#include <QNetworkProxy>
#include <QTimer>
#include <QUuid>
#include <QList>
#include <QByteArray>
#include <QUrlQuery>
#include <QWebEngineHttpRequest>
#if !defined QT_NO_SSL
#include <QSslCertificate>
#include <QSslKey>
#include <QSslConfiguration>
#endif

namespace wkhtmltopdf {
/*!
  \file multipageloader.hh
  \brief Defines the MultiPageLoader class
*/

/*!
  \file multipageloader_p.hh
  \brief Defines the MultiPageLoaderPrivate class
*/


LoaderObject::LoaderObject(QWebEnginePage & p): page(p), skip(false) {};

MyNetworkAccessManager::MyNetworkAccessManager(const settings::LoadPage & s):
	disposed(false),
	settings(s) {

	if ( !s.cacheDir.isEmpty() ){
		QNetworkDiskCache *cache = new QNetworkDiskCache(this);
		cache->setCacheDirectory(s.cacheDir);
		QNetworkAccessManager::setCache(cache);
	}
}

void MyNetworkAccessManager::dispose() {
	disposed = true;
}

void MyNetworkAccessManager::allow(QString path) {
	QString x = QFileInfo(path).canonicalFilePath();
	if (x.isEmpty()) return;
	allowed.insert(x);
}

QNetworkReply * MyNetworkAccessManager::createRequest(Operation op, const QNetworkRequest & req, QIODevice * outgoingData) {
	emit debug(QString("Creating request: ") + req.url().toString());

	if (disposed)
	{
		emit warning("Received createRequest signal on a disposed ResourceObject's NetworkAccessManager. "
			     "This might be an indication of an iframe taking too long to load.");
		// Needed to avoid race conditions by spurious network requests
		// by scripts or iframes taking too long to load.
		QNetworkRequest r2 = req;
		r2.setUrl(QUrl("about:blank"));
		return QNetworkAccessManager::createRequest(op, r2, outgoingData);
	}

	bool isLocalFileAccess = req.url().scheme().length() <= 1 || req.url().scheme() == "file";
	if (isLocalFileAccess && settings.blockLocalFileAccess) {
		bool ok=false;
		QString path = QFileInfo(req.url().toLocalFile()).canonicalFilePath();
		QString old = "";
		while (path != old) {
			if (allowed.contains(path)) {
				ok=true;
				break;
			}
			old = path;
			path = QFileInfo(path).path();
		}
		if (!ok) {
			QNetworkRequest r2 = req;
			emit warning(QString("Blocked access to file %1").arg(QFileInfo(req.url().toLocalFile()).canonicalFilePath()));
			r2.setUrl(QUrl("about:blank"));
			return QNetworkAccessManager::createRequest(op, r2, outgoingData);
		}
	}
	QNetworkRequest r3 = req;
	if (settings.repeatCustomHeaders) {
		typedef QPair<QString, QString> HT;
		foreach (const HT & j, settings.customHeaders)
			r3.setRawHeader(j.first.toLatin1(), j.second.toLatin1());
	}

	#if (QT_VERSION >= 0x050000 && !defined QT_NO_SSL) || !defined QT_NO_OPENSSL
	if(!settings.clientSslKeyPath.isEmpty() && !settings.clientSslKeyPassword.isEmpty()
			&& !settings.clientSslCrtPath.isEmpty()){
		QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();

		QFile keyFile(settings.clientSslKeyPath);
		if(keyFile.open(QFile::ReadOnly)){
			QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey, settings.clientSslKeyPassword.toUtf8());
			sslConfig.setPrivateKey(key);
			keyFile.close();

			QList<QSslCertificate> chainCerts =
				QSslCertificate::fromPath(settings.clientSslCrtPath.toLatin1(),  QSsl::Pem, QSslCertificate::PatternSyntax::FixedString);
			QList<QSslCertificate> cas =  sslConfig.caCertificates();
			cas.append(chainCerts);
			if(!chainCerts.isEmpty()){
				sslConfig.setLocalCertificate(chainCerts.first());
				sslConfig.setCaCertificates(cas);

				r3.setSslConfiguration(sslConfig);
			}
		}
	}
	#endif

	return QNetworkAccessManager::createRequest(op, r3, outgoingData);
}

MyNetworkProxyFactory::MyNetworkProxyFactory (QNetworkProxy proxy, QList<QString> bph):
	bypassHosts(bph),
	originalProxy(QList<QNetworkProxy>() << proxy),
	noProxy(QList<QNetworkProxy>() << QNetworkProxy(QNetworkProxy::DefaultProxy)){}

QList<QNetworkProxy> MyNetworkProxyFactory::queryProxy (const QNetworkProxyQuery & query) {
	QString host = query.url().host();
	foreach (const QString & bypassHost, bypassHosts) {
		if (host.compare(bypassHost, Qt::CaseInsensitive) == 0)
			return noProxy;
	}
	return originalProxy;
}

ResourceObject::ResourceObject(MultiPageLoaderPrivate & mpl, const QUrl & u, const settings::LoadPage & s):
	networkAccessManager(s),
	url(u),
	loginTry(0),
	progress(0),
	windowStatusCounter(0),
	finished(false),
	signalPrint(false),
	multiPageLoader(mpl),
	webPage(),
	lo(webPage),
	httpErrorCode(0),
	settings(s) {

	foreach (const QString & path, s.allowed)
		networkAccessManager.allow(path);
	if (url.scheme() == "file")
		networkAccessManager.allow(url.toLocalFile());

	connect(&webPage, SIGNAL(loadStarted()), this, SLOT(loadStarted()));
	connect(&webPage, SIGNAL(loadProgress(int)), this, SLOT(loadProgress(int)));
	connect(&webPage, SIGNAL(loadFinished(bool)), this, SLOT(loadFinished(bool)));
	connect(&webPage, SIGNAL(printRequested()), this, SLOT(printRequested()));

	// QWebEnginePage uses its own profile-based network stack; custom per-page
	// QNetworkAccessManager is not supported. Connect the NAM signals to the
	// interceptor/profile instead for error tracking and authentication.
	connect(&networkAccessManager, SIGNAL(finished (QNetworkReply *)),
			this, SLOT(amfinished (QNetworkReply *) ) );

	connect(&networkAccessManager, SIGNAL(debug(const QString &)),
			this, SLOT(debug(const QString &)));

	connect(&networkAccessManager, SIGNAL(info(const QString &)),
			this, SLOT(info(const QString &)));

	connect(&networkAccessManager, SIGNAL(warning(const QString &)),
			this, SLOT(warning(const QString &)));

	connect(&networkAccessManager, SIGNAL(error(const QString &)),
			this, SLOT(error(const QString &)));

	// QWebEnginePage.authenticationRequired is available directly
	connect(&webPage, SIGNAL(authenticationRequired(const QUrl &, QAuthenticator *)),
	        this, SLOT(handleAuthenticationRequired(const QUrl &, QAuthenticator *)));

	// WebEngine manages cookies via QWebEngineProfile::cookieStore(); the
	// QNetworkCookieJar is kept for potential non-WebEngine network requests.

	// Proxy: WebEngine uses system/application proxy; set application proxy if configured.
	if (!settings.proxy.host.isEmpty()) {
		QNetworkProxy proxy;
		proxy.setHostName(settings.proxy.host);
		proxy.setPort(settings.proxy.port);
		proxy.setType(settings.proxy.type);

		if (settings.proxy.type == QNetworkProxy::HttpProxy) {
			QNetworkProxy::Capabilities capabilities = QNetworkProxy::CachingCapability | QNetworkProxy::TunnelingCapability;
			if (settings.proxyHostNameLookup)
				capabilities |= QNetworkProxy::HostNameLookupCapability;
			proxy.setCapabilities(capabilities);
		}
		if (!settings.proxy.user.isEmpty())
			proxy.setUser(settings.proxy.user);
		if (!settings.proxy.password.isEmpty())
			proxy.setPassword(settings.proxy.password);
		// Apply as application-level proxy (best effort for WebEngine).
		if (settings.bypassProxyForHosts.isEmpty())
			QNetworkProxy::setApplicationProxy(proxy);
		else
			networkAccessManager.setProxyFactory(
				new MyNetworkProxyFactory(proxy, settings.bypassProxyForHosts));
	}

	if (settings.zoomFactor != 1.0)
		webPage.setZoomFactor(settings.zoomFactor);
}

/*!
 * Once loading starting, this is called
 */
void ResourceObject::loadStarted() {
	debug("QWebEnginePage load started.");
	if (finished == true) {
		++multiPageLoader.loading;
		finished = false;
	}
	if (multiPageLoader.loadStartedEmitted) return;
	multiPageLoader.loadStartedEmitted=true;
	emit multiPageLoader.outer.loadStarted();
}


/*!
 * Called when the page is loading, display some progress to the using
 * \param progress the loading progress in percent
 */
void ResourceObject::loadProgress(int p) {
	// If we are finished, ignore this signal.
	if (finished || multiPageLoader.resources.size() <= 0) {
		warning("A finished ResourceObject received a loading progress signal. "
			"This might be an indication of an iframe taking too long to load.");
		return;
	}

	multiPageLoader.progressSum -= progress;
	progress = p;
	multiPageLoader.progressSum += progress;

	emit multiPageLoader.outer.loadProgress(multiPageLoader.progressSum / multiPageLoader.resources.size());
}


void ResourceObject::loadFinished(bool ok) {
	debug("QWebEnginePage load finished.");

	// If we are finished, this might be a potential bug.
	if (finished || multiPageLoader.resources.size() <= 0) {
		warning("A finished ResourceObject received a loading finished signal. "
			"This might be an indication of an iframe taking too long to load.");
		return;
	}

	multiPageLoader.hasError = multiPageLoader.hasError || (!ok && settings.loadErrorHandling == settings::LoadPage::abort);
	if (!ok) {
		if (settings.loadErrorHandling == settings::LoadPage::abort)
			error(QString("Failed loading page ") + url.toString() + " (sometimes it will work just to ignore this error with --load-error-handling ignore)");
		else if (settings.loadErrorHandling == settings::LoadPage::skip) {
			warning(QString("Failed loading page ") + url.toString() + " (skipped)");
			lo.skip = true;
		} else
			warning(QString("Failed loading page ") + url.toString() + " (ignored)");
	}

	bool isMain = multiPageLoader.isMainLoader;

	// Evaluate extra user supplied javascript for the main loader
	if (isMain)
		foreach (const QString & str, settings.runScript)
			webPage.runJavaScript(str);

	// XXX: If loading failed there's no need to wait
	//      for javascript on this resource.
	if (!ok || signalPrint || settings.jsdelay == 0) loadDone();
	else if (isMain && !settings.windowStatus.isEmpty()) waitWindowStatus();
	else QTimer::singleShot(settings.jsdelay, this, SLOT(loadDone()));
}

void ResourceObject::waitWindowStatus() {
	webPage.runJavaScript("window.status", [&](const QVariant &result) {
	QString windowStatus = result.toString();
	if (windowStatus != settings.windowStatus) {
		// This is once a second
		if ((++windowStatusCounter % 20) == 0) {
			windowStatusCounter = 0;
			debug(QString("Waiting for window.status; Found: \"" + windowStatus + "\", but expecting: \"" + settings.windowStatus + "\"."));
		}

		QTimer::singleShot(50, this, SLOT(waitWindowStatus()));
	} else {
		debug("Window status \"" + settings.windowStatus + "\" found.");
		QTimer::singleShot(settings.jsdelay, this, SLOT(loadDone()));
	}

					});
}

void ResourceObject::printRequested() {
	signalPrint=true;
	loadDone();
}

void ResourceObject::loadDone() {
	if (finished) return;
	finished=true;

	debug("Loading done; Stopping QWebPage and any possible page refreshes.");

	// Ensure no more loading goes..
	webPage.triggerAction(QWebEnginePage::Stop);
	networkAccessManager.dispose();
	//disconnect(this, 0, 0, 0);

	--multiPageLoader.loading;
	if (multiPageLoader.loading == 0)
		multiPageLoader.loadDone();
}

/*!
 * Called when the page requires authentication, fills in the username
 * and password supplied on the command line
 */
void ResourceObject::handleAuthenticationRequired(const QUrl &, QAuthenticator *authenticator) {
	if (settings.username.isEmpty()) {
		//If no username is given, complain the such is required
		error("Authentication Required");
	} else if (loginTry >= 2) {
		//If the login has failed a sufficient number of times,
		//the username or password must be wrong
		error("Invalid username or password");
	} else {
		authenticator->setUser(settings.username);
		authenticator->setPassword(settings.password);
		++loginTry;
	}
}

void ResourceObject::debug(const QString & str) {
	emit multiPageLoader.outer.debug(str);
}

void ResourceObject::info(const QString & str) {
	emit multiPageLoader.outer.info(str);
}

void ResourceObject::warning(const QString & str) {
	emit multiPageLoader.outer.warning(str);
}

void ResourceObject::error(const QString & str) {
	emit multiPageLoader.outer.error(str);
}

/*!
 * Track and handle network errors
 * \param reply The networkreply that has finished
 */
void ResourceObject::amfinished(QNetworkReply * reply) {
	debug(QString("Finished request: ") + reply->url().toString());

	int networkStatus = reply->error();
	int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if ((networkStatus != 0 && networkStatus != 5) || (httpStatus > 399 && httpErrorCode == 0))
	{
		QFileInfo fi(reply->url().toString());
		QString extension = fi.completeSuffix().toLower().remove(QRegularExpression("\\?.*$"));
		bool mediaFile = settings::LoadPage::mediaFilesExtensions.contains(extension);
		if ( ! mediaFile) {
			// XXX: Notify network errors as higher priority than HTTP errors.
			//      QT's QNetworkReply::NetworkError enum uses values overlapping
			//      HTTP status codes, so adding 1000 to QT's codes will avoid
			//      confusion. Also a network error at this point will probably mean
			//      no HTTP access at all, so we want network errors to be reported
			//      with a higher priority than HTTP ones.
			//      See: http://doc-snapshot.qt-project.org/4.8/qnetworkreply.html#NetworkError-enum
			error(QString("Failed to load %1, with network status code %2 and http status code %3 - %4")
				.arg(reply->url().toString()).arg(networkStatus).arg(httpStatus).arg(reply->errorString()));
			httpErrorCode = networkStatus > 0 ? (networkStatus + 1000) : httpStatus;
			return;
		}
		if (settings.mediaLoadErrorHandling == settings::LoadPage::abort)
		{
			httpErrorCode = networkStatus > 0 ? (networkStatus + 1000) : httpStatus;
			error(QString("Failed to load ") + reply->url().toString() + ", with code: " + QString::number(httpErrorCode) +
				" (sometimes it will work just to ignore this error with --load-media-error-handling ignore)");
		}
		else {
			warning(QString("Failed to load %1 (%2)")
					.arg(reply->url().toString())
					.arg(settings::loadErrorHandlingToStr(settings.mediaLoadErrorHandling))
					);
		}
	}
}

void ResourceObject::load() {
	finished=false;
	++multiPageLoader.loading;

	bool hasFiles=false;
	foreach (const settings::PostItem & pi, settings.post) hasFiles |= pi.file;
	QByteArray postData;
	QString boundary;
	if (hasFiles) {
		boundary = QUuid::createUuid().toString().remove('-').remove('{').remove('}');
		foreach (const settings::PostItem & pi, settings.post) {
			postData.append("--");
			postData.append(boundary.toUtf8());
			postData.append("\ncontent-disposition: form-data; name=\"");
			postData.append(pi.name.toUtf8());
			postData.append('\"');
			if (pi.file) {
				QFile f(pi.value);
				if (!f.open(QIODevice::ReadOnly) ) {
					error(QString("Unable to open file ")+pi.value);
					multiPageLoader.fail();
				}
				postData.append("; filename=\"");
				postData.append( QFileInfo(pi.value).fileName().toUtf8());
				postData.append("\"\n\n");
				postData.append( f.readAll() );
			} else {
				postData.append("\n\n");
				postData.append(pi.value.toUtf8());
			}
			postData.append('\n');
		}
		if (!postData.isEmpty()) {
			postData.append("--");
			postData.append(boundary.toUtf8());
			postData.append("--\n");
		}
	} else {
		QUrlQuery q;
		foreach (const settings::PostItem & pi, settings.post)
			q.addQueryItem(pi.name, pi.value);
		postData = q.query(QUrl::FullyEncoded).toLocal8Bit();
	}

	// Build a QWebEngineHttpRequest so we can set custom headers and POST data.
	QWebEngineHttpRequest req(url);
	typedef QPair<QString, QString> HT;
	foreach (const HT & j, settings.customHeaders)
		req.setHeader(j.first.toLatin1(), j.second.toLatin1());

	if (!postData.isEmpty()) {
		req.setMethod(QWebEngineHttpRequest::Post);
		req.setPostData(postData);
		if (hasFiles)
			req.setHeader("Content-Type",
				(QString("multipart/form-data, boundary=") + boundary).toLatin1());
	}

	webPage.load(req);
}

void MyCookieJar::clearExtraCookies() {
	extraCookies.clear();
}

void MyCookieJar::useCookie(const QUrl &, const QString & name, const QString & value) {
	extraCookies.push_back(QNetworkCookie(name.toUtf8(), value.toUtf8()));
}

QList<QNetworkCookie> MyCookieJar::cookiesForUrl(const QUrl & url) const {
	QList<QNetworkCookie> list = QNetworkCookieJar::cookiesForUrl(url);
	list.append(extraCookies);
	return list;
}

void MyCookieJar::loadFromFile(const QString & path) {
	QFile cookieJar(path);
	if (cookieJar.open(QIODevice::ReadOnly | QIODevice::Text) )
		setAllCookies(QNetworkCookie::parseCookies(cookieJar.readAll()));
}

void MyCookieJar::saveToFile(const QString & path) {
	QFile cookieJar(path);
	if (cookieJar.open(QIODevice::WriteOnly | QIODevice::Text) )
		foreach (const QNetworkCookie & cookie, allCookies()) {
			cookieJar.write(cookie.toRawForm());
			cookieJar.write(";\n");
		}
}

void MultiPageLoaderPrivate::loadDone() {
	 if (!settings.cookieJar.isEmpty())
	 	cookieJar->saveToFile(settings.cookieJar);

	if (!finishedEmitted) {
		finishedEmitted = true;
		emit outer.loadFinished(!hasError);
	}
}



/*!
 * Copy a file from some place to another
 * \param src The source to copy from
 * \param dst The destination to copy to
 */
bool MultiPageLoader::copyFile(QFile & src, QFile & dst) {
//      TODO enable again when
//      http://bugreports.qt.nokia.com/browse/QTBUG-6894
//      is fixed
//      QByteArray buf(1024*1024*5,0);
//      while ( qint64 r=src.read(buf.data(),buf.size())) {
//          if (r == -1) return false;
//          if (dst.write(buf.data(),r) != r) return false;
//      }

    if (dst.write( src.readAll() ) == -1) return false;

	src.close();
	dst.close();
	return true;
}

MultiPageLoaderPrivate::MultiPageLoaderPrivate(const settings::LoadGlobal & s, int dpi_, MultiPageLoader & o):
	outer(o), settings(s), dpi(dpi_) {

	cookieJar = new MyCookieJar();

	if (!settings.cookieJar.isEmpty())
		cookieJar->loadFromFile(settings.cookieJar);
}

MultiPageLoaderPrivate::~MultiPageLoaderPrivate() {
	clearResources();
}

LoaderObject * MultiPageLoaderPrivate::addResource(const QUrl & url, const settings::LoadPage & page) {
	ResourceObject * ro = new ResourceObject(*this, url, page);
	resources.push_back(ro);

	return &ro->lo;
}

void MultiPageLoaderPrivate::load() {
	progressSum=0;
	loadStartedEmitted=false;
	finishedEmitted=false;
	hasError=false;
	loading=0;

	for (int i=0; i < resources.size(); ++i)
		resources[i]->load();

	if (resources.size() == 0) loadDone();
}

void MultiPageLoaderPrivate::clearResources() {
	while (resources.size() > 0)
	{
		ResourceObject *tmp = resources.takeFirst();
		delete tmp;
	}
	// Flush any pending deletions so QWebEnginePages are destroyed before the
	// WebEngine profile is torn down (avoids "profile released but page still
	// alive" warnings).
	QCoreApplication::processEvents();
	tempIn.removeAll();
}

void MultiPageLoaderPrivate::cancel() {
	//foreach (QWebPage * page, pages)
	//	page->triggerAction(QWebPage::Stop);
}

void MultiPageLoaderPrivate::fail() {
	hasError = true;
	cancel();
	clearResources();
}

/*!
  \brief Construct a multipage loader object, load settings read from the supplied settings
  \param s The settings to be used while loading pages
*/
MultiPageLoader::MultiPageLoader(settings::LoadGlobal & s, int dpi, bool mainLoader):
	d(new MultiPageLoaderPrivate(s, dpi, *this)) {
	d->isMainLoader = mainLoader;
}

MultiPageLoader::~MultiPageLoader() {
	MultiPageLoaderPrivate *tmp = d;
	d = 0;
	delete tmp;
}

/*!
  \brief Add a resource, to be loaded described by a string
  @param string Url describing the resource to load
*/
LoaderObject * MultiPageLoader::addResource(const QString & string, const settings::LoadPage & s, const QString * data) {
	QString url=string;
	if (data && !data->isEmpty()) {
		url = d->tempIn.create(".html");
		QFile tmp(url);
		if (!tmp.open(QIODevice::WriteOnly) || tmp.write(data->toUtf8())==0) {
			emit error("Unable to create temporary file");
			return NULL;
		}
	} else if (url == "-") {
		QFile in;
		in.open(stdin,QIODevice::ReadOnly);
		url = d->tempIn.create(".html");
		QFile tmp(url);
		if (!tmp.open(QIODevice::WriteOnly) || !copyFile(in, tmp)) {
			emit error("Unable to create temporary file");
			return NULL;
		}
	}
	return addResource(guessUrlFromString(url), s);
}

/*!
  \brief Add a page to be loaded
  @param url Url of the page to load
*/
LoaderObject * MultiPageLoader::addResource(const QUrl & url, const settings::LoadPage & s) {
	return d->addResource(url, s);
}

/*!
  \brief Guess a url, by looking at a string

  (shamelessly copied from Arora Project)
  \param string The string the is suppose to be some kind of url
*/
QUrl MultiPageLoader::guessUrlFromString(const QString &string) {
	QString urlStr = string.trimmed();

	// check if the string is just a host with a port
	QRegularExpression hostWithPort(QLatin1String("^[a-zA-Z\\.]+\\:[0-9]*$"));
	if (hostWithPort.match(urlStr).hasMatch())
		urlStr = QLatin1String("http://") + urlStr;

	// Check if it looks like a qualified URL. Try parsing it and see.
	QRegularExpression test(QLatin1String("^[a-zA-Z]+://"));
	bool hasSchema = test.match(urlStr).hasMatch();
	if (hasSchema) {
		bool isAscii = true;
		foreach (const QChar &c, urlStr) {
			if (c.unicode() >= 0x80) {
				isAscii = false;
				break;
			}
		}

		QUrl url;
		if (isAscii) {
			url = QUrl::fromEncoded(urlStr.toLatin1(), QUrl::TolerantMode);
		} else {
			url = QUrl(urlStr, QUrl::TolerantMode);
		}
		if (url.isValid())
			return url;
	}

	// Might be a file.
	if (QFile::exists(urlStr)) {
		QFileInfo info(urlStr);
		return QUrl::fromLocalFile(info.absoluteFilePath());
	}

	// Might be a shorturl - try to detect the schema.
	if (!hasSchema) {
		int dotIndex = urlStr.indexOf(QLatin1Char('.'));
		if (dotIndex != -1) {
			QString prefix = urlStr.left(dotIndex).toLower();
			QString schema = (prefix == QLatin1String("ftp")) ? prefix : QLatin1String("http");
			QUrl url(schema + QLatin1String("://") + urlStr, QUrl::TolerantMode);
			if (url.isValid())
				return url;
		}
	}

	// Fall back to QUrl's own tolerant parser.
	QUrl url = QUrl(string, QUrl::TolerantMode);

	// finally for cases where the user just types in a hostname add http
	if (url.scheme().isEmpty())
		url = QUrl(QLatin1String("http://") + string, QUrl::TolerantMode);
	return url;
}

/*!
  \brief Return the most severe http error code returned during loading
 */
int MultiPageLoader::httpErrorCode() {
	int res=0;
	foreach (const ResourceObject * ro, d->resources)
		if (ro->httpErrorCode > res) res = ro->httpErrorCode;
	return res;
}

/*!
  \brief Begin loading all the resources added
*/
void MultiPageLoader::load() {
	d->load();
}

/*!
  \brief Clear all the resources
*/
void MultiPageLoader::clearResources() {
	d->clearResources();
}

/*!
  \brief Cancel the loading of the pages
*/
void MultiPageLoader::cancel() {
	d->cancel();
}

/*!
  \fn MultiPageLoader::loadFinished(bool ok)
  \brief Signal emitted when all pages have been loaded
  \param ok True if all the pages have been loaded successfully
*/

/*!
  \fn MultiPageLoader::loadProgress(int progress)
  \brief Signal emitted once load has progressed
  \param progress Progress in percent
*/

/*!
  \fn MultiPageLoader::loadStarted()
  \brief Signal emitted when loading has started
*/

/*!
  \fn void MultiPageLoader::debug(QString text)
  \brief Signal emitted when a debug message was generated
  \param text The debug message
*/

/*!
  \fn void MultiPageLoader::info(QString text)
  \brief Signal emitted when an info message was generated
  \param text The info message
*/

/*!
  \fn void MultiPageLoader::warning(QString text)
  \brief Signal emitted when a non fatal warning has occurred
  \param text A string describing the warning
*/

/*!
  \fn void MultiPageLoader::error(QString text)
  \brief Signal emitted when a fatal error has occurred
  \param text A string describing the error
*/
}
