/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Core {

// HTTP API on 127.0.0.1 for local automation, guarded by a token file
// in the working dir. Endpoints: GET /dialogs, GET /history, POST /send.
void StartLocalApi(not_null<QObject*> parent);

} // namespace Core
