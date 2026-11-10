#include "Herta/Networking/NetworkTransport.h"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <random>
#include <string>
#include <utility>

namespace Herta
{
namespace
{
// GameNetworkingSockets is one library-wide instance, so transports share it and the last one shuts it down.
std::mutex LibraryMutex;
std::size_t LibraryUsers = 0;
// The library explains failures only through its debug output, so the latest warning goes into the next error.
std::mutex WarningMutex;
std::string LastWarning;

void OnDebugOutput(ESteamNetworkingSocketsDebugOutputType, const char* const Message)
{
	const std::scoped_lock Lock(WarningMutex);
	LastWarning = Message;
}

[[nodiscard]] FNetworkError MakeError(std::string Message)
{
	const std::scoped_lock Lock(WarningMutex);
	if (!LastWarning.empty())
	{
		Message += std::format(" ({})", std::exchange(LastWarning, {}));
	}

	return FNetworkError{std::move(Message)};
}

[[nodiscard]] std::expected<void, FNetworkError> AcquireLibrary()
{
	const std::scoped_lock Lock(LibraryMutex);
	if (LibraryUsers++ > 0)
	{
		return {};
	}

	SteamNetworkingErrMsg Error{};
	if (!GameNetworkingSockets_Init(nullptr, Error))
	{
		LibraryUsers = 0;
		return std::unexpected(FNetworkError{std::format("Cannot start GameNetworkingSockets: {}", Error)});
	}

	// Direct-IP play has no certificate authority, so peers connect encrypted with keys of their own rather than signed identities.
	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
	SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, &OnDebugOutput);
	return {};
}

void ReleaseLibrary()
{
	const std::scoped_lock Lock(LibraryMutex);
	if (LibraryUsers > 0 && --LibraryUsers == 0)
	{
		GameNetworkingSockets_Kill();
	}
}
}

struct FNetworkTransport::FImplementation
{
	static void OnStatusChanged(SteamNetConnectionStatusChangedCallback_t* const Info)
	{
		auto* const Transport = reinterpret_cast<FImplementation*>(static_cast<std::intptr_t>(Info->m_info.m_nUserData));
		if (Transport)
		{
			Transport->HandleStatus(*Info);
		}
	}

	void HandleStatus(const SteamNetConnectionStatusChangedCallback_t& Info)
	{
		const FNetworkConnectionId Connection{Info.m_hConn};
		switch (Info.m_info.m_eState)
		{
			case k_ESteamNetworkingConnectionState_Connecting:
				// Only a listen socket's incoming connections wait for an answer; outgoing ones are already ours.
				if (Info.m_info.m_hListenSocket != k_HSteamListenSocket_Invalid)
				{
					Events.push_back({.Connection = Connection, .State = ENetworkConnectionState::Requested});
				}

				break;
			case k_ESteamNetworkingConnectionState_Connected:
				Events.push_back({.Connection = Connection, .State = ENetworkConnectionState::Connected});
				break;
			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
				Events.push_back({.Connection = Connection, .State = ENetworkConnectionState::Closed, .Reason = Info.m_info.m_szEndDebug});
				Sockets->CloseConnection(Info.m_hConn, 0, nullptr, false);
				break;
			default:
				break;
		}
	}

	[[nodiscard]] std::array<SteamNetworkingConfigValue_t, 2> MakeOptions()
	{
		std::array<SteamNetworkingConfigValue_t, 2> Options{};
		Options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&OnStatusChanged));
		Options[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData, static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(this)));
		return Options;
	}

	ISteamNetworkingSockets* Sockets = nullptr;
	HSteamListenSocket ListenSocket = k_HSteamListenSocket_Invalid;
	HSteamNetPollGroup PollGroup = k_HSteamNetPollGroup_Invalid;
	std::uint16_t ListenPort = 0;
	std::vector<FNetworkConnectionEvent> Events{};
	std::vector<HSteamNetConnection> Connections{};
};

