// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_LOG_H_
#define MUMBLE_MUMBLE_LOG_H_

#include <set>

#include <functional>

#include <QSystemTrayIcon>
#include <QtCore/QDate>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QVector>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>

#include "ConfigDialog.h"
#include "ui_Log.h"

#ifndef USE_NO_TTS
class TextToSpeech;
#endif

class ChatVideoPlayer;
class QMovie;
class QTimer;

class LogConfig : public ConfigWidget, public Ui::LogConfig {
private:
	Q_OBJECT
	Q_DISABLE_COPY(LogConfig)

	QTreeWidgetItem *allMessagesItem;

protected:
	void updateSelectAllButtons();

public:
	/// The unique name of this ConfigWidget
	static const QString name;
	enum Column {
		ColMessage,
		ColConsole,
		ColNotification,
		ColHighlight,
		ColTTS,
		ColMessageLimit,
		ColStaticSound,
		ColStaticSoundPath
	};
	LogConfig(Settings &st);
	QString title() const Q_DECL_OVERRIDE;
	const QString &getName() const Q_DECL_OVERRIDE;
	QIcon icon() const Q_DECL_OVERRIDE;
public slots:
	void accept() const Q_DECL_OVERRIDE;
	void save() const Q_DECL_OVERRIDE;
	void load(const Settings &) Q_DECL_OVERRIDE;

	void on_qtwMessages_itemChanged(QTreeWidgetItem *, int);
	void on_qtwMessages_itemClicked(QTreeWidgetItem *, int);
	void on_qtwMessages_itemDoubleClicked(QTreeWidgetItem *, int);
	void browseForAudioFile();

	void on_qsNotificationVolume_valueChanged(int value);
	void on_qsCueVolume_valueChanged(int value);
	void on_qsTTSVolume_valueChanged(int value);
	void on_qsbNotificationVolume_valueChanged(int value);
	void on_qsbCueVolume_valueChanged(int value);
	void on_qsbTTSVolume_valueChanged(int value);
};

class ClientUser;
class Channel;
class LogMessage;

class Log : public QObject {
	friend class LogConfig;

private:
	Q_OBJECT
	Q_DISABLE_COPY(Log)
public:
	enum MsgType {
		DebugInfo,
		CriticalError,
		Warning,
		Information,
		ServerConnected,
		ServerDisconnected,
		UserJoin,
		UserLeave,
		Recording,
		YouKicked,
		UserKicked,
		SelfMute,
		OtherSelfMute,
		YouMuted,
		YouMutedOther,
		OtherMutedOther,
		ChannelJoin,
		ChannelLeave,
		PermissionDenied,
		TextMessage,
		SelfUnmute,
		SelfDeaf,
		SelfUndeaf,
		UserRenamed,
		SelfChannelJoin,
		SelfChannelJoinOther,
		ChannelJoinConnect,
		ChannelLeaveDisconnect,
		PrivateTextMessage,
		ChannelListeningAdd,
		ChannelListeningRemove,
		PluginMessage
	};

	enum LogColorType { Time, Server, Privilege, Source, Target };
	static const MsgType firstMsgType = DebugInfo;
	static const MsgType lastMsgType  = ChannelListeningRemove;

	// Display order in settingsscreen, allows to insert new events without breaking config-compatibility with older
	// versions.
	static const MsgType msgOrder[];

protected:
	/// Mutex for qvDeferredLogs
	static QMutex qmDeferredLogs;
	/// A vector containing deferred log messages
	static QVector< LogMessage > qvDeferredLogs;

