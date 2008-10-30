/// Pass to eliminate redundant checks in monotonic loops
///

 
#include <set>
#include "SCUtils.h"
#include "safecode/Config/config.h"
#include "InsertPoolChecks.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/ADT/Statistic.h"

namespace {
  RegisterPass<llvm::MonotonicLoopOpt> X("sc-monotonic-loop-opt", "Monotonic Loop Optimization for SAFECode");

  STATISTIC (MonotonicLoopOptPoolCheck,
	     "Number of monotonic loop optimization performed for poolcheck");
  STATISTIC (MonotonicLoopOptPoolCheckUI,
	     "Number of monotonic loop optimization performed for poolcheckUI");
  STATISTIC (MonotonicLoopOptExactCheck,
	     "Number of monotonic loop optimization performed for exactcheck");
  STATISTIC (MonotonicLoopOptExactCheck2,
	     "Number of monotonic loop optimization performed for exactcheck2");
  STATISTIC (MonotonicLoopOptBoundsCheck,
	     "Number of monotonic loop optimization performed for boundscheck");
  STATISTIC (MonotonicLoopOptBoundsCheckUI,
	     "Number of monotonic loop optimization performed for boundscheckUI");

  enum {
    CHECK_FUNC_POOLCHECK = 0,
    CHECK_FUNC_POOLCHECKUI,
		CHECK_FUNC_POOLCHECKALIGN,
    CHECK_FUNC_EXACTCHECK,
    CHECK_FUNC_EXACTCHECK2,
    CHECK_FUNC_BOUNDSCHECK,
    CHECK_FUNC_BOUNDSCHECKUI,
    CHECK_FUNC_COUNT
  };
  
  static llvm::Statistic * statData[] = {
    &MonotonicLoopOptPoolCheck,
    &MonotonicLoopOptPoolCheckUI,
    &MonotonicLoopOptExactCheck,
    &MonotonicLoopOptExactCheck2,
    &MonotonicLoopOptBoundsCheck,
    &MonotonicLoopOptBoundsCheckUI,
  };

  struct checkFunctionInfo {
    const int id; 
    const std::string name;
    // The operand position in the checking function, 0 means not applicable
    const int argPoolHandlePos;
    const int argSrcPtrPos;
    const int argDestPtrPos;
    explicit checkFunctionInfo(int id, const char * name, int argPoolHandlePos, int argSrcPtrPos, int argDestPtrPos) :
      id(id), name(name), argPoolHandlePos(argPoolHandlePos), argSrcPtrPos(argSrcPtrPos), argDestPtrPos(argDestPtrPos) {}
  };
  
  static const checkFunctionInfo checkFunctions[] = {
    checkFunctionInfo(CHECK_FUNC_POOLCHECK,     "poolcheck",     1, 0, 2),
    checkFunctionInfo(CHECK_FUNC_POOLCHECKUI,   "poolcheckui",   1, 0, 2),
    checkFunctionInfo(CHECK_FUNC_POOLCHECKALIGN,"poolcheckalign",1, 0, 2),
    checkFunctionInfo(CHECK_FUNC_EXACTCHECK,    "exactcheck",    0, 0, 3),
    checkFunctionInfo(CHECK_FUNC_EXACTCHECK2,   "exactcheck2",   0, 1, 2),
    checkFunctionInfo(CHECK_FUNC_BOUNDSCHECK,   "boundscheck",   0, 2, 3),
    checkFunctionInfo(CHECK_FUNC_BOUNDSCHECKUI, "boundscheckui", 0, 2, 3)
  };

  typedef std::map<std::string, int> checkFuncMapType;
  checkFuncMapType checkFuncMap;
  
  // FIXME: There is duplicated codes in Parallel checkings
  // See whether a call instruction is calling the checking functions
  // It should be only called after doInitialization
  static bool isCheckingCall(CallInst * CI) {
    Function * F = CI->getCalledFunction();
    if (!F) return false;
    return checkFuncMap.find(F->getName()) != checkFuncMap.end();
  }

  // Try to find the GEP from the call instruction of the checking function

  static GetElementPtrInst * getGEPFromCheckCallInst(int checkFunctionId, CallInst * callInst) {
    const checkFunctionInfo & info = checkFunctions[checkFunctionId];
    Value * inst = callInst->getOperand(info.argDestPtrPos);
    if (isa<GetElementPtrInst>(inst)) {
      return dyn_cast<GetElementPtrInst>(inst);
    } else if (isa<BitCastInst>(inst)) {
      return dyn_cast<GetElementPtrInst>
	(dyn_cast<BitCastInst>(inst)->getOperand(0));
    }
    return NULL;
  }
  static std::set<Loop*> optimizedLoops;
}


namespace llvm {
  char MonotonicLoopOpt::ID = 0;

