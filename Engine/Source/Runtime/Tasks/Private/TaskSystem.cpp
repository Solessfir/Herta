#include "Herta/Tasks/TaskSystem.h"

#include "Herta/Core/Log.h"

#include <TaskScheduler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <format>
#include <latch>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
struct FQueuedTask;
struct FCpuTaskNode;
inline constexpr FLogCategory TasksLog{.Name = "Tasks"};
inline constexpr std::chrono::milliseconds MainThreadWaitWarningThreshold{16};
thread_local FTaskSystem::FImplementation* GBlockingIoWorkerSystem = nullptr;

[[nodiscard]] constexpr bool IsTerminalState(const ETaskState State) noexcept
{
	return State == ETaskState::Succeeded || State == ETaskState::Cancelled || State == ETaskState::Failed;
}

[[nodiscard]] constexpr std::size_t GetPriorityIndex(const ETaskPriority Priority) noexcept
{
	return static_cast<std::size_t>(Priority);
}

[[nodiscard]] constexpr enki::TaskPriority ToEnkiPriority(const ETaskPriority Priority) noexcept
{
	switch (Priority)
	{
		case ETaskPriority::High:
			return enki::TASK_PRIORITY_HIGH;
		case ETaskPriority::Normal:
			return enki::TASK_PRIORITY_MED;
		case ETaskPriority::Low:
			return enki::TASK_PRIORITY_LOW;
	}

	return enki::TASK_PRIORITY_MED;
}
}

struct FCancellationToken::FState
{
	std::atomic_bool bCancellationRequested = false;
};

struct FTaskScope::FState
{
	std::string Name;
	FCancellationSource CancellationSource;
	std::weak_ptr<FTaskSystem::FImplementation> System;
	mutable std::mutex SubmissionMutex;
	bool bAcceptingTasks = true;
	std::atomic_size_t OutstandingTaskCount = 0;
	mutable std::mutex WaitMutex;
	std::condition_variable WaitCondition;
};

struct FTaskHandle::FState
{
	struct FCompletionCallback
	{
		std::function<void()> Function;
		std::shared_ptr<FState> Dependent;
	};

	std::string Name;
	ETaskLane Lane = ETaskLane::Cpu;
	std::atomic<ETaskState> State = ETaskState::Queued;
	std::shared_ptr<std::atomic<float>> Progress = std::make_shared<std::atomic<float>>(0.f);
	std::weak_ptr<FTaskSystem::FImplementation> System;
	std::weak_ptr<FTaskScope::FState> Scope;
	std::weak_ptr<FCpuTaskNode> CpuTask;
	mutable std::mutex CompletionMutex;
	std::condition_variable CompletionCondition;
	std::string ErrorMessage;
	std::vector<FCompletionCallback> CompletionCallbacks;
	std::atomic_bool bCapacityClaimed = false;
	std::atomic_bool bCompletionBookkeepingFinished = false;
};

namespace
{
struct FQueuedTask
{
	FTaskDescription Description;
	FTaskFunction Function;
	std::shared_ptr<FTaskHandle::FState> State;
	std::shared_ptr<FTaskScope::FState> Scope;
};

struct FWhenAllState
{
	std::atomic_size_t RemainingCount = 0;
	std::atomic_bool bAnyCancelled = false;
	std::atomic_bool bAnyFailed = false;
	std::shared_ptr<FTaskHandle::FState> Barrier;
	std::weak_ptr<FTaskSystem::FImplementation> System;
};

struct FIoWorkerRegistrationState
{
	explicit FIoWorkerRegistrationState(const std::size_t WorkerCount)
	    : RegistrationLatch(static_cast<std::ptrdiff_t>(WorkerCount))
	{
	}

	std::latch RegistrationLatch;
	std::atomic_bool bRegistrationFailed = false;
};

struct FCpuTaskNode final : enki::ITaskSet
{
	FCpuTaskNode(FTaskSystem::FImplementation* System, std::shared_ptr<FQueuedTask> Task);
	void ExecuteRange(enki::TaskSetPartition Range, std::uint32_t ThreadIndex) override;

	FTaskSystem::FImplementation* System = nullptr;
	std::shared_ptr<FQueuedTask> Task;
};
}

struct FTaskSystem::FImplementation final : std::enable_shared_from_this<FImplementation>
{
	explicit FImplementation(FTaskSystemOptions InOptions)
	    : Options(InOptions)
	    , MainThreadId(std::this_thread::get_id())
	{
		if (!Options.bDeterministic)
		{
			const std::size_t HardwareThreadCount = std::max<std::size_t>(1, std::thread::hardware_concurrency());
			CpuWorkerCount = Options.CpuWorkerCount == 0 ? std::max<std::size_t>(1, HardwareThreadCount - 1) : Options.CpuWorkerCount;
			BlockingIoWorkerCount = Options.BlockingIoWorkerCount;
		}
	}