FNetworkTransport::FNetworkTransport(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FNetworkTransport::~FNetworkTransport()
{
	FImplementation& Transport = *Implementation;
	for (const HSteamNetConnection Connection : Transport.Connections)
	{
		Transport.Sockets->CloseConnection(Connection, 0, "Shutting down", false);
	}

	if (Transport.ListenSocket != k_HSteamListenSocket_Invalid)
	{
		Transport.Sockets->CloseListenSocket(Transport.ListenSocket);
	}

	Transport.Sockets->DestroyPollGroup(Transport.PollGroup);
	ReleaseLibrary();
}

std::expected<std::unique_ptr<FNetworkTransport>, FNetworkError> FNetworkTransport::Create()
{
	if (auto Acquired = AcquireLibrary(); !Acquired)
	{
		return std::unexpected(Acquired.error());
	}

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Sockets = SteamNetworkingSockets();
	Implementation->PollGroup = Implementation->Sockets->CreatePollGroup();
	if (Implementation->PollGroup == k_HSteamNetPollGroup_Invalid)
	{
		ReleaseLibrary();
		return std::unexpected(FNetworkError{"Cannot create a network poll group"});
	}

	return std::unique_ptr<FNetworkTransport>(new FNetworkTransport(std::move(Implementation)));
}

std::expected<void, FNetworkError> FNetworkTransport::Listen(const std::uint16_t Port)
{
	FImplementation& Transport = *Implementation;
	if (Transport.ListenSocket != k_HSteamListenSocket_Invalid)
	{
		return std::unexpected(FNetworkError{"The transport is already listening"});
	}

	// The library needs a port, so zero tries random ones until one binds. Each try is independent because Windows reserves whole runs of
	// ports for Hyper-V, which a sequential search could spend every attempt inside.
	constexpr int Attempts = 64;
	std::mt19937 Random(std::random_device{}());
	std::uniform_int_distribution<int> Ports(20000, 60999);
	const auto Options = Transport.MakeOptions();
	for (int Attempt = 0; Attempt < (Port == 0 ? Attempts : 1); ++Attempt)
	{
		SteamNetworkingIPAddr Address{};
		Address.Clear();
		Address.m_port = Port != 0 ? Port : static_cast<std::uint16_t>(Ports(Random));
		Transport.ListenSocket = Transport.Sockets->CreateListenSocketIP(Address, static_cast<int>(Options.size()), Options.data());
		if (Transport.ListenSocket != k_HSteamListenSocket_Invalid)
		{
			Transport.ListenPort = Address.m_port;
			return {};
		}
	}

	return std::unexpected(MakeError(Port != 0 ? std::format("Cannot listen on port {}", Port) : std::string("Cannot find a free port to listen on")));
}

std::uint16_t FNetworkTransport::GetListenPort() const
{
	return Implementation->ListenPort;
}

std::expected<FNetworkConnectionId, FNetworkError> FNetworkTransport::Connect(const std::string_view Address)
{
	FImplementation& Transport = *Implementation;
	SteamNetworkingIPAddr Remote{};
	if (!Remote.ParseString(std::string(Address).c_str()) || Remote.m_port == 0)
	{
		return std::unexpected(FNetworkError{std::format("'{}' is not an address and port", Address)});
	}

	const auto Options = Transport.MakeOptions();
	const HSteamNetConnection Connection = Transport.Sockets->ConnectByIPAddress(Remote, static_cast<int>(Options.size()), Options.data());
	if (Connection == k_HSteamNetConnection_Invalid)
	{
		return std::unexpected(MakeError(std::format("Cannot connect to {}", Address)));
	}

	Transport.Sockets->SetConnectionPollGroup(Connection, Transport.PollGroup);
	Transport.Connections.push_back(Connection);
	return FNetworkConnectionId{Connection};
}

std::expected<std::pair<FNetworkConnectionId, FNetworkConnectionId>, FNetworkError> FNetworkTransport::ConnectLoopback(FNetworkTransport& Peer)
{
	FImplementation& Transport = *Implementation;
	HSteamNetConnection First = k_HSteamNetConnection_Invalid;
	HSteamNetConnection Second = k_HSteamNetConnection_Invalid;
	if (!Transport.Sockets->CreateSocketPair(&First, &Second, true, nullptr, nullptr))
	{
		return std::unexpected(MakeError("Cannot create a loopback connection"));
	}

	// Each end belongs to its own transport, so each one's Poll sees only its own end, as across a real network.
	for (const auto& [Connection, Owner] : {std::pair{First, &Transport}, std::pair{Second, Peer.Implementation.get()}})
	{
		Owner->Sockets->SetConnectionPollGroup(Connection, Owner->PollGroup);
		Owner->Sockets->SetConnectionUserData(Connection, static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(Owner)));
		Owner->Connections.push_back(Connection);
		// A socket pair starts connected, so its connections are announced here rather than by status changes.
		Owner->Events.push_back({.Connection = {Connection}, .State = ENetworkConnectionState::Connected});
	}

	return std::pair{FNetworkConnectionId{First}, FNetworkConnectionId{Second}};
}

