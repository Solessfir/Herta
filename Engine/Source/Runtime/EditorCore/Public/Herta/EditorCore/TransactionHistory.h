#pragma once

#include "Herta/EditorCore/CommandRegistry.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FEditorTransaction
{
	std::string Label;
	// Failed application must leave the document unchanged.
	std::function<std::expected<void, FEditorCommandError>(bool bUndo)> Apply;
	// Estimated captured payload bytes. Entry and label storage are counted separately.
	std::size_t MemoryCost = 0;
};

// Single-owner, non-reentrant history. Callbacks report ordinary failures with expected.
class FEditorTransactionHistory final
{
public:
	explicit FEditorTransactionHistory(std::size_t MaximumTransactions = 256, std::size_t MaximumMemoryCost = 64 * 1024 * 1024);
	FEditorTransactionHistory(const FEditorTransactionHistory&) = delete;
	FEditorTransactionHistory& operator=(const FEditorTransactionHistory&) = delete;
	FEditorTransactionHistory(FEditorTransactionHistory&&) = delete;
	FEditorTransactionHistory& operator=(FEditorTransactionHistory&&) = delete;
	~FEditorTransactionHistory() = default;

	[[nodiscard]] std::expected<void, FEditorCommandError> Execute(FEditorTransaction Transaction);
	// Records a live edit already applied by the caller. It never invokes Apply.
	[[nodiscard]] std::expected<void, FEditorCommandError> RecordApplied(FEditorTransaction Transaction);
	[[nodiscard]] std::expected<void, FEditorCommandError> Undo();
	[[nodiscard]] std::expected<void, FEditorCommandError> Redo();
	bool CanUndo() const;
	bool CanRedo() const;
	std::string_view GetUndoLabel() const;
	std::string_view GetRedoLabel() const;
	// Discards undo/redo without changing the saved-state marker or document dirty state.
	void Clear();
	void MarkSaved();
	bool IsDirty() const;

private:
	struct FEntry
	{
		FEditorTransaction Transaction;
		std::size_t MemoryCost = 0;
		std::uint64_t BeforeState = 0;
		std::uint64_t AfterState = 0;
	};

	[[nodiscard]] std::expected<std::size_t, FEditorCommandError> Validate(const FEditorTransaction& Transaction) const;
	void Record(FEditorTransaction Transaction, std::size_t Cost);

	std::vector<FEntry> Entries;
	std::size_t Cursor = 0;
	std::size_t MemoryCost = 0;
	std::size_t TransactionLimit = 0;
	std::size_t MemoryLimit = 0;

	std::uint64_t CurrentState = 0;
	std::uint64_t SavedState = 0;
	std::uint64_t NextState = 1;
};
}
