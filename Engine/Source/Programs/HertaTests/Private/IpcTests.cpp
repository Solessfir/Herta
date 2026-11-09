#include "Herta/Platform/Ipc.h"

#include <doctest/doctest.h>

#include <chrono>
#include <format>
#include <random>
#include <string>
#include <thread>

namespace Herta
{
namespace
{
using namespace std::chrono_literals;

std::string UniqueEndpoint()
{
	std::random_device Device;
	return MakeIpcEndpoint(std::format("Test-{:08x}{:08x}", Device(), Device()));
}
}

TEST_CASE("IPC connections carry whole messages both ways and report a closed peer")
{
	const std::string Endpoint = UniqueEndpoint();
	auto Listener = FIpcListener::Listen(Endpoint);
	REQUIRE_MESSAGE(Listener, (Listener ? std::string() : Listener.error().Message));
	CHECK_FALSE(FIpcListener::Listen(Endpoint));

	// Nobody connects yet, so accepting times out empty.
	const auto Nobody = Listener->Accept(10ms);
	REQUIRE(Nobody);
	CHECK_FALSE(Nobody->has_value());

	std::string Large(300'000, 'x');
	Large.back() = 'y';
	std::jthread Client([&Endpoint, &Large]
	{
		auto Connection = FIpcConnection::Connect(Endpoint, 2s);
		REQUIRE(Connection);
		REQUIRE(Connection->Send("hello"));
		REQUIRE(Connection->Send(""));
		REQUIRE(Connection->Send(Large));
		const auto Reply = Connection->Receive(5s);
		REQUIRE(Reply);
		REQUIRE(Reply->has_value());
		CHECK(**Reply == "world");
	});

	std::optional<FIpcConnection> Server;
	for (int Attempt = 0; Attempt < 200 && !Server; ++Attempt)
	{
		auto Accepted = Listener->Accept(20ms);
		REQUIRE(Accepted);
		if (*Accepted)
		{
			Server = std::move(*Accepted);
		}
	}

	REQUIRE(Server);
	const auto First = Server->Receive(5s);
	REQUIRE(First);
	REQUIRE(First->has_value());
	CHECK(**First == "hello");
	const auto Empty = Server->Receive(5s);
	REQUIRE(Empty);
	REQUIRE(Empty->has_value());
	CHECK((*Empty)->empty());
	const auto Big = Server->Receive(5s);
	REQUIRE(Big);
	REQUIRE(Big->has_value());
	CHECK(**Big == Large);
	REQUIRE(Server->Send("world"));
	Client.join();

	// Once the client is gone, reading reports the disconnect instead of waiting forever.
	auto Closed = Server->Receive(2s);
	for (int Attempt = 0; Attempt < 20 && Closed && !Closed->has_value(); ++Attempt)
	{
		Closed = Server->Receive(100ms);
	}

	REQUIRE_FALSE(Closed);
	CHECK(Closed.error().bDisconnected);

	CHECK_FALSE(Server->Send(std::string(MaximumIpcMessageBytes + 1, 'z')));
}

TEST_CASE("IPC connections fail promptly without a listener and time out without a message")
{
	CHECK_FALSE(FIpcConnection::Connect(UniqueEndpoint(), 50ms));

	const std::string Endpoint = UniqueEndpoint();
	auto Listener = FIpcListener::Listen(Endpoint);
	REQUIRE(Listener);
	auto Client = FIpcConnection::Connect(Endpoint, 2s);
	REQUIRE(Client);
	std::optional<FIpcConnection> Server;
	for (int Attempt = 0; Attempt < 100 && !Server; ++Attempt)
	{
		auto Accepted = Listener->Accept(20ms);
		REQUIRE(Accepted);
		if (*Accepted)
		{
			Server = std::move(*Accepted);
		}
	}

	REQUIRE(Server);
	const auto Start = std::chrono::steady_clock::now();
	const auto Nothing = Server->Receive(50ms);
	REQUIRE(Nothing);
	CHECK_FALSE(Nothing->has_value());
	CHECK(std::chrono::steady_clock::now() - Start < 2s);

	// A second client can connect after the first while the listener keeps running.
	auto Second = FIpcConnection::Connect(Endpoint, 2s);
	CHECK(Second);
}
}
