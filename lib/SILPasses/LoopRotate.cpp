//===--------- LoopSimplify.cpp - Loop structure simplify -*- C++ -*-------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-looprotate"

#include "swift/SIL/Dominance.h"
#include "swift/SILAnalysis/Analysis.h"
#include "swift/SILAnalysis/DominanceAnalysis.h"
#include "swift/SILAnalysis/SILLoopInfo.h"
#include "swift/SILPasses/Passes.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/SILPasses/Utils/CFG.h"
#include "swift/SILPasses/Utils/SILSSAUpdater.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"

#include "llvm/Support/Debug.h"

using namespace swift;

/// Splits the critical edges between from and to. This code assumes there is
/// only one edge between the two basic blocks.
static SILBasicBlock *splitIfCriticalEdge(SILBasicBlock *From,
                                        SILBasicBlock *To,
                                        DominanceInfo *DT,
                                        SILLoopInfo *LI) {
  auto *T = From->getTerminator();
  for (unsigned i = 0, e = T->getSuccessors().size(); i != e; ++i) {
    if (T->getSuccessors()[i] == To)
      return splitCriticalEdge(T, i, DT, LI);
  }
  llvm_unreachable("Destination block not found");
}

/// Check whether all operands are loop invariant.
static bool hasLoopInvariantOperands(SILInstruction *I, SILLoop *L,
                                     llvm::DenseSet<SILInstruction *> &Inv) {
  auto Opds = I->getAllOperands();

  return std::all_of(Opds.begin(), Opds.end(), [=](Operand &Op) {

    auto *Def = Op.get().getDef();
    // Operand is outside the loop or marked invariant.
    if (auto *Inst = dyn_cast<SILInstruction>(Def))
      return !L->contains(Inst->getParent()) || Inv.count(Inst);
    if (auto *Arg = dyn_cast<SILArgument>(Def))
      return !L->contains(Arg->getParent());

    return false;
  });
}

/// We can not duplicate blocks with AllocStack instructions (they need to be
/// FIFO). Other instructions can be moved to the preheader.
static bool
canDuplicateOrMoveToPreheader(SILLoop *L, SILBasicBlock *Preheader,
                              SILBasicBlock *Blk,
                              SmallVectorImpl<SILInstruction *> &Move) {
  llvm::DenseSet<SILInstruction *> Invariant;
  for (auto &I : *Blk) {
    auto *Inst = &I;
    if (isa<FunctionRefInst>(Inst) || isa<BuiltinFunctionRefInst>(Inst)) {
      Move.push_back(Inst);
      Invariant.insert(Inst);
    } else if (isa<AllocStackInst>(Inst) || isa<DeallocStackInst>(Inst)) {
      return false;
    } else if (isa<IntegerLiteralInst>(Inst)) {
      Move.push_back(Inst);
      Invariant.insert(Inst);
    } else if (!Inst->mayHaveSideEffects() &&
               !Inst->mayReadFromMemory() &&
               !isa<TermInst>(Inst) &&
               !isa<AllocationInst>(Inst) && /* not marked mayhavesideffects */
               hasLoopInvariantOperands(Inst, L, Invariant)) {
      Move.push_back(Inst);
      Invariant.insert(Inst);
    }
  }

  return true;
}

static void mapOperands(SILInstruction *I,
                        llvm::DenseMap<ValueBase *, SILValue> ValueMap) {
  for (auto &Opd : I->getAllOperands()) {
    SILValue OrigVal = Opd.get();
    ValueBase *OrigDef = OrigVal.getDef();
    if (SILValue MappedVal = ValueMap[OrigDef]) {
      unsigned ResultIdx = OrigVal.getResultNumber();
      // All mapped instructions have their result number set to zero. Except
      // for arguments that we followed along one edge to their incoming value
      // on that edge.
      if (isa<SILArgument>(OrigDef))
        ResultIdx = MappedVal.getResultNumber();
      Opd.set(SILValue(MappedVal.getDef(), ResultIdx));
    }
  }
}