  /// Find the induction variable for a loop
  /// Based on include/llvm/Analysis/LoopInfo.h
  static bool getPossibleLoopVariable(Loop * L, std::vector<PHINode*> & list) {
    list.clear();
    BasicBlock *H = L->getHeader();

    BasicBlock *Incoming = 0, *Backedge = 0;
    typedef GraphTraits<Inverse<BasicBlock*> > InvBasicBlockraits;
    InvBasicBlockraits::ChildIteratorType PI = InvBasicBlockraits::child_begin(H);
    assert(PI != InvBasicBlockraits::child_end(H) &&
	   "Loop must have at least one backedge!");
    Backedge = *PI++;
    if (PI == InvBasicBlockraits::child_end(H)) return 0;  // dead loop
    Incoming = *PI++;
    if (PI != InvBasicBlockraits::child_end(H)) return 0;  // multiple backedges?
    // FIXME: Check incoming edges

    if (L->contains(Incoming)) {
      if (L->contains(Backedge))
        return 0;
      std::swap(Incoming, Backedge);
    } else if (!L->contains(Backedge))
      return 0;

    // Loop over all of the PHI nodes, looking for a canonical indvar.
    for (BasicBlock::iterator I = H->begin(), E=H->end(); I != E;  ++I) {
      isa<PHINode>(I);
      PHINode *PN = dyn_cast<PHINode>(I);
      if (PN) {
        list.push_back(PN);
      }
    }
    return list.size() > 0;
  }
  
  bool
  MonotonicLoopOpt::doFinalization() { 
    optimizedLoops.clear();
    return false;
  }

  // Initialization for the check function name -> check function id
  bool
  MonotonicLoopOpt::doInitialization(Loop *L, LPPassManager &LPM) { 
    optimizedLoops.clear();
    for (size_t i = 0; i < CHECK_FUNC_COUNT; ++i) {
      checkFuncMap[checkFunctions[i].name] = checkFunctions[i].id;
    }
    return false;
  }

  // Check whether specific loop is monotonic, and returns the start and end
  // values if it is a monotonic one.
  bool
  MonotonicLoopOpt::isMonotonicLoop(Loop * L, Value * loopVar) {
    bool HasConstantItCount = isa<SCEVConstant>(scevPass->getIterationCount(L));

    SCEVHandle SH = scevPass->getSCEV(loopVar);
    if (SH->hasComputableLoopEvolution(L) ||    // Varies predictably
        HasConstantItCount) {
      SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(SH);
      if (AR && AR->isAffine()) {
        SCEVHandle startVal = AR->getStart();
        SCEVHandle endVal = scevPass->getSCEVAtScope(loopVar, L->getParentLoop());
        if (!isa<SCEVCouldNotCompute>(startVal) && !isa<SCEVCouldNotCompute>(endVal)){
          // Success
          return true;
        }
      }
    }
    // It does not seem like a monotonic one.
    return false;
  }
 
  /// Determines if a GEP can be hoisted
  bool 
  MonotonicLoopOpt::isHoistableGEP(GetElementPtrInst * GEP, Loop * L) {
    for(int i = 0, end = GEP->getNumOperands(); i != end; ++i) {
      Value * op = GEP->getOperand(i);
      if (L->isLoopInvariant(op)) continue;

      SCEVHandle SH = scevPass->getSCEV(op);
      if (!SH->hasComputableLoopEvolution(L)) return false;
      SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(SH);
      if (!AR || !AR->isAffine()) return false;
      SCEVHandle startVal = AR->getStart();
      SCEVHandle endVal = scevPass->getSCEVAtScope(op, L->getParentLoop());
      if (isa<SCEVCouldNotCompute>(startVal) || isa<SCEVCouldNotCompute>(endVal)){
	return false;
      }
    }
    return true;
  }

  /// Insert checks for edge condition

  void
  MonotonicLoopOpt::insertEdgeBoundsCheck(int checkFunctionId, Loop * L, const CallInst * callInst, GetElementPtrInst * origGEP, Instruction *
					  ptIns, int type)
  {
    enum {
      LOWER_BOUND,
      UPPER_BOUND
    };
    
    static const char * suffixes[] = {".lower", ".upper"};

    SCEVExpander Rewriter(*scevPass, *LI); 
    
    GetElementPtrInst *newGEP = origGEP->clone();
    newGEP->setName(origGEP->getName() + suffixes[type]);
    for(int i = 0, end = origGEP->getNumOperands(); i != end; ++i) {
      Value * op = origGEP->getOperand(i);
      if (L->isLoopInvariant(op)) continue;
      
      SCEVHandle SH = scevPass->getSCEV(op);
      SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(SH);
      SCEVHandle startVal = AR->getStart();
      SCEVHandle endVal = scevPass->getSCEVAtScope(op, L->getParentLoop());
      SCEVHandle & val = type == LOWER_BOUND ? startVal : endVal; 
      Value * boundsVal = Rewriter.expandCodeFor(val, ptIns);
      newGEP->setOperand(i, boundsVal);
    }
    
    newGEP->insertBefore(ptIns);
   
    CastInst * castedNewGEP = CastInst::CreatePointerCast(newGEP,
							  PointerType::getUnqual(Type::Int8Ty), newGEP->getName() + ".casted",
							  ptIns);

    CallInst * checkInst = callInst->clone();
    const checkFunctionInfo & info = checkFunctions[checkFunctionId];

    if (info.argSrcPtrPos) {
      // Copy the srcPtr if necessary
      CastInst * newSrcPtr = CastInst::CreatePointerCast
	(origGEP->getPointerOperand(),
	 PointerType::getUnqual(Type::Int8Ty), origGEP->getName() + ".casted",
	 newGEP);
      checkInst->setOperand(info.argSrcPtrPos, newSrcPtr);
    }
    
    if (info.argPoolHandlePos) {
      // Copy the pool handle if necessary
      Instruction * newPH = cast<Instruction>(checkInst->getOperand(1))->clone();
      newPH->insertBefore(ptIns);
      checkInst->setOperand(info.argPoolHandlePos, newPH);
    }
    
    checkInst->setOperand(info.argDestPtrPos, castedNewGEP);
    checkInst->insertBefore(ptIns);
  }
  

