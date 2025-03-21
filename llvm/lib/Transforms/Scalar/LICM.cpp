//===-- LICM.cpp - Loop Invariant Code Motion Pass ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass performs loop invariant code motion, attempting to remove as much
// code from the body of a loop as possible.  It does this by either hoisting
// code into the preheader block, or by sinking code to the exit blocks if it is
// safe.  This pass also promotes must-aliased memory locations in the loop to
// live in registers, thus hoisting and sinking "invariant" loads and stores.
//
// Hoisting operations out of loops is a canonicalization transform.  It
// enables and simplifies subsequent optimizations in the middle-end.
// Rematerialization of hoisted instructions to reduce register pressure is the
// responsibility of the back-end, which has more accurate information about
// register pressure and also handles other optimizations than LICM that
// increase live-ranges.
//
// This pass uses alias analysis for two purposes:
//
//  1. Moving loop invariant loads and calls out of loops.  If we can determine
//     that a load or call inside of a loop never aliases anything stored to,
//     we can hoist it or sink it like any other instruction.
//  2. Scalar Promotion of Memory - If there is a store instruction inside of
//     the loop, we try to move the store to happen AFTER the loop instead of
//     inside of the loop.  This can only happen if a few conditions are true:
//       A. The pointer stored through is loop invariant
//       B. There are no stores or loads in the loop which _may_ alias the
//          pointer.  There are no calls in the loop which mod/ref the pointer.
//     If these conditions are true, we can promote the loads and stores in the
//     loop of the pointer to use a temporary alloca'd variable.  We then use
//     the SSAUpdater to construct the appropriate SSA form for the value.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/ADT/PriorityWorklist.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/GuardUtils.h"
#include "llvm/Analysis/LazyBlockFrequencyInfo.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/LoopNestAnalysis.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/MustExecute.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/PredIteratorCache.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/AssumeBundleBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include <algorithm>
#include <utility>
using namespace llvm;

namespace llvm {
class BlockFrequencyInfo;
class LPMUpdater;
} // namespace llvm

#define DEBUG_TYPE "licm"

STATISTIC(NumCreatedBlocks, "Number of blocks created");
STATISTIC(NumClonedBranches, "Number of branches cloned");
STATISTIC(NumSunk, "Number of instructions sunk out of loop");
STATISTIC(NumHoisted, "Number of instructions hoisted out of loop");
STATISTIC(NumMovedLoads, "Number of load insts hoisted or sunk");
STATISTIC(NumMovedCalls, "Number of call insts hoisted or sunk");
STATISTIC(NumPromoted, "Number of memory locations promoted to registers");

/// Memory promotion is enabled by default.
static cl::opt<bool>
    DisablePromotion("disable-licm-promotion", cl::Hidden, cl::init(false),
                     cl::desc("Disable memory promotion in LICM pass"));

static cl::opt<bool> ControlFlowHoisting(
    "licm-control-flow-hoisting", cl::Hidden, cl::init(false),
    cl::desc("Enable control flow (and PHI) hoisting in LICM"));

static cl::opt<uint32_t> MaxNumUsesTraversed(
    "licm-max-num-uses-traversed", cl::Hidden, cl::init(8),
    cl::desc("Max num uses visited for identifying load "
             "invariance in loop using invariant start (default = 8)"));

// Experimental option to allow imprecision in LICM in pathological cases, in
// exchange for faster compile. This is to be removed if MemorySSA starts to
// address the same issue. LICM calls MemorySSAWalker's
// getClobberingMemoryAccess, up to the value of the Cap, getting perfect
// accuracy. Afterwards, LICM will call into MemorySSA's getDefiningAccess,
// which may not be precise, since optimizeUses is capped. The result is
// correct, but we may not get as "far up" as possible to get which access is
// clobbering the one queried.
cl::opt<unsigned> llvm::SetLicmMssaOptCap(
    "licm-mssa-optimization-cap", cl::init(100), cl::Hidden,
    cl::desc("Enable imprecision in LICM in pathological cases, in exchange "
             "for faster compile. Caps the MemorySSA clobbering calls."));

// Experimentally, memory promotion carries less importance than sinking and
// hoisting. Limit when we do promotion when using MemorySSA, in order to save
// compile time.
cl::opt<unsigned> llvm::SetLicmMssaNoAccForPromotionCap(
    "licm-mssa-max-acc-promotion", cl::init(250), cl::Hidden,
    cl::desc("[LICM & MemorySSA] When MSSA in LICM is disabled, this has no "
             "effect. When MSSA in LICM is enabled, then this is the maximum "
             "number of accesses allowed to be present in a loop in order to "
             "enable memory promotion."));

static bool inSubLoop(BasicBlock *BB, Loop *CurLoop, LoopInfo *LI);
static bool isNotUsedOrFreeInLoop(const Instruction &I, const Loop *CurLoop,
                                  const LoopSafetyInfo *SafetyInfo,
                                  TargetTransformInfo *TTI, bool &FreeInLoop,
                                  bool LoopNestMode);
static void hoist(Instruction &I, const DominatorTree *DT, const Loop *CurLoop,
                  BasicBlock *Dest, ICFLoopSafetyInfo *SafetyInfo,
                  MemorySSAUpdater &MSSAU, ScalarEvolution *SE,
                  OptimizationRemarkEmitter *ORE);
static bool sink(Instruction &I, LoopInfo *LI, DominatorTree *DT,
                 BlockFrequencyInfo *BFI, const Loop *CurLoop,
                 ICFLoopSafetyInfo *SafetyInfo, MemorySSAUpdater &MSSAU,
                 OptimizationRemarkEmitter *ORE);
static bool isSafeToExecuteUnconditionally(
    Instruction &Inst, const DominatorTree *DT, const TargetLibraryInfo *TLI,
    const Loop *CurLoop, const LoopSafetyInfo *SafetyInfo,
    OptimizationRemarkEmitter *ORE, const Instruction *CtxI,
    bool AllowSpeculation);
static bool pointerInvalidatedByLoop(MemorySSA *MSSA, MemoryUse *MU,
                                     Loop *CurLoop, Instruction &I,
                                     SinkAndHoistLICMFlags &Flags);
static bool pointerInvalidatedByBlock(BasicBlock &BB, MemorySSA &MSSA,
                                      MemoryUse &MU);
static Instruction *cloneInstructionInExitBlock(
    Instruction &I, BasicBlock &ExitBlock, PHINode &PN, const LoopInfo *LI,
    const LoopSafetyInfo *SafetyInfo, MemorySSAUpdater &MSSAU);

static void eraseInstruction(Instruction &I, ICFLoopSafetyInfo &SafetyInfo,
                             MemorySSAUpdater &MSSAU);

static void moveInstructionBefore(Instruction &I, Instruction &Dest,
                                  ICFLoopSafetyInfo &SafetyInfo,
                                  MemorySSAUpdater &MSSAU, ScalarEvolution *SE);

static void foreachMemoryAccess(MemorySSA *MSSA, Loop *L,
                                function_ref<void(Instruction *)> Fn);
static SmallVector<SmallSetVector<Value *, 8>, 0>
collectPromotionCandidates(MemorySSA *MSSA, AliasAnalysis *AA, Loop *L);

namespace {
struct LoopInvariantCodeMotion {
  bool runOnLoop(Loop *L, AAResults *AA, LoopInfo *LI, DominatorTree *DT,
                 BlockFrequencyInfo *BFI, TargetLibraryInfo *TLI,
                 TargetTransformInfo *TTI, ScalarEvolution *SE, MemorySSA *MSSA,
                 OptimizationRemarkEmitter *ORE, bool LoopNestMode = false);

  LoopInvariantCodeMotion(unsigned LicmMssaOptCap,
                          unsigned LicmMssaNoAccForPromotionCap,
                          bool LicmAllowSpeculation)
      : LicmMssaOptCap(LicmMssaOptCap),
        LicmMssaNoAccForPromotionCap(LicmMssaNoAccForPromotionCap),
        LicmAllowSpeculation(LicmAllowSpeculation) {}

private:
  unsigned LicmMssaOptCap;
  unsigned LicmMssaNoAccForPromotionCap;
  bool LicmAllowSpeculation;
};

struct LegacyLICMPass : public LoopPass {
  static char ID; // Pass identification, replacement for typeid
  LegacyLICMPass(
      unsigned LicmMssaOptCap = SetLicmMssaOptCap,
      unsigned LicmMssaNoAccForPromotionCap = SetLicmMssaNoAccForPromotionCap,
      bool LicmAllowSpeculation = true)
      : LoopPass(ID), LICM(LicmMssaOptCap, LicmMssaNoAccForPromotionCap,
                           LicmAllowSpeculation) {
    initializeLegacyLICMPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override {
    if (skipLoop(L))
      return false;

    LLVM_DEBUG(dbgs() << "Perform LICM on Loop with header at block "
                      << L->getHeader()->getNameOrAsOperand() << "\n");

    auto *SE = getAnalysisIfAvailable<ScalarEvolutionWrapperPass>();
    MemorySSA *MSSA = &getAnalysis<MemorySSAWrapperPass>().getMSSA();
    bool hasProfileData = L->getHeader()->getParent()->hasProfileData();
    BlockFrequencyInfo *BFI =
        hasProfileData ? &getAnalysis<LazyBlockFrequencyInfoPass>().getBFI()
                       : nullptr;
    // For the old PM, we can't use OptimizationRemarkEmitter as an analysis
    // pass. Function analyses need to be preserved across loop transformations
    // but ORE cannot be preserved (see comment before the pass definition).
    OptimizationRemarkEmitter ORE(L->getHeader()->getParent());
    return LICM.runOnLoop(
        L, &getAnalysis<AAResultsWrapperPass>().getAAResults(),
        &getAnalysis<LoopInfoWrapperPass>().getLoopInfo(),
        &getAnalysis<DominatorTreeWrapperPass>().getDomTree(), BFI,
        &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(
            *L->getHeader()->getParent()),
        &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(
            *L->getHeader()->getParent()),
        SE ? &SE->getSE() : nullptr, MSSA, &ORE);
  }

  /// This transformation requires natural loop information & requires that
  /// loop preheaders be inserted into the CFG...
  ///
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<MemorySSAWrapperPass>();
    AU.addPreserved<MemorySSAWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
    getLoopAnalysisUsage(AU);
    LazyBlockFrequencyInfoPass::getLazyBFIAnalysisUsage(AU);
    AU.addPreserved<LazyBlockFrequencyInfoPass>();
    AU.addPreserved<LazyBranchProbabilityInfoPass>();
  }

private:
  LoopInvariantCodeMotion LICM;
};
} // namespace

PreservedAnalyses LICMPass::run(Loop &L, LoopAnalysisManager &AM,
                                LoopStandardAnalysisResults &AR, LPMUpdater &) {
  if (!AR.MSSA)
    report_fatal_error("LICM requires MemorySSA (loop-mssa)");

  // For the new PM, we also can't use OptimizationRemarkEmitter as an analysis
  // pass.  Function analyses need to be preserved across loop transformations
  // but ORE cannot be preserved (see comment before the pass definition).
  OptimizationRemarkEmitter ORE(L.getHeader()->getParent());

  LoopInvariantCodeMotion LICM(Opts.MssaOptCap, Opts.MssaNoAccForPromotionCap,
                               Opts.AllowSpeculation);
  if (!LICM.runOnLoop(&L, &AR.AA, &AR.LI, &AR.DT, AR.BFI, &AR.TLI, &AR.TTI,
                      &AR.SE, AR.MSSA, &ORE))
    return PreservedAnalyses::all();

  auto PA = getLoopPassPreservedAnalyses();

  PA.preserve<DominatorTreeAnalysis>();
  PA.preserve<LoopAnalysis>();
  PA.preserve<MemorySSAAnalysis>();

  return PA;
}

void LICMPass::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) {
  static_cast<PassInfoMixin<LICMPass> *>(this)->printPipeline(
      OS, MapClassName2PassName);

  OS << "<";
  OS << (Opts.AllowSpeculation ? "" : "no-") << "allowspeculation";
  OS << ">";
}