std::expected<void, FNetworkError> FNetworkTransport::Accept(const FNetworkConnectionId Connection)
{
	FImplementation& Transport = *Implementation;
	if (Transport.Sockets->AcceptConnection(Connection.Value) != k_EResultOK)
	{
		return std::unexpected(FNetworkError{"Cannot accept the connection; it may have closed"});
	}

	Transport.Sockets->SetConnectionPollGroup(Connection.Value, Transport.PollGroup);
	Transport.Connections.push_back(Connection.Value);
	return {};
}

void FNetworkTransport::Close(const FNetworkConnectionId Connection, const std::string_view Reason)
{
	FImplementation& Transport = *Implementation;
	Transport.Sockets->CloseConnection(Connection.Value, 0, std::string(Reason).c_str(), true);
	std::erase(Transport.Connections, Connection.Value);
}

std::expected<void, FNetworkError> FNetworkTransport::Send(const FNetworkConnectionId Connection, const std::span<const std::byte> Data, const ENetworkDelivery Delivery)
{
	const int Flags = Delivery == ENetworkDelivery::Reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle;
	const EResult Result = Implementation->Sockets->SendMessageToConnection(Connection.Value, Data.data(), static_cast<std::uint32_t>(Data.size()), Flags, nullptr);
	if (Result != k_EResultOK)
	{
		return std::unexpected(FNetworkError{std::format("Cannot send {} bytes: result {}", Data.size(), static_cast<int>(Result))});
	}

	return {};
}

void FNetworkTransport::Poll(std::vector<FNetworkConnectionEvent>& Events, std::vector<FNetworkMessage>& Messages)
{
	FImplementation& Transport = *Implementation;
	Transport.Sockets->RunCallbacks();
	for (const FNetworkConnectionEvent& Event : Transport.Events)
	{
		if (Event.State == ENetworkConnectionState::Closed)
		{
			std::erase(Transport.Connections, Event.Connection.Value);
		}
	}

	Events.insert(Events.end(), Transport.Events.begin(), Transport.Events.end());
	Transport.Events.clear();
	std::array<SteamNetworkingMessage_t*, 64> Received{};
	for (int Count = 0; (Count = Transport.Sockets->ReceiveMessagesOnPollGroup(Transport.PollGroup, Received.data(), static_cast<int>(Received.size()))) > 0;)
	{
		for (int Index = 0; Index < Count; ++Index)
		{
			SteamNetworkingMessage_t* const Message = Received[static_cast<std::size_t>(Index)];
			FNetworkMessage& Copy = Messages.emplace_back(FNetworkMessage{.Connection = {Message->m_conn}});
			Copy.Data.resize(static_cast<std::size_t>(Message->m_cbSize));
			std::memcpy(Copy.Data.data(), Message->m_pData, Copy.Data.size());
			Message->Release();
		}
	}
}

FNetworkConnectionStats FNetworkTransport::GetStats(const FNetworkConnectionId Connection) const
{
	SteamNetConnectionRealTimeStatus_t Status{};
	if (Implementation->Sockets->GetConnectionRealTimeStatus(Connection.Value, &Status, 0, nullptr) != k_EResultOK)
	{
		return {};
	}

	return {
	    .PingMilliseconds = static_cast<float>(Status.m_nPing),
	    .Quality = std::max(Status.m_flConnectionQualityLocal, 0.f),
	    .SentBytesPerSecond = Status.m_flOutBytesPerSec,
	    .ReceivedBytesPerSecond = Status.m_flInBytesPerSec,
	    .PendingReliableBytes = static_cast<std::uint32_t>(std::max(Status.m_cbPendingReliable, 0)),
	};
}

void FNetworkTransport::SetConditions(const FNetworkConditions& Conditions)
{
	ISteamNetworkingUtils* const Utils = SteamNetworkingUtils();
	Utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, Conditions.LossPercent);
	Utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, static_cast<std::int32_t>(Conditions.LagMilliseconds));
	Utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, static_cast<std::int32_t>(Conditions.JitterMilliseconds));
	Utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, static_cast<std::int32_t>(Conditions.JitterMilliseconds * 0.5f));
	Utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, Conditions.JitterMilliseconds > 0.f ? 100.f : 0.f);
	Utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Send, Conditions.ReorderPercent);
	Utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketReorder_Time, 30);
	Utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Send, Conditions.DuplicatePercent);
	Utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketDup_TimeMax, 20);
	Utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakeRateLimit_Send_Rate, static_cast<std::int32_t>(Conditions.RateLimitBytesPerSecond));
}
}
