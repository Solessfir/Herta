#include "Herta/Networking/NetworkTransport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <thread>

namespace Herta
{
namespace
{
[[nodiscard]] std::vector<std::byte> Bytes(const std::string_view Text)
{
	const auto View = std::as_bytes(std::span(Text.data(), Text.size()));
	return {View.begin(), View.end()};
}

// Polls until Done holds or two seconds pass, since real sockets take a few milliseconds.
bool PollUntil(FNetworkTransport& Transport, std::vector<FNetworkConnectionEvent>& Events, std::vector<FNetworkMessage>& Messages, const std::function<bool()>& Done)
{
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < Deadline)
	{
		Transport.Poll(Events, Messages);
		if (Done())
		{
			return true;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	return false;
}
}

TEST_CASE("Network transports connect over UDP and loopback and exchange reliable and unreliable messages")
{
	auto Server = FNetworkTransport::Create();
	auto Client = FNetworkTransport::Create();
	REQUIRE(Server);
	REQUIRE(Client);
	const auto Listening = (*Server)->Listen(0);
	INFO((Listening ? std::string{} : Listening.error().Message));
	REQUIRE(Listening);
	REQUIRE((*Server)->GetListenPort() != 0);
	CHECK(!(*Client)->Connect("not an address"));

	const auto Outgoing = (*Client)->Connect(std::format("127.0.0.1:{}", (*Server)->GetListenPort()));
	REQUIRE(Outgoing);
	std::vector<FNetworkConnectionEvent> ServerEvents;
	std::vector<FNetworkMessage> ServerMessages;
	REQUIRE(PollUntil(**Server, ServerEvents, ServerMessages, [&] { return !ServerEvents.empty(); }));
	REQUIRE(ServerEvents.front().State == ENetworkConnectionState::Requested);
	const FNetworkConnectionId Incoming = ServerEvents.front().Connection;
	REQUIRE((*Server)->Accept(Incoming));

	std::vector<FNetworkConnectionEvent> ClientEvents;
	std::vector<FNetworkMessage> ClientMessages;
	REQUIRE(PollUntil(**Client, ClientEvents, ClientMessages, [&] { return std::ranges::any_of(ClientEvents, [](const auto& Event) { return Event.State == ENetworkConnectionState::Connected; }); }));
	REQUIRE((*Client)->Send(*Outgoing, Bytes("hello"), ENetworkDelivery::Reliable));
	REQUIRE((*Client)->Send(*Outgoing, Bytes("state"), ENetworkDelivery::Unreliable));
	REQUIRE(PollUntil(**Server, ServerEvents, ServerMessages, [&] { return ServerMessages.size() >= 2; }));
	CHECK(ServerMessages[0].Data == Bytes("hello"));
	CHECK(ServerMessages[0].Connection == Incoming);

	(*Client)->Close(*Outgoing, "Bye");
	ServerEvents.clear();
	REQUIRE(PollUntil(**Server, ServerEvents, ServerMessages, [&] { return std::ranges::any_of(ServerEvents, [](const auto& Event) { return Event.State == ENetworkConnectionState::Closed; }); }));

	// A loopback pair goes through the same messages within one transport.
	const auto Pair = (*Server)->ConnectLoopback();
	REQUIRE(Pair);
	REQUIRE((*Server)->Send(Pair->first, Bytes("loop"), ENetworkDelivery::Reliable));
	ServerMessages.clear();
	REQUIRE(PollUntil(**Server, ServerEvents, ServerMessages, [&] { return !ServerMessages.empty(); }));
	CHECK(ServerMessages.front().Connection == Pair->second);
	CHECK(ServerMessages.front().Data == Bytes("loop"));
}
}