PreservedAnalyses LNICMPass::run(LoopNest &LN, LoopAnalysisManager &AM,
                                 LoopStandardAnalysisResults &AR,
                                 LPMUpdater &) {
  if (!AR.MSSA)
    report_fatal_error("LNICM requires MemorySSA (loop-mssa)");

  // For the new PM, we also can't use OptimizationRemarkEmitter as an analysis
  // pass.  Function analyses need to be preserved across loop transformations
  // but ORE cannot be preserved (see comment before the pass definition).
  OptimizationRemarkEmitter ORE(LN.getParent());

  LoopInvariantCodeMotion LICM(Opts.MssaOptCap, Opts.MssaNoAccForPromotionCap,
                               Opts.AllowSpeculation);

  Loop &OutermostLoop = LN.getOutermostLoop();
  bool Changed = LICM.runOnLoop(&OutermostLoop, &AR.AA, &AR.LI, &AR.DT, AR.BFI,
                                &AR.TLI, &AR.TTI, &AR.SE, AR.MSSA, &ORE, true);

  if (!Changed)
    return PreservedAnalyses::all();

  auto PA = getLoopPassPreservedAnalyses();

  PA.preserve<DominatorTreeAnalysis>();
  PA.preserve<LoopAnalysis>();
  PA.preserve<MemorySSAAnalysis>();

  return PA;
}

void LNICMPass::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) {
  static_cast<PassInfoMixin<LNICMPass> *>(this)->printPipeline(
      OS, MapClassName2PassName);

  OS << "<";
  OS << (Opts.AllowSpeculation ? "" : "no-") << "allowspeculation";
  OS << ">";
}

char LegacyLICMPass::ID = 0;
INITIALIZE_PASS_BEGIN(LegacyLICMPass, "licm", "Loop Invariant Code Motion",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LoopPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MemorySSAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LazyBFIPass)
INITIALIZE_PASS_END(LegacyLICMPass, "licm", "Loop Invariant Code Motion", false,
                    false)

Pass *llvm::createLICMPass() { return new LegacyLICMPass(); }
Pass *llvm::createLICMPass(unsigned LicmMssaOptCap,
                           unsigned LicmMssaNoAccForPromotionCap,
                           bool LicmAllowSpeculation) {
  return new LegacyLICMPass(LicmMssaOptCap, LicmMssaNoAccForPromotionCap,
                            LicmAllowSpeculation);
}

llvm::SinkAndHoistLICMFlags::SinkAndHoistLICMFlags(bool IsSink, Loop *L,
                                                   MemorySSA *MSSA)
    : SinkAndHoistLICMFlags(SetLicmMssaOptCap, SetLicmMssaNoAccForPromotionCap,
                            IsSink, L, MSSA) {}

llvm::SinkAndHoistLICMFlags::SinkAndHoistLICMFlags(
    unsigned LicmMssaOptCap, unsigned LicmMssaNoAccForPromotionCap, bool IsSink,
    Loop *L, MemorySSA *MSSA)
    : LicmMssaOptCap(LicmMssaOptCap),
      LicmMssaNoAccForPromotionCap(LicmMssaNoAccForPromotionCap),
      IsSink(IsSink) {
  assert(((L != nullptr) == (MSSA != nullptr)) &&
         "Unexpected values for SinkAndHoistLICMFlags");
  if (!MSSA)
    return;

  unsigned AccessCapCount = 0;
  for (auto *BB : L->getBlocks())
    if (const auto *Accesses = MSSA->getBlockAccesses(BB))
      for (const auto &MA : *Accesses) {
        (void)MA;
        ++AccessCapCount;
        if (AccessCapCount > LicmMssaNoAccForPromotionCap) {
          NoOfMemAccTooLarge = true;
          return;
        }
      }
}

/// Hoist expressions out of the specified loop. Note, alias info for inner
/// loop is not preserved so it is not a good idea to run LICM multiple
/// times on one loop.
bool LoopInvariantCodeMotion::runOnLoop(
    Loop *L, AAResults *AA, LoopInfo *LI, DominatorTree *DT,
    BlockFrequencyInfo *BFI, TargetLibraryInfo *TLI, TargetTransformInfo *TTI,
    ScalarEvolution *SE, MemorySSA *MSSA, OptimizationRemarkEmitter *ORE,
    bool LoopNestMode) {
  bool Changed = false;

  assert(L->isLCSSAForm(*DT) && "Loop is not in LCSSA form.");
  MSSA->ensureOptimizedUses();

  // If this loop has metadata indicating that LICM is not to be performed then
  // just exit.
  if (hasDisableLICMTransformsHint(L)) {
    return false;
  }

  // Don't sink stores from loops with coroutine suspend instructions.
  // LICM would sink instructions into the default destination of
  // the coroutine switch. The default destination of the switch is to
  // handle the case where the coroutine is suspended, by which point the
  // coroutine frame may have been destroyed. No instruction can be sunk there.
  // FIXME: This would unfortunately hurt the performance of coroutines, however
  // there is currently no general solution for this. Similar issues could also
  // potentially happen in other passes where instructions are being moved
  // across that edge.
  bool HasCoroSuspendInst = llvm::any_of(L->getBlocks(), [](BasicBlock *BB) {
    return llvm::any_of(*BB, [](Instruction &I) {
      IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I);
      return II && II->getIntrinsicID() == Intrinsic::coro_suspend;
    });
  });

  MemorySSAUpdater MSSAU(MSSA);
  SinkAndHoistLICMFlags Flags(LicmMssaOptCap, LicmMssaNoAccForPromotionCap,
                              /*IsSink=*/true, L, MSSA);

  // Get the preheader block to move instructions into...
  BasicBlock *Preheader = L->getLoopPreheader();

  // Compute loop safety information.
  ICFLoopSafetyInfo SafetyInfo;
  SafetyInfo.computeLoopSafetyInfo(L);

  // We want to visit all of the instructions in this loop... that are not parts
  // of our subloops (they have already had their invariants hoisted out of
  // their loop, into this loop, so there is no need to process the BODIES of
  // the subloops).
  //
  // Traverse the body of the loop in depth first order on the dominator tree so
  // that we are guaranteed to see definitions before we see uses.  This allows
  // us to sink instructions in one pass, without iteration.  After sinking
  // instructions, we perform another pass to hoist them out of the loop.
  if (L->hasDedicatedExits())
    Changed |= LoopNestMode
                   ? sinkRegionForLoopNest(DT->getNode(L->getHeader()), AA, LI,
                                           DT, BFI, TLI, TTI, L, MSSAU,
                                           &SafetyInfo, Flags, ORE)
                   : sinkRegion(DT->getNode(L->getHeader()), AA, LI, DT, BFI,
                                TLI, TTI, L, MSSAU, &SafetyInfo, Flags, ORE);
  Flags.setIsSink(false);
  if (Preheader)
    Changed |= hoistRegion(DT->getNode(L->getHeader()), AA, LI, DT, BFI, TLI, L,
                           MSSAU, SE, &SafetyInfo, Flags, ORE, LoopNestMode,
                           LicmAllowSpeculation);

  // Now that all loop invariants have been removed from the loop, promote any
  // memory references to scalars that we can.
  // Don't sink stores from loops without dedicated block exits. Exits
  // containing indirect branches are not transformed by loop simplify,
  // make sure we catch that. An additional load may be generated in the
  // preheader for SSA updater, so also avoid sinking when no preheader
  // is available.
  if (!DisablePromotion && Preheader && L->hasDedicatedExits() &&
      !Flags.tooManyMemoryAccesses() && !HasCoroSuspendInst) {
    // Figure out the loop exits and their insertion points
    SmallVector<BasicBlock *, 8> ExitBlocks;
    L->getUniqueExitBlocks(ExitBlocks);

    // We can't insert into a catchswitch.
    bool HasCatchSwitch = llvm::any_of(ExitBlocks, [](BasicBlock *Exit) {
      return isa<CatchSwitchInst>(Exit->getTerminator());
    });

    if (!HasCatchSwitch) {
      SmallVector<Instruction *, 8> InsertPts;
      SmallVector<MemoryAccess *, 8> MSSAInsertPts;
      InsertPts.reserve(ExitBlocks.size());
      MSSAInsertPts.reserve(ExitBlocks.size());
      for (BasicBlock *ExitBlock : ExitBlocks) {
        InsertPts.push_back(&*ExitBlock->getFirstInsertionPt());
        MSSAInsertPts.push_back(nullptr);
      }

      PredIteratorCache PIC;

      // Promoting one set of accesses may make the pointers for another set
      // loop invariant, so run this in a loop.
      bool Promoted = false;
      bool LocalPromoted;
      do {
        LocalPromoted = false;
        for (const SmallSetVector<Value *, 8> &PointerMustAliases :
             collectPromotionCandidates(MSSA, AA, L)) {
          LocalPromoted |= promoteLoopAccessesToScalars(
              PointerMustAliases, ExitBlocks, InsertPts, MSSAInsertPts, PIC, LI,
              DT, TLI, L, MSSAU, &SafetyInfo, ORE, LicmAllowSpeculation);
        }
        Promoted |= LocalPromoted;
      } while (LocalPromoted);

      // Once we have promoted values across the loop body we have to
      // recursively reform LCSSA as any nested loop may now have values defined
      // within the loop used in the outer loop.
      // FIXME: This is really heavy handed. It would be a bit better to use an
      // SSAUpdater strategy during promotion that was LCSSA aware and reformed
      // it as it went.
      if (Promoted)
        formLCSSARecursively(*L, *DT, LI, SE);

      Changed |= Promoted;
    }
  }

  // Check that neither this loop nor its parent have had LCSSA broken. LICM is
  // specifically moving instructions across the loop boundary and so it is
  // especially in need of basic functional correctness checking here.
  assert(L->isLCSSAForm(*DT) && "Loop not left in LCSSA form after LICM!");
  assert((L->isOutermost() || L->getParentLoop()->isLCSSAForm(*DT)) &&
         "Parent loop not left in LCSSA form after LICM!");

  if (VerifyMemorySSA)
    MSSA->verifyMemorySSA();

  if (Changed && SE)
    SE->forgetLoopDispositions(L);
  return Changed;
}

/// Walk the specified region of the CFG (defined by all blocks dominated by
/// the specified block, and that are in the current loop) in reverse depth
/// first order w.r.t the DominatorTree.  This allows us to visit uses before
/// definitions, allowing us to sink a loop body in one pass without iteration.
///
bool llvm::sinkRegion(DomTreeNode *N, AAResults *AA, LoopInfo *LI,
                      DominatorTree *DT, BlockFrequencyInfo *BFI,
                      TargetLibraryInfo *TLI, TargetTransformInfo *TTI,
                      Loop *CurLoop, MemorySSAUpdater &MSSAU,
                      ICFLoopSafetyInfo *SafetyInfo,
                      SinkAndHoistLICMFlags &Flags,
                      OptimizationRemarkEmitter *ORE, Loop *OutermostLoop) {

  // Verify inputs.
  assert(N != nullptr && AA != nullptr && LI != nullptr && DT != nullptr &&
         CurLoop != nullptr && SafetyInfo != nullptr &&
         "Unexpected input to sinkRegion.");

  // We want to visit children before parents. We will enqueue all the parents
  // before their children in the worklist and process the worklist in reverse
  // order.
  SmallVector<DomTreeNode *, 16> Worklist = collectChildrenInLoop(N, CurLoop);

  bool Changed = false;
  for (DomTreeNode *DTN : reverse(Worklist)) {
    BasicBlock *BB = DTN->getBlock();
    // Only need to process the contents of this block if it is not part of a
    // subloop (which would already have been processed).
    if (inSubLoop(BB, CurLoop, LI))
      continue;

    for (BasicBlock::iterator II = BB->end(); II != BB->begin();) {
      Instruction &I = *--II;

      // The instruction is not used in the loop if it is dead.  In this case,
      // we just delete it instead of sinking it.
      if (isInstructionTriviallyDead(&I, TLI)) {
        LLVM_DEBUG(dbgs() << "LICM deleting dead inst: " << I << '\n');
        salvageKnowledge(&I);
        salvageDebugInfo(I);
        ++II;
        eraseInstruction(I, *SafetyInfo, MSSAU);
        Changed = true;
        continue;
      }

      // Check to see if we can sink this instruction to the exit blocks
      // of the loop.  We can do this if the all users of the instruction are
      // outside of the loop.  In this case, it doesn't even matter if the
      // operands of the instruction are loop invariant.
      //
      bool FreeInLoop = false;
      bool LoopNestMode = OutermostLoop != nullptr;
      if (!I.mayHaveSideEffects() &&
          isNotUsedOrFreeInLoop(I, LoopNestMode ? OutermostLoop : CurLoop,
                                SafetyInfo, TTI, FreeInLoop, LoopNestMode) &&
          canSinkOrHoistInst(I, AA, DT, CurLoop, MSSAU, true, Flags, ORE)) {
        if (sink(I, LI, DT, BFI, CurLoop, SafetyInfo, MSSAU, ORE)) {
          if (!FreeInLoop) {
            ++II;
            salvageDebugInfo(I);
            eraseInstruction(I, *SafetyInfo, MSSAU);
          }
          Changed = true;
        }
      }
    }
  }
  if (VerifyMemorySSA)
    MSSAU.getMemorySSA()->verifyMemorySSA();
  return Changed;
}

