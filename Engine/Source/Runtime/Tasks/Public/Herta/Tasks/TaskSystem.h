#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace Herta
{
class FLogService;

enum class ETaskLane : std::uint8_t
{
	Cpu,
	BlockingIo,
	MainThread
};

enum class ETaskPriority : std::uint8_t
{
	High,
	Normal,
	Low
};

enum class ETaskState : std::uint8_t
{
	Invalid,
	Queued,
	Running,
	Succeeded,
	Cancelled,
	Failed
};

enum class ETaskErrorCode : std::uint8_t
{
	InvalidDescription,
	InvalidHandle,
	InvalidScope,
	QueueFull,
	ShuttingDown,
	UnregisteredThread,
	InitializationFailed
};

struct FTaskError
{
	ETaskErrorCode Code = ETaskErrorCode::InitializationFailed;
	std::string Message;
};

struct FTaskResult
{
	ETaskState State = ETaskState::Invalid;
	std::string ErrorMessage;
};

struct FTaskDescription
{
	std::string Name;
	ETaskLane Lane = ETaskLane::Cpu;
	ETaskPriority Priority = ETaskPriority::Normal;
};

struct FTaskSystemOptions
{
	std::size_t CpuWorkerCount = 0;
	std::size_t BlockingIoWorkerCount = 2;
	std::size_t MaximumQueuedCpuTasks = 128;
	std::size_t MaximumQueuedIoTasks = 128;
	bool bDeterministic = false;
	// The composition root owns logging and keeps it alive until the task system has drained.
	FLogService* Log = nullptr;
};

class FCancellationToken final
{
public:
	struct FState;

	FCancellationToken() = default;

	[[nodiscard]] bool IsCancellationRequested() const noexcept;
	[[nodiscard]] bool IsValid() const noexcept;

private:
	explicit FCancellationToken(std::shared_ptr<FState> State) noexcept;

	std::shared_ptr<FState> State;

	friend class FCancellationSource;
};

class FCancellationSource final
{
public:
	FCancellationSource();

	[[nodiscard]] FCancellationToken GetToken() const noexcept;
	void RequestCancellation() noexcept;
	[[nodiscard]] bool IsCancellationRequested() const noexcept;

private:
	std::shared_ptr<FCancellationToken::FState> State;
};

class FTaskContext final
{
public:
	[[nodiscard]] bool IsCancellationRequested() const noexcept;
	[[nodiscard]] const FCancellationToken& GetCancellationToken() const noexcept;
	void ReportProgress(float Progress) noexcept;

private:
	FTaskContext(FCancellationToken CancellationToken, std::shared_ptr<std::atomic<float>> Progress) noexcept;

	FCancellationToken CancellationToken;
	std::shared_ptr<std::atomic<float>> Progress;

	friend class FTaskSystem;
};

using FTaskFunction = std::move_only_function<void(FTaskContext&)>;
using FParallelForFunction = std::function<void(std::size_t, FTaskContext&)>;

class FTaskHandle final
{
public:
	struct FState;

	FTaskHandle() = default;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool IsComplete() const noexcept;
	[[nodiscard]] ETaskState GetState() const noexcept;
	[[nodiscard]] float GetProgress() const noexcept;
	[[nodiscard]] FTaskResult GetResult() const;
	[[nodiscard]] FTaskResult Wait() const;

private:
	explicit FTaskHandle(std::shared_ptr<FState> State) noexcept;

	std::shared_ptr<FState> State;

	friend class FTaskSystem;
};

class FTaskScope final
{
public:
	struct FState;

	~FTaskScope();

	FTaskScope(const FTaskScope&) = delete;
	FTaskScope& operator=(const FTaskScope&) = delete;
	FTaskScope(FTaskScope&&) = delete;
	FTaskScope& operator=(FTaskScope&&) = delete;

	[[nodiscard]] std::string_view GetName() const noexcept;
	[[nodiscard]] FCancellationToken GetCancellationToken() const noexcept;
	[[nodiscard]] bool IsCancellationRequested() const noexcept;
	[[nodiscard]] std::size_t GetOutstandingTaskCount() const noexcept;
	void RequestCancellation() noexcept;
	// Scope waits pump main-thread continuations, so scopes are owned and destroyed on the creating thread.
	void Wait() noexcept;

private:
	explicit FTaskScope(std::shared_ptr<FState> State) noexcept;

	std::shared_ptr<FState> State;

	friend class FTaskSystem;
};

class FTaskSystem final
{
public:
	struct FImplementation;

	[[nodiscard]] static std::expected<std::unique_ptr<FTaskSystem>, FTaskError> Create(FTaskSystemOptions Options = {});

	~FTaskSystem();

	FTaskSystem(const FTaskSystem&) = delete;
	FTaskSystem& operator=(const FTaskSystem&) = delete;
	FTaskSystem(FTaskSystem&&) = delete;
	FTaskSystem& operator=(FTaskSystem&&) = delete;

	[[nodiscard]] std::expected<std::unique_ptr<FTaskScope>, FTaskError> CreateScope(std::string Name);
	[[nodiscard]] std::expected<FTaskHandle, FTaskError> Submit(FTaskScope& Scope, FTaskDescription Description, FTaskFunction Function);
	[[nodiscard]] std::expected<FTaskHandle, FTaskError> ContinueOnMainThread(FTaskScope& Scope, const FTaskHandle& Prerequisite, std::string Name, FTaskFunction Function);
	[[nodiscard]] std::expected<FTaskHandle, FTaskError> WhenAll(FTaskScope& Scope, std::span<const FTaskHandle> Prerequisites, std::string Name);
	[[nodiscard]] std::expected<FTaskHandle, FTaskError> ParallelFor(FTaskScope& Scope, FTaskDescription Description, std::size_t ItemCount, std::size_t MinimumItemsPerTask, FParallelForFunction Function);

	// These functions own the main continuation lane and are called only from the thread that created the system.
	std::size_t RunMainThreadTasks(std::size_t MaximumTasks = std::numeric_limits<std::size_t>::max());
	std::size_t RunUntilIdle();
	void Shutdown() noexcept;

	[[nodiscard]] bool IsDeterministic() const noexcept;
	[[nodiscard]] std::size_t GetCpuWorkerCount() const noexcept;
	[[nodiscard]] std::size_t GetBlockingIoWorkerCount() const noexcept;

private:
	explicit FTaskSystem(std::shared_ptr<FImplementation> Implementation) noexcept;
	[[nodiscard]] static FTaskContext CreateTaskContext(FCancellationToken CancellationToken, std::shared_ptr<std::atomic<float>> Progress) noexcept;

	std::shared_ptr<FImplementation> Implementation;

	friend class FTaskHandle;
	friend class FTaskScope;
};
}