	[[nodiscard]] std::expected<void, FTaskError> Initialize()
	{
		if (Options.MaximumQueuedCpuTasks == 0 || Options.MaximumQueuedIoTasks == 0)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "Task queue capacities must be greater than zero"});
		}

		if (Options.bDeterministic)
		{
			return {};
		}

		if (CpuWorkerCount > std::numeric_limits<std::uint32_t>::max() || BlockingIoWorkerCount > std::numeric_limits<std::uint32_t>::max())
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "Task worker counts exceed the enkiTS thread limit"});
		}

		if (BlockingIoWorkerCount == 0)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "At least one blocking IO worker is required"});
		}

		try
		{
			enki::TaskSchedulerConfig Config = CpuScheduler.GetConfig();
			Config.numTaskThreadsToCreate = static_cast<std::uint32_t>(CpuWorkerCount);
			Config.numExternalTaskThreads = static_cast<std::uint32_t>(BlockingIoWorkerCount);
			CpuScheduler.Initialize(Config);
			bCpuSchedulerInitialized = true;

			const auto RegistrationState = std::make_shared<FIoWorkerRegistrationState>(BlockingIoWorkerCount);
			BlockingIoThreads.reserve(BlockingIoWorkerCount);

			for (std::size_t Index = 0; Index < BlockingIoWorkerCount; ++Index)
			{
				BlockingIoThreads.emplace_back([this, RegistrationState]
				{
					const bool bRegistered = CpuScheduler.RegisterExternalTaskThread();
					if (!bRegistered)
					{
						RegistrationState->bRegistrationFailed.store(true, std::memory_order_relaxed);
					}

					RegistrationState->RegistrationLatch.count_down();

					if (!bRegistered)
					{
						return;
					}

					RunBlockingIoWorker();
					CpuScheduler.DeRegisterExternalTaskThread();
				});
			}

			RegistrationState->RegistrationLatch.wait();
			if (RegistrationState->bRegistrationFailed.load(std::memory_order_relaxed))
			{
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "Could not register a blocking IO worker with enkiTS"});
			}
		}
		catch (const std::exception& Exception)
		{
			StopPartiallyInitializedWorkers();
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = std::format("Could not initialize the task system: {}", Exception.what())});
		}
		catch (...)
		{
			StopPartiallyInitializedWorkers();
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "Could not initialize the task system due to an unknown error"});
		}

		return {};
	}

	[[nodiscard]] std::shared_ptr<FTaskScope::FState> CreateScope(std::string Name)
	{
		auto Scope = std::make_shared<FTaskScope::FState>();
		Scope->Name = std::move(Name);
		Scope->System = shared_from_this();

		std::scoped_lock Lock(ScopesMutex);
		std::erase_if(Scopes, [](const std::weak_ptr<FTaskScope::FState>& ExistingScope)
		{
			return ExistingScope.expired();
		});

		Scopes.emplace_back(Scope);
		return Scope;
	}

	[[nodiscard]] std::expected<std::shared_ptr<FTaskHandle::FState>, FTaskError> Submit(const std::shared_ptr<FTaskScope::FState>& Scope, FTaskDescription Description, FTaskFunction Function)
	{
		if (!Scope || Scope->System.lock().get() != this)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidScope, .Message = "The task scope does not belong to this task system"});
		}

		if (Description.Name.empty() || !Function)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidDescription, .Message = "A task requires a profiling name and callable"});
		}

		const ETaskLane Lane = Description.Lane;
		const std::string TaskName = Description.Name;
		bool bScopeTaskClaimed = false;
		bool bQueueCapacityClaimed = false;

		try
		{
			auto State = std::make_shared<FTaskHandle::FState>();
			State->Name = Description.Name;
			State->Lane = Description.Lane;
			State->System = shared_from_this();
			State->Scope = Scope;
			State->bCapacityClaimed = Description.Lane != ETaskLane::MainThread;

			auto Task = std::make_shared<FQueuedTask>(FQueuedTask{
			    .Description = std::move(Description),
			    .Function = std::move(Function),
			    .State = State,
			    .Scope = Scope});

			std::scoped_lock LifecycleLock(LifecycleMutex);
			if (!bAcceptingTasks)
			{
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::ShuttingDown, .Message = "The task system is shutting down"});
			}

			if (Lane == ETaskLane::Cpu)
			{
				CollectCompletedCpuTasks();
			}

			if (!Options.bDeterministic && Lane == ETaskLane::Cpu && CpuScheduler.GetThreadNum() == enki::NO_THREAD_NUM)
			{
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::UnregisteredThread, .Message = "CPU tasks may be submitted from the main thread, task workers, or registered IO workers"});
			}

			if (!ClaimQueueCapacity(Lane))
			{
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::QueueFull, .Message = std::format("The {} task queue is full", GetLaneName(Lane))});
			}

			bQueueCapacityClaimed = true;

			if (!TryClaimScopeTask(Scope))
			{
				ReleaseQueueCapacity(Lane);
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidScope, .Message = "The task scope is cancelled or closing"});
			}

			bScopeTaskClaimed = true;

			Enqueue(Task);
			return State;
		}
		catch (const std::exception& Exception)
		{
			if (bQueueCapacityClaimed)
			{
				ReleaseQueueCapacity(Lane);
			}

			if (bScopeTaskClaimed)
			{
				ReleaseScopeTask(Scope);
			}

			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = std::format("Could not submit task '{}': {}", TaskName, Exception.what())});
		}
		catch (...)
		{
			if (bQueueCapacityClaimed)
			{
				ReleaseQueueCapacity(Lane);
			}

			if (bScopeTaskClaimed)
			{
				ReleaseScopeTask(Scope);
			}

			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = std::format("Could not submit task '{}' due to an unknown error", TaskName)});
		}
	}

	[[nodiscard]] std::expected<std::shared_ptr<FTaskHandle::FState>, FTaskError> ContinueOnMainThread(const std::shared_ptr<FTaskScope::FState>& Scope, const std::shared_ptr<FTaskHandle::FState>& Prerequisite, std::string Name, FTaskFunction Function)
	{
		if (!Scope || Scope->System.lock().get() != this)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidScope, .Message = "The task scope does not belong to this task system"});
		}

		if (!Prerequisite || Prerequisite->System.lock().get() != this || Prerequisite->Scope.lock() != Scope)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidHandle, .Message = "The prerequisite task must belong to the same task system and scope"});
		}

		if (Name.empty() || !Function)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidDescription, .Message = "A continuation requires a profiling name and callable"});
		}

		std::shared_ptr<FTaskHandle::FState> State;
		bool bScopeTaskClaimed = false;
		try
		{
			State = std::make_shared<FTaskHandle::FState>();
			State->Name = Name;
			State->Lane = ETaskLane::MainThread;
			State->System = shared_from_this();
			State->Scope = Scope;

			auto Task = std::make_shared<FQueuedTask>(FQueuedTask{
			    .Description = FTaskDescription{.Name = std::move(Name), .Lane = ETaskLane::MainThread, .Priority = ETaskPriority::Normal},
			    .Function = std::move(Function),
			    .State = State,
			    .Scope = Scope});

			std::scoped_lock LifecycleLock(LifecycleMutex);
			if (!bAcceptingTasks)
			{
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::ShuttingDown, .Message = "The task system is shutting down"});
			}

			if (!TryClaimScopeTask(Scope))
			{
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidScope, .Message = "The task scope is cancelled or closing"});
			}

			bScopeTaskClaimed = true;

			const std::weak_ptr<FImplementation> WeakSystem = shared_from_this();
			AddCompletionCallback(Prerequisite, State, [WeakSystem, Task]
			{
				if (const std::shared_ptr<FImplementation> System = WeakSystem.lock())
				{
					System->TryEnqueueDependent(Task);
				}
			});

			return State;
		}
		catch (const std::exception& Exception)
		{
			if (bScopeTaskClaimed)
			{
				FinishTask(State, ETaskState::Failed, Exception.what());
			}

			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = std::format("Could not create main-thread continuation: {}", Exception.what())});
		}
		catch (...)
		{
			if (bScopeTaskClaimed)
			{
				FinishTask(State, ETaskState::Failed, "Could not register the main-thread continuation");
			}

			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "Could not create main-thread continuation due to an unknown error"});
		}
	}

	[[nodiscard]] std::expected<std::shared_ptr<FTaskHandle::FState>, FTaskError> WhenAll(const std::shared_ptr<FTaskScope::FState>& Scope, const std::span<const FTaskHandle> Prerequisites, std::string Name)
	{
		if (!Scope || Scope->System.lock().get() != this)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidScope, .Message = "The task scope does not belong to this task system"});
		}

		if (Name.empty())
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidDescription, .Message = "A task barrier requires a profiling name"});
		}

		for (const FTaskHandle& Prerequisite : Prerequisites)
		{
			if (!Prerequisite.State || Prerequisite.State->System.lock().get() != this || Prerequisite.State->Scope.lock() != Scope)
			{
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidHandle, .Message = "Every prerequisite task must belong to the same task system and scope"});
			}
		}

		std::shared_ptr<FTaskHandle::FState> Barrier;
		bool bScopeTaskClaimed = false;
		try
		{
			Barrier = std::make_shared<FTaskHandle::FState>();
			Barrier->Name = std::move(Name);
			Barrier->Lane = ETaskLane::MainThread;
			Barrier->System = shared_from_this();
			Barrier->Scope = Scope;

			auto WhenAllState = std::make_shared<FWhenAllState>();
			WhenAllState->RemainingCount.store(Prerequisites.size(), std::memory_order_relaxed);
			WhenAllState->Barrier = Barrier;
			WhenAllState->System = shared_from_this();

			std::scoped_lock LifecycleLock(LifecycleMutex);
			if (!bAcceptingTasks)
			{
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::ShuttingDown, .Message = "The task system is shutting down"});
			}

			if (!TryClaimScopeTask(Scope))
			{
				return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidScope, .Message = "The task scope is cancelled or closing"});
			}

			bScopeTaskClaimed = true;

			if (Prerequisites.empty())
			{
				FinishTask(Barrier, Scope->CancellationSource.IsCancellationRequested() ? ETaskState::Cancelled : ETaskState::Succeeded, {});
				return Barrier;
			}

			for (const FTaskHandle& Prerequisite : Prerequisites)
			{
				const std::shared_ptr<FTaskHandle::FState> PrerequisiteState = Prerequisite.State;
				AddCompletionCallback(PrerequisiteState, Barrier, [WhenAllState, PrerequisiteState]
				{
					const ETaskState State = PrerequisiteState->State.load(std::memory_order_acquire);
					if (State == ETaskState::Failed)
					{
						WhenAllState->bAnyFailed.store(true, std::memory_order_relaxed);
					}
					else if (State == ETaskState::Cancelled)
					{
						WhenAllState->bAnyCancelled.store(true, std::memory_order_relaxed);
					}

					if (WhenAllState->RemainingCount.fetch_sub(1, std::memory_order_acq_rel) != 1)
					{
						return;
					}

					if (const std::shared_ptr<FImplementation> System = WhenAllState->System.lock())
					{
						if (WhenAllState->bAnyFailed.load(std::memory_order_relaxed))
						{
							System->FinishTask(WhenAllState->Barrier, ETaskState::Failed, "One or more prerequisite tasks failed");
						}
						else if (WhenAllState->bAnyCancelled.load(std::memory_order_relaxed))
						{
							System->FinishTask(WhenAllState->Barrier, ETaskState::Cancelled, {});
						}
						else
						{
							System->FinishTask(WhenAllState->Barrier, ETaskState::Succeeded, {});
						}
					}
				});
			}

			return Barrier;
		}
		catch (const std::exception& Exception)
		{
			if (bScopeTaskClaimed)
			{
				FinishTask(Barrier, ETaskState::Failed, Exception.what());
			}

			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = std::format("Could not create task barrier: {}", Exception.what())});
		}
		catch (...)
		{
			if (bScopeTaskClaimed)
			{
				FinishTask(Barrier, ETaskState::Failed, "Could not register the task barrier");
			}

			return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "Could not create task barrier due to an unknown error"});
		}
	}

	void TryEnqueueDependent(const std::shared_ptr<FQueuedTask>& Task) noexcept
	{
		try
		{
			std::scoped_lock LifecycleLock(LifecycleMutex);
			if (!bAcceptingTasks || !IsScopeAcceptingTasks(Task->Scope))
			{
				FinishTask(Task->State, ETaskState::Cancelled, {});
				return;
			}

			Enqueue(Task);
		}
		catch (const std::exception& Exception)
		{
			FinishTask(Task->State, ETaskState::Failed, Exception.what());
		}
		catch (...)
		{
			FinishTask(Task->State, ETaskState::Failed, "Could not enqueue dependent task");
		}
	}

	void Enqueue(const std::shared_ptr<FQueuedTask>& Task)
	{
		if (Options.bDeterministic)
		{
			std::scoped_lock Lock(DeterministicQueueMutex);
			DeterministicQueues[GetPriorityIndex(Task->Description.Priority)].push_back(Task);
			return;
		}

		switch (Task->Description.Lane)
		{
			case ETaskLane::Cpu:
			{
				auto CpuTask = std::make_shared<FCpuTaskNode>(this, Task);
				Task->State->CpuTask = CpuTask;

				{
					std::scoped_lock Lock(CpuTasksMutex);
					CpuTasks.emplace_back(CpuTask);
				}

				CpuScheduler.AddTaskSetToPipe(CpuTask.get());
				break;
			}
			case ETaskLane::BlockingIo:
			{
				std::scoped_lock Lock(BlockingIoQueueMutex);
				BlockingIoQueues[GetPriorityIndex(Task->Description.Priority)].push_back(Task);
				BlockingIoCondition.notify_one();
				break;
			}
			case ETaskLane::MainThread:
			{
				std::scoped_lock Lock(MainThreadQueueMutex);
				MainThreadQueues[GetPriorityIndex(Task->Description.Priority)].push_back(Task);
				break;
			}
		}
	}

	void ExecuteTask(const std::shared_ptr<FQueuedTask>& Task) noexcept
	{
		ReleaseTaskCapacity(*Task->State);

		if (Task->Scope->CancellationSource.IsCancellationRequested())
		{
			FinishTask(Task->State, ETaskState::Cancelled, {});
			return;
		}

		Task->State->State.store(ETaskState::Running, std::memory_order_release);

		try
		{
			FTaskContext Context = FTaskSystem::CreateTaskContext(Task->Scope->CancellationSource.GetToken(), Task->State->Progress);
			Task->Function(Context);

			const ETaskState FinalState = Context.IsCancellationRequested() ? ETaskState::Cancelled : ETaskState::Succeeded;
			FinishTask(Task->State, FinalState, {});
		}
		catch (const std::exception& Exception)
		{
			FinishTask(Task->State, ETaskState::Failed, Exception.what());
		}
		catch (...)
		{
			FinishTask(Task->State, ETaskState::Failed, "Task failed due to an unknown exception");
		}
	}

	void FinishTask(const std::shared_ptr<FTaskHandle::FState>& State, const ETaskState FinalState, std::string ErrorMessage) noexcept
	{
		std::vector<FTaskHandle::FState::FCompletionCallback> CompletionCallbacks;
		std::shared_ptr<FTaskScope::FState> Scope;

		{
			std::scoped_lock Lock(State->CompletionMutex);
			if (IsTerminalState(State->State.load(std::memory_order_relaxed)))
			{
				return;
			}

			State->ErrorMessage = std::move(ErrorMessage);
			if (FinalState == ETaskState::Succeeded)
			{
				State->Progress->store(1.f, std::memory_order_relaxed);
			}

			State->State.store(FinalState, std::memory_order_release);
			CompletionCallbacks = std::move(State->CompletionCallbacks);
			Scope = State->Scope.lock();
		}

		for (FTaskHandle::FState::FCompletionCallback& Callback : CompletionCallbacks)
		{
			try
			{
				Callback.Function();
			}
			catch (const std::exception& Exception)
			{
				FinishTask(Callback.Dependent, ETaskState::Failed, Exception.what());
			}
			catch (...)
			{
				FinishTask(Callback.Dependent, ETaskState::Failed, "Completion callback failed due to an unknown exception");
			}
		}

		if (Scope)
		{
			ReleaseScopeTask(Scope);
		}

		{
			// The predicate must change under the wait mutex so completion cannot notify between the waiter's check and sleep.
			std::scoped_lock Lock(State->CompletionMutex);
			State->bCompletionBookkeepingFinished.store(true, std::memory_order_release);
		}
		State->CompletionCondition.notify_all();
	}

	void AddCompletionCallback(const std::shared_ptr<FTaskHandle::FState>& State, const std::shared_ptr<FTaskHandle::FState>& Dependent, std::function<void()> Callback)
	{
		{
			std::scoped_lock Lock(State->CompletionMutex);
			if (!IsTerminalState(State->State.load(std::memory_order_relaxed)))
			{
				State->CompletionCallbacks.emplace_back(FTaskHandle::FState::FCompletionCallback{.Function = std::move(Callback), .Dependent = Dependent});
				return;
			}
		}

		try
		{
			Callback();
		}
		catch (const std::exception& Exception)
		{
			FinishTask(Dependent, ETaskState::Failed, Exception.what());
		}
		catch (...)
		{
			FinishTask(Dependent, ETaskState::Failed, "Completion callback failed due to an unknown exception");
		}
	}

	static void ReleaseScopeTask(const std::shared_ptr<FTaskScope::FState>& Scope) noexcept
	{
		if (Scope->OutstandingTaskCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			Scope->WaitCondition.notify_all();
		}
	}

	[[nodiscard]] static bool IsScopeAcceptingTasks(const std::shared_ptr<FTaskScope::FState>& Scope) noexcept
	{
		std::scoped_lock Lock(Scope->SubmissionMutex);
		return Scope->bAcceptingTasks;
	}

	[[nodiscard]] static bool TryClaimScopeTask(const std::shared_ptr<FTaskScope::FState>& Scope) noexcept
	{
		std::scoped_lock Lock(Scope->SubmissionMutex);
		if (!Scope->bAcceptingTasks)
		{
			return false;
		}

		Scope->OutstandingTaskCount.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	static void CloseScope(const std::shared_ptr<FTaskScope::FState>& Scope) noexcept
	{
		{
			std::scoped_lock Lock(Scope->SubmissionMutex);
			Scope->bAcceptingTasks = false;
		}
		Scope->CancellationSource.RequestCancellation();
	}

	[[nodiscard]] FTaskResult WaitForTask(const std::shared_ptr<FTaskHandle::FState>& State)
	{
		const auto WaitStart = std::chrono::steady_clock::now();
		if (Options.bDeterministic)
		{
			while (!State->bCompletionBookkeepingFinished.load(std::memory_order_acquire) && ExecuteOneDeterministicTask())
			{
			}
		}
		else if (State->Lane == ETaskLane::Cpu)
		{
			if (const std::shared_ptr<FCpuTaskNode> CpuTask = State->CpuTask.lock(); CpuTask && CpuScheduler.GetThreadNum() != enki::NO_THREAD_NUM)
			{
				CpuScheduler.WaitforTask(CpuTask.get());
			}
		}
		else if (State->Lane == ETaskLane::BlockingIo && GBlockingIoWorkerSystem == this)
		{
			while (!State->bCompletionBookkeepingFinished.load(std::memory_order_acquire) && ExecuteOneBlockingIoTask())
			{
			}
		}
		else if (State->Lane == ETaskLane::MainThread && std::this_thread::get_id() == MainThreadId)
		{
			while (!State->bCompletionBookkeepingFinished.load(std::memory_order_acquire) && RunMainThreadTasks(1) > 0)
			{
			}
		}

		std::unique_lock Lock(State->CompletionMutex);
		State->CompletionCondition.wait(Lock, [&State]
		{
			return State->bCompletionBookkeepingFinished.load(std::memory_order_acquire);
		});

		ReportMainThreadWait(State->Name, WaitStart);
		return {.State = State->State.load(std::memory_order_acquire), .ErrorMessage = State->ErrorMessage};
	}

	void WaitForScope(const std::shared_ptr<FTaskScope::FState>& Scope) noexcept
	{
		const auto WaitStart = std::chrono::steady_clock::now();
		while (Scope->OutstandingTaskCount.load(std::memory_order_acquire) > 0)
		{
			if (Options.bDeterministic)
			{
				if (ExecuteOneDeterministicTask())
				{
					continue;
				}
			}
			else if (std::this_thread::get_id() == MainThreadId && RunMainThreadTasks(1) > 0)
			{
				continue;
			}

			if (!Options.bDeterministic && CpuScheduler.GetThreadNum() != enki::NO_THREAD_NUM)
			{
				const std::shared_ptr<FCpuTaskNode> CpuTask = FindIncompleteCpuTask(Scope);
				if (CpuTask)
				{
					CpuScheduler.WaitforTask(CpuTask.get());
					continue;
				}
			}

			std::unique_lock Lock(Scope->WaitMutex);
			Scope->WaitCondition.wait_for(Lock, std::chrono::milliseconds(2), [&Scope]
			{
				return Scope->OutstandingTaskCount.load(std::memory_order_acquire) == 0;
			});
		}

		CollectCompletedCpuTasks();
		ReportMainThreadWait(Scope->Name, WaitStart);
	}

	void ReportMainThreadWait(const std::string_view Name, const std::chrono::steady_clock::time_point WaitStart) const noexcept
	{
		if (!Options.Log || std::this_thread::get_id() != MainThreadId)
		{
			return;
		}

		const auto WaitDuration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - WaitStart);
		if (WaitDuration >= MainThreadWaitWarningThreshold)
		{
			HERTA_LOG_WARNING(*Options.Log, TasksLog, "Main thread waited {} ms for task work '{}'", WaitDuration.count(), Name);
		}
	}

	[[nodiscard]] std::shared_ptr<FCpuTaskNode> FindIncompleteCpuTask(const std::shared_ptr<FTaskScope::FState>& Scope)
	{
		std::scoped_lock Lock(CpuTasksMutex);
		const auto Iterator = std::ranges::find_if(CpuTasks, [&Scope](const std::shared_ptr<FCpuTaskNode>& Task)
		{
			return !Task->GetIsComplete() && Task->Task->Scope == Scope;
		});

		return Iterator == CpuTasks.end() ? nullptr : *Iterator;
	}

	void RunBlockingIoWorker()
	{
		struct FWorkerRegistration final
		{
			explicit FWorkerRegistration(FImplementation& System) noexcept
			{
				GBlockingIoWorkerSystem = &System;
			}

			~FWorkerRegistration()
			{
				GBlockingIoWorkerSystem = nullptr;
			}

			FWorkerRegistration(const FWorkerRegistration&) = delete;
			FWorkerRegistration& operator=(const FWorkerRegistration&) = delete;
			FWorkerRegistration(FWorkerRegistration&&) = delete;
			FWorkerRegistration& operator=(FWorkerRegistration&&) = delete;
		} WorkerRegistration(*this);

		while (true)
		{
			std::shared_ptr<FQueuedTask> Task;

			{
				std::unique_lock Lock(BlockingIoQueueMutex);
				BlockingIoCondition.wait(Lock, [this]
				{
					return bBlockingIoShutdown || HasQueuedTask(BlockingIoQueues);
				});

				if (bBlockingIoShutdown && !HasQueuedTask(BlockingIoQueues))
				{
					return;
				}

				Task = PopNextTask(BlockingIoQueues);
			}

			if (Task)
			{
				ExecuteTask(Task);
			}
		}
	}

	[[nodiscard]] bool ExecuteOneBlockingIoTask()
	{
		std::shared_ptr<FQueuedTask> Task;
		{
			std::scoped_lock Lock(BlockingIoQueueMutex);
			Task = PopNextTask(BlockingIoQueues);
		}

		if (!Task)
		{
			return false;
		}

		ExecuteTask(Task);
		return true;
	}

	std::size_t RunMainThreadTasks(const std::size_t MaximumTasks)
	{
		if (std::this_thread::get_id() != MainThreadId)
		{
			return 0;
		}

		std::size_t ExecutedTaskCount = 0;
		while (ExecutedTaskCount < MaximumTasks)
		{
			std::shared_ptr<FQueuedTask> Task;

			{
				std::scoped_lock Lock(MainThreadQueueMutex);
				Task = PopNextTask(MainThreadQueues);
			}

			if (!Task)
			{
				break;
			}

			ExecuteTask(Task);
			++ExecutedTaskCount;
		}

		CollectCompletedCpuTasks();
		return ExecutedTaskCount;
	}

	std::size_t RunUntilIdle()
	{
		if (!Options.bDeterministic || std::this_thread::get_id() != MainThreadId)
		{
			return 0;
		}

		std::size_t ExecutedTaskCount = 0;
		while (ExecuteOneDeterministicTask())
		{
			++ExecutedTaskCount;
		}

		return ExecutedTaskCount;
	}

	[[nodiscard]] bool ExecuteOneDeterministicTask()
	{
		std::shared_ptr<FQueuedTask> Task;

		{
			std::scoped_lock Lock(DeterministicQueueMutex);
			Task = PopNextTask(DeterministicQueues);
		}

		if (!Task)
		{
			return false;
		}

		ExecuteTask(Task);
		return true;
	}

	void Shutdown() noexcept
	{
		if (std::this_thread::get_id() != MainThreadId)
		{
			std::terminate();
		}

		{
			std::scoped_lock LifecycleLock(LifecycleMutex);
			if (bShutdownStarted)
			{
				return;
			}

			bShutdownStarted = true;
			bAcceptingTasks = false;
		}

		CancelAllScopes();

		try
		{
			if (Options.bDeterministic)
			{
				RunUntilIdle();
			}
			else
			{
				{
					std::scoped_lock Lock(BlockingIoQueueMutex);
					bBlockingIoShutdown = true;
				}
				BlockingIoCondition.notify_all();
				BlockingIoThreads.clear();
				CpuScheduler.WaitforAllAndShutdown();
				bCpuSchedulerInitialized = false;

				if (std::this_thread::get_id() == MainThreadId)
				{
					RunMainThreadTasks(std::numeric_limits<std::size_t>::max());
				}
				else
				{
					CancelQueuedMainThreadTasks();
				}
			}
		}
		catch (...)
		{
			CancelQueuedTasks();
		}

		CollectCompletedCpuTasks();
	}

	void StopPartiallyInitializedWorkers() noexcept
	{
		{
			std::scoped_lock Lock(BlockingIoQueueMutex);
			bBlockingIoShutdown = true;
		}
		BlockingIoCondition.notify_all();
		BlockingIoThreads.clear();

		if (bCpuSchedulerInitialized)
		{
			CpuScheduler.WaitforAllAndShutdown();
			bCpuSchedulerInitialized = false;
		}
	}

	void CancelAllScopes() noexcept
	{
		std::scoped_lock Lock(ScopesMutex);
		for (const std::weak_ptr<FTaskScope::FState>& WeakScope : Scopes)
		{
			if (const std::shared_ptr<FTaskScope::FState> Scope = WeakScope.lock())
			{
				CloseScope(Scope);
			}
		}
	}

	void CancelQueuedMainThreadTasks() noexcept
	{
		std::array<std::deque<std::shared_ptr<FQueuedTask>>, 3> Queues;
		{
			std::scoped_lock Lock(MainThreadQueueMutex);
			Queues.swap(MainThreadQueues);
		}

		CancelQueues(Queues);
	}

	void CancelQueuedTasks() noexcept
	{
		CancelQueuedMainThreadTasks();

		std::array<std::deque<std::shared_ptr<FQueuedTask>>, 3> IoQueues;
		{
			std::scoped_lock Lock(BlockingIoQueueMutex);
			IoQueues.swap(BlockingIoQueues);
		}
		CancelQueues(IoQueues);

		std::array<std::deque<std::shared_ptr<FQueuedTask>>, 3> DeterministicTaskQueues;
		{
			std::scoped_lock Lock(DeterministicQueueMutex);
			DeterministicTaskQueues.swap(DeterministicQueues);
		}
		CancelQueues(DeterministicTaskQueues);
	}

	void CancelQueues(std::array<std::deque<std::shared_ptr<FQueuedTask>>, 3>& Queues) noexcept
	{
		for (std::deque<std::shared_ptr<FQueuedTask>>& Queue : Queues)
		{
			for (const std::shared_ptr<FQueuedTask>& Task : Queue)
			{
				ReleaseTaskCapacity(*Task->State);
				FinishTask(Task->State, ETaskState::Cancelled, {});
			}
		}
	}

	[[nodiscard]] bool ClaimQueueCapacity(const ETaskLane Lane) noexcept
	{
		if (Lane == ETaskLane::MainThread)
		{
			return true;
		}

		std::atomic_size_t& Count = Lane == ETaskLane::Cpu ? QueuedCpuTaskCount : QueuedIoTaskCount;
		const std::size_t Maximum = Lane == ETaskLane::Cpu ? Options.MaximumQueuedCpuTasks : Options.MaximumQueuedIoTasks;
		std::size_t Current = Count.load(std::memory_order_relaxed);

		while (Current < Maximum)
		{
			if (Count.compare_exchange_weak(Current, Current + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
			{
				return true;
			}
		}

		return false;
	}

	void ReleaseQueueCapacity(const ETaskLane Lane) noexcept
	{
		if (Lane == ETaskLane::Cpu)
		{
			QueuedCpuTaskCount.fetch_sub(1, std::memory_order_relaxed);
		}
		else if (Lane == ETaskLane::BlockingIo)
		{
			QueuedIoTaskCount.fetch_sub(1, std::memory_order_relaxed);
		}
	}

	void ReleaseTaskCapacity(FTaskHandle::FState& State) noexcept
	{
		if (State.bCapacityClaimed.exchange(false, std::memory_order_acq_rel))
		{
			ReleaseQueueCapacity(State.Lane);
		}
	}

	void CollectCompletedCpuTasks()
	{
		std::scoped_lock Lock(CpuTasksMutex);
		std::erase_if(CpuTasks, [](const std::shared_ptr<FCpuTaskNode>& Task)
		{
			return Task->GetIsComplete();
		});
	}

	[[nodiscard]] static std::string_view GetLaneName(const ETaskLane Lane) noexcept
	{
		switch (Lane)
		{
			case ETaskLane::Cpu:
				return "CPU";
			case ETaskLane::BlockingIo:
				return "blocking IO";
			case ETaskLane::MainThread:
				return "main-thread";
		}

		return "unknown";
	}

	[[nodiscard]] static bool HasQueuedTask(const std::array<std::deque<std::shared_ptr<FQueuedTask>>, 3>& Queues) noexcept
	{
		return std::ranges::any_of(Queues, [](const std::deque<std::shared_ptr<FQueuedTask>>& Queue)
		{
			return !Queue.empty();
		});
	}

	[[nodiscard]] static std::shared_ptr<FQueuedTask> PopNextTask(std::array<std::deque<std::shared_ptr<FQueuedTask>>, 3>& Queues)
	{
		for (std::deque<std::shared_ptr<FQueuedTask>>& Queue : Queues)
		{
			if (!Queue.empty())
			{
				std::shared_ptr<FQueuedTask> Task = std::move(Queue.front());
				Queue.pop_front();
				return Task;
			}
		}

		return nullptr;
	}

	FTaskSystemOptions Options;
	std::thread::id MainThreadId;
	std::size_t CpuWorkerCount = 0;
	std::size_t BlockingIoWorkerCount = 0;
	std::recursive_mutex LifecycleMutex;
	bool bAcceptingTasks = true;
	bool bShutdownStarted = false;
	bool bCpuSchedulerInitialized = false;

	enki::TaskScheduler CpuScheduler;
	std::mutex CpuTasksMutex;
	std::vector<std::shared_ptr<FCpuTaskNode>> CpuTasks;
	std::atomic_size_t QueuedCpuTaskCount = 0;

	std::mutex BlockingIoQueueMutex;
	std::condition_variable BlockingIoCondition;
	std::array<std::deque<std::shared_ptr<FQueuedTask>>, 3> BlockingIoQueues;
	std::vector<std::jthread> BlockingIoThreads;
	std::atomic_size_t QueuedIoTaskCount = 0;
	bool bBlockingIoShutdown = false;

	std::mutex MainThreadQueueMutex;
	std::array<std::deque<std::shared_ptr<FQueuedTask>>, 3> MainThreadQueues;

	std::mutex DeterministicQueueMutex;
	std::array<std::deque<std::shared_ptr<FQueuedTask>>, 3> DeterministicQueues;

	std::mutex ScopesMutex;
	std::vector<std::weak_ptr<FTaskScope::FState>> Scopes;
};

namespace
{
FCpuTaskNode::FCpuTaskNode(FTaskSystem::FImplementation* const InSystem, std::shared_ptr<FQueuedTask> InTask)
    : System(InSystem)
    , Task(std::move(InTask))
{
	m_Priority = ToEnkiPriority(Task->Description.Priority);
}

void FCpuTaskNode::ExecuteRange(enki::TaskSetPartition, std::uint32_t)
{
	System->ExecuteTask(Task);
}
}

FCancellationToken::FCancellationToken(std::shared_ptr<FState> InState) noexcept
    : State(std::move(InState))
{
}

bool FCancellationToken::IsCancellationRequested() const noexcept
{
	return State && State->bCancellationRequested.load(std::memory_order_acquire);
}

bool FCancellationToken::IsValid() const noexcept
{
	return State != nullptr;
}

FCancellationSource::FCancellationSource()
    : State(std::make_shared<FCancellationToken::FState>())
{
}

FCancellationToken FCancellationSource::GetToken() const noexcept
{
	return FCancellationToken(State);
}

void FCancellationSource::RequestCancellation() noexcept
{
	State->bCancellationRequested.store(true, std::memory_order_release);
}

bool FCancellationSource::IsCancellationRequested() const noexcept
{
	return State->bCancellationRequested.load(std::memory_order_acquire);
}

FTaskContext::FTaskContext(FCancellationToken InCancellationToken, std::shared_ptr<std::atomic<float>> InProgress) noexcept
    : CancellationToken(std::move(InCancellationToken))
    , Progress(std::move(InProgress))
{
}

bool FTaskContext::IsCancellationRequested() const noexcept
{
	return CancellationToken.IsCancellationRequested();
}

const FCancellationToken& FTaskContext::GetCancellationToken() const noexcept
{
	return CancellationToken;
}

void FTaskContext::ReportProgress(const float InProgress) noexcept
{
	Progress->store(std::clamp(InProgress, 0.f, 1.f), std::memory_order_relaxed);
}

FTaskHandle::FTaskHandle(std::shared_ptr<FState> InState) noexcept
    : State(std::move(InState))
{
}

bool FTaskHandle::IsValid() const noexcept
{
	return State != nullptr;
}

bool FTaskHandle::IsComplete() const noexcept
{
	return State && State->bCompletionBookkeepingFinished.load(std::memory_order_acquire);
}

ETaskState FTaskHandle::GetState() const noexcept
{
	if (!State)
	{
		return ETaskState::Invalid;
	}

	const ETaskState CurrentState = State->State.load(std::memory_order_acquire);
	if (IsTerminalState(CurrentState) && !State->bCompletionBookkeepingFinished.load(std::memory_order_acquire))
	{
		return ETaskState::Running;
	}

	return CurrentState;
}

float FTaskHandle::GetProgress() const noexcept
{
	return State ? State->Progress->load(std::memory_order_relaxed) : 0.f;
}

FTaskResult FTaskHandle::GetResult() const
{
	if (!State)
	{
		return {};
	}

	std::scoped_lock Lock(State->CompletionMutex);
	return {.State = GetState(), .ErrorMessage = State->bCompletionBookkeepingFinished.load(std::memory_order_acquire) ? State->ErrorMessage : std::string{}};
}

FTaskResult FTaskHandle::Wait() const
{
	if (!State)
	{
		return {};
	}

	if (const std::shared_ptr<FTaskSystem::FImplementation> System = State->System.lock())
	{
		return System->WaitForTask(State);
	}

	std::unique_lock Lock(State->CompletionMutex);
	State->CompletionCondition.wait(Lock, [this]
	{
		return State->bCompletionBookkeepingFinished.load(std::memory_order_acquire);
	});

	return {.State = State->State.load(std::memory_order_acquire), .ErrorMessage = State->ErrorMessage};
}

FTaskScope::FTaskScope(std::shared_ptr<FState> InState) noexcept
    : State(std::move(InState))
{
}

FTaskScope::~FTaskScope()
{
	RequestCancellation();
	Wait();
}

std::string_view FTaskScope::GetName() const noexcept
{
	return State->Name;
}

FCancellationToken FTaskScope::GetCancellationToken() const noexcept
{
	return State->CancellationSource.GetToken();
}

bool FTaskScope::IsCancellationRequested() const noexcept
{
	return State->CancellationSource.IsCancellationRequested();
}

std::size_t FTaskScope::GetOutstandingTaskCount() const noexcept
{
	return State->OutstandingTaskCount.load(std::memory_order_acquire);
}

void FTaskScope::RequestCancellation() noexcept
{
	FTaskSystem::FImplementation::CloseScope(State);
}

void FTaskScope::Wait() noexcept
{
	if (const std::shared_ptr<FTaskSystem::FImplementation> System = State->System.lock())
	{
		if (std::this_thread::get_id() != System->MainThreadId)
		{
			std::terminate();
		}

		System->WaitForScope(State);
	}
}

std::expected<std::unique_ptr<FTaskSystem>, FTaskError> FTaskSystem::Create(FTaskSystemOptions Options)
{
	try
	{
		const std::shared_ptr<FImplementation> Implementation = std::make_shared<FImplementation>(Options);
		const std::expected<void, FTaskError> InitializationResult = Implementation->Initialize();
		if (!InitializationResult)
		{
			Implementation->Shutdown();
			return std::unexpected(InitializationResult.error());
		}

		return std::unique_ptr<FTaskSystem>(new FTaskSystem(Implementation));
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = std::format("Could not create the task system: {}", Exception.what())});
	}
	catch (...)
	{
		return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "Could not create the task system due to an unknown error"});
	}
}