bool llvm::sinkRegionForLoopNest(
    DomTreeNode *N, AAResults *AA, LoopInfo *LI, DominatorTree *DT,
    BlockFrequencyInfo *BFI, TargetLibraryInfo *TLI, TargetTransformInfo *TTI,
    Loop *CurLoop, MemorySSAUpdater &MSSAU, ICFLoopSafetyInfo *SafetyInfo,
    SinkAndHoistLICMFlags &Flags, OptimizationRemarkEmitter *ORE) {

  bool Changed = false;
  SmallPriorityWorklist<Loop *, 4> Worklist;
  Worklist.insert(CurLoop);
  appendLoopsToWorklist(*CurLoop, Worklist);
  while (!Worklist.empty()) {
    Loop *L = Worklist.pop_back_val();
    Changed |= sinkRegion(DT->getNode(L->getHeader()), AA, LI, DT, BFI, TLI,
                          TTI, L, MSSAU, SafetyInfo, Flags, ORE, CurLoop);
  }
  return Changed;
}

namespace {
// This is a helper class for hoistRegion to make it able to hoist control flow
// in order to be able to hoist phis. The way this works is that we initially
// start hoisting to the loop preheader, and when we see a loop invariant branch
// we make note of this. When we then come to hoist an instruction that's
// conditional on such a branch we duplicate the branch and the relevant control
// flow, then hoist the instruction into the block corresponding to its original
// block in the duplicated control flow.
class ControlFlowHoister {
private:
  // Information about the loop we are hoisting from
  LoopInfo *LI;
  DominatorTree *DT;
  Loop *CurLoop;
  MemorySSAUpdater &MSSAU;

  // A map of blocks in the loop to the block their instructions will be hoisted
  // to.
  DenseMap<BasicBlock *, BasicBlock *> HoistDestinationMap;

  // The branches that we can hoist, mapped to the block that marks a
  // convergence point of their control flow.
  DenseMap<BranchInst *, BasicBlock *> HoistableBranches;

public:
  ControlFlowHoister(LoopInfo *LI, DominatorTree *DT, Loop *CurLoop,
                     MemorySSAUpdater &MSSAU)
      : LI(LI), DT(DT), CurLoop(CurLoop), MSSAU(MSSAU) {}

  void registerPossiblyHoistableBranch(BranchInst *BI) {
    // We can only hoist conditional branches with loop invariant operands.
    if (!ControlFlowHoisting || !BI->isConditional() ||
        !CurLoop->hasLoopInvariantOperands(BI))
      return;

    // The branch destinations need to be in the loop, and we don't gain
    // anything by duplicating conditional branches with duplicate successors,
    // as it's essentially the same as an unconditional branch.
    BasicBlock *TrueDest = BI->getSuccessor(0);
    BasicBlock *FalseDest = BI->getSuccessor(1);
    if (!CurLoop->contains(TrueDest) || !CurLoop->contains(FalseDest) ||
        TrueDest == FalseDest)
      return;

    // We can hoist BI if one branch destination is the successor of the other,
    // or both have common successor which we check by seeing if the
    // intersection of their successors is non-empty.
    // TODO: This could be expanded to allowing branches where both ends
    // eventually converge to a single block.
    SmallPtrSet<BasicBlock *, 4> TrueDestSucc, FalseDestSucc;
    TrueDestSucc.insert(succ_begin(TrueDest), succ_end(TrueDest));
    FalseDestSucc.insert(succ_begin(FalseDest), succ_end(FalseDest));
    BasicBlock *CommonSucc = nullptr;
    if (TrueDestSucc.count(FalseDest)) {
      CommonSucc = FalseDest;
    } else if (FalseDestSucc.count(TrueDest)) {
      CommonSucc = TrueDest;
    } else {
      set_intersect(TrueDestSucc, FalseDestSucc);
      // If there's one common successor use that.
      if (TrueDestSucc.size() == 1)
        CommonSucc = *TrueDestSucc.begin();
      // If there's more than one pick whichever appears first in the block list
      // (we can't use the value returned by TrueDestSucc.begin() as it's
      // unpredicatable which element gets returned).
      else if (!TrueDestSucc.empty()) {
        Function *F = TrueDest->getParent();
        auto IsSucc = [&](BasicBlock &BB) { return TrueDestSucc.count(&BB); };
        auto It = llvm::find_if(*F, IsSucc);
        assert(It != F->end() && "Could not find successor in function");
        CommonSucc = &*It;
      }
    }
    // The common successor has to be dominated by the branch, as otherwise
    // there will be some other path to the successor that will not be
    // controlled by this branch so any phi we hoist would be controlled by the
    // wrong condition. This also takes care of avoiding hoisting of loop back
    // edges.
    // TODO: In some cases this could be relaxed if the successor is dominated
    // by another block that's been hoisted and we can guarantee that the
    // control flow has been replicated exactly.
    if (CommonSucc && DT->dominates(BI, CommonSucc))
      HoistableBranches[BI] = CommonSucc;
  }

  bool canHoistPHI(PHINode *PN) {
    // The phi must have loop invariant operands.
    if (!ControlFlowHoisting || !CurLoop->hasLoopInvariantOperands(PN))
      return false;
    // We can hoist phis if the block they are in is the target of hoistable
    // branches which cover all of the predecessors of the block.
    SmallPtrSet<BasicBlock *, 8> PredecessorBlocks;
    BasicBlock *BB = PN->getParent();
    for (BasicBlock *PredBB : predecessors(BB))
      PredecessorBlocks.insert(PredBB);
    // If we have less predecessor blocks than predecessors then the phi will
    // have more than one incoming value for the same block which we can't
    // handle.
    // TODO: This could be handled be erasing some of the duplicate incoming
    // values.
    if (PredecessorBlocks.size() != pred_size(BB))
      return false;
    for (auto &Pair : HoistableBranches) {
      if (Pair.second == BB) {
        // Which blocks are predecessors via this branch depends on if the
        // branch is triangle-like or diamond-like.
        if (Pair.first->getSuccessor(0) == BB) {
          PredecessorBlocks.erase(Pair.first->getParent());
          PredecessorBlocks.erase(Pair.first->getSuccessor(1));
        } else if (Pair.first->getSuccessor(1) == BB) {
          PredecessorBlocks.erase(Pair.first->getParent());
          PredecessorBlocks.erase(Pair.first->getSuccessor(0));
        } else {
          PredecessorBlocks.erase(Pair.first->getSuccessor(0));
          PredecessorBlocks.erase(Pair.first->getSuccessor(1));
        }
      }
    }
    // PredecessorBlocks will now be empty if for every predecessor of BB we
    // found a hoistable branch source.
    return PredecessorBlocks.empty();
  }

  BasicBlock *getOrCreateHoistedBlock(BasicBlock *BB) {
    if (!ControlFlowHoisting)
      return CurLoop->getLoopPreheader();
    // If BB has already been hoisted, return that
    if (HoistDestinationMap.count(BB))
      return HoistDestinationMap[BB];

    // Check if this block is conditional based on a pending branch
    auto HasBBAsSuccessor =
        [&](DenseMap<BranchInst *, BasicBlock *>::value_type &Pair) {
          return BB != Pair.second && (Pair.first->getSuccessor(0) == BB ||
                                       Pair.first->getSuccessor(1) == BB);
        };
    auto It = llvm::find_if(HoistableBranches, HasBBAsSuccessor);

    // If not involved in a pending branch, hoist to preheader
    BasicBlock *InitialPreheader = CurLoop->getLoopPreheader();
    if (It == HoistableBranches.end()) {
      LLVM_DEBUG(dbgs() << "LICM using "
                        << InitialPreheader->getNameOrAsOperand()
                        << " as hoist destination for "
                        << BB->getNameOrAsOperand() << "\n");
      HoistDestinationMap[BB] = InitialPreheader;
      return InitialPreheader;
    }
    BranchInst *BI = It->first;
    assert(std::find_if(++It, HoistableBranches.end(), HasBBAsSuccessor) ==
               HoistableBranches.end() &&
           "BB is expected to be the target of at most one branch");

    LLVMContext &C = BB->getContext();
    BasicBlock *TrueDest = BI->getSuccessor(0);
    BasicBlock *FalseDest = BI->getSuccessor(1);
    BasicBlock *CommonSucc = HoistableBranches[BI];
    BasicBlock *HoistTarget = getOrCreateHoistedBlock(BI->getParent());

    // Create hoisted versions of blocks that currently don't have them
    auto CreateHoistedBlock = [&](BasicBlock *Orig) {
      if (HoistDestinationMap.count(Orig))
        return HoistDestinationMap[Orig];
      BasicBlock *New =
          BasicBlock::Create(C, Orig->getName() + ".licm", Orig->getParent());
      HoistDestinationMap[Orig] = New;
      DT->addNewBlock(New, HoistTarget);
      if (CurLoop->getParentLoop())
        CurLoop->getParentLoop()->addBasicBlockToLoop(New, *LI);
      ++NumCreatedBlocks;
      LLVM_DEBUG(dbgs() << "LICM created " << New->getName()
                        << " as hoist destination for " << Orig->getName()
                        << "\n");
      return New;
    };
    BasicBlock *HoistTrueDest = CreateHoistedBlock(TrueDest);
    BasicBlock *HoistFalseDest = CreateHoistedBlock(FalseDest);
    BasicBlock *HoistCommonSucc = CreateHoistedBlock(CommonSucc);

    // Link up these blocks with branches.
    if (!HoistCommonSucc->getTerminator()) {
      // The new common successor we've generated will branch to whatever that
      // hoist target branched to.
      BasicBlock *TargetSucc = HoistTarget->getSingleSuccessor();
      assert(TargetSucc && "Expected hoist target to have a single successor");
      HoistCommonSucc->moveBefore(TargetSucc);
      BranchInst::Create(TargetSucc, HoistCommonSucc);
    }
    if (!HoistTrueDest->getTerminator()) {
      HoistTrueDest->moveBefore(HoistCommonSucc);
      BranchInst::Create(HoistCommonSucc, HoistTrueDest);
    }
    if (!HoistFalseDest->getTerminator()) {
      HoistFalseDest->moveBefore(HoistCommonSucc);
      BranchInst::Create(HoistCommonSucc, HoistFalseDest);
    }

    // If BI is being cloned to what was originally the preheader then
    // HoistCommonSucc will now be the new preheader.
    if (HoistTarget == InitialPreheader) {
      // Phis in the loop header now need to use the new preheader.
      InitialPreheader->replaceSuccessorsPhiUsesWith(HoistCommonSucc);
      MSSAU.wireOldPredecessorsToNewImmediatePredecessor(
          HoistTarget->getSingleSuccessor(), HoistCommonSucc, {HoistTarget});
      // The new preheader dominates the loop header.
      DomTreeNode *PreheaderNode = DT->getNode(HoistCommonSucc);
      DomTreeNode *HeaderNode = DT->getNode(CurLoop->getHeader());
      DT->changeImmediateDominator(HeaderNode, PreheaderNode);
      // The preheader hoist destination is now the new preheader, with the
      // exception of the hoist destination of this branch.
      for (auto &Pair : HoistDestinationMap)
        if (Pair.second == InitialPreheader && Pair.first != BI->getParent())
          Pair.second = HoistCommonSucc;
    }

    // Now finally clone BI.
    ReplaceInstWithInst(
        HoistTarget->getTerminator(),
        BranchInst::Create(HoistTrueDest, HoistFalseDest, BI->getCondition()));
    ++NumClonedBranches;

    assert(CurLoop->getLoopPreheader() &&
           "Hoisting blocks should not have destroyed preheader");
    return HoistDestinationMap[BB];
  }
};
} // namespace