	QHash< MsgType, int > qmIgnore;
	static const char *msgNames[];
	static const char *colorClasses[];
#ifndef USE_NO_TTS
	TextToSpeech *tts;
#endif
	unsigned int uiLastId;
	QDate qdDate;
	static const QStringList allowedSchemes();

public:
	Log(QObject *p = nullptr);
	QString msgName(MsgType t) const;
	void setIgnore(MsgType t, int ignore = 1 << 30);
	void clearIgnore();
	static QString validHtml(const QString &html, QTextCursor *tc = nullptr);
	static QString imageToImg(const QByteArray &format, const QByteArray &image);
	static QString imageToImg(QImage img, int maxSize = 0);
	/// Creates an HTML image tag for the given image. If rawImageData contains an animated GIF and the
	/// generated HTML fits into maxSize, the raw data is embedded as-is, thereby preserving the
	/// animation. Otherwise (and for all other images), the image is re-encoded as JPEG, stepping the
	/// quality down until it fits into maxSize (see the imageToImg overloads above).
	/// In case an animated GIF had to be converted to a static image because it was too large, this is
	/// logged as an information entry.
	/// @param rawImageData The raw image data the given image was created from, if available
	/// @param image The decoded image corresponding to rawImageData
	/// @param maxSize The maximum length of the generated HTML string
	/// @return The image HTML or an empty string, if the image could not be processed
	static QString imageToImg(const QByteArray &rawImageData, const QImage &image, int maxSize);
	/// Creates an HTML image tag embedding the given raw WebM video data, so that it can be
	/// played inline in the chat log (by clients that support it).
	/// Unlike images, videos cannot be re-encoded, so the raw data is embedded as-is and
	/// sending is refused entirely if the generated HTML does not fit into maxSize.
	/// @param rawVideoData The raw WebM container data
	/// @param maxSize The maximum length of the generated HTML string
	/// @return The video HTML or an empty string, if the data is not WebM or too large
	static QString videoToImg(const QByteArray &rawVideoData, int maxSize);
	/// Returns whether the given data looks like a WebM container (EBML magic)
	static bool isWebM(const QByteArray &rawVideoData);

#ifdef USE_FILE_SHARING
	/// HTML that embeds a file-transfer card into the log. The card is
	/// resolved by LogDocument through the lookup installed by MainWindow.
	static QString fileCardToHtml(const QByteArray &transferId);
#endif

	/// Extracts the raw image data from a data-URL as it is created by the imageToImg functions above.
	/// @param url The data-URL to extract the image data from
	/// @param imageFormat Receives the image format declared in the URL (e.g. "gif")
	/// @return The raw image data or an empty byte array, if the URL could not be parsed
	static QByteArray imageDataFromDataUrl(const QUrl &url, QByteArray &imageFormat);
	static QString msgColor(const QString &text, LogColorType t);
	static QString formatClientUser(ClientUser *cu, LogColorType t, const QString &displayName = QString());
	static QString formatChannel(::Channel *c);
	/// Either defers the LogMessage or defers it, depending on whether Global::l is created already
	/// (if it is, it is used to directly log the msg)
	static void logOrDefer(Log::MsgType mt, const QString &console, const QString &terse = QString(),
						   bool ownMessage = false, const QString &overrideTTS = QString(), bool ignoreTTS = false);
public slots:
	// We have to explicitly use Log::MsgType and not only MsgType in order to be able to use QMetaObject::invokeMethod
	// with this function.
	void log(Log::MsgType mt, const QString &console, const QString &terse = QString(), bool ownMessage = false,
			 const QString &overrideTTS = QString(), bool ignoreTTS = false);
	/// Logs LogMessages that have been deferred so far
	void processDeferredLogs();

signals:
	/// Signal emitted when there was a message received whose type was configured to spawn a notification
	void notificationSpawned(QString title, QString body, QSystemTrayIcon::MessageIcon icon);

	/// Signal emitted when there was a message received whose type was configured to highlight the application
	void highlightSpawned();
};

class LogMessage {
public:
	Log::MsgType mt;
	QString console;
	QString terse;
	bool ownMessage;
	QString overrideTTS;
	bool ignoreTTS;

	LogMessage() = default;
	LogMessage(Log::MsgType mt, const QString &console, const QString &terse, bool ownMessage,
			   const QString &overrideTTS, bool ignoreTTS);
};

class LogDocument : public QTextDocument {
private:
	Q_OBJECT
	Q_DISABLE_COPY(LogDocument)

	/// Maximum number of images animated at the same time. Once reached, the animation of the oldest
	/// image is stopped (freezing it at its current frame).
	static constexpr int MAX_ANIMATED_IMAGES = 16;

	/// Whether animated images (currently: GIFs) in this document should be played
	bool m_animateImages;
	/// The movies of the currently animated images, keyed by their data-URL
	QHash< QUrl, QMovie * > m_qmAnimatedImages;
	/// The URLs of m_qmAnimatedImages in the order in which their animation was started (oldest first)
	QList< QUrl > m_qlAnimatedImageOrder;