  bool
  MonotonicLoopOpt::runOnLoop(Loop *L, LPPassManager &LPM) {
    LI = &getAnalysis<LoopInfo>();
    scevPass = &getAnalysis<ScalarEvolution>();

    for (Loop::iterator LoopItr = L->begin(), LoopItrE = L->end();
	 LoopItr != LoopItrE; ++LoopItr) {
      if (optimizedLoops.find(*LoopItr) == optimizedLoops.end())
        {
          // Handle sub loops first
          LPM.redoLoop(L);
          return false;
        }
    }
    optimizedLoops.insert(L);
    return optimizeCheck(L);
  }

  bool
  MonotonicLoopOpt::optimizeCheck(Loop *L) {
    bool changed = false;
    if (!isEligibleForOptimization(L)) return false;
    // Get the preheader block to move instructions into...
    BasicBlock * Preheader = L->getLoopPreheader();
      
    std::vector<PHINode *> loopVarList;
    getPossibleLoopVariable(L, loopVarList);
    PHINode * loopVar = NULL;
    for (std::vector<PHINode*>::iterator it = loopVarList.begin(), end = loopVarList.end(); it != end; ++it) {
      if (!isMonotonicLoop(L, *it)) continue;
      loopVar = *it;

      // Loop over the body of this loop, looking for calls, invokes, and stores.
      // Because subloops have already been incorporated into AST, we skip blocks in
      // subloops.
      //
      std::vector<CallInst*> toBeRemoved;
      for (Loop::block_iterator I = L->block_begin(), E = L->block_end();
	   I != E; ++I) {
	BasicBlock *BB = *I;
	if (LI->getLoopFor(BB) != L) continue; // Ignore blocks in subloops...

	for (BasicBlock::iterator it = BB->begin(), end = BB->end(); it != end;
	     ++it) {
	  CallInst * callInst = dyn_cast<CallInst>(it);
	  if (!callInst) continue;

	  Function * F = callInst->getCalledFunction();
	  if (!F) continue;

	  checkFuncMapType::iterator it = checkFuncMap.find(F->getName());
	  if (it == checkFuncMap.end()) continue;

	  int checkFunctionId = it->second;
	  GetElementPtrInst * GEP = getGEPFromCheckCallInst(checkFunctionId, callInst);

	  if (!GEP || !isHoistableGEP(GEP, L)) continue;
          
	  Instruction *ptIns = Preheader->getTerminator();

	  insertEdgeBoundsCheck(checkFunctionId, L, callInst, GEP, ptIns, 0);
	  insertEdgeBoundsCheck(checkFunctionId, L, callInst, GEP, ptIns, 1);
	  toBeRemoved.push_back(callInst);

	  ++(*(statData[checkFunctionId]));
	  changed = true;
	}

      }
      for (std::vector<CallInst*>::iterator it = toBeRemoved.begin(), end = toBeRemoved.end(); it != end; ++it) {
	(*it)->eraseFromParent();
      }
    }
    return changed;
  }

  /// Test whether a loop is eligible for monotonic optmization
  /// A loop should satisfy all these following conditions before optmization:
  /// 1. Have an preheader
  /// 2. There is only *one* exitblock in the loop
  /// 3. There is no other instructions (actually we only handle call instruction) in the loop change the bounds of the check
  bool
  MonotonicLoopOpt::isEligibleForOptimization(const Loop * L) {
    BasicBlock * Preheader = L->getLoopPreheader();
    if (!Preheader) return false;
    
    SmallVector<BasicBlock*, 4> exitBlocks;
    L->getExitingBlocks(exitBlocks);
    if (exitBlocks.size() != 1) {
      return false;
    }
    // TODO: we should run a bottom-up call graph analysis to identify the 
    // calls that are SAFE, i.e., calls that do not affect the bounds of arrays.
    //
    // Currently we scan through the loop (including sub-loops), we
    // don't do the optimization if there exists a call instruction in
    // the loop.

    for (Loop::block_iterator I = L->block_begin(), E = L->block_end();
	 I != E; ++I) {
      BasicBlock *BB = *I;
      for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
	if (CallInst * CI = dyn_cast<CallInst>(I)) {
	  if (!isCheckingCall(CI)) 
	    return false;
	}
      }
    }     
    return true;
  }
}
