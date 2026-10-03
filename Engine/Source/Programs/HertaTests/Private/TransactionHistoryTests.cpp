#include "Herta/EditorCore/TransactionHistory.h"

#include <doctest/doctest.h>

#include <limits>
#include <ostream>
#include <utility>

namespace Herta
{
namespace
{
FEditorTransaction MakeValueTransaction(std::string Label, int& Value, const int Before, const int After, const std::size_t MemoryCost = 0, const bool* const Fail = nullptr)
{
	return {
	    .Label = std::move(Label),
	    .Apply = [&Value, Before, After, Fail](const bool bUndo) -> std::expected<void, FEditorCommandError>
	{
		if (Fail && *Fail)
		{
			return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = "Rejected by domain"});
		}

		Value = bUndo ? Before : After;
		return {};
	},
	    .MemoryCost = MemoryCost,
	};
}
}

TEST_CASE("Editor transaction history executes undo and redo with descriptive labels")
{
	FEditorTransactionHistory History;
	int Value = 0;
	CHECK_FALSE(History.CanUndo());
	CHECK_FALSE(History.CanRedo());
	CHECK(History.GetUndoLabel().empty());
	CHECK(History.GetRedoLabel().empty());
	CHECK_FALSE(History.IsDirty());
	CHECK_FALSE(History.Undo().has_value());
	CHECK_FALSE(History.Redo().has_value());
	REQUIRE(History.Execute(MakeValueTransaction("Set value", Value, 0, 1)));
	CHECK(Value == 1);
	CHECK(History.CanUndo());
	CHECK_FALSE(History.CanRedo());
	CHECK(History.GetUndoLabel() == "Set value");
	CHECK(History.IsDirty());
	REQUIRE(History.Undo());
	CHECK(Value == 0);
	CHECK_FALSE(History.CanUndo());
	CHECK(History.GetRedoLabel() == "Set value");
	CHECK_FALSE(History.IsDirty());
	REQUIRE(History.Redo());
	CHECK(Value == 1);
	CHECK(History.GetUndoLabel() == "Set value");
	CHECK(History.IsDirty());
}

TEST_CASE("Editor transaction history records applied previews without replaying them")
{
	FEditorTransactionHistory History;
	int Value = 5;
	int Calls = 0;
	REQUIRE(History.RecordApplied({
	    .Label = "Preview value",
	    .Apply = [&Value, &Calls](const bool bUndo) -> std::expected<void, FEditorCommandError>
	{
		++Calls;
		Value = bUndo ? 0 : 5;
		return {};
	},
	}));

	CHECK(Calls == 0);
	CHECK(Value == 5);
	REQUIRE(History.Undo());
	CHECK(Calls == 1);
	CHECK(Value == 0);
	REQUIRE(History.Redo());
	CHECK(Calls == 2);
	CHECK(Value == 5);
}

TEST_CASE("Failed editor transactions preserve the cursor saved state and redo branch")
{
	FEditorTransactionHistory History;
	int Value = 0;
	bool bFail = false;
	REQUIRE(History.Execute(MakeValueTransaction("First", Value, 0, 1, 0, &bFail)));
	History.MarkSaved();
	REQUIRE(History.Execute(MakeValueTransaction("Second", Value, 1, 2, 0, &bFail)));
	bFail = true;
	CHECK_FALSE(History.Undo().has_value());
	CHECK(Value == 2);
	CHECK(History.GetUndoLabel() == "Second");
	CHECK_FALSE(History.CanRedo());
	CHECK(History.IsDirty());
	bFail = false;
	REQUIRE(History.Undo());
	CHECK_FALSE(History.IsDirty());
	bFail = true;
	CHECK_FALSE(History.Redo().has_value());
	CHECK_FALSE(History.Execute(MakeValueTransaction("Failed branch", Value, 1, 9, 0, &bFail)).has_value());
	CHECK(Value == 1);
	CHECK(History.GetUndoLabel() == "First");
	CHECK(History.GetRedoLabel() == "Second");
	CHECK_FALSE(History.IsDirty());
	bFail = false;
	REQUIRE(History.Redo());
	CHECK(Value == 2);
}

TEST_CASE("Editor saved states use unique identities across history branches")
{
	FEditorTransactionHistory History;
	int Value = 0;
	REQUIRE(History.Execute(MakeValueTransaction("Saved value", Value, 0, 1)));
	History.MarkSaved();
	REQUIRE(History.Execute(MakeValueTransaction("Second value", Value, 1, 2)));
	REQUIRE(History.Undo());
	CHECK_FALSE(History.IsDirty());
	REQUIRE(History.Undo());
	CHECK(History.IsDirty());
	REQUIRE(History.Execute(MakeValueTransaction("New branch", Value, 0, 10)));
	CHECK(Value == 10);
	CHECK(History.IsDirty());
	CHECK_FALSE(History.CanRedo());
	History.MarkSaved();
	REQUIRE(History.Undo());
	CHECK(History.IsDirty());
	REQUIRE(History.Redo());
	CHECK_FALSE(History.IsDirty());
}