static void
updateSSAForUseOfInst(SILSSAUpdater &Updater,
                      llvm::DenseMap<ValueBase *, SILValue> &ValueMap,
                      SILBasicBlock *Header, SILBasicBlock *EntryCheckBlock,
                      ValueBase *Inst) {
  if (Inst->use_empty())
    return;

  // Find the mapped instruction.
  SILValue MappedValue = ValueMap[Inst];
  auto *MappedInst = MappedValue.getDef();
  assert(MappedValue);
  assert(MappedInst);

  // For each use of a specific result value of the instruction.
  for (unsigned i = 0, e = Inst->getNumTypes(); i != e; ++i) {
    SILValue Res(Inst, i);
    SILValue MappedRes(MappedInst, i);

    Updater.Initialize(Res.getType());
    Updater.AddAvailableValue(Header, Res);
    Updater.AddAvailableValue(EntryCheckBlock, MappedRes);


    // Because of the way that phi nodes are represented we have to collect all
    // uses before we update SSA. Modifying one phi node can invalidate another
    // unrelated phi nodes operands through the common branch instruction (that
    // has to be modified). This would invalidate a plain ValueUseIterator.
    // Instead we collect uses wrapping uses in branches specially so that we
    // can reconstruct the use even after the branch has been modified.
    SmallVector<UseWrapper, 8> StoredUses;
    for (auto *U : Res.getUses())
      StoredUses.push_back(UseWrapper(U));
    for (auto U : StoredUses) {
      Operand *Use = U;
      SILInstruction *User = Use->getUser();
      assert(User && "Missing user");

      // Ignore uses in the same basic block.
      if (User->getParent() == Header)
        continue;

      assert(User->getParent() != EntryCheckBlock &&
             "The entry check block should dominate the header");
      Updater.RewriteUse(*Use);
    }
  }
}

/// Rewrite the code we just created in the preheader and update SSA form.
static void
rewriteNewLoopEntryCheckBlock(SILBasicBlock *Header,
                              SILBasicBlock *EntryCheckBlock,
                              llvm::DenseMap<ValueBase *, SILValue> ValueMap) {

  SILSSAUpdater Updater;

  // For each instruction in the original header including phis (arguments to
  // the block).
  SmallVector<ValueBase *, 16> Insts;
  std::copy(Header->getBBArgs().begin(), Header->getBBArgs().end(),
            std::back_inserter(Insts));
  for (auto &Inst: *Header)
    Insts.push_back(&Inst);

  // Fix PHIs (incomming arguments).
  for (auto *Inst: Header->getBBArgs())
    updateSSAForUseOfInst(Updater, ValueMap, Header, EntryCheckBlock, Inst);

  auto InstIter = Header->begin();

  // The terminator might change from under us.
  while (InstIter != Header->end()) {
    auto &Inst = *InstIter;
    updateSSAForUseOfInst(Updater, ValueMap, Header, EntryCheckBlock, &Inst);
    InstIter++;
  }
}

/// Update the dominator tree after rotating the loop.
/// The former preheader now dominates all of the former headers children. The
/// former latch now dominates the former header.
static void updateDomTree(DominanceInfo *DT, SILBasicBlock *Preheader,
                          SILBasicBlock *Latch, SILBasicBlock *Header) {
  auto *HeaderN = DT->getNode(Header);
  SmallVector<DominanceInfoNode *, 4> Children(HeaderN->begin(),
                                               HeaderN->end());
  auto *PreheaderN = DT->getNode(Preheader);
  for (auto *Child : Children)
    DT->changeImmediateDominator(Child, PreheaderN);

  if (Header != Latch)
    DT->changeImmediateDominator(HeaderN, DT->getNode(Latch));
}

