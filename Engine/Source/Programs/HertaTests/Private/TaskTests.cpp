#include "Herta/Core/Log.h"
#include "Herta/Tasks/TaskSystem.h"

#include <array>
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Herta
{
namespace
{
[[nodiscard]] std::unique_ptr<FTaskSystem> CreateDeterministicTaskSystem(const std::size_t CpuQueueCapacity = 128, const std::size_t IoQueueCapacity = 128)
{
	FTaskSystemOptions Options{
	    .MaximumQueuedCpuTasks = CpuQueueCapacity,
	    .MaximumQueuedIoTasks = IoQueueCapacity,
	    .bDeterministic = true};
	std::expected<std::unique_ptr<FTaskSystem>, FTaskError> Result = FTaskSystem::Create(Options);
	REQUIRE(Result.has_value());
	return std::move(*Result);
}

[[nodiscard]] FTaskHandle RequireTask(std::expected<FTaskHandle, FTaskError> Result)
{
	REQUIRE(Result.has_value());
	return std::move(*Result);
}

[[nodiscard]] std::unique_ptr<FTaskScope> RequireScope(FTaskSystem& TaskSystem, std::string Name)
{
	std::expected<std::unique_ptr<FTaskScope>, FTaskError> Result = TaskSystem.CreateScope(std::move(Name));
	REQUIRE(Result.has_value());
	return std::move(*Result);
}
}

TEST_CASE("Cancellation source shares one cooperative state")
{
	FCancellationSource Source;
	const FCancellationToken Token = Source.GetToken();

	CHECK(Token.IsValid());
	CHECK_FALSE(Token.IsCancellationRequested());
	Source.RequestCancellation();
	CHECK(Token.IsCancellationRequested());
}

TEST_CASE("Cancelled scopes reject new work")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem();
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Closed scope test");
	Scope->RequestCancellation();

	const std::expected<FTaskHandle, FTaskError> Result = TaskSystem->Submit(*Scope, {"Rejected work", ETaskLane::Cpu}, [](FTaskContext&) {});

	REQUIRE_FALSE(Result.has_value());
	CHECK(Result.error().Code == ETaskErrorCode::InvalidScope);
}

TEST_CASE("Dependencies cannot cross structured task scopes")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem();
	const std::unique_ptr<FTaskScope> FirstScope = RequireScope(*TaskSystem, "First scope");
	const std::unique_ptr<FTaskScope> SecondScope = RequireScope(*TaskSystem, "Second scope");
	const FTaskHandle Prerequisite = RequireTask(TaskSystem->Submit(*FirstScope, {"Prerequisite", ETaskLane::Cpu}, [](FTaskContext&) {}));

	const std::expected<FTaskHandle, FTaskError> Continuation = TaskSystem->ContinueOnMainThread(*SecondScope, Prerequisite, "Invalid continuation", [](FTaskContext&) {});
	const std::array<FTaskHandle, 1> Prerequisites{Prerequisite};
	const std::expected<FTaskHandle, FTaskError> Barrier = TaskSystem->WhenAll(*SecondScope, Prerequisites, "Invalid barrier");

	REQUIRE_FALSE(Continuation.has_value());
	CHECK(Continuation.error().Code == ETaskErrorCode::InvalidHandle);
	REQUIRE_FALSE(Barrier.has_value());
	CHECK(Barrier.error().Code == ETaskErrorCode::InvalidHandle);
	CHECK(TaskSystem->RunUntilIdle() == 1);
}

TEST_CASE("Deterministic tasks preserve priority and FIFO order")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem();
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Priority test");
	std::vector<int> ExecutionOrder;

	const FTaskHandle Low = RequireTask(TaskSystem->Submit(*Scope, {"Low", ETaskLane::Cpu, ETaskPriority::Low}, [&ExecutionOrder](FTaskContext&)
	                                                       {
		                                                       ExecutionOrder.push_back(4);
	                                                       }));
	const FTaskHandle HighFirst = RequireTask(TaskSystem->Submit(*Scope, {"High first", ETaskLane::Cpu, ETaskPriority::High}, [&ExecutionOrder](FTaskContext&)
	                                                             {
		                                                             ExecutionOrder.push_back(1);
	                                                             }));
	const FTaskHandle Normal = RequireTask(TaskSystem->Submit(*Scope, {"Normal", ETaskLane::Cpu, ETaskPriority::Normal}, [&ExecutionOrder](FTaskContext&)
	                                                          {
		                                                          ExecutionOrder.push_back(3);
	                                                          }));
	const FTaskHandle HighSecond = RequireTask(TaskSystem->Submit(*Scope, {"High second", ETaskLane::Cpu, ETaskPriority::High}, [&ExecutionOrder](FTaskContext&)
	                                                              {
		                                                              ExecutionOrder.push_back(2);
	                                                              }));

	CHECK(TaskSystem->RunUntilIdle() == 4);
	CHECK(ExecutionOrder == std::vector<int>{1, 2, 3, 4});
	CHECK(Low.GetState() == ETaskState::Succeeded);
	CHECK(HighFirst.GetState() == ETaskState::Succeeded);
	CHECK(Normal.GetState() == ETaskState::Succeeded);
	CHECK(HighSecond.GetState() == ETaskState::Succeeded);
	CHECK(Low.GetProgress() == doctest::Approx(1.0f));
	CHECK(Scope->GetOutstandingTaskCount() == 0);
}

