/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cstdint>

struct AppEvent;
using EventHandler = void (*)(const AppEvent &);

struct AppEvent {
	enum AppEventTypes {
		kEventType_Timer = 0,
	};

	uint16_t Type;

	union {
		struct {
			void *Context;
		} TimerEvent;
	};

	EventHandler Handler;
};