static bool rotateLoopAtMostUpToLatch(SILLoop *L, DominanceInfo *DT,
                                      SILLoopInfo *LI, bool ShouldVerify) {
  auto *Latch = L->getLoopLatch();
  if (!Latch) {
    DEBUG(llvm::dbgs() << *L << " does not have a single latch block\n");
    return false;
  }

  // Rotate single basic block loops.
  bool DidRotate = rotateLoop(L, DT, LI, true, Latch, ShouldVerify);

  // Keep rotating at most until we hit the original latch.
  if (DidRotate)
    while (rotateLoop(L, DT, LI, false, Latch, ShouldVerify)) {}

  return DidRotate;
}

/// We rotated a loop if it has the following properties.
///
/// * It has an exiting header with a conditional branch.
/// * It has a preheader.
///
/// We will rotate at most up to the basic block passed as an argument.
/// We will not rotate a loop where the header is equal to the latch except is
/// RotateSingleBlockLoops is true.
///
/// Note: The code relies on the 'UpTo' basic block to stay within the rotate
/// loop for termination.
bool swift::rotateLoop(SILLoop *L, DominanceInfo *DT, SILLoopInfo *LI,
                       bool RotateSingleBlockLoops, SILBasicBlock *UpTo,
                       bool ShouldVerify) {
  assert(L != nullptr && DT != nullptr && LI != nullptr &&
         "Missing loop information");

  SILBasicBlock *Header = L->getHeader();
  if (!Header || (!RotateSingleBlockLoops && Header == UpTo))
    return false;

  assert(RotateSingleBlockLoops || L->getBlocks().size() != 1);

  // Need a conditional branch that guards the entry into the loop.
  auto *LoopEntryBranch = dyn_cast<CondBranchInst>(Header->getTerminator());
  if (!LoopEntryBranch)
    return false;

  // The header needs to exit the loop.
  if (!L->isLoopExiting(Header)) {
    DEBUG(llvm::dbgs() << *L << " not a exiting header\n");
    DEBUG(L->getHeader()->getParent()->dump());
    return false;
  }

  // We need a preheader.
  auto *Preheader = L->getLoopPreheader();
  if (!Preheader) {
    DEBUG(llvm::dbgs() << *L << " no preheader\n");
    DEBUG(L->getHeader()->getParent()->dump());
    return false;
  }

  // We need a single backedge and the latch must not exit the loop if it is
  // also the header.
  auto *Latch = L->getLoopLatch();
  if (!Latch) {
    DEBUG(llvm::dbgs() << *L << " no single latch\n");
    return false;
  }

  // Make sure we can duplicate the header.
  SmallVector<SILInstruction *, 8> MoveToPreheader;
  if (!canDuplicateOrMoveToPreheader(L, Preheader, Header, MoveToPreheader)) {
    DEBUG(llvm::dbgs() << *L << " instructions in header preventing rotating\n");
    return false;
  }

  DEBUG(llvm::dbgs() << " Rotating " << *L);

  // Now that we know we can perform the rotation move the instructions that
  // need moving.
  for (auto *Inst : MoveToPreheader)
    Inst->moveBefore(Preheader->getTerminator());

  // We can rotate the loop.

  auto *NewHeader = LoopEntryBranch->getTrueBB();
  auto *Exit = LoopEntryBranch->getFalseBB();
  if (L->contains(Exit))
    std::swap(NewHeader, Exit);
  assert(L->contains(NewHeader) && !L->contains(Exit) &&
         "Could not find loop header and exit block");
  bool IsHeaderExitDominated = DT->dominates(Header, Exit);

  assert(NewHeader->getSinglePredecessor() || Header == Latch);

  // Map the values for the duplicated header block. We are duplicating the
  // header instructions into the end of the preheader.
  llvm::DenseMap<ValueBase *, SILValue> ValueMap;

  // The original 'phi' argument values are just the values coming from the
  // preheader edge.
  ArrayRef<SILArgument *> PHIs = Header->getBBArgs();
  OperandValueArrayRef PreheaderArgs =
      cast<BranchInst>(Preheader->getTerminator())->getArgs();
  assert(PHIs.size() == PreheaderArgs.size() &&
         "Basic block arguments and incoming edge mismatch");

  // Here we also store the value index to use into the value map (versus
  // non-argument values where the operand use decides which value index to
  // use).
  for (unsigned Idx = 0, E = PHIs.size(); Idx != E; ++Idx)
    ValueMap[PHIs[Idx]] = PreheaderArgs[Idx];

  // The other instructions are just cloned to the preheader.
  TermInst *PreheaderBranch = Preheader->getTerminator();
  for (auto &Inst : *Header) {
    SILInstruction *I = Inst.clone(PreheaderBranch);
    mapOperands(I, ValueMap);

    // The actual operand will sort out which result idx to use.
    ValueMap[&Inst] = SILValue(I, 0);
  }

  PreheaderBranch->dropAllReferences();
  PreheaderBranch->eraseFromParent();

  // If there were any uses of instructions in the duplicated loop entry check
  // block rewrite them using the ssa updater.
  rewriteNewLoopEntryCheckBlock(Header, Preheader, ValueMap);

  L->moveToHeader(NewHeader);

  // Now the original preheader dominates all of headers children and the
  // original latch dominates the header.
  updateDomTree(DT, Preheader, Latch, Header);

  assert(DT->getNode(NewHeader)->getIDom() == DT->getNode(Preheader));
  assert(!IsHeaderExitDominated ||
         DT->getNode(Exit)->getIDom() == DT->getNode(Preheader));
  assert(DT->getNode(Header)->getIDom() == DT->getNode(Latch) ||
         ((Header == Latch) &&
          DT->getNode(Header)->getIDom() == DT->getNode(Preheader)));

  // Create a new preheader.
  splitIfCriticalEdge(Preheader, NewHeader, DT, LI);

  // Beautify the IR. Move the old header to after the old latch as it is now
  // the latch.
  Header->moveAfter(Latch);

  if (ShouldVerify) {
    DT->verify();
    LI->verify();
    Latch->getParent()->verify();
  }

  DEBUG(llvm::dbgs() << "  to " << *L);
  DEBUG(L->getHeader()->getParent()->dump());
  return true;
}