TEST_CASE("Task queues are bounded independently")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem(1, 1);
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Capacity test");

	const FTaskHandle CpuTask = RequireTask(TaskSystem->Submit(*Scope, {"CPU one", ETaskLane::Cpu}, [](FTaskContext&) {}));
	const std::expected<FTaskHandle, FTaskError> RejectedCpuTask = TaskSystem->Submit(*Scope, {"CPU two", ETaskLane::Cpu}, [](FTaskContext&) {});
	REQUIRE_FALSE(RejectedCpuTask.has_value());
	CHECK(RejectedCpuTask.error().Code == ETaskErrorCode::QueueFull);

	const FTaskHandle IoTask = RequireTask(TaskSystem->Submit(*Scope, {"IO one", ETaskLane::BlockingIo}, [](FTaskContext&) {}));
	const std::expected<FTaskHandle, FTaskError> RejectedIoTask = TaskSystem->Submit(*Scope, {"IO two", ETaskLane::BlockingIo}, [](FTaskContext&) {});
	REQUIRE_FALSE(RejectedIoTask.has_value());
	CHECK(RejectedIoTask.error().Code == ETaskErrorCode::QueueFull);

	CHECK(TaskSystem->RunUntilIdle() == 2);
	CHECK(CpuTask.IsComplete());
	CHECK(IoTask.IsComplete());
}

TEST_CASE("Queued cancellation skips task work")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem();
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Cancellation test");
	bool bExecuted = false;
	const FTaskHandle Task = RequireTask(TaskSystem->Submit(*Scope, {"Cancelled task", ETaskLane::Cpu}, [&bExecuted](FTaskContext&)
	                                                        {
		                                                        bExecuted = true;
	                                                        }));

	Scope->RequestCancellation();
	CHECK(TaskSystem->RunUntilIdle() == 1);
	CHECK_FALSE(bExecuted);
	CHECK(Task.GetState() == ETaskState::Cancelled);
}

TEST_CASE("Task progress is clamped and failures do not cross the boundary")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem();
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Failure test");
	const FTaskHandle Task = RequireTask(TaskSystem->Submit(*Scope, {"Failing task", ETaskLane::Cpu}, [](FTaskContext& Context)
	                                                        {
		                                                        Context.ReportProgress(1.5f);
		                                                        throw std::runtime_error("Expected failure");
	                                                        }));

	const FTaskResult Result = Task.Wait();
	CHECK(Result.State == ETaskState::Failed);
	CHECK(Result.ErrorMessage == "Expected failure");
	CHECK(Task.GetProgress() == doctest::Approx(1.0f));
}

TEST_CASE("Main-thread continuations execute after their prerequisite")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem();
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Continuation test");
	int Value = 0;
	const FTaskHandle Prerequisite = RequireTask(TaskSystem->Submit(*Scope, {"Prerequisite", ETaskLane::Cpu}, [&Value](FTaskContext&)
	                                                                {
		                                                                Value = 1;
	                                                                }));
	const FTaskHandle Continuation = RequireTask(TaskSystem->ContinueOnMainThread(*Scope, Prerequisite, "Continuation", [&Value](FTaskContext&)
	                                                                              {
		                                                                              Value *= 2;
	                                                                              }));

	CHECK(TaskSystem->RunUntilIdle() == 2);
	CHECK(Value == 2);
	CHECK(Continuation.GetState() == ETaskState::Succeeded);
}

TEST_CASE("ParallelFor covers each item and completes through WhenAll")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem();
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Parallel test");
	std::array<int, 37> VisitCounts{};

	const FTaskHandle ParallelTask = RequireTask(TaskSystem->ParallelFor(*Scope, {"Visit items", ETaskLane::Cpu, ETaskPriority::Normal}, VisitCounts.size(), 5, [&VisitCounts](const std::size_t Index, FTaskContext&)
	                                                                     {
		                                                                     ++VisitCounts[Index];
	                                                                     }));

	CHECK(ParallelTask.Wait().State == ETaskState::Succeeded);
	for (const int VisitCount : VisitCounts)
	{
		CHECK(VisitCount == 1);
	}
}