/// Walk the specified region of the CFG (defined by all blocks dominated by
/// the specified block, and that are in the current loop) in depth first
/// order w.r.t the DominatorTree.  This allows us to visit definitions before
/// uses, allowing us to hoist a loop body in one pass without iteration.
///
bool llvm::hoistRegion(DomTreeNode *N, AAResults *AA, LoopInfo *LI,
                       DominatorTree *DT, BlockFrequencyInfo *BFI,
                       TargetLibraryInfo *TLI, Loop *CurLoop,
                       MemorySSAUpdater &MSSAU, ScalarEvolution *SE,
                       ICFLoopSafetyInfo *SafetyInfo,
                       SinkAndHoistLICMFlags &Flags,
                       OptimizationRemarkEmitter *ORE, bool LoopNestMode,
                       bool AllowSpeculation) {
  // Verify inputs.
  assert(N != nullptr && AA != nullptr && LI != nullptr && DT != nullptr &&
         CurLoop != nullptr && SafetyInfo != nullptr &&
         "Unexpected input to hoistRegion.");

  ControlFlowHoister CFH(LI, DT, CurLoop, MSSAU);

  // Keep track of instructions that have been hoisted, as they may need to be
  // re-hoisted if they end up not dominating all of their uses.
  SmallVector<Instruction *, 16> HoistedInstructions;

  // For PHI hoisting to work we need to hoist blocks before their successors.
  // We can do this by iterating through the blocks in the loop in reverse
  // post-order.
  LoopBlocksRPO Worklist(CurLoop);
  Worklist.perform(LI);
  bool Changed = false;
  for (BasicBlock *BB : Worklist) {
    // Only need to process the contents of this block if it is not part of a
    // subloop (which would already have been processed).
    if (!LoopNestMode && inSubLoop(BB, CurLoop, LI))
      continue;

    for (Instruction &I : llvm::make_early_inc_range(*BB)) {
      // Try constant folding this instruction.  If all the operands are
      // constants, it is technically hoistable, but it would be better to
      // just fold it.
      if (Constant *C = ConstantFoldInstruction(
              &I, I.getModule()->getDataLayout(), TLI)) {
        LLVM_DEBUG(dbgs() << "LICM folding inst: " << I << "  --> " << *C
                          << '\n');
        // FIXME MSSA: Such replacements may make accesses unoptimized (D51960).
        I.replaceAllUsesWith(C);
        if (isInstructionTriviallyDead(&I, TLI))
          eraseInstruction(I, *SafetyInfo, MSSAU);
        Changed = true;
        continue;
      }

      // Try hoisting the instruction out to the preheader.  We can only do
      // this if all of the operands of the instruction are loop invariant and
      // if it is safe to hoist the instruction. We also check block frequency
      // to make sure instruction only gets hoisted into colder blocks.
      // TODO: It may be safe to hoist if we are hoisting to a conditional block
      // and we have accurately duplicated the control flow from the loop header
      // to that block.
      if (CurLoop->hasLoopInvariantOperands(&I) &&
          canSinkOrHoistInst(I, AA, DT, CurLoop, MSSAU, true, Flags, ORE) &&
          isSafeToExecuteUnconditionally(
              I, DT, TLI, CurLoop, SafetyInfo, ORE,
              CurLoop->getLoopPreheader()->getTerminator(), AllowSpeculation)) {
        hoist(I, DT, CurLoop, CFH.getOrCreateHoistedBlock(BB), SafetyInfo,
              MSSAU, SE, ORE);
        HoistedInstructions.push_back(&I);
        Changed = true;
        continue;
      }

      // Attempt to remove floating point division out of the loop by
      // converting it to a reciprocal multiplication.
      if (I.getOpcode() == Instruction::FDiv && I.hasAllowReciprocal() &&
          CurLoop->isLoopInvariant(I.getOperand(1))) {
        auto Divisor = I.getOperand(1);
        auto One = llvm::ConstantFP::get(Divisor->getType(), 1.0);
        auto ReciprocalDivisor = BinaryOperator::CreateFDiv(One, Divisor);
        ReciprocalDivisor->setFastMathFlags(I.getFastMathFlags());
        SafetyInfo->insertInstructionTo(ReciprocalDivisor, I.getParent());
        ReciprocalDivisor->insertBefore(&I);

        auto Product =
            BinaryOperator::CreateFMul(I.getOperand(0), ReciprocalDivisor);
        Product->setFastMathFlags(I.getFastMathFlags());
        SafetyInfo->insertInstructionTo(Product, I.getParent());
        Product->insertAfter(&I);
        I.replaceAllUsesWith(Product);
        eraseInstruction(I, *SafetyInfo, MSSAU);

        hoist(*ReciprocalDivisor, DT, CurLoop, CFH.getOrCreateHoistedBlock(BB),
              SafetyInfo, MSSAU, SE, ORE);
        HoistedInstructions.push_back(ReciprocalDivisor);
        Changed = true;
        continue;
      }

      auto IsInvariantStart = [&](Instruction &I) {
        using namespace PatternMatch;
        return I.use_empty() &&
               match(&I, m_Intrinsic<Intrinsic::invariant_start>());
      };
      auto MustExecuteWithoutWritesBefore = [&](Instruction &I) {
        return SafetyInfo->isGuaranteedToExecute(I, DT, CurLoop) &&
               SafetyInfo->doesNotWriteMemoryBefore(I, CurLoop);
      };
      if ((IsInvariantStart(I) || isGuard(&I)) &&
          CurLoop->hasLoopInvariantOperands(&I) &&
          MustExecuteWithoutWritesBefore(I)) {
        hoist(I, DT, CurLoop, CFH.getOrCreateHoistedBlock(BB), SafetyInfo,
              MSSAU, SE, ORE);
        HoistedInstructions.push_back(&I);
        Changed = true;
        continue;
      }

      if (PHINode *PN = dyn_cast<PHINode>(&I)) {
        if (CFH.canHoistPHI(PN)) {
          // Redirect incoming blocks first to ensure that we create hoisted
          // versions of those blocks before we hoist the phi.
          for (unsigned int i = 0; i < PN->getNumIncomingValues(); ++i)
            PN->setIncomingBlock(
                i, CFH.getOrCreateHoistedBlock(PN->getIncomingBlock(i)));
          hoist(*PN, DT, CurLoop, CFH.getOrCreateHoistedBlock(BB), SafetyInfo,
                MSSAU, SE, ORE);
          assert(DT->dominates(PN, BB) && "Conditional PHIs not expected");
          Changed = true;
          continue;
        }
      }

      // Remember possibly hoistable branches so we can actually hoist them
      // later if needed.
      if (BranchInst *BI = dyn_cast<BranchInst>(&I))
        CFH.registerPossiblyHoistableBranch(BI);
    }
  }

  // If we hoisted instructions to a conditional block they may not dominate
  // their uses that weren't hoisted (such as phis where some operands are not
  // loop invariant). If so make them unconditional by moving them to their
  // immediate dominator. We iterate through the instructions in reverse order
  // which ensures that when we rehoist an instruction we rehoist its operands,
  // and also keep track of where in the block we are rehoisting to to make sure
  // that we rehoist instructions before the instructions that use them.
  Instruction *HoistPoint = nullptr;
  if (ControlFlowHoisting) {
    for (Instruction *I : reverse(HoistedInstructions)) {
      if (!llvm::all_of(I->uses(),
                        [&](Use &U) { return DT->dominates(I, U); })) {
        BasicBlock *Dominator =
            DT->getNode(I->getParent())->getIDom()->getBlock();
        if (!HoistPoint || !DT->dominates(HoistPoint->getParent(), Dominator)) {
          if (HoistPoint)
            assert(DT->dominates(Dominator, HoistPoint->getParent()) &&
                   "New hoist point expected to dominate old hoist point");
          HoistPoint = Dominator->getTerminator();
        }
        LLVM_DEBUG(dbgs() << "LICM rehoisting to "
                          << HoistPoint->getParent()->getNameOrAsOperand()
                          << ": " << *I << "\n");
        moveInstructionBefore(*I, *HoistPoint, *SafetyInfo, MSSAU, SE);
        HoistPoint = I;
        Changed = true;
      }
    }
  }
  if (VerifyMemorySSA)
    MSSAU.getMemorySSA()->verifyMemorySSA();

    // Now that we've finished hoisting make sure that LI and DT are still
    // valid.
#ifdef EXPENSIVE_CHECKS
  if (Changed) {
    assert(DT->verify(DominatorTree::VerificationLevel::Fast) &&
           "Dominator tree verification failed");
    LI->verify(*DT);
  }
#endif

  return Changed;
}

// Return true if LI is invariant within scope of the loop. LI is invariant if
// CurLoop is dominated by an invariant.start representing the same memory
// location and size as the memory location LI loads from, and also the
// invariant.start has no uses.
static bool isLoadInvariantInLoop(LoadInst *LI, DominatorTree *DT,
                                  Loop *CurLoop) {
  Value *Addr = LI->getOperand(0);
  const DataLayout &DL = LI->getModule()->getDataLayout();
  const TypeSize LocSizeInBits = DL.getTypeSizeInBits(LI->getType());

  // It is not currently possible for clang to generate an invariant.start
  // intrinsic with scalable vector types because we don't support thread local
  // sizeless types and we don't permit sizeless types in structs or classes.
  // Furthermore, even if support is added for this in future the intrinsic
  // itself is defined to have a size of -1 for variable sized objects. This
  // makes it impossible to verify if the intrinsic envelops our region of
  // interest. For example, both <vscale x 32 x i8> and <vscale x 16 x i8>
  // types would have a -1 parameter, but the former is clearly double the size
  // of the latter.
  if (LocSizeInBits.isScalable())
    return false;

  // if the type is i8 addrspace(x)*, we know this is the type of
  // llvm.invariant.start operand
  auto *PtrInt8Ty = PointerType::get(Type::getInt8Ty(LI->getContext()),
                                     LI->getPointerAddressSpace());
  unsigned BitcastsVisited = 0;
  // Look through bitcasts until we reach the i8* type (this is invariant.start
  // operand type).
  while (Addr->getType() != PtrInt8Ty) {
    auto *BC = dyn_cast<BitCastInst>(Addr);
    // Avoid traversing high number of bitcast uses.
    if (++BitcastsVisited > MaxNumUsesTraversed || !BC)
      return false;
    Addr = BC->getOperand(0);
  }
  // If we've ended up at a global/constant, bail. We shouldn't be looking at
  // uselists for non-local Values in a loop pass.
  if (isa<Constant>(Addr))
    return false;

  unsigned UsesVisited = 0;
  // Traverse all uses of the load operand value, to see if invariant.start is
  // one of the uses, and whether it dominates the load instruction.
  for (auto *U : Addr->users()) {
    // Avoid traversing for Load operand with high number of users.
    if (++UsesVisited > MaxNumUsesTraversed)
      return false;
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(U);
    // If there are escaping uses of invariant.start instruction, the load maybe
    // non-invariant.
    if (!II || II->getIntrinsicID() != Intrinsic::invariant_start ||
        !II->use_empty())
      continue;
    ConstantInt *InvariantSize = cast<ConstantInt>(II->getArgOperand(0));
    // The intrinsic supports having a -1 argument for variable sized objects
    // so we should check for that here.
    if (InvariantSize->isNegative())
      continue;
    uint64_t InvariantSizeInBits = InvariantSize->getSExtValue() * 8;
    // Confirm the invariant.start location size contains the load operand size
    // in bits. Also, the invariant.start should dominate the load, and we
    // should not hoist the load out of a loop that contains this dominating
    // invariant.start.
    if (LocSizeInBits.getFixedSize() <= InvariantSizeInBits &&
        DT->properlyDominates(II->getParent(), CurLoop->getHeader()))
      return true;
  }

  return false;
}

namespace {
/// Return true if-and-only-if we know how to (mechanically) both hoist and
/// sink a given instruction out of a loop.  Does not address legality
/// concerns such as aliasing or speculation safety.
bool isHoistableAndSinkableInst(Instruction &I) {
  // Only these instructions are hoistable/sinkable.
  return (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<CallInst>(I) ||
          isa<FenceInst>(I) || isa<CastInst>(I) || isa<UnaryOperator>(I) ||
          isa<BinaryOperator>(I) || isa<SelectInst>(I) ||
          isa<GetElementPtrInst>(I) || isa<CmpInst>(I) ||
          isa<InsertElementInst>(I) || isa<ExtractElementInst>(I) ||
          isa<ShuffleVectorInst>(I) || isa<ExtractValueInst>(I) ||
          isa<InsertValueInst>(I) || isa<FreezeInst>(I));
}
/// Return true if MSSA knows there are no MemoryDefs in the loop.
bool isReadOnly(const MemorySSAUpdater &MSSAU, const Loop *L) {
  for (auto *BB : L->getBlocks())
    if (MSSAU.getMemorySSA()->getBlockDefs(BB))
      return false;
  return true;
}

/// Return true if I is the only Instruction with a MemoryAccess in L.
bool isOnlyMemoryAccess(const Instruction *I, const Loop *L,
                        const MemorySSAUpdater &MSSAU) {
  for (auto *BB : L->getBlocks())
    if (auto *Accs = MSSAU.getMemorySSA()->getBlockAccesses(BB)) {
      int NotAPhi = 0;
      for (const auto &Acc : *Accs) {
        if (isa<MemoryPhi>(&Acc))
          continue;
        const auto *MUD = cast<MemoryUseOrDef>(&Acc);
        if (MUD->getMemoryInst() != I || NotAPhi++ == 1)
          return false;
      }
    }
  return true;
}
}

