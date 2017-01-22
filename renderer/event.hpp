#pragma once

#include <vector>
#include <memory>
#include <unordered_map>

namespace Granite
{

namespace Detail
{
constexpr uint64_t fnv_iterate(uint64_t hash, char c)
{
	return (hash * 0x100000001b3ull) ^ uint8_t(c);
}

template<size_t index>
constexpr uint64_t compile_time_fnv1_inner(uint64_t hash, const char *str)
{
	return compile_time_fnv1_inner<index - 1>(fnv_iterate(hash, str[index]), str);
}

template<>
constexpr uint64_t compile_time_fnv1_inner<size_t(-1)>(uint64_t hash, const char *)
{
	return hash;
}

template<size_t len>
constexpr uint64_t compile_time_fnv1(const char (&str)[len])
{
	return compile_time_fnv1_inner<len - 2>(0xcbf29ce484222325ull, str);
}
}

#define GRANITE_EVENT_TYPE_HASH(x) Detail::compile_time_fnv1(#x)
using EventType = uint64_t;

class Event
{
public:
	virtual ~Event() = default;

	template<typename T>
	T &as()
	{
		if (id != T::type_id)
			throw std::bad_cast();

		return static_cast<T&>(*this);
	}

	template<typename T>
	const T &as() const
	{
		if (id != T::type_id)
			throw std::bad_cast();

		return static_cast<const T&>(*this);
	}

	void set_type(EventType type_id)
	{
		id = type_id;
	}

	void set_cookie(uint64_t cookie)
	{
		this->cookie = cookie;
	}

	uint64_t get_cookie() const
	{
		return cookie;
	}

private:
	EventType id;
	uint64_t cookie;
};

struct EventHandler
{
	~EventHandler();
};

class EventManager
{
public:
	static EventManager &get_global()
	{
		static EventManager static_manager;
		return static_manager;
	}

	template<typename T, typename... P>
	void enqueue(P&&... p)
	{
		EventType type = T::type_id;
		auto &l = events[type];

		auto ptr = std::unique_ptr<Event>(new T(std::forward<P>(p)...));
		ptr->set_type(type);
		l.queued_events.emplace_back(std::move(ptr));
	}

	template<typename T, typename... P>
	uint64_t enqueue_latched(P&&... p)
	{
		EventType type = T::type_id;
		auto &l = latched_events[type];
		auto ptr = std::unique_ptr<Event>(new T(std::forward<P>(p)...));
		uint64_t cookie = ++cookie_counter;
		ptr->set_type(type);
		ptr->set_cookie(cookie);

		dispatch_up_event(l.handlers, *ptr);
		l.queued_events.emplace_back(std::move(ptr));
		return cookie;
	}

	void dequeue_latched(uint64_t cookie);

	template<typename T>
	void dispatch_inline(const T &t)
	{
		EventType type = T::type_id;
		auto &l = events[type];
		dispatch_event(l.handlers, t);
	}

	void dispatch();

	template<typename T>
	void register_handler(EventType type, bool (T::*mem_fn)(const Event &event), T *handler)
	{
		events[type].handlers.push_back({ static_cast<bool (EventHandler::*)(const Event &event)>(mem_fn), static_cast<EventHandler *>(handler) });
	}

	void unregister_handler(EventHandler *handler);
	template<typename T>
	void unregister_handler(bool (T::*mem_fn)(const Event &event), EventHandler *handler)
	{
		Handler h{ static_cast<bool (EventHandler::*)(const Event &event)>(mem_fn), static_cast<EventHandler *>(handler) };
		unregister_handler(h);
	}

	template<typename T>
	void register_latch_handler(EventType type, void (T::*up_fn)(const Event &event), void (T::*down_fn)(const Event &event), T *handler)
	{
		LatchHandler h{
			static_cast<void (EventHandler::*)(const Event &event)>(up_fn),
			static_cast<void (EventHandler::*)(const Event &event)>(down_fn),
			static_cast<EventHandler *>(handler) };

		auto &events = latched_events[type];
		dispatch_up_events(events.queued_events, h);
		latched_events[type].handlers.push_back(h);
	}

	void unregister_latch_handler(EventHandler *handler);
	template<typename T>
	void unregister_latch_handler(void (T::*up_fn)(const Event &event), void (T::*down_fn)(const Event &event), T *handler)
	{
		LatchHandler h{
			static_cast<void (EventHandler::*)(const Event &event)>(up_fn),
			static_cast<void (EventHandler::*)(const Event &event)>(down_fn),
			static_cast<EventHandler *>(handler) };

		unregister_latch_handler(h);
	}

	~EventManager();

private:
	struct Handler
	{
		bool (EventHandler::*mem_fn)(const Event &event);
		EventHandler *handler;
	};

	struct LatchHandler
	{
		void (EventHandler::*up_fn)(const Event &event);
		void (EventHandler::*down_fn)(const Event &event);
		EventHandler *handler;
	};

	struct EventTypeData
	{
		std::vector<std::unique_ptr<Event>> queued_events;
		std::vector<Handler> handlers;
	};

	struct LatchEventTypeData
	{
		std::vector<std::unique_ptr<Event>> queued_events;
		std::vector<LatchHandler> handlers;
	};

	void dispatch_event(std::vector<Handler> &handlers, const Event &e);
	void dispatch_up_events(std::vector<std::unique_ptr<Event>> &events, const LatchHandler &handler);
	void dispatch_down_events(std::vector<std::unique_ptr<Event>> &events, const LatchHandler &handler);
	void dispatch_up_event(std::vector<LatchHandler> &handlers, const Event &event);
	void dispatch_down_event(std::vector<LatchHandler> &handlers, const Event &event);

	void unregister_handler(const Handler &handler);
	void unregister_latch_handler(const LatchHandler &handler);

	struct EventHasher
	{
		size_t operator()(EventType hash) const
		{
			return static_cast<size_t>(hash);
		}
	};
	std::unordered_map<EventType, EventTypeData, EventHasher> events;
	std::unordered_map<EventType, LatchEventTypeData, EventHasher> latched_events;
	uint64_t cookie_counter = 0;
};

class AEvent : public Event
{
public:
	static constexpr EventType type_id = GRANITE_EVENT_TYPE_HASH("AEvent");

	AEvent(int a) : a(a) {}
	int a;
};

class BEvent : public Event
{
public:
	static constexpr EventType type_id = GRANITE_EVENT_TYPE_HASH("BEvent");

	BEvent(int a, int b) : a(a), b(b) {}
	int a, b;
};
}