TEST_CASE("ParallelFor adapts its chunk count to bounded queue capacity")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem(3, 1);
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Bounded parallel test");
	std::array<int, 1000> VisitCounts{};

	const FTaskHandle ParallelTask = RequireTask(TaskSystem->ParallelFor(*Scope, {"Bounded visit", ETaskLane::Cpu}, VisitCounts.size(), 1, [&VisitCounts](const std::size_t Index, FTaskContext&)
	                                                                     {
		                                                                     ++VisitCounts[Index];
	                                                                     }));

	CHECK(ParallelTask.Wait().State == ETaskState::Succeeded);
	for (const int VisitCount : VisitCounts)
	{
		CHECK(VisitCount == 1);
	}
}

TEST_CASE("WhenAll propagates prerequisite failure")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem();
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Barrier test");
	const std::array Prerequisites{
	    RequireTask(TaskSystem->Submit(*Scope, {"Successful", ETaskLane::Cpu}, [](FTaskContext&) {})),
	    RequireTask(TaskSystem->Submit(*Scope, {"Failed", ETaskLane::Cpu}, [](FTaskContext&)
	                                   {
		                                   throw std::runtime_error("Failure");
	                                   }))};
	const FTaskHandle Barrier = RequireTask(TaskSystem->WhenAll(*Scope, Prerequisites, "Barrier"));

	const FTaskResult Result = Barrier.Wait();
	CHECK(Result.State == ETaskState::Failed);
	CHECK_FALSE(Result.ErrorMessage.empty());
}

TEST_CASE("Blocking IO cannot starve CPU workers")
{
	FTaskSystemOptions Options{
	    .CpuWorkerCount = 1,
	    .BlockingIoWorkerCount = 1,
	    .MaximumQueuedCpuTasks = 8,
	    .MaximumQueuedIoTasks = 8};
	std::expected<std::unique_ptr<FTaskSystem>, FTaskError> CreationResult = FTaskSystem::Create(Options);
	REQUIRE(CreationResult.has_value());
	const std::unique_ptr<FTaskSystem> TaskSystem = std::move(*CreationResult);
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Lane isolation test");
	std::promise<void> ReleaseIo;
	const std::shared_future<void> ReleaseIoFuture = ReleaseIo.get_future().share();
	std::atomic_bool bCpuExecuted = false;

	const FTaskHandle IoTask = RequireTask(TaskSystem->Submit(*Scope, {"Blocked IO", ETaskLane::BlockingIo}, [ReleaseIoFuture](FTaskContext&)
	                                                          {
		                                                          ReleaseIoFuture.wait();
	                                                          }));
	const FTaskHandle CpuTask = RequireTask(TaskSystem->Submit(*Scope, {"CPU work", ETaskLane::Cpu}, [&bCpuExecuted](FTaskContext&)
	                                                           {
		                                                           bCpuExecuted.store(true, std::memory_order_release);
	                                                           }));

	CHECK(CpuTask.Wait().State == ETaskState::Succeeded);
	CHECK(bCpuExecuted.load(std::memory_order_acquire));
	ReleaseIo.set_value();
	CHECK(IoTask.Wait().State == ETaskState::Succeeded);
}

TEST_CASE("CPU workers help while waiting for child tasks")
{
	FTaskSystemOptions Options{
	    .CpuWorkerCount = 1,
	    .BlockingIoWorkerCount = 1,
	    .MaximumQueuedCpuTasks = 8,
	    .MaximumQueuedIoTasks = 8};
	std::expected<std::unique_ptr<FTaskSystem>, FTaskError> CreationResult = FTaskSystem::Create(Options);
	REQUIRE(CreationResult.has_value());
	const std::unique_ptr<FTaskSystem> TaskSystem = std::move(*CreationResult);
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Nested task test");
	std::atomic_int Value = 0;

	const FTaskHandle Parent = RequireTask(TaskSystem->Submit(*Scope, {"Parent", ETaskLane::Cpu}, [&TaskSystem, &Scope, &Value](FTaskContext&)
	                                                          {
		                                                          std::expected<FTaskHandle, FTaskError> ChildResult = TaskSystem->Submit(*Scope, {"Child", ETaskLane::Cpu}, [&Value](FTaskContext&)
		                                                                                                                                  {
			                                                                                                                                  Value.store(42, std::memory_order_release);
		                                                                                                                                  });
		                                                          if (!ChildResult || ChildResult->Wait().State != ETaskState::Succeeded)
		                                                          {
			                                                          throw std::runtime_error("Child task failed");
		                                                          }
	                                                          }));

	CHECK(Parent.Wait().State == ETaskState::Succeeded);
	CHECK(Value.load(std::memory_order_acquire) == 42);
}