bool llvm::canSinkOrHoistInst(Instruction &I, AAResults *AA, DominatorTree *DT,
                              Loop *CurLoop, MemorySSAUpdater &MSSAU,
                              bool TargetExecutesOncePerLoop,
                              SinkAndHoistLICMFlags &Flags,
                              OptimizationRemarkEmitter *ORE) {
  // If we don't understand the instruction, bail early.
  if (!isHoistableAndSinkableInst(I))
    return false;

  MemorySSA *MSSA = MSSAU.getMemorySSA();
  // Loads have extra constraints we have to verify before we can hoist them.
  if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
    if (!LI->isUnordered())
      return false; // Don't sink/hoist volatile or ordered atomic loads!

    // Loads from constant memory are always safe to move, even if they end up
    // in the same alias set as something that ends up being modified.
    if (AA->pointsToConstantMemory(LI->getOperand(0)))
      return true;
    if (LI->hasMetadata(LLVMContext::MD_invariant_load))
      return true;

    if (LI->isAtomic() && !TargetExecutesOncePerLoop)
      return false; // Don't risk duplicating unordered loads

    // This checks for an invariant.start dominating the load.
    if (isLoadInvariantInLoop(LI, DT, CurLoop))
      return true;

    bool Invalidated = pointerInvalidatedByLoop(
        MSSA, cast<MemoryUse>(MSSA->getMemoryAccess(LI)), CurLoop, I, Flags);
    // Check loop-invariant address because this may also be a sinkable load
    // whose address is not necessarily loop-invariant.
    if (ORE && Invalidated && CurLoop->isLoopInvariant(LI->getPointerOperand()))
      ORE->emit([&]() {
        return OptimizationRemarkMissed(
                   DEBUG_TYPE, "LoadWithLoopInvariantAddressInvalidated", LI)
               << "failed to move load with loop-invariant address "
                  "because the loop may invalidate its value";
      });

    return !Invalidated;
  } else if (CallInst *CI = dyn_cast<CallInst>(&I)) {
    // Don't sink or hoist dbg info; it's legal, but not useful.
    if (isa<DbgInfoIntrinsic>(I))
      return false;

    // Don't sink calls which can throw.
    if (CI->mayThrow())
      return false;

    // Convergent attribute has been used on operations that involve
    // inter-thread communication which results are implicitly affected by the
    // enclosing control flows. It is not safe to hoist or sink such operations
    // across control flow.
    if (CI->isConvergent())
      return false;

    using namespace PatternMatch;
    if (match(CI, m_Intrinsic<Intrinsic::assume>()))
      // Assumes don't actually alias anything or throw
      return true;

    if (match(CI, m_Intrinsic<Intrinsic::experimental_widenable_condition>()))
      // Widenable conditions don't actually alias anything or throw
      return true;

    // Handle simple cases by querying alias analysis.
    FunctionModRefBehavior Behavior = AA->getModRefBehavior(CI);
    if (Behavior == FMRB_DoesNotAccessMemory)
      return true;
    if (AAResults::onlyReadsMemory(Behavior)) {
      // A readonly argmemonly function only reads from memory pointed to by
      // it's arguments with arbitrary offsets.  If we can prove there are no
      // writes to this memory in the loop, we can hoist or sink.
      if (AAResults::onlyAccessesArgPointees(Behavior)) {
        // TODO: expand to writeable arguments
        for (Value *Op : CI->args())
          if (Op->getType()->isPointerTy() &&
              pointerInvalidatedByLoop(
                  MSSA, cast<MemoryUse>(MSSA->getMemoryAccess(CI)), CurLoop, I,
                  Flags))
            return false;
        return true;
      }

      // If this call only reads from memory and there are no writes to memory
      // in the loop, we can hoist or sink the call as appropriate.
      if (isReadOnly(MSSAU, CurLoop))
        return true;
    }

    // FIXME: This should use mod/ref information to see if we can hoist or
    // sink the call.

    return false;
  } else if (auto *FI = dyn_cast<FenceInst>(&I)) {
    // Fences alias (most) everything to provide ordering.  For the moment,
    // just give up if there are any other memory operations in the loop.
    return isOnlyMemoryAccess(FI, CurLoop, MSSAU);
  } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
    if (!SI->isUnordered())
      return false; // Don't sink/hoist volatile or ordered atomic store!

    // We can only hoist a store that we can prove writes a value which is not
    // read or overwritten within the loop.  For those cases, we fallback to
    // load store promotion instead.  TODO: We can extend this to cases where
    // there is exactly one write to the location and that write dominates an
    // arbitrary number of reads in the loop.
    if (isOnlyMemoryAccess(SI, CurLoop, MSSAU))
      return true;
    // If there are more accesses than the Promotion cap or no "quota" to
    // check clobber, then give up as we're not walking a list that long.
    if (Flags.tooManyMemoryAccesses() || Flags.tooManyClobberingCalls())
      return false;
    // If there are interfering Uses (i.e. their defining access is in the
    // loop), or ordered loads (stored as Defs!), don't move this store.
    // Could do better here, but this is conservatively correct.
    // TODO: Cache set of Uses on the first walk in runOnLoop, update when
    // moving accesses. Can also extend to dominating uses.
    auto *SIMD = MSSA->getMemoryAccess(SI);
    for (auto *BB : CurLoop->getBlocks())
      if (auto *Accesses = MSSA->getBlockAccesses(BB)) {
        for (const auto &MA : *Accesses)
          if (const auto *MU = dyn_cast<MemoryUse>(&MA)) {
            auto *MD = MU->getDefiningAccess();
            if (!MSSA->isLiveOnEntryDef(MD) &&
                CurLoop->contains(MD->getBlock()))
              return false;
            // Disable hoisting past potentially interfering loads. Optimized
            // Uses may point to an access outside the loop, as getClobbering
            // checks the previous iteration when walking the backedge.
            // FIXME: More precise: no Uses that alias SI.
            if (!Flags.getIsSink() && !MSSA->dominates(SIMD, MU))
              return false;
          } else if (const auto *MD = dyn_cast<MemoryDef>(&MA)) {
            if (auto *LI = dyn_cast<LoadInst>(MD->getMemoryInst())) {
              (void)LI; // Silence warning.
              assert(!LI->isUnordered() && "Expected unordered load");
              return false;
            }
            // Any call, while it may not be clobbering SI, it may be a use.
            if (auto *CI = dyn_cast<CallInst>(MD->getMemoryInst())) {
              // Check if the call may read from the memory location written
              // to by SI. Check CI's attributes and arguments; the number of
              // such checks performed is limited above by NoOfMemAccTooLarge.
              ModRefInfo MRI = AA->getModRefInfo(CI, MemoryLocation::get(SI));
              if (isModOrRefSet(MRI))
                return false;
            }
          }
      }
    auto *Source = MSSA->getSkipSelfWalker()->getClobberingMemoryAccess(SI);
    Flags.incrementClobberingCalls();
    // If there are no clobbering Defs in the loop, store is safe to hoist.
    return MSSA->isLiveOnEntryDef(Source) ||
           !CurLoop->contains(Source->getBlock());
  }

  assert(!I.mayReadOrWriteMemory() && "unhandled aliasing");

  // We've established mechanical ability and aliasing, it's up to the caller
  // to check fault safety
  return true;
}

/// Returns true if a PHINode is a trivially replaceable with an
/// Instruction.
/// This is true when all incoming values are that instruction.
/// This pattern occurs most often with LCSSA PHI nodes.
///
static bool isTriviallyReplaceablePHI(const PHINode &PN, const Instruction &I) {
  for (const Value *IncValue : PN.incoming_values())
    if (IncValue != &I)
      return false;

  return true;
}

/// Return true if the instruction is free in the loop.
static bool isFreeInLoop(const Instruction &I, const Loop *CurLoop,
                         const TargetTransformInfo *TTI) {
  InstructionCost CostI =
      TTI->getInstructionCost(&I, TargetTransformInfo::TCK_SizeAndLatency);

  if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    if (CostI != TargetTransformInfo::TCC_Free)
      return false;
    // For a GEP, we cannot simply use getInstructionCost because currently
    // it optimistically assumes that a GEP will fold into addressing mode
    // regardless of its users.
    const BasicBlock *BB = GEP->getParent();
    for (const User *U : GEP->users()) {
      const Instruction *UI = cast<Instruction>(U);
      if (CurLoop->contains(UI) &&
          (BB != UI->getParent() ||
           (!isa<StoreInst>(UI) && !isa<LoadInst>(UI))))
        return false;
    }
    return true;
  }

  return CostI == TargetTransformInfo::TCC_Free;
}

/// Return true if the only users of this instruction are outside of
/// the loop. If this is true, we can sink the instruction to the exit
/// blocks of the loop.
///
/// We also return true if the instruction could be folded away in lowering.
/// (e.g.,  a GEP can be folded into a load as an addressing mode in the loop).
static bool isNotUsedOrFreeInLoop(const Instruction &I, const Loop *CurLoop,
                                  const LoopSafetyInfo *SafetyInfo,
                                  TargetTransformInfo *TTI, bool &FreeInLoop,
                                  bool LoopNestMode) {
  const auto &BlockColors = SafetyInfo->getBlockColors();
  bool IsFree = isFreeInLoop(I, CurLoop, TTI);
  for (const User *U : I.users()) {
    const Instruction *UI = cast<Instruction>(U);
    if (const PHINode *PN = dyn_cast<PHINode>(UI)) {
      const BasicBlock *BB = PN->getParent();
      // We cannot sink uses in catchswitches.
      if (isa<CatchSwitchInst>(BB->getTerminator()))
        return false;

      // We need to sink a callsite to a unique funclet.  Avoid sinking if the
      // phi use is too muddled.
      if (isa<CallInst>(I))
        if (!BlockColors.empty() &&
            BlockColors.find(const_cast<BasicBlock *>(BB))->second.size() != 1)
          return false;

      if (LoopNestMode) {
        while (isa<PHINode>(UI) && UI->hasOneUser() &&
               UI->getNumOperands() == 1) {
          if (!CurLoop->contains(UI))
            break;
          UI = cast<Instruction>(UI->user_back());
        }
      }
    }

    if (CurLoop->contains(UI)) {
      if (IsFree) {
        FreeInLoop = true;
        continue;
      }
      return false;
    }
  }
  return true;
}

static Instruction *cloneInstructionInExitBlock(
    Instruction &I, BasicBlock &ExitBlock, PHINode &PN, const LoopInfo *LI,
    const LoopSafetyInfo *SafetyInfo, MemorySSAUpdater &MSSAU) {
  Instruction *New;
  if (auto *CI = dyn_cast<CallInst>(&I)) {
    const auto &BlockColors = SafetyInfo->getBlockColors();

    // Sinking call-sites need to be handled differently from other
    // instructions.  The cloned call-site needs a funclet bundle operand
    // appropriate for its location in the CFG.
    SmallVector<OperandBundleDef, 1> OpBundles;
    for (unsigned BundleIdx = 0, BundleEnd = CI->getNumOperandBundles();
         BundleIdx != BundleEnd; ++BundleIdx) {
      OperandBundleUse Bundle = CI->getOperandBundleAt(BundleIdx);
      if (Bundle.getTagID() == LLVMContext::OB_funclet)
        continue;

      OpBundles.emplace_back(Bundle);
    }

    if (!BlockColors.empty()) {
      const ColorVector &CV = BlockColors.find(&ExitBlock)->second;
      assert(CV.size() == 1 && "non-unique color for exit block!");
      BasicBlock *BBColor = CV.front();
      Instruction *EHPad = BBColor->getFirstNonPHI();
      if (EHPad->isEHPad())
        OpBundles.emplace_back("funclet", EHPad);
    }

    New = CallInst::Create(CI, OpBundles);
  } else {
    New = I.clone();
  }

  ExitBlock.getInstList().insert(ExitBlock.getFirstInsertionPt(), New);
  if (!I.getName().empty())
    New->setName(I.getName() + ".le");

  if (MSSAU.getMemorySSA()->getMemoryAccess(&I)) {
    // Create a new MemoryAccess and let MemorySSA set its defining access.
    MemoryAccess *NewMemAcc = MSSAU.createMemoryAccessInBB(
        New, nullptr, New->getParent(), MemorySSA::Beginning);
    if (NewMemAcc) {
      if (auto *MemDef = dyn_cast<MemoryDef>(NewMemAcc))
        MSSAU.insertDef(MemDef, /*RenameUses=*/true);
      else {
        auto *MemUse = cast<MemoryUse>(NewMemAcc);
        MSSAU.insertUse(MemUse, /*RenameUses=*/true);
      }
    }
  }

  // Build LCSSA PHI nodes for any in-loop operands (if legal).  Note that
  // this is particularly cheap because we can rip off the PHI node that we're
  // replacing for the number and blocks of the predecessors.
  // OPT: If this shows up in a profile, we can instead finish sinking all
  // invariant instructions, and then walk their operands to re-establish
  // LCSSA. That will eliminate creating PHI nodes just to nuke them when
  // sinking bottom-up.
  for (Use &Op : New->operands())
    if (LI->wouldBeOutOfLoopUseRequiringLCSSA(Op.get(), PN.getParent())) {
      auto *OInst = cast<Instruction>(Op.get());
      PHINode *OpPN =
        PHINode::Create(OInst->getType(), PN.getNumIncomingValues(),
                        OInst->getName() + ".lcssa", &ExitBlock.front());
      for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i)
        OpPN->addIncoming(OInst, PN.getIncomingBlock(i));
      Op = OpPN;
    }
  return New;
}

