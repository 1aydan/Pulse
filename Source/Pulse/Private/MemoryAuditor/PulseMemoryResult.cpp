// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseMemoryResult.h"

#include "Algo/Reverse.h"

static constexpr int64 GPulseBytesPerMB = 1024LL * 1024LL;

void FPulseMemoryReport::ComputeReach(int32 RootNodeIndex, bool bIncludeSoft, FPulseMemoryReach& OutReach) const
{
	OutReach.Nodes.Reset();
	OutReach.ParentSlots.Reset();
	OutReach.Depths.Reset();
	OutReach.NumHard = 0;

	if (!Nodes.IsValidIndex(RootNodeIndex))
	{
		return;
	}

	TBitArray<> Visited(false, Nodes.Num());
	auto Visit = [this, &OutReach, &Visited](int32 NodeIndex, int32 ParentSlot)
	{
		if (!Visited[NodeIndex])
		{
			Visited[NodeIndex] = true;
			OutReach.Nodes.Add(NodeIndex);
			OutReach.ParentSlots.Add(ParentSlot);
			OutReach.Depths.Add(ParentSlot == INDEX_NONE ? 0 : OutReach.Depths[ParentSlot] + 1);
		}
	};

	// Hard closure first, breadth-first, so every entry's parent chain is a shortest reference chain.
	Visit(RootNodeIndex, INDEX_NONE);
	for (int32 Slot = 0; Slot < OutReach.Nodes.Num(); ++Slot)
	{
		for (const int32 Dependency : Nodes[OutReach.Nodes[Slot]].HardDependencies)
		{
			Visit(Dependency, Slot);
		}
	}
	OutReach.NumHard = OutReach.Nodes.Num();

	if (!bIncludeSoft)
	{
		return;
	}

	// Then everything a soft reference could pull in: a soft edge out of any entry, and once past a
	// soft edge, hard edges too — a softly loaded asset brings its hard closure with it.
	for (int32 Slot = 0; Slot < OutReach.Nodes.Num(); ++Slot)
	{
		const FPulseMemoryNode& Node = Nodes[OutReach.Nodes[Slot]];
		for (const int32 Dependency : Node.SoftDependencies)
		{
			Visit(Dependency, Slot);
		}
		if (Slot >= OutReach.NumHard)
		{
			for (const int32 Dependency : Node.HardDependencies)
			{
				Visit(Dependency, Slot);
			}
		}
	}
}

FString FPulseMemoryReport::BuildViaPath(const FPulseMemoryReach& Reach, int32 Slot) const
{
	TArray<FString> Chain;
	for (int32 Parent = Reach.ParentSlots.IsValidIndex(Slot) ? Reach.ParentSlots[Slot] : INDEX_NONE;
		Parent != INDEX_NONE; Parent = Reach.ParentSlots[Parent])
	{
		Chain.Add(Nodes[Reach.Nodes[Parent]].AssetName);
	}
	Algo::Reverse(Chain);
	return FString::Join(Chain, TEXT(" > "));
}

void FPulseMemoryReport::CollectReferencers(const FPulseMemoryReach& Reach, TArray<TArray<int32>>& OutReferencerSlots) const
{
	OutReferencerSlots.Reset();
	OutReferencerSlots.SetNum(Reach.Nodes.Num());

	TArray<int32> SlotByNode;
	SlotByNode.Init(INDEX_NONE, Nodes.Num());
	for (int32 Slot = 0; Slot < Reach.Nodes.Num(); ++Slot)
	{
		SlotByNode[Reach.Nodes[Slot]] = Slot;
	}

	auto AddEdges = [&](int32 FromSlot, const TArray<int32>& Dependencies, bool bTargetsSoftOnly)
	{
		for (const int32 Dependency : Dependencies)
		{
			const int32 ToSlot = SlotByNode[Dependency];
			if (ToSlot != INDEX_NONE && ToSlot != FromSlot && (ToSlot >= Reach.NumHard) == bTargetsSoftOnly)
			{
				OutReferencerSlots[ToSlot].AddUnique(FromSlot);
			}
		}
	};

	for (int32 Slot = 0; Slot < Reach.Nodes.Num(); ++Slot)
	{
		const FPulseMemoryNode& Node = Nodes[Reach.Nodes[Slot]];
		const bool bFromHard = Slot < Reach.NumHard;

		// What keeps a hard entry resident: hard references from other hard entries.
		if (bFromHard)
		{
			AddEdges(Slot, Node.HardDependencies, /*bTargetsSoftOnly*/ false);
		}

		// What could pull in a soft-only entry: any reference from anywhere in the reach.
		AddEdges(Slot, Node.HardDependencies, /*bTargetsSoftOnly*/ true);
		AddEdges(Slot, Node.SoftDependencies, /*bTargetsSoftOnly*/ true);
	}
}

FString FPulseMemoryReport::FormatReferencers(const FPulseMemoryReach& Reach, const TArray<int32>& ReferencerSlots, int32 MaxNames) const
{
	TArray<FString> Names;
	for (const int32 Slot : ReferencerSlots)
	{
		Names.Add(Nodes[Reach.Nodes[Slot]].AssetName);
	}
	// Sorted so the cell is stable across runs whatever order the edges were walked in.
	Names.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });

	const int32 NumHidden = Names.Num() - MaxNames;
	if (MaxNames > 0 && NumHidden > 0)
	{
		Names.SetNum(MaxNames);
		return FString::Printf(TEXT("%s (+%d more)"), *FString::Join(Names, TEXT("; ")), NumHidden);
	}
	return FString::Join(Names, TEXT("; "));
}

