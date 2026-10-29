#include "runtime/EntryPoints.h"
#include "runtime/HandleRegistry.h"
#include "../TestSupport.h"

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

struct Object
{
	int value = 7;
};

XrResult XRAPI_PTR Throw(bool allocation)
{
	if (allocation)
		throw std::bad_alloc();
	throw std::runtime_error("ABI failure");
}

void ReentrantRetirement()
{
	struct ReentrantObject
	{
		std::function<void()> destroy;

		~ReentrantObject()
		{
			destroy();
		}
	};

	using Registry = simxr::HandleRegistry<XrSession, ReentrantObject, XrInstance>;
	const auto parent = reinterpret_cast<XrInstance>(1);
	for (const bool explicitRetirement : {false, true})
	{
		std::size_t destroyed = 0;
		bool emptyDuringDestruction = true;
		{
			Registry registry;
			for (std::size_t index = 0; index < 3; ++index)
			{
				registry.Create(parent, [&](XrSession handle) {
					auto object = std::make_unique<ReentrantObject>();
					object->destroy = [&, handle] {
						emptyDuringDestruction &=
							registry.Children(parent).empty() && !registry.Find(handle) && !registry.Erase(handle);
						++destroyed;
					};
					return object;
				});
			}
			if (explicitRetirement)
			{
				auto retired = registry.RetireAll();
				Require(destroyed == 0 && registry.Children(parent).empty(), "Retirement released its owners early");
				std::size_t retained = 0;
				retired.ForEach([&](const auto& object) { retained += static_cast<bool>(object); });
				Require(retained == 3, "Retirement lost detached owners");
			}
		}
		Require(destroyed == 3 && emptyDuringDestruction, "Registry destruction exposed partially retired entries");
	}
}

int main()
{
	ReentrantRetirement();
	using namespace simxr;
	HandleRegistry<XrSession, Object, XrInstance> sessions;
	HandleRegistry<XrSpace, Object, XrSession> spaces;
	auto instance = reinterpret_cast<XrInstance>(1);
	auto other = reinterpret_cast<XrInstance>(2);
	auto session = sessions.Insert(instance, std::make_unique<Object>());
	Require(sessions.Find(session, instance)->value == 7, "Owner lookup");
	Require(!sessions.Find(session, other), "Wrong owner accepted");
	auto space = spaces.Insert(session, std::make_unique<Object>());
	Require(!spaces.Find(reinterpret_cast<XrSpace>(static_cast<XrSession>(session))), "Wrong family accepted");
	Require(spaces.Children(session).size() == 1, "Child ownership");
	XrSession stale = session;
	Require(sessions.Erase(session), "Erase handle");
	Require(!sessions.Find(stale), "Removed handle stayed valid");
	Require(session->value == 7, "Deferred reference lost ownership");
	auto next = sessions.Insert(instance, std::make_unique<Object>());
	Require(static_cast<XrSession>(next) != stale && !sessions.Find(stale), "Handle reused");
	XrSession unpublished = XR_NULL_HANDLE;
	bool failed = false;
	try
	{
		sessions.Create(instance, [&](XrSession handle) -> std::unique_ptr<Object> {
			unpublished = handle;
			if (sessions.Find(handle))
				throw std::logic_error("Factory published incomplete object");
			throw std::runtime_error("Initialization failed");
		});
	}
	catch (const std::runtime_error&)
	{
		failed = true;
	}
	Require(failed && unpublished && !sessions.Find(unpublished), "Failed factory leaked handle");
	auto constructed = sessions.Create(instance, [&](XrSession handle) {
		Require(handle != unpublished, "Failed identity reused");
		return std::make_unique<Object>(Object{42});
	});
	Require(sessions.Find(constructed)->value == 42, "Factory object not published");
	Require(spaces.Erase(space), "Erase child");
	{
		auto retired = spaces.RetireAll();
	}
	auto restartedSpace = spaces.Insert(session, std::make_unique<Object>());
	Require(spaces.Find(restartedSpace, static_cast<XrSession>(session))->value == 7,
			"Cleared registry did not admit new child handles");
	Require(AbiBoundary<Throw>::Invoke(true) == XR_ERROR_OUT_OF_MEMORY, "Allocation exception escaped ABI");
	Require(AbiBoundary<Throw>::Invoke(false) == XR_ERROR_RUNTIME_FAILURE, "Exception escaped ABI");
	std::cout << "Handle registry tests passed\n";
}