static void eraseInstruction(Instruction &I, ICFLoopSafetyInfo &SafetyInfo,
                             MemorySSAUpdater &MSSAU) {
  MSSAU.removeMemoryAccess(&I);
  SafetyInfo.removeInstruction(&I);
  I.eraseFromParent();
}

static void moveInstructionBefore(Instruction &I, Instruction &Dest,
                                  ICFLoopSafetyInfo &SafetyInfo,
                                  MemorySSAUpdater &MSSAU,
                                  ScalarEvolution *SE) {
  SafetyInfo.removeInstruction(&I);
  SafetyInfo.insertInstructionTo(&I, Dest.getParent());
  I.moveBefore(&Dest);
  if (MemoryUseOrDef *OldMemAcc = cast_or_null<MemoryUseOrDef>(
          MSSAU.getMemorySSA()->getMemoryAccess(&I)))
    MSSAU.moveToPlace(OldMemAcc, Dest.getParent(), MemorySSA::BeforeTerminator);
  if (SE)
    SE->forgetValue(&I);
}

static Instruction *sinkThroughTriviallyReplaceablePHI(
    PHINode *TPN, Instruction *I, LoopInfo *LI,
    SmallDenseMap<BasicBlock *, Instruction *, 32> &SunkCopies,
    const LoopSafetyInfo *SafetyInfo, const Loop *CurLoop,
    MemorySSAUpdater &MSSAU) {
  assert(isTriviallyReplaceablePHI(*TPN, *I) &&
         "Expect only trivially replaceable PHI");
  BasicBlock *ExitBlock = TPN->getParent();
  Instruction *New;
  auto It = SunkCopies.find(ExitBlock);
  if (It != SunkCopies.end())
    New = It->second;
  else
    New = SunkCopies[ExitBlock] = cloneInstructionInExitBlock(
        *I, *ExitBlock, *TPN, LI, SafetyInfo, MSSAU);
  return New;
}

static bool canSplitPredecessors(PHINode *PN, LoopSafetyInfo *SafetyInfo) {
  BasicBlock *BB = PN->getParent();
  if (!BB->canSplitPredecessors())
    return false;
  // It's not impossible to split EHPad blocks, but if BlockColors already exist
  // it require updating BlockColors for all offspring blocks accordingly. By
  // skipping such corner case, we can make updating BlockColors after splitting
  // predecessor fairly simple.
  if (!SafetyInfo->getBlockColors().empty() && BB->getFirstNonPHI()->isEHPad())
    return false;
  for (BasicBlock *BBPred : predecessors(BB)) {
    if (isa<IndirectBrInst>(BBPred->getTerminator()))
      return false;
  }
  return true;
}

static void splitPredecessorsOfLoopExit(PHINode *PN, DominatorTree *DT,
                                        LoopInfo *LI, const Loop *CurLoop,
                                        LoopSafetyInfo *SafetyInfo,
                                        MemorySSAUpdater *MSSAU) {
#ifndef NDEBUG
  SmallVector<BasicBlock *, 32> ExitBlocks;
  CurLoop->getUniqueExitBlocks(ExitBlocks);
  SmallPtrSet<BasicBlock *, 32> ExitBlockSet(ExitBlocks.begin(),
                                             ExitBlocks.end());
#endif
  BasicBlock *ExitBB = PN->getParent();
  assert(ExitBlockSet.count(ExitBB) && "Expect the PHI is in an exit block.");

  // Split predecessors of the loop exit to make instructions in the loop are
  // exposed to exit blocks through trivially replaceable PHIs while keeping the
  // loop in the canonical form where each predecessor of each exit block should
  // be contained within the loop. For example, this will convert the loop below
  // from
  //
  // LB1:
  //   %v1 =
  //   br %LE, %LB2
  // LB2:
  //   %v2 =
  //   br %LE, %LB1
  // LE:
  //   %p = phi [%v1, %LB1], [%v2, %LB2] <-- non-trivially replaceable
  //
  // to
  //
  // LB1:
  //   %v1 =
  //   br %LE.split, %LB2
  // LB2:
  //   %v2 =
  //   br %LE.split2, %LB1
  // LE.split:
  //   %p1 = phi [%v1, %LB1]  <-- trivially replaceable
  //   br %LE
  // LE.split2:
  //   %p2 = phi [%v2, %LB2]  <-- trivially replaceable
  //   br %LE
  // LE:
  //   %p = phi [%p1, %LE.split], [%p2, %LE.split2]
  //
  const auto &BlockColors = SafetyInfo->getBlockColors();
  SmallSetVector<BasicBlock *, 8> PredBBs(pred_begin(ExitBB), pred_end(ExitBB));
  while (!PredBBs.empty()) {
    BasicBlock *PredBB = *PredBBs.begin();
    assert(CurLoop->contains(PredBB) &&
           "Expect all predecessors are in the loop");
    if (PN->getBasicBlockIndex(PredBB) >= 0) {
      BasicBlock *NewPred = SplitBlockPredecessors(
          ExitBB, PredBB, ".split.loop.exit", DT, LI, MSSAU, true);
      // Since we do not allow splitting EH-block with BlockColors in
      // canSplitPredecessors(), we can simply assign predecessor's color to
      // the new block.
      if (!BlockColors.empty())
        // Grab a reference to the ColorVector to be inserted before getting the
        // reference to the vector we are copying because inserting the new
        // element in BlockColors might cause the map to be reallocated.
        SafetyInfo->copyColors(NewPred, PredBB);
    }
    PredBBs.remove(PredBB);
  }
}

/// When an instruction is found to only be used outside of the loop, this
/// function moves it to the exit blocks and patches up SSA form as needed.
/// This method is guaranteed to remove the original instruction from its
/// position, and may either delete it or move it to outside of the loop.
///
static bool sink(Instruction &I, LoopInfo *LI, DominatorTree *DT,
                 BlockFrequencyInfo *BFI, const Loop *CurLoop,
                 ICFLoopSafetyInfo *SafetyInfo, MemorySSAUpdater &MSSAU,
                 OptimizationRemarkEmitter *ORE) {
  bool Changed = false;
  LLVM_DEBUG(dbgs() << "LICM sinking instruction: " << I << "\n");

  // Iterate over users to be ready for actual sinking. Replace users via
  // unreachable blocks with undef and make all user PHIs trivially replaceable.
  SmallPtrSet<Instruction *, 8> VisitedUsers;
  for (Value::user_iterator UI = I.user_begin(), UE = I.user_end(); UI != UE;) {
    auto *User = cast<Instruction>(*UI);
    Use &U = UI.getUse();
    ++UI;

    if (VisitedUsers.count(User) || CurLoop->contains(User))
      continue;

    if (!DT->isReachableFromEntry(User->getParent())) {
      U = PoisonValue::get(I.getType());
      Changed = true;
      continue;
    }

    // The user must be a PHI node.
    PHINode *PN = cast<PHINode>(User);

    // Surprisingly, instructions can be used outside of loops without any
    // exits.  This can only happen in PHI nodes if the incoming block is
    // unreachable.
    BasicBlock *BB = PN->getIncomingBlock(U);
    if (!DT->isReachableFromEntry(BB)) {
      U = PoisonValue::get(I.getType());
      Changed = true;
      continue;
    }

    VisitedUsers.insert(PN);
    if (isTriviallyReplaceablePHI(*PN, I))
      continue;

    if (!canSplitPredecessors(PN, SafetyInfo))
      return Changed;

    // Split predecessors of the PHI so that we can make users trivially
    // replaceable.
    splitPredecessorsOfLoopExit(PN, DT, LI, CurLoop, SafetyInfo, &MSSAU);

    // Should rebuild the iterators, as they may be invalidated by
    // splitPredecessorsOfLoopExit().
    UI = I.user_begin();
    UE = I.user_end();
  }

  if (VisitedUsers.empty())
    return Changed;

  ORE->emit([&]() {
    return OptimizationRemark(DEBUG_TYPE, "InstSunk", &I)
           << "sinking " << ore::NV("Inst", &I);
  });
  if (isa<LoadInst>(I))
    ++NumMovedLoads;
  else if (isa<CallInst>(I))
    ++NumMovedCalls;
  ++NumSunk;

#ifndef NDEBUG
  SmallVector<BasicBlock *, 32> ExitBlocks;
  CurLoop->getUniqueExitBlocks(ExitBlocks);
  SmallPtrSet<BasicBlock *, 32> ExitBlockSet(ExitBlocks.begin(),
                                             ExitBlocks.end());
#endif

  // Clones of this instruction. Don't create more than one per exit block!
  SmallDenseMap<BasicBlock *, Instruction *, 32> SunkCopies;

  // If this instruction is only used outside of the loop, then all users are
  // PHI nodes in exit blocks due to LCSSA form. Just RAUW them with clones of
  // the instruction.
  // First check if I is worth sinking for all uses. Sink only when it is worth
  // across all uses.
  SmallSetVector<User*, 8> Users(I.user_begin(), I.user_end());
  for (auto *UI : Users) {
    auto *User = cast<Instruction>(UI);

    if (CurLoop->contains(User))
      continue;

    PHINode *PN = cast<PHINode>(User);
    assert(ExitBlockSet.count(PN->getParent()) &&
           "The LCSSA PHI is not in an exit block!");

    // The PHI must be trivially replaceable.
    Instruction *New = sinkThroughTriviallyReplaceablePHI(
        PN, &I, LI, SunkCopies, SafetyInfo, CurLoop, MSSAU);
    PN->replaceAllUsesWith(New);
    eraseInstruction(*PN, *SafetyInfo, MSSAU);
    Changed = true;
  }
  return Changed;
}

/// When an instruction is found to only use loop invariant operands that
/// is safe to hoist, this instruction is called to do the dirty work.
///
static void hoist(Instruction &I, const DominatorTree *DT, const Loop *CurLoop,
                  BasicBlock *Dest, ICFLoopSafetyInfo *SafetyInfo,
                  MemorySSAUpdater &MSSAU, ScalarEvolution *SE,
                  OptimizationRemarkEmitter *ORE) {
  LLVM_DEBUG(dbgs() << "LICM hoisting to " << Dest->getNameOrAsOperand() << ": "
                    << I << "\n");
  ORE->emit([&]() {
    return OptimizationRemark(DEBUG_TYPE, "Hoisted", &I) << "hoisting "
                                                         << ore::NV("Inst", &I);
  });

  // Metadata can be dependent on conditions we are hoisting above.
  // Conservatively strip all metadata on the instruction unless we were
  // guaranteed to execute I if we entered the loop, in which case the metadata
  // is valid in the loop preheader.
  // Similarly, If I is a call and it is not guaranteed to execute in the loop,
  // then moving to the preheader means we should strip attributes on the call
  // that can cause UB since we may be hoisting above conditions that allowed
  // inferring those attributes. They may not be valid at the preheader.
  if ((I.hasMetadataOtherThanDebugLoc() || isa<CallInst>(I)) &&
      // The check on hasMetadataOtherThanDebugLoc is to prevent us from burning
      // time in isGuaranteedToExecute if we don't actually have anything to
      // drop.  It is a compile time optimization, not required for correctness.
      !SafetyInfo->isGuaranteedToExecute(I, DT, CurLoop))
    I.dropUndefImplyingAttrsAndUnknownMetadata();

  if (isa<PHINode>(I))
    // Move the new node to the end of the phi list in the destination block.
    moveInstructionBefore(I, *Dest->getFirstNonPHI(), *SafetyInfo, MSSAU, SE);
  else
    // Move the new node to the destination block, before its terminator.
    moveInstructionBefore(I, *Dest->getTerminator(), *SafetyInfo, MSSAU, SE);

  I.updateLocationAfterHoist();

  if (isa<LoadInst>(I))
    ++NumMovedLoads;
  else if (isa<CallInst>(I))
    ++NumMovedCalls;
  ++NumHoisted;
}