TEST_CASE("Editor history trimming preserves reachable saved-state identities")
{
	FEditorTransactionHistory History(2);
	int Value = 0;
	REQUIRE(History.Execute(MakeValueTransaction("First", Value, 0, 1)));
	History.MarkSaved();
	REQUIRE(History.Execute(MakeValueTransaction("Second", Value, 1, 2)));
	REQUIRE(History.Execute(MakeValueTransaction("Third", Value, 2, 3)));
	REQUIRE(History.Undo());
	CHECK(Value == 2);
	CHECK(History.IsDirty());
	REQUIRE(History.Undo());
	CHECK(Value == 1);
	CHECK_FALSE(History.IsDirty());
	CHECK_FALSE(History.CanUndo());
	CHECK(History.GetRedoLabel() == "Second");
	REQUIRE(History.Execute(MakeValueTransaction("Branch after trim", Value, 1, 4)));
	CHECK(History.IsDirty());
	CHECK_FALSE(History.CanRedo());
}

TEST_CASE("Editor history memory budget evicts oldest entries without losing dirty identity")
{
	FEditorTransactionHistory History(256, 1024);
	int Value = 0;
	REQUIRE(History.Execute(MakeValueTransaction("First", Value, 0, 1, 300)));
	REQUIRE(History.Execute(MakeValueTransaction("Second", Value, 1, 2, 300)));
	REQUIRE(History.Execute(MakeValueTransaction("Third", Value, 2, 3, 300)));
	REQUIRE(History.Undo());
	REQUIRE(History.Undo());
	CHECK(Value == 1);
	CHECK_FALSE(History.CanUndo());
	CHECK(History.IsDirty());
}

TEST_CASE("Editor history rejects invalid and oversize transactions before applying or recording")
{
	FEditorTransactionHistory History(2, 1024);
	int Value = 0;
	REQUIRE(History.Execute(MakeValueTransaction("First", Value, 0, 1)));
	History.MarkSaved();
	REQUIRE(History.Execute(MakeValueTransaction("Second", Value, 1, 2)));
	REQUIRE(History.Undo());
	CHECK_FALSE(History.Execute(MakeValueTransaction("Oversize", Value, 1, 9, 2048)).has_value());
	CHECK_FALSE(History.RecordApplied(MakeValueTransaction("Oversize preview", Value, 1, 9, std::numeric_limits<std::size_t>::max())).has_value());
	CHECK_FALSE(History.Execute(MakeValueTransaction(std::string(1024, 'x'), Value, 1, 9)).has_value());
	CHECK_FALSE(History.Execute({.Label = "Missing callback", .Apply = {}}).has_value());
	CHECK_FALSE(History.RecordApplied(MakeValueTransaction("", Value, 1, 9)).has_value());
	CHECK(Value == 1);
	CHECK(History.GetUndoLabel() == "First");
	CHECK(History.GetRedoLabel() == "Second");
	CHECK_FALSE(History.IsDirty());
	REQUIRE(History.Redo());
	CHECK(Value == 2);

	FEditorTransactionHistory Disabled(0);
	CHECK_FALSE(Disabled.Execute(MakeValueTransaction("Disabled", Value, 2, 9)).has_value());
	CHECK(Value == 2);
}

TEST_CASE("Editor history enforces its default command and memory ceilings")
{
	FEditorTransactionHistory History(1000, std::numeric_limits<std::size_t>::max());
	int Value = 0;
	CHECK_FALSE(History.Execute(MakeValueTransaction("Above absolute memory ceiling", Value, 0, 9, 64 * 1024 * 1024)).has_value());
	CHECK(Value == 0);

	for (int Index = 0; Index < 260; ++Index)
	{
		REQUIRE(History.Execute(MakeValueTransaction("Increment", Value, Index, Index + 1)));
	}

	for (int Index = 0; Index < 256; ++Index)
	{
		REQUIRE(History.Undo());
	}

	CHECK(Value == 4);
	CHECK_FALSE(History.CanUndo());
	CHECK(History.IsDirty());
}

TEST_CASE("Clearing editor history leaves dirty state under explicit saved-state control")
{
	FEditorTransactionHistory History;
	int Value = 0;
	REQUIRE(History.Execute(MakeValueTransaction("First", Value, 0, 1)));
	History.Clear();
	CHECK_FALSE(History.CanUndo());
	CHECK_FALSE(History.CanRedo());
	CHECK(History.IsDirty());
	History.MarkSaved();
	CHECK_FALSE(History.IsDirty());
	REQUIRE(History.Execute(MakeValueTransaction("After clear", Value, 1, 2)));
	REQUIRE(History.Undo());
	CHECK(Value == 1);
	CHECK_FALSE(History.IsDirty());
}
}