TEST_CASE("Blocking IO workers help while waiting for child IO tasks")
{
	FTaskSystemOptions Options{
	    .CpuWorkerCount = 1,
	    .BlockingIoWorkerCount = 1,
	    .MaximumQueuedCpuTasks = 8,
	    .MaximumQueuedIoTasks = 8};
	std::expected<std::unique_ptr<FTaskSystem>, FTaskError> CreationResult = FTaskSystem::Create(Options);
	REQUIRE(CreationResult.has_value());
	const std::unique_ptr<FTaskSystem> TaskSystem = std::move(*CreationResult);
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Nested IO task test");
	std::atomic_int Value = 0;

	const FTaskHandle Parent = RequireTask(TaskSystem->Submit(*Scope, {"IO parent", ETaskLane::BlockingIo}, [&TaskSystem, &Scope, &Value](FTaskContext&)
	                                                          {
		                                                          std::expected<FTaskHandle, FTaskError> ChildResult = TaskSystem->Submit(*Scope, {"IO child", ETaskLane::BlockingIo}, [&Value](FTaskContext&)
		                                                                                                                                  {
			                                                                                                                                  Value.store(42, std::memory_order_release);
		                                                                                                                                  });
		                                                          if (!ChildResult || ChildResult->Wait().State != ETaskState::Succeeded)
		                                                          {
			                                                          throw std::runtime_error("IO child task failed");
		                                                          }
	                                                          }));

	CHECK(Parent.Wait().State == ETaskState::Succeeded);
	CHECK(Value.load(std::memory_order_acquire) == 42);
}

TEST_CASE("Production continuations run on the composing main thread")
{
	FTaskSystemOptions Options{
	    .CpuWorkerCount = 1,
	    .BlockingIoWorkerCount = 1,
	    .MaximumQueuedCpuTasks = 8,
	    .MaximumQueuedIoTasks = 8};
	std::expected<std::unique_ptr<FTaskSystem>, FTaskError> CreationResult = FTaskSystem::Create(Options);
	REQUIRE(CreationResult.has_value());
	const std::unique_ptr<FTaskSystem> TaskSystem = std::move(*CreationResult);
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Main thread test");
	const std::thread::id MainThreadId = std::this_thread::get_id();
	std::thread::id ContinuationThreadId;
	const FTaskHandle Prerequisite = RequireTask(TaskSystem->Submit(*Scope, {"Background work", ETaskLane::Cpu}, [](FTaskContext&) {}));
	const FTaskHandle Continuation = RequireTask(TaskSystem->ContinueOnMainThread(*Scope, Prerequisite, "Publish result", [&ContinuationThreadId](FTaskContext&)
	                                                                              {
		                                                                              ContinuationThreadId = std::this_thread::get_id();
	                                                                              }));

	CHECK(Prerequisite.Wait().State == ETaskState::Succeeded);
	CHECK_FALSE(Continuation.IsComplete());
	CHECK(TaskSystem->RunMainThreadTasks() == 1);
	CHECK(Continuation.GetState() == ETaskState::Succeeded);
	CHECK(ContinuationThreadId == MainThreadId);
}