/// Only sink or hoist an instruction if it is not a trapping instruction,
/// or if the instruction is known not to trap when moved to the preheader.
/// or if it is a trapping instruction and is guaranteed to execute.
static bool isSafeToExecuteUnconditionally(
    Instruction &Inst, const DominatorTree *DT, const TargetLibraryInfo *TLI,
    const Loop *CurLoop, const LoopSafetyInfo *SafetyInfo,
    OptimizationRemarkEmitter *ORE, const Instruction *CtxI,
    bool AllowSpeculation) {
  if (AllowSpeculation && isSafeToSpeculativelyExecute(&Inst, CtxI, DT, TLI))
    return true;

  bool GuaranteedToExecute =
      SafetyInfo->isGuaranteedToExecute(Inst, DT, CurLoop);

  if (!GuaranteedToExecute) {
    auto *LI = dyn_cast<LoadInst>(&Inst);
    if (LI && CurLoop->isLoopInvariant(LI->getPointerOperand()))
      ORE->emit([&]() {
        return OptimizationRemarkMissed(
                   DEBUG_TYPE, "LoadWithLoopInvariantAddressCondExecuted", LI)
               << "failed to hoist load with loop-invariant address "
                  "because load is conditionally executed";
      });
  }

  return GuaranteedToExecute;
}

namespace {
class LoopPromoter : public LoadAndStorePromoter {
  Value *SomePtr; // Designated pointer to store to.
  const SmallSetVector<Value *, 8> &PointerMustAliases;
  SmallVectorImpl<BasicBlock *> &LoopExitBlocks;
  SmallVectorImpl<Instruction *> &LoopInsertPts;
  SmallVectorImpl<MemoryAccess *> &MSSAInsertPts;
  PredIteratorCache &PredCache;
  MemorySSAUpdater &MSSAU;
  LoopInfo &LI;
  DebugLoc DL;
  Align Alignment;
  bool UnorderedAtomic;
  AAMDNodes AATags;
  ICFLoopSafetyInfo &SafetyInfo;
  bool CanInsertStoresInExitBlocks;

  // We're about to add a use of V in a loop exit block.  Insert an LCSSA phi
  // (if legal) if doing so would add an out-of-loop use to an instruction
  // defined in-loop.
  Value *maybeInsertLCSSAPHI(Value *V, BasicBlock *BB) const {
    if (!LI.wouldBeOutOfLoopUseRequiringLCSSA(V, BB))
      return V;

    Instruction *I = cast<Instruction>(V);
    // We need to create an LCSSA PHI node for the incoming value and
    // store that.
    PHINode *PN = PHINode::Create(I->getType(), PredCache.size(BB),
                                  I->getName() + ".lcssa", &BB->front());
    for (BasicBlock *Pred : PredCache.get(BB))
      PN->addIncoming(I, Pred);
    return PN;
  }

public:
  LoopPromoter(Value *SP, ArrayRef<const Instruction *> Insts, SSAUpdater &S,
               const SmallSetVector<Value *, 8> &PMA,
               SmallVectorImpl<BasicBlock *> &LEB,
               SmallVectorImpl<Instruction *> &LIP,
               SmallVectorImpl<MemoryAccess *> &MSSAIP, PredIteratorCache &PIC,
               MemorySSAUpdater &MSSAU, LoopInfo &li, DebugLoc dl,
               Align Alignment, bool UnorderedAtomic, const AAMDNodes &AATags,
               ICFLoopSafetyInfo &SafetyInfo, bool CanInsertStoresInExitBlocks)
      : LoadAndStorePromoter(Insts, S), SomePtr(SP), PointerMustAliases(PMA),
        LoopExitBlocks(LEB), LoopInsertPts(LIP), MSSAInsertPts(MSSAIP),
        PredCache(PIC), MSSAU(MSSAU), LI(li), DL(std::move(dl)),
        Alignment(Alignment), UnorderedAtomic(UnorderedAtomic), AATags(AATags),
        SafetyInfo(SafetyInfo),
        CanInsertStoresInExitBlocks(CanInsertStoresInExitBlocks) {}

  bool isInstInList(Instruction *I,
                    const SmallVectorImpl<Instruction *> &) const override {
    Value *Ptr;
    if (LoadInst *LI = dyn_cast<LoadInst>(I))
      Ptr = LI->getOperand(0);
    else
      Ptr = cast<StoreInst>(I)->getPointerOperand();
    return PointerMustAliases.count(Ptr);
  }

  void insertStoresInLoopExitBlocks() {
    // Insert stores after in the loop exit blocks.  Each exit block gets a
    // store of the live-out values that feed them.  Since we've already told
    // the SSA updater about the defs in the loop and the preheader
    // definition, it is all set and we can start using it.
    for (unsigned i = 0, e = LoopExitBlocks.size(); i != e; ++i) {
      BasicBlock *ExitBlock = LoopExitBlocks[i];
      Value *LiveInValue = SSA.GetValueInMiddleOfBlock(ExitBlock);
      LiveInValue = maybeInsertLCSSAPHI(LiveInValue, ExitBlock);
      Value *Ptr = maybeInsertLCSSAPHI(SomePtr, ExitBlock);
      Instruction *InsertPos = LoopInsertPts[i];
      StoreInst *NewSI = new StoreInst(LiveInValue, Ptr, InsertPos);
      if (UnorderedAtomic)
        NewSI->setOrdering(AtomicOrdering::Unordered);
      NewSI->setAlignment(Alignment);
      NewSI->setDebugLoc(DL);
      if (AATags)
        NewSI->setAAMetadata(AATags);

      MemoryAccess *MSSAInsertPoint = MSSAInsertPts[i];
      MemoryAccess *NewMemAcc;
      if (!MSSAInsertPoint) {
        NewMemAcc = MSSAU.createMemoryAccessInBB(
            NewSI, nullptr, NewSI->getParent(), MemorySSA::Beginning);
      } else {
        NewMemAcc =
            MSSAU.createMemoryAccessAfter(NewSI, nullptr, MSSAInsertPoint);
      }
      MSSAInsertPts[i] = NewMemAcc;
      MSSAU.insertDef(cast<MemoryDef>(NewMemAcc), true);
      // FIXME: true for safety, false may still be correct.
    }
  }

  void doExtraRewritesBeforeFinalDeletion() override {
    if (CanInsertStoresInExitBlocks)
      insertStoresInLoopExitBlocks();
  }

  void instructionDeleted(Instruction *I) const override {
    SafetyInfo.removeInstruction(I);
    MSSAU.removeMemoryAccess(I);
  }

  bool shouldDelete(Instruction *I) const override {
    if (isa<StoreInst>(I))
      return CanInsertStoresInExitBlocks;
    return true;
  }
};

bool isNotCapturedBeforeOrInLoop(const Value *V, const Loop *L,
                                 DominatorTree *DT) {
  // We can perform the captured-before check against any instruction in the
  // loop header, as the loop header is reachable from any instruction inside
  // the loop.
  // TODO: ReturnCaptures=true shouldn't be necessary here.
  return !PointerMayBeCapturedBefore(V, /* ReturnCaptures */ true,
                                     /* StoreCaptures */ true,
                                     L->getHeader()->getTerminator(), DT);
}

/// Return true if we can prove that a caller cannot inspect the object if an
/// unwind occurs inside the loop.
bool isNotVisibleOnUnwindInLoop(const Value *Object, const Loop *L,
                                DominatorTree *DT) {
  bool RequiresNoCaptureBeforeUnwind;
  if (!isNotVisibleOnUnwind(Object, RequiresNoCaptureBeforeUnwind))
    return false;

  return !RequiresNoCaptureBeforeUnwind ||
         isNotCapturedBeforeOrInLoop(Object, L, DT);
}

} // namespace