FTaskSystem::FTaskSystem(std::shared_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FTaskSystem::~FTaskSystem()
{
	Shutdown();
}

std::expected<std::unique_ptr<FTaskScope>, FTaskError> FTaskSystem::CreateScope(std::string Name)
{
	if (Name.empty())
	{
		return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidDescription, .Message = "A task scope requires a profiling name"});
	}

	try
	{
		std::scoped_lock LifecycleLock(Implementation->LifecycleMutex);
		if (!Implementation->bAcceptingTasks)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::ShuttingDown, .Message = "The task system is shutting down"});
		}

		return std::unique_ptr<FTaskScope>(new FTaskScope(Implementation->CreateScope(std::move(Name))));
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = std::format("Could not create task scope: {}", Exception.what())});
	}
	catch (...)
	{
		return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "Could not create task scope due to an unknown error"});
	}
}

std::expected<FTaskHandle, FTaskError> FTaskSystem::Submit(FTaskScope& Scope, FTaskDescription Description, FTaskFunction Function)
{
	std::expected<std::shared_ptr<FTaskHandle::FState>, FTaskError> Result = Implementation->Submit(Scope.State, std::move(Description), std::move(Function));
	if (!Result)
	{
		return std::unexpected(std::move(Result.error()));
	}

	return FTaskHandle(std::move(*Result));
}