TEST_CASE("Concurrent submission cannot cross task-system shutdown")
{
	FTaskSystemOptions Options{
	    .CpuWorkerCount = 2,
	    .BlockingIoWorkerCount = 1,
	    .MaximumQueuedCpuTasks = 4096,
	    .MaximumQueuedIoTasks = 8};
	std::expected<std::unique_ptr<FTaskSystem>, FTaskError> CreationResult = FTaskSystem::Create(Options);
	REQUIRE(CreationResult.has_value());
	const std::unique_ptr<FTaskSystem> TaskSystem = std::move(*CreationResult);
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Shutdown race test");
	std::atomic_size_t AcceptedTaskCount = 0;
	std::atomic_bool bObservedShutdown = false;

	const FTaskHandle Producer = RequireTask(TaskSystem->Submit(*Scope, {"Concurrent producer", ETaskLane::Cpu}, [&TaskSystem, &Scope, &AcceptedTaskCount, &bObservedShutdown](FTaskContext&)
	                                                            {
		                                                            for (;;)
		                                                            {
			                                                            std::expected<FTaskHandle, FTaskError> Result = TaskSystem->Submit(*Scope, {"Concurrent submission", ETaskLane::Cpu}, [](FTaskContext&) {});
			                                                            if (Result)
			                                                            {
				                                                            AcceptedTaskCount.fetch_add(1, std::memory_order_release);
				                                                            continue;
			                                                            }

			                                                            if (Result.error().Code == ETaskErrorCode::QueueFull)
			                                                            {
				                                                            std::this_thread::yield();
				                                                            continue;
			                                                            }

			                                                            bObservedShutdown.store(Result.error().Code == ETaskErrorCode::ShuttingDown, std::memory_order_release);
			                                                            return;
		                                                            }
	                                                            }));

	while (AcceptedTaskCount.load(std::memory_order_acquire) < 32)
	{
		std::this_thread::yield();
	}

	TaskSystem->Shutdown();

	CHECK(bObservedShutdown.load(std::memory_order_acquire));
	CHECK(Producer.IsComplete());
	CHECK(Scope->GetOutstandingTaskCount() == 0);
}

TEST_CASE("Task wait publishes scope completion before returning")
{
	FTaskSystemOptions Options{
	    .CpuWorkerCount = 2,
	    .BlockingIoWorkerCount = 1,
	    .MaximumQueuedCpuTasks = 8,
	    .MaximumQueuedIoTasks = 8};
	std::expected<std::unique_ptr<FTaskSystem>, FTaskError> CreationResult = FTaskSystem::Create(Options);
	REQUIRE(CreationResult.has_value());
	const std::unique_ptr<FTaskSystem> TaskSystem = std::move(*CreationResult);
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Completion ordering test");

	for (int Iteration = 0; Iteration < 100; ++Iteration)
	{
		const FTaskHandle Task = RequireTask(TaskSystem->Submit(*Scope, {"Completion ordering task", ETaskLane::Cpu}, [](FTaskContext&) {}));
		CHECK(Task.Wait().State == ETaskState::Succeeded);
		CHECK(Scope->GetOutstandingTaskCount() == 0);
	}
}

TEST_CASE("Task scopes cannot be created after shutdown")
{
	const std::unique_ptr<FTaskSystem> TaskSystem = CreateDeterministicTaskSystem();
	TaskSystem->Shutdown();

	const std::expected<std::unique_ptr<FTaskScope>, FTaskError> Result = TaskSystem->CreateScope("Late scope");
	REQUIRE_FALSE(Result.has_value());
	CHECK(Result.error().Code == ETaskErrorCode::ShuttingDown);
}

TEST_CASE("Blocking the main thread on task work emits a diagnostic")
{
	FLogOptions LogOptions{
	    .EditorBufferCapacity = 8,
	    .bConsoleOutput = false,
	    .bDebuggerOutput = false,
	    .bFileOutput = false};
	std::expected<std::unique_ptr<FLogService>, FLogError> LogResult = FLogService::Create(std::move(LogOptions));
	REQUIRE(LogResult.has_value());
	const std::unique_ptr<FLogService> Log = std::move(*LogResult);

	FTaskSystemOptions Options{
	    .CpuWorkerCount = 1,
	    .BlockingIoWorkerCount = 1,
	    .MaximumQueuedCpuTasks = 8,
	    .MaximumQueuedIoTasks = 8,
	    .Log = Log.get()};
	std::expected<std::unique_ptr<FTaskSystem>, FTaskError> CreationResult = FTaskSystem::Create(Options);
	REQUIRE(CreationResult.has_value());
	const std::unique_ptr<FTaskSystem> TaskSystem = std::move(*CreationResult);
	const std::unique_ptr<FTaskScope> Scope = RequireScope(*TaskSystem, "Main thread diagnostic test");
	std::atomic_bool bStarted = false;
	const FTaskHandle Task = RequireTask(TaskSystem->Submit(*Scope, {"Slow task", ETaskLane::Cpu}, [&bStarted](FTaskContext&)
	                                                        {
		                                                        bStarted.store(true, std::memory_order_release);
		                                                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
	                                                        }));
	while (!bStarted.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}

	CHECK(Task.Wait().State == ETaskState::Succeeded);
	const std::expected<FLogReadResult, FLogError> Records = Log->ReadEditorBuffer();
	REQUIRE(Records.has_value());
	REQUIRE(Records->Records.size() == 1);
	CHECK(Records->Records.front().Level == ELogLevel::Warning);
	CHECK(Records->Records.front().Message.find("Slow task") != std::string::npos);
}
}