/// Try to promote memory values to scalars by sinking stores out of the
/// loop and moving loads to before the loop.  We do this by looping over
/// the stores in the loop, looking for stores to Must pointers which are
/// loop invariant.
///
bool llvm::promoteLoopAccessesToScalars(
    const SmallSetVector<Value *, 8> &PointerMustAliases,
    SmallVectorImpl<BasicBlock *> &ExitBlocks,
    SmallVectorImpl<Instruction *> &InsertPts,
    SmallVectorImpl<MemoryAccess *> &MSSAInsertPts, PredIteratorCache &PIC,
    LoopInfo *LI, DominatorTree *DT, const TargetLibraryInfo *TLI,
    Loop *CurLoop, MemorySSAUpdater &MSSAU, ICFLoopSafetyInfo *SafetyInfo,
    OptimizationRemarkEmitter *ORE, bool AllowSpeculation) {
  // Verify inputs.
  assert(LI != nullptr && DT != nullptr && CurLoop != nullptr &&
         SafetyInfo != nullptr &&
         "Unexpected Input to promoteLoopAccessesToScalars");

  Value *SomePtr = *PointerMustAliases.begin();
  BasicBlock *Preheader = CurLoop->getLoopPreheader();

  // It is not safe to promote a load/store from the loop if the load/store is
  // conditional.  For example, turning:
  //
  //    for () { if (c) *P += 1; }
  //
  // into:
  //
  //    tmp = *P;  for () { if (c) tmp +=1; } *P = tmp;
  //
  // is not safe, because *P may only be valid to access if 'c' is true.
  //
  // The safety property divides into two parts:
  // p1) The memory may not be dereferenceable on entry to the loop.  In this
  //    case, we can't insert the required load in the preheader.
  // p2) The memory model does not allow us to insert a store along any dynamic
  //    path which did not originally have one.
  //
  // If at least one store is guaranteed to execute, both properties are
  // satisfied, and promotion is legal.
  //
  // This, however, is not a necessary condition. Even if no store/load is
  // guaranteed to execute, we can still establish these properties.
  // We can establish (p1) by proving that hoisting the load into the preheader
  // is safe (i.e. proving dereferenceability on all paths through the loop). We
  // can use any access within the alias set to prove dereferenceability,
  // since they're all must alias.
  //
  // There are two ways establish (p2):
  // a) Prove the location is thread-local. In this case the memory model
  // requirement does not apply, and stores are safe to insert.
  // b) Prove a store dominates every exit block. In this case, if an exit
  // blocks is reached, the original dynamic path would have taken us through
  // the store, so inserting a store into the exit block is safe. Note that this
  // is different from the store being guaranteed to execute. For instance,
  // if an exception is thrown on the first iteration of the loop, the original
  // store is never executed, but the exit blocks are not executed either.

  bool DereferenceableInPH = false;
  bool SafeToInsertStore = false;
  bool StoreIsGuanteedToExecute = false;
  bool FoundLoadToPromote = false;

  SmallVector<Instruction *, 64> LoopUses;

  // We start with an alignment of one and try to find instructions that allow
  // us to prove better alignment.
  Align Alignment;
  // Keep track of which types of access we see
  bool SawUnorderedAtomic = false;
  bool SawNotAtomic = false;
  AAMDNodes AATags;

  const DataLayout &MDL = Preheader->getModule()->getDataLayout();

  bool IsKnownThreadLocalObject = false;
  if (SafetyInfo->anyBlockMayThrow()) {
    // If a loop can throw, we have to insert a store along each unwind edge.
    // That said, we can't actually make the unwind edge explicit. Therefore,
    // we have to prove that the store is dead along the unwind edge.  We do
    // this by proving that the caller can't have a reference to the object
    // after return and thus can't possibly load from the object.
    Value *Object = getUnderlyingObject(SomePtr);
    if (!isNotVisibleOnUnwindInLoop(Object, CurLoop, DT))
      return false;
    // Subtlety: Alloca's aren't visible to callers, but *are* potentially
    // visible to other threads if captured and used during their lifetimes.
    IsKnownThreadLocalObject = !isa<AllocaInst>(Object);
  }

  // Check that all accesses to pointers in the alias set use the same type.
  // We cannot (yet) promote a memory location that is loaded and stored in
  // different sizes.  While we are at it, collect alignment and AA info.
  Type *AccessTy = nullptr;
  for (Value *ASIV : PointerMustAliases) {
    for (Use &U : ASIV->uses()) {
      // Ignore instructions that are outside the loop.
      Instruction *UI = dyn_cast<Instruction>(U.getUser());
      if (!UI || !CurLoop->contains(UI))
        continue;

      // If there is an non-load/store instruction in the loop, we can't promote
      // it.
      if (LoadInst *Load = dyn_cast<LoadInst>(UI)) {
        if (!Load->isUnordered())
          return false;

        SawUnorderedAtomic |= Load->isAtomic();
        SawNotAtomic |= !Load->isAtomic();
        FoundLoadToPromote = true;

        Align InstAlignment = Load->getAlign();

        // Note that proving a load safe to speculate requires proving
        // sufficient alignment at the target location.  Proving it guaranteed
        // to execute does as well.  Thus we can increase our guaranteed
        // alignment as well.
        if (!DereferenceableInPH || (InstAlignment > Alignment))
          if (isSafeToExecuteUnconditionally(
                  *Load, DT, TLI, CurLoop, SafetyInfo, ORE,
                  Preheader->getTerminator(), AllowSpeculation)) {
            DereferenceableInPH = true;
            Alignment = std::max(Alignment, InstAlignment);
          }
      } else if (const StoreInst *Store = dyn_cast<StoreInst>(UI)) {
        // Stores *of* the pointer are not interesting, only stores *to* the
        // pointer.
        if (U.getOperandNo() != StoreInst::getPointerOperandIndex())
          continue;
        if (!Store->isUnordered())
          return false;

        SawUnorderedAtomic |= Store->isAtomic();
        SawNotAtomic |= !Store->isAtomic();

        // If the store is guaranteed to execute, both properties are satisfied.
        // We may want to check if a store is guaranteed to execute even if we
        // already know that promotion is safe, since it may have higher
        // alignment than any other guaranteed stores, in which case we can
        // raise the alignment on the promoted store.
        Align InstAlignment = Store->getAlign();
        bool GuaranteedToExecute =
            SafetyInfo->isGuaranteedToExecute(*UI, DT, CurLoop);
        StoreIsGuanteedToExecute |= GuaranteedToExecute;
        if (!DereferenceableInPH || !SafeToInsertStore ||
            (InstAlignment > Alignment)) {
          if (GuaranteedToExecute) {
            DereferenceableInPH = true;
            SafeToInsertStore = true;
            Alignment = std::max(Alignment, InstAlignment);
          }
        }

        // If a store dominates all exit blocks, it is safe to sink.
        // As explained above, if an exit block was executed, a dominating
        // store must have been executed at least once, so we are not
        // introducing stores on paths that did not have them.
        // Note that this only looks at explicit exit blocks. If we ever
        // start sinking stores into unwind edges (see above), this will break.
        if (!SafeToInsertStore)
          SafeToInsertStore = llvm::all_of(ExitBlocks, [&](BasicBlock *Exit) {
            return DT->dominates(Store->getParent(), Exit);
          });

        // If the store is not guaranteed to execute, we may still get
        // deref info through it.
        if (!DereferenceableInPH) {
          DereferenceableInPH = isDereferenceableAndAlignedPointer(
              Store->getPointerOperand(), Store->getValueOperand()->getType(),
              Store->getAlign(), MDL, Preheader->getTerminator(), DT, TLI);
        }
      } else
        return false; // Not a load or store.

      if (!AccessTy)
        AccessTy = getLoadStoreType(UI);
      else if (AccessTy != getLoadStoreType(UI))
        return false;

      // Merge the AA tags.
      if (LoopUses.empty()) {
        // On the first load/store, just take its AA tags.
        AATags = UI->getAAMetadata();
      } else if (AATags) {
        AATags = AATags.merge(UI->getAAMetadata());
      }

      LoopUses.push_back(UI);
    }
  }

  // If we found both an unordered atomic instruction and a non-atomic memory
  // access, bail.  We can't blindly promote non-atomic to atomic since we
  // might not be able to lower the result.  We can't downgrade since that
  // would violate memory model.  Also, align 0 is an error for atomics.
  if (SawUnorderedAtomic && SawNotAtomic)
    return false;

  // If we're inserting an atomic load in the preheader, we must be able to
  // lower it.  We're only guaranteed to be able to lower naturally aligned
  // atomics.
  if (SawUnorderedAtomic && Alignment < MDL.getTypeStoreSize(AccessTy))
    return false;

  // If we couldn't prove we can hoist the load, bail.
  if (!DereferenceableInPH)
    return false;

  // We know we can hoist the load, but don't have a guaranteed store.
  // Check whether the location is thread-local. If it is, then we can insert
  // stores along paths which originally didn't have them without violating the
  // memory model.
  if (!SafeToInsertStore) {
    if (IsKnownThreadLocalObject)
      SafeToInsertStore = true;
    else {
      Value *Object = getUnderlyingObject(SomePtr);
      SafeToInsertStore =
          (isNoAliasCall(Object) || isa<AllocaInst>(Object)) &&
          isNotCapturedBeforeOrInLoop(Object, CurLoop, DT);
    }
  }

  // If we've still failed to prove we can sink the store, hoist the load
  // only, if possible.
  if (!SafeToInsertStore && !FoundLoadToPromote)
    // If we cannot hoist the load either, give up.
    return false;

  // Lets do the promotion!
  if (SafeToInsertStore)
    LLVM_DEBUG(dbgs() << "LICM: Promoting load/store of the value: " << *SomePtr
                      << '\n');
  else
    LLVM_DEBUG(dbgs() << "LICM: Promoting load of the value: " << *SomePtr
                      << '\n');

  ORE->emit([&]() {
    return OptimizationRemark(DEBUG_TYPE, "PromoteLoopAccessesToScalar",
                              LoopUses[0])
           << "Moving accesses to memory location out of the loop";
  });
  ++NumPromoted;

  // Look at all the loop uses, and try to merge their locations.
  std::vector<const DILocation *> LoopUsesLocs;
  for (auto U : LoopUses)
    LoopUsesLocs.push_back(U->getDebugLoc().get());
  auto DL = DebugLoc(DILocation::getMergedLocations(LoopUsesLocs));

  // We use the SSAUpdater interface to insert phi nodes as required.
  SmallVector<PHINode *, 16> NewPHIs;
  SSAUpdater SSA(&NewPHIs);
  LoopPromoter Promoter(SomePtr, LoopUses, SSA, PointerMustAliases, ExitBlocks,
                        InsertPts, MSSAInsertPts, PIC, MSSAU, *LI, DL,
                        Alignment, SawUnorderedAtomic, AATags, *SafetyInfo,
                        SafeToInsertStore);

  // Set up the preheader to have a definition of the value.  It is the live-out
  // value from the preheader that uses in the loop will use.
  LoadInst *PreheaderLoad = nullptr;
  if (FoundLoadToPromote || !StoreIsGuanteedToExecute) {
    PreheaderLoad =
        new LoadInst(AccessTy, SomePtr, SomePtr->getName() + ".promoted",
                     Preheader->getTerminator());
    if (SawUnorderedAtomic)
      PreheaderLoad->setOrdering(AtomicOrdering::Unordered);
    PreheaderLoad->setAlignment(Alignment);
    PreheaderLoad->setDebugLoc(DebugLoc());
    if (AATags)
      PreheaderLoad->setAAMetadata(AATags);

    MemoryAccess *PreheaderLoadMemoryAccess = MSSAU.createMemoryAccessInBB(
        PreheaderLoad, nullptr, PreheaderLoad->getParent(), MemorySSA::End);
    MemoryUse *NewMemUse = cast<MemoryUse>(PreheaderLoadMemoryAccess);
    MSSAU.insertUse(NewMemUse, /*RenameUses=*/true);
    SSA.AddAvailableValue(Preheader, PreheaderLoad);
  } else {
    SSA.AddAvailableValue(Preheader, PoisonValue::get(AccessTy));
  }

  if (VerifyMemorySSA)
    MSSAU.getMemorySSA()->verifyMemorySSA();
  // Rewrite all the loads in the loop and remember all the definitions from
  // stores in the loop.
  Promoter.run(LoopUses);

  if (VerifyMemorySSA)
    MSSAU.getMemorySSA()->verifyMemorySSA();
  // If the SSAUpdater didn't use the load in the preheader, just zap it now.
  if (PreheaderLoad && PreheaderLoad->use_empty())
    eraseInstruction(*PreheaderLoad, *SafetyInfo, MSSAU);

  return true;
}

static void foreachMemoryAccess(MemorySSA *MSSA, Loop *L,
                                function_ref<void(Instruction *)> Fn) {
  for (const BasicBlock *BB : L->blocks())
    if (const auto *Accesses = MSSA->getBlockAccesses(BB))
      for (const auto &Access : *Accesses)
        if (const auto *MUD = dyn_cast<MemoryUseOrDef>(&Access))
          Fn(MUD->getMemoryInst());
}

static SmallVector<SmallSetVector<Value *, 8>, 0>
collectPromotionCandidates(MemorySSA *MSSA, AliasAnalysis *AA, Loop *L) {
  AliasSetTracker AST(*AA);

  auto IsPotentiallyPromotable = [L](const Instruction *I) {
    if (const auto *SI = dyn_cast<StoreInst>(I))
      return L->isLoopInvariant(SI->getPointerOperand());
    if (const auto *LI = dyn_cast<LoadInst>(I))
      return L->isLoopInvariant(LI->getPointerOperand());
    return false;
  };

  // Populate AST with potentially promotable accesses.
  SmallPtrSet<Value *, 16> AttemptingPromotion;
  foreachMemoryAccess(MSSA, L, [&](Instruction *I) {
    if (IsPotentiallyPromotable(I)) {
      AttemptingPromotion.insert(I);
      AST.add(I);
    }
  });

  // We're only interested in must-alias sets that contain a mod.
  SmallVector<const AliasSet *, 8> Sets;
  for (AliasSet &AS : AST)
    if (!AS.isForwardingAliasSet() && AS.isMod() && AS.isMustAlias())
      Sets.push_back(&AS);

  if (Sets.empty())
    return {}; // Nothing to promote...

  // Discard any sets for which there is an aliasing non-promotable access.
  foreachMemoryAccess(MSSA, L, [&](Instruction *I) {
    if (AttemptingPromotion.contains(I))
      return;

    llvm::erase_if(Sets, [&](const AliasSet *AS) {
      return AS->aliasesUnknownInst(I, *AA);
    });
  });

  SmallVector<SmallSetVector<Value *, 8>, 0> Result;
  for (const AliasSet *Set : Sets) {
    SmallSetVector<Value *, 8> PointerMustAliases;
    for (const auto &ASI : *Set)
      PointerMustAliases.insert(ASI.getValue());
    Result.push_back(std::move(PointerMustAliases));
  }

  return Result;
}

static bool pointerInvalidatedByLoop(MemorySSA *MSSA, MemoryUse *MU,
                                     Loop *CurLoop, Instruction &I,
                                     SinkAndHoistLICMFlags &Flags) {
  // For hoisting, use the walker to determine safety
  if (!Flags.getIsSink()) {
    MemoryAccess *Source;
    // See declaration of SetLicmMssaOptCap for usage details.
    if (Flags.tooManyClobberingCalls())
      Source = MU->getDefiningAccess();
    else {
      Source = MSSA->getSkipSelfWalker()->getClobberingMemoryAccess(MU);
      Flags.incrementClobberingCalls();
    }
    return !MSSA->isLiveOnEntryDef(Source) &&
           CurLoop->contains(Source->getBlock());
  }

  // For sinking, we'd need to check all Defs below this use. The getClobbering
  // call will look on the backedge of the loop, but will check aliasing with
  // the instructions on the previous iteration.
  // For example:
  // for (i ... )
  //   load a[i] ( Use (LoE)
  //   store a[i] ( 1 = Def (2), with 2 = Phi for the loop.
  //   i++;
  // The load sees no clobbering inside the loop, as the backedge alias check
  // does phi translation, and will check aliasing against store a[i-1].
  // However sinking the load outside the loop, below the store is incorrect.

  // For now, only sink if there are no Defs in the loop, and the existing ones
  // precede the use and are in the same block.
  // FIXME: Increase precision: Safe to sink if Use post dominates the Def;
  // needs PostDominatorTreeAnalysis.
  // FIXME: More precise: no Defs that alias this Use.
  if (Flags.tooManyMemoryAccesses())
    return true;
  for (auto *BB : CurLoop->getBlocks())
    if (pointerInvalidatedByBlock(*BB, *MSSA, *MU))
      return true;
  // When sinking, the source block may not be part of the loop so check it.
  if (!CurLoop->contains(&I))
    return pointerInvalidatedByBlock(*I.getParent(), *MSSA, *MU);

  return false;
}

bool pointerInvalidatedByBlock(BasicBlock &BB, MemorySSA &MSSA, MemoryUse &MU) {
  if (const auto *Accesses = MSSA.getBlockDefs(&BB))
    for (const auto &MA : *Accesses)
      if (const auto *MD = dyn_cast<MemoryDef>(&MA))
        if (MU.getBlock() != MD->getBlock() || !MSSA.locallyDominates(MD, &MU))
          return true;
  return false;
}

/// Little predicate that returns true if the specified basic block is in
/// a subloop of the current one, not the current one itself.
///
static bool inSubLoop(BasicBlock *BB, Loop *CurLoop, LoopInfo *LI) {
  assert(CurLoop->contains(BB) && "Only valid if BB is IN the loop");
  return LI->getLoopFor(BB) != CurLoop;
}