	/// Stops the animation that was started first, freezing the image at its current frame
	void stopOldestAnimation();
	/// Stops animations whose URL is no longer referenced by an image in the document
	void removeUnreferencedAnimations();
	/// Creates a QMovie playing the given (animated GIF) image data under the given URL
	/// @return The started movie or nullptr, if the data turned out not to be animated
	QMovie *createAnimation(const QUrl &url, const QByteArray &imageData);

#ifdef USE_FILE_SHARING
	/// Live file-card state lookup (transferId -> rendered card image)
	std::function< bool(const QByteArray &, QImage &) > m_fileCardLookup;
#endif

#ifdef USE_CHAT_WEBM
	/// The players of the chat videos, keyed by their data-URL
	QHash< QUrl, ChatVideoPlayer * > m_qmChatVideos;
	/// The URLs of m_qmChatVideos in the order in which they were created (oldest first)
	QList< QUrl > m_qlChatVideoOrder;
	/// The URLs of the currently playing chat videos in the order in which playback was
	/// started (oldest first)
	QList< QUrl > m_qlPlayingChatVideoOrder;
	/// Shared presentation timer, ticking while any chat video is playing
	QTimer *m_qtVideoPresentTimer = nullptr;
	/// Whether the "WebM videos cannot be played" hint was logged for this document already
	bool m_webmUnsupportedLogged = false;

	/// Pauses the chat video whose playback was started first
	void stopOldestChatVideo();
	/// Destroys the chat video that was created first (its last frame stays displayed)
	void destroyOldestChatVideo();
	/// Pauses and destroys the chat video registered under the given URL, if any
	void removeChatVideo(const QUrl &url);
	/// Creates a player for the given WebM data under the given URL. The player starts out
	/// showing the poster - it does not play anything until toggled.
	ChatVideoPlayer *createChatVideo(const QUrl &url, const QByteArray &videoData, QImage poster);
	/// Starts and stops the presentation timer according to whether any video is playing
	void updateVideoPresentTimer();
	/// Logs (once per document) that WebM videos cannot be played by this build
	void logWebmUnsupported();

private slots:
	void onChatVideoFrame(const QUrl &url, const QImage &image);
	void onChatVideoStateChanged(const QUrl &url, bool playing);
	/// Presents due video frames of all playing chat videos (timer tick)
	void presentChatVideoFrames();
#endif

public:
	LogDocument(QObject *p = nullptr, bool animateImages = false);
	QVariant loadResource(int, const QUrl &) Q_DECL_OVERRIDE;
#ifdef USE_CHAT_WEBM
	/// Maximum number of chat videos held by a document. Once reached, the oldest one is
	/// destroyed, leaving its last shown frame cached as a static image.
	static constexpr int MAX_CHAT_VIDEOS = 16;
	/// Maximum number of chat videos playing at the same time. Once reached, the oldest
	/// playing one is paused (frozen at its current frame).
	static constexpr int MAX_PLAYING_CHAT_VIDEOS = 4;

	/// Returns whether the given data-URL refers to a chat video in this document.
	/// This includes players destroyed earlier at the capacity limit: their last
	/// image stays in the document under the original data-URL, and clicking it
	/// re-creates the player (see toggleChatVideo).
	bool isChatVideo(const QUrl &url) const;
	/// Toggles playback of the chat video referred to by the given data-URL. If the
	/// player was destroyed earlier (capacity eviction), it is re-created from the
	/// URL's data.
	void toggleChatVideo(const QUrl &url);
#endif

#ifdef USE_FILE_SHARING
	/// Installs the live card lookup for mumble-file data URLs (called by
	/// MainWindow). Returns false when no such transfer exists.
	void setFileCardLookup(
		const std::function< bool(const QByteArray &, class QImage &) > &lookup) {
		m_fileCardLookup = lookup;
	}
#endif

signals:
	/// Emitted whenever an animated image or chat video advanced a frame, so that the widget
	/// displaying this document has to be repainted
	void animationFrameChanged();
};

class NotificationSoundBlocker {
public:
	friend class Log;
	NotificationSoundBlocker(Log::MsgType msgType);
	~NotificationSoundBlocker();

private:
	static std::set< Log::MsgType > s_blockedNotificationSounds;
	const Log::MsgType m_msgType;
};

Q_DECLARE_METATYPE(Log::MsgType)

#endif