namespace {

class LoopRotation : public SILFunctionTransform {

  StringRef getName() override { return "SIL Loop Rotation"; }

  void run() override {
    SILLoopAnalysis *LA = PM->getAnalysis<SILLoopAnalysis>();
    assert(LA);
    DominanceAnalysis *DA = PM->getAnalysis<DominanceAnalysis>();
    assert(DA);

    SILFunction *F = getFunction();
    assert(F);
    SILLoopInfo *LI = LA->getLoopInfo(F);
    assert(LI);
    DominanceInfo *DT = DA->getDomInfo(F);

    if (LI->empty()) {
      DEBUG(llvm::dbgs() << "No loops in " << F->getName() << "\n");
      return;
    }

    DEBUG(llvm::dbgs() << "Rotating loops in " << F->getName() << "\n");
    bool ShouldVerify = getOptions().VerifyAll;

    bool Changed = false;
    for (auto *LoopIt : *LI) {
      // Rotate loops recursively bottom-up in the loop tree.
      SmallVector<SILLoop *, 8> Worklist;
      Worklist.push_back(LoopIt);
      for (unsigned i = 0; i < Worklist.size(); ++i) {
        auto *L = Worklist[i];
        for (auto *SubLoop : *L)
          Worklist.push_back(SubLoop);
      }

      while (!Worklist.empty()) {
        Changed |= rotateLoopAtMostUpToLatch(Worklist.pop_back_val(), DT, LI,
                                             ShouldVerify);
      }
    }

    if (Changed) {
      // TODO: Verify: We have updated the DominanceInfo and SILLoopInfo. It
      // should be safe not to invalidate the CFG.
      PM->invalidateAnalysis(F, SILAnalysis::InvalidationKind::CFG);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createLoopRotatePass() {
  return new LoopRotation();
}
