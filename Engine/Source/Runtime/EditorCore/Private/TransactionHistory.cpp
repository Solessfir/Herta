#include "Herta/EditorCore/TransactionHistory.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::size_t MaximumTransactionCount = 256;
constexpr std::size_t MaximumHistoryBytes = 64 * 1024 * 1024;

std::expected<void, FEditorCommandError> ApplyTransaction(const FEditorTransaction& Transaction, const bool bUndo)
{
	try
	{
		return Transaction.Apply(bUndo);
	}
	catch (...)
	{
		// Transactions report ordinary failures explicitly. Unexpected exceptions cannot cross modules.
		std::terminate();
	}
}
}

FEditorTransactionHistory::FEditorTransactionHistory(const std::size_t MaximumTransactions, const std::size_t MaximumMemoryCost)
    : TransactionLimit(std::min(MaximumTransactions, MaximumTransactionCount))
    , MemoryLimit(std::min(MaximumMemoryCost, MaximumHistoryBytes))
{
}

std::expected<std::size_t, FEditorCommandError> FEditorTransactionHistory::Validate(const FEditorTransaction& Transaction) const
{
	if (Transaction.Label.empty() || !Transaction.Apply)
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::InvalidDescriptor, .Message = "Transaction requires a label and an apply callback"});
	}

	if (TransactionLimit == 0 || MemoryLimit < sizeof(FEntry) || Transaction.MemoryCost > MemoryLimit - sizeof(FEntry)
	    || Transaction.Label.size() >= MemoryLimit - sizeof(FEntry) - Transaction.MemoryCost)
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = "Transaction exceeds the history memory or command budget"});
	}

	return sizeof(FEntry) + Transaction.Label.size() + 1 + Transaction.MemoryCost;
}

void FEditorTransactionHistory::Record(FEditorTransaction Transaction, const std::size_t Cost)
{
	for (std::size_t Index = Cursor; Index < Entries.size(); ++Index)
	{
		MemoryCost -= Entries[Index].MemoryCost;
	}

	Entries.erase(Entries.begin() + static_cast<std::ptrdiff_t>(Cursor), Entries.end());

	while (!Entries.empty() && (Entries.size() >= TransactionLimit || MemoryCost > MemoryLimit - Cost))
	{
		MemoryCost -= Entries.front().MemoryCost;
		Entries.erase(Entries.begin());
		--Cursor;
	}

	const std::uint64_t AfterState = NextState++;
	if (AfterState == 0)
	{
		std::terminate();
	}

	Entries.push_back({
	    .Transaction = std::move(Transaction),
	    .MemoryCost = Cost,
	    .BeforeState = CurrentState,
	    .AfterState = AfterState,
	});

	CurrentState = AfterState;
	MemoryCost += Cost;
	Cursor = Entries.size();
}

std::expected<void, FEditorCommandError> FEditorTransactionHistory::Execute(FEditorTransaction Transaction)
{
	const auto Cost = Validate(Transaction);
	if (!Cost)
	{
		return std::unexpected(Cost.error());
	}

	const auto Applied = ApplyTransaction(Transaction, false);
	if (!Applied)
	{
		return Applied;
	}

	Record(std::move(Transaction), *Cost);
	return {};
}

std::expected<void, FEditorCommandError> FEditorTransactionHistory::RecordApplied(FEditorTransaction Transaction)
{
	const auto Cost = Validate(Transaction);
	if (!Cost)
	{
		return std::unexpected(Cost.error());
	}

	Record(std::move(Transaction), *Cost);
	return {};
}

std::expected<void, FEditorCommandError> FEditorTransactionHistory::Undo()
{
	if (!CanUndo())
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = "No transaction is available to undo"});
	}

	const FEntry& Entry = Entries[Cursor - 1];
	const auto Applied = ApplyTransaction(Entry.Transaction, true);
	if (!Applied)
	{
		return Applied;
	}

	CurrentState = Entry.BeforeState;
	--Cursor;
	return {};
}

std::expected<void, FEditorCommandError> FEditorTransactionHistory::Redo()
{
	if (!CanRedo())
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = "No transaction is available to redo"});
	}

	const FEntry& Entry = Entries[Cursor];
	const auto Applied = ApplyTransaction(Entry.Transaction, false);
	if (!Applied)
	{
		return Applied;
	}

	CurrentState = Entry.AfterState;
	++Cursor;
	return {};
}

bool FEditorTransactionHistory::CanUndo() const
{
	return Cursor != 0;
}

bool FEditorTransactionHistory::CanRedo() const
{
	return Cursor < Entries.size();
}

std::string_view FEditorTransactionHistory::GetUndoLabel() const
{
	return CanUndo() ? std::string_view{Entries[Cursor - 1].Transaction.Label} : std::string_view{};
}

std::string_view FEditorTransactionHistory::GetRedoLabel() const
{
	return CanRedo() ? std::string_view{Entries[Cursor].Transaction.Label} : std::string_view{};
}

void FEditorTransactionHistory::Clear()
{
	Entries.clear();
	Cursor = 0;
	MemoryCost = 0;
}

void FEditorTransactionHistory::MarkSaved()
{
	SavedState = CurrentState;
}

bool FEditorTransactionHistory::IsDirty() const
{
	return CurrentState != SavedState;
}
}