/**
 * Retained cost of every node reachable from RootSuccessors, via Cooper, Harvey & Kennedy, "A Simple,
 * Fast Dominance Algorithm". A virtual root (index N) points at every given root, so a package's
 * immediate dominator is the closest node every load path to it passes through. Its retained cost is
 * its subtree in the dominator tree: what would leave memory if that one package stopped loading.
 * OutReachable marks the nodes the result covers; the rest are left at their own cost.
 */
void FPulseMemoryReport::ComputeRetainedFrom(const TArray<int32>& RootSuccessors, TArray<FPulseMemoryCost>& OutRetained, TBitArray<>& OutReachable) const
{
	const FPulseMemoryReport& Report = *this;
	const int32 NumNodes = Report.Nodes.Num();
	const int32 VirtualRoot = NumNodes;

	auto GetSuccessors = [&Report, &RootSuccessors, VirtualRoot](int32 Node) -> const TArray<int32>&
	{
		return Node == VirtualRoot ? RootSuccessors : Report.Nodes[Node].HardDependencies;
	};

	// Iterative DFS for a postorder: a deep reference chain must not be able to overflow the stack.
	TArray<int32> PostOrderNumber;
	PostOrderNumber.Init(INDEX_NONE, NumNodes + 1);
	TArray<int32> PostOrder;
	PostOrder.Reserve(NumNodes + 1);
	{
		TBitArray<> Discovered(false, NumNodes + 1);
		TArray<TPair<int32, int32>> Stack;
		Stack.Emplace(VirtualRoot, 0);
		Discovered[VirtualRoot] = true;
		while (Stack.Num() > 0)
		{
			TPair<int32, int32>& Top = Stack.Last();
			const TArray<int32>& Successors = GetSuccessors(Top.Key);
			if (Top.Value < Successors.Num())
			{
				const int32 Next = Successors[Top.Value++];
				if (!Discovered[Next])
				{
					Discovered[Next] = true;
					Stack.Emplace(Next, 0);
				}
			}
			else
			{
				PostOrderNumber[Top.Key] = PostOrder.Num();
				PostOrder.Add(Top.Key);
				Stack.Pop(EAllowShrinking::No);
			}
		}
	}

	// Predecessors, restricted to reachable nodes.
	TArray<TArray<int32>> Predecessors;
	Predecessors.SetNum(NumNodes + 1);
	for (const int32 Node : PostOrder)
	{
		for (const int32 Successor : GetSuccessors(Node))
		{
			Predecessors[Successor].Add(Node);
		}
	}

	TArray<int32> ImmediateDominator;
	ImmediateDominator.Init(INDEX_NONE, NumNodes + 1);
	ImmediateDominator[VirtualRoot] = VirtualRoot;

	auto Intersect = [&ImmediateDominator, &PostOrderNumber](int32 Left, int32 Right)
	{
		while (Left != Right)
		{
			while (PostOrderNumber[Left] < PostOrderNumber[Right])
			{
				Left = ImmediateDominator[Left];
			}
			while (PostOrderNumber[Right] < PostOrderNumber[Left])
			{
				Right = ImmediateDominator[Right];
			}
		}
		return Left;
	};

	bool bChanged = true;
	while (bChanged)
	{
		bChanged = false;
		// Reverse postorder, skipping the virtual root, which is last in postorder.
		for (int32 Position = PostOrder.Num() - 2; Position >= 0; --Position)
		{
			const int32 Node = PostOrder[Position];
			int32 NewDominator = INDEX_NONE;
			for (const int32 Predecessor : Predecessors[Node])
			{
				if (ImmediateDominator[Predecessor] == INDEX_NONE)
				{
					continue;
				}
				NewDominator = NewDominator == INDEX_NONE ? Predecessor : Intersect(Predecessor, NewDominator);
			}
			if (NewDominator != INDEX_NONE && ImmediateDominator[Node] != NewDominator)
			{
				ImmediateDominator[Node] = NewDominator;
				bChanged = true;
			}
		}
	}

	// A node's dominator is always its DFS ancestor and so later in postorder: walking postorder
	// forward finishes every subtree before its total is pushed up to the parent.
	OutRetained.SetNum(NumNodes);
	OutReachable.Init(false, NumNodes);
	for (int32 Node = 0; Node < NumNodes; ++Node)
	{
		OutRetained[Node] = Report.Nodes[Node].Self;
	}
	for (const int32 Node : PostOrder)
	{
		if (Node == VirtualRoot)
		{
			continue;
		}
		OutReachable[Node] = true;
		const int32 Dominator = ImmediateDominator[Node];
		if (Dominator != INDEX_NONE && Dominator != VirtualRoot)
		{
			OutRetained[Dominator] += OutRetained[Node];
		}
	}
}

bool FPulseMemoryReport::IsRootOverBudget(const FPulseMemoryRoot& Root) const
{
	return (Budget.RootRAMBudgetMB > 0 && Root.Total.RAMBytes > Budget.RootRAMBudgetMB * GPulseBytesPerMB)
		|| (Budget.RootVRAMBudgetMB > 0 && Root.Total.VRAMBytes > Budget.RootVRAMBudgetMB * GPulseBytesPerMB);
}

bool FPulseMemoryReport::IsBaselineOverBudget() const
{
	return (Budget.BaselineRAMBudgetMB > 0 && Baseline.RAMBytes > Budget.BaselineRAMBudgetMB * GPulseBytesPerMB)
		|| (Budget.BaselineVRAMBudgetMB > 0 && Baseline.VRAMBytes > Budget.BaselineVRAMBudgetMB * GPulseBytesPerMB);
}