std::expected<FTaskHandle, FTaskError> FTaskSystem::ContinueOnMainThread(FTaskScope& Scope, const FTaskHandle& Prerequisite, std::string Name, FTaskFunction Function)
{
	std::expected<std::shared_ptr<FTaskHandle::FState>, FTaskError> Result = Implementation->ContinueOnMainThread(Scope.State, Prerequisite.State, std::move(Name), std::move(Function));
	if (!Result)
	{
		return std::unexpected(std::move(Result.error()));
	}

	return FTaskHandle(std::move(*Result));
}

std::expected<FTaskHandle, FTaskError> FTaskSystem::WhenAll(FTaskScope& Scope, const std::span<const FTaskHandle> Prerequisites, std::string Name)
{
	std::expected<std::shared_ptr<FTaskHandle::FState>, FTaskError> Result = Implementation->WhenAll(Scope.State, Prerequisites, std::move(Name));
	if (!Result)
	{
		return std::unexpected(std::move(Result.error()));
	}

	return FTaskHandle(std::move(*Result));
}

std::expected<FTaskHandle, FTaskError> FTaskSystem::ParallelFor(FTaskScope& Scope, FTaskDescription Description, const std::size_t ItemCount, const std::size_t MinimumItemsPerTask, FParallelForFunction Function)
{
	if (Description.Lane != ETaskLane::Cpu || Description.Name.empty() || MinimumItemsPerTask == 0 || !Function)
	{
		return std::unexpected(FTaskError{.Code = ETaskErrorCode::InvalidDescription, .Message = "ParallelFor requires a named CPU task, a non-zero grain size, and callable"});
	}

	try
	{
		if (ItemCount == 0)
		{
			return WhenAll(Scope, {}, std::move(Description.Name));
		}

		std::unique_lock LifecycleLock(Implementation->LifecycleMutex);
		if (!Implementation->bAcceptingTasks)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::ShuttingDown, .Message = "The task system is shutting down"});
		}

		const std::size_t QueuedTaskCount = Implementation->QueuedCpuTaskCount.load(std::memory_order_acquire);
		const std::size_t AvailableTaskCount = Implementation->Options.MaximumQueuedCpuTasks - std::min(Implementation->Options.MaximumQueuedCpuTasks, QueuedTaskCount);
		if (AvailableTaskCount == 0)
		{
			return std::unexpected(FTaskError{.Code = ETaskErrorCode::QueueFull, .Message = "The CPU task queue is full"});
		}

		const std::size_t DesiredTaskCount = ItemCount / MinimumItemsPerTask + static_cast<std::size_t>(ItemCount % MinimumItemsPerTask != 0);
		const std::size_t TaskCount = std::min(DesiredTaskCount, AvailableTaskCount);
		const std::size_t BaseItemsPerTask = ItemCount / TaskCount;
		const std::size_t TasksWithExtraItem = ItemCount % TaskCount;
		std::vector<FTaskHandle> Tasks;
		Tasks.reserve(TaskCount);
		const auto SharedFunction = std::make_shared<FParallelForFunction>(std::move(Function));
		std::optional<FTaskError> SubmissionError;

		for (std::size_t TaskIndex = 0; TaskIndex < TaskCount; ++TaskIndex)
		{
			const std::size_t BeginIndex = TaskIndex * BaseItemsPerTask + std::min(TaskIndex, TasksWithExtraItem);
			const std::size_t EndIndex = BeginIndex + BaseItemsPerTask + static_cast<std::size_t>(TaskIndex < TasksWithExtraItem);
			FTaskDescription ChunkDescription{
			    .Name = std::format("{} [{}..{})", Description.Name, BeginIndex, EndIndex),
			    .Lane = ETaskLane::Cpu,
			    .Priority = Description.Priority,
			};

			std::expected<FTaskHandle, FTaskError> Task = Submit(Scope, std::move(ChunkDescription), [SharedFunction, BeginIndex, EndIndex](FTaskContext& Context)
			{
				for (std::size_t Index = BeginIndex; Index < EndIndex && !Context.IsCancellationRequested(); ++Index)
				{
					(*SharedFunction)(Index, Context);
				}
			});

			if (!Task)
			{
				SubmissionError = std::move(Task.error());
				break;
			}

			Tasks.emplace_back(std::move(*Task));
		}

		if (SubmissionError)
		{
			LifecycleLock.unlock();
			for (const FTaskHandle& SubmittedTask : Tasks)
			{
				static_cast<void>(SubmittedTask.Wait());
			}

			return std::unexpected(std::move(*SubmissionError));
		}

		std::expected<FTaskHandle, FTaskError> Barrier = WhenAll(Scope, Tasks, std::move(Description.Name));
		LifecycleLock.unlock();
		return Barrier;
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = std::format("Could not create parallel task: {}", Exception.what())});
	}
	catch (...)
	{
		return std::unexpected(FTaskError{.Code = ETaskErrorCode::InitializationFailed, .Message = "Could not create parallel task due to an unknown error"});
	}
}

std::size_t FTaskSystem::RunMainThreadTasks(const std::size_t MaximumTasks)
{
	if (std::this_thread::get_id() != Implementation->MainThreadId)
	{
		std::terminate();
	}

	return Implementation->RunMainThreadTasks(MaximumTasks);
}

std::size_t FTaskSystem::RunUntilIdle()
{
	if (std::this_thread::get_id() != Implementation->MainThreadId)
	{
		std::terminate();
	}

	return Implementation->RunUntilIdle();
}

void FTaskSystem::Shutdown() noexcept
{
	Implementation->Shutdown();
}

bool FTaskSystem::IsDeterministic() const noexcept
{
	return Implementation->Options.bDeterministic;
}

std::size_t FTaskSystem::GetCpuWorkerCount() const noexcept
{
	return Implementation->CpuWorkerCount;
}

std::size_t FTaskSystem::GetBlockingIoWorkerCount() const noexcept
{
	return Implementation->BlockingIoWorkerCount;
}

FTaskContext FTaskSystem::CreateTaskContext(FCancellationToken CancellationToken, std::shared_ptr<std::atomic<float>> Progress) noexcept
{
	return {std::move(CancellationToken), std::move(Progress)};
}
}
