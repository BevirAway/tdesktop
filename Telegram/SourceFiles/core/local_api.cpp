/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/local_api.h"

#include "core/application.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "settings.h"

#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrlQuery>
#include <QtCore/QFile>
#include <QtCore/QRandomGenerator>

namespace Core {
namespace {

constexpr auto kPort = quint16(18790);
constexpr auto kMaxRequest = 1024 * 1024;

QByteArray Token;

void Reply(not_null<QTcpSocket*> socket, int code, const QJsonValue &body) {
	const auto json = body.isArray()
		? QJsonDocument(body.toArray()).toJson(QJsonDocument::Compact)
		: QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact);
	const auto status = (code == 200) ? "OK" : "Error";
	socket->write("HTTP/1.1 " + QByteArray::number(code) + ' ' + status
		+ "\r\nContent-Type: application/json; charset=utf-8"
		+ "\r\nContent-Length: " + QByteArray::number(json.size())
		+ "\r\nConnection: close\r\n\r\n" + json);
	socket->disconnectFromHost();
}

void Fail(not_null<QTcpSocket*> socket, int code, const QString &error) {
	Reply(socket, code, QJsonObject{ { u"error"_q, error } });
}

QString PeerKey(not_null<PeerData*> peer) {
	return QString::number(SerializePeerId(peer->id));
}

PeerData *FindPeer(not_null<Main::Session*> session, const QString &key) {
	auto ok = false;
	const auto value = key.toULongLong(&ok);
	return ok ? session->data().peerLoaded(DeserializePeerId(value)) : nullptr;
}

QJsonArray Dialogs(not_null<Main::Session*> session, int limit) {
	auto result = QJsonArray();
	for (const auto &row : *session->data().chatsList()->indexed()) {
		if (result.size() >= limit) {
			break;
		}
		const auto history = row->history();
		if (!history) {
			continue;
		}
		const auto last = history->lastMessage();
		result.push_back(QJsonObject{
			{ u"peer"_q, PeerKey(history->peer) },
			{ u"name"_q, history->peer->name() },
			{ u"unread"_q, history->unreadCount() },
			{ u"date"_q, last ? qint64(last->date()) : 0 },
			{ u"last"_q, last ? last->originalText().text : QString() },
		});
	}
	return result;
}

QJsonObject Message(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const MTPMessage &message) {
	return message.match([&](const MTPDmessage &data) {
		const auto fromId = data.vfrom_id()
			? peerFromMTP(*data.vfrom_id())
			: data.is_out()
			? session->userPeerId()
			: peer->id;
		const auto from = session->data().peerLoaded(fromId);
		return QJsonObject{
			{ u"id"_q, qint64(data.vid().v) },
			{ u"date"_q, qint64(data.vdate().v) },
			{ u"out"_q, data.is_out() },
			{ u"from"_q, from ? from->name() : QString() },
			{ u"text"_q, qs(data.vmessage()) },
			{ u"media"_q, data.vmedia().has_value() },
		};
	}, [](const auto &data) {
		return QJsonObject{
			{ u"id"_q, qint64(data.vid().v) },
			{ u"service"_q, true },
		};
	});
}

void History(
		not_null<Main::Session*> session,
		not_null<QTcpSocket*> socket,
		not_null<PeerData*> peer,
		int limit) {
	const auto guard = QPointer<QTcpSocket>(socket.get());
	session->api().request(MTPmessages_GetHistory(
		peer->input(),
		MTP_int(0), // offset_id
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(limit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (!guard) {
			return;
		}
		auto list = QJsonArray();
		result.match([](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			for (const auto &message : data.vmessages().v) {
				list.push_back(Message(session, peer, message));
			}
		});
		Reply(guard.data(), 200, list);
	}).fail([=](const MTP::Error &error) {
		if (guard) {
			Fail(guard.data(), 502, error.type());
		}
	}).send();
}

void Send(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const QString &text) {
	auto message = Api::MessageToSend(
		Api::SendAction(session->data().history(peer)));
	message.textWithTags = TextWithTags{ text };
	session->api().sendMessage(std::move(message));
}

void Handle(
		not_null<QTcpSocket*> socket,
		const QByteArray &method,
		const QUrl &url,
		const QByteArray &token,
		const QByteArray &body) {
	if (Token.isEmpty() || token != Token) {
		return Fail(socket, 401, u"bad token"_q);
	}
	const auto session = Core::App().maybePrimarySession();
	if (!session) {
		return Fail(socket, 503, u"not logged in"_q);
	}
	const auto query = QUrlQuery(url);
	const auto limit = std::clamp(
		query.queryItemValue(u"limit"_q).toInt(),
		1,
		100);
	const auto path = url.path();
	if (method == "GET" && path == u"/dialogs"_q) {
		return Reply(socket, 200, Dialogs(session, limit));
	} else if (method == "GET" && path == u"/history"_q) {
		const auto peer = FindPeer(session, query.queryItemValue(u"peer"_q));
		if (!peer) {
			return Fail(socket, 404, u"unknown peer"_q);
		}
		return History(session, socket, peer, limit);
	} else if (method == "POST" && path == u"/send"_q) {
		const auto json = QJsonDocument::fromJson(body).object();
		const auto peer = FindPeer(session, json.value(u"peer"_q).toString());
		const auto text = json.value(u"text"_q).toString();
		if (!peer) {
			return Fail(socket, 404, u"unknown peer"_q);
		} else if (text.trimmed().isEmpty()) {
			return Fail(socket, 400, u"empty text"_q);
		}
		Send(session, peer, text);
		return Reply(socket, 200, QJsonObject{ { u"ok"_q, true } });
	}
	Fail(socket, 404, u"unknown endpoint"_q);
}

void Read(not_null<QTcpSocket*> socket) {
	auto buffer = socket->property("buffer").toByteArray()
		+ socket->readAll();
	if (buffer.size() > kMaxRequest) {
		return Fail(socket, 413, u"too large"_q);
	}
	const auto headerEnd = buffer.indexOf("\r\n\r\n");
	if (headerEnd < 0) {
		socket->setProperty("buffer", buffer);
		return;
	}
	const auto lines = buffer.left(headerEnd).split('\n');
	const auto request = lines.front().trimmed().split(' ');
	if (request.size() < 2) {
		return Fail(socket, 400, u"bad request"_q);
	}
	auto length = 0;
	auto token = QByteArray();
	for (const auto &line : lines) {
		const auto colon = line.indexOf(':');
		if (colon < 0) {
			continue;
		}
		const auto name = line.left(colon).trimmed().toLower();
		const auto value = line.mid(colon + 1).trimmed();
		if (name == "content-length") {
			length = value.toInt();
		} else if (name == "x-token") {
			token = value;
		}
	}
	const auto body = buffer.mid(headerEnd + 4);
	if (body.size() < length) {
		socket->setProperty("buffer", buffer);
		return;
	}
	socket->setProperty("buffer", QByteArray());
	Handle(
		socket,
		request[0],
		QUrl(QString::fromUtf8(request[1])),
		token,
		body.left(length));
}

void LoadToken() {
	auto file = QFile(cWorkingDir() + u"tdata/local_api_token"_q);
	if (file.open(QIODevice::ReadOnly)) {
		Token = file.readAll().trimmed();
	}
	if (Token.isEmpty() && file.open(QIODevice::WriteOnly)) {
		auto bytes = QByteArray(24, Qt::Uninitialized);
		for (auto &byte : bytes) {
			byte = char(QRandomGenerator::system()->bounded(256));
		}
		Token = bytes.toHex();
		file.write(Token);
		file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	}
}

} // namespace

void StartLocalApi(not_null<QObject*> parent) {
	LoadToken();
	const auto server = new QTcpServer(parent);
	QObject::connect(server, &QTcpServer::newConnection, [=] {
		while (const auto socket = server->nextPendingConnection()) {
			QObject::connect(
				socket,
				&QTcpSocket::disconnected,
				socket,
				&QObject::deleteLater);
			QObject::connect(socket, &QTcpSocket::readyRead, [=] {
				Read(socket);
			});
		}
	});
	if (!server->listen(QHostAddress::LocalHost, kPort)) {
		LOG(("Local API: could not listen on port %1.").arg(kPort));
	}
}

} // namespace Core
