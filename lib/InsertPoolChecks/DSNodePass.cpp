//===- DSNodePass.cpp - ------------------------------------------------------//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "dsnode"

#include <iostream>
#include <signal.h>

#include "safecode/Config/config.h"
#include "InsertPoolChecks.h"
#include "llvm/Support/CommandLine.h"

namespace llvm {

char DSNodePass::ID = 0; 
static RegisterPass<DSNodePass> passDSNode("ds-node", "Prepare DS Graph and Pool Handle information for SAFECode", false, true);

cl::opt<bool> CheckEveryGEPUse("check-every-gep-use", cl::init(false),
  cl::desc("Check every use of GEP"));

bool
DSNodePass::runOnModule(Module & M) {
//  std::cerr << "Running DSNodePass" << std::endl; 
#ifndef LLVA_KERNEL  
  paPass = &getAnalysis<PoolAllocateGroup>();
  assert (paPass && "Pool Allocation Transform *must* be run first!");
#if 0
  efPass = &getAnalysis<EmbeCFreeRemoval>();
#endif
#endif
  return false;
}

// Method: getDSGraph()
//
// Description:
//  Return the DSGraph for the given function.  This method automatically
//  selects the correct pass to query for the graph based upon whether we're
//  doing user-space or kernel analysis.
//
DSGraph *
DSNodePass::getDSGraph(Function & F) {
#ifndef LLVA_KERNEL
  return paPass->getDSGraph(F);
#else  
  return TDPass->getDSGraph(F);
#endif  
}

#ifndef LLVA_KERNEL
//
// Method: getPoolHandle()
//
// Description:
//  Return the pool handle assigned to this value.
//
// Inputs:
//  V         - The value for which we seek the pool handle.
//  collapsed - Flags whether we are willing to get pool handles for collapsed
//              pools.
//
// Return value:
//  0 - No pool handle was found.
//  Otherwise, returns either the pool handle or a pointer to a NULL pool
//  handle.
//
// Note:
//  Currently, collapsed is always set to true, meaning that we never use
//  information from the EmbeC pass like in the Olden days (ha ha!).  That
//  code, therefore, is disabled in order to speed up the revival of SAFECode
//  with Automatic Pool Allocation.
//
Value *
DSNodePass::getPoolHandle (const Value *V,
                           Function *FClone,
                           PA::FuncInfo &FI,
                           bool collapsed) {

  //
  // Ensure that the caller is okay with collapsed pools.  Code below for
  // handling the case when we don't want collapsed pools is disabled to
  // remove dependence on the old EmbeC passes.
  //
  assert (collapsed && "For now, we must always handle collapsed pools!\n");

  //
  // Get the DSNode for the value.  Don't worry about mapping back to the
  // original function because getDSNode() will take care of that for us.
  //
  const DSNode *Node = getDSNode (V, FClone);
  if (!Node) {
    std::cerr << "JTC: getPoolHandle: No DSNode: Function: "
              << FClone->getName() << ", Value: " << *V << std::endl;
    return 0;
  }

  //
  // If this function has a clone, then try to grab the original.
  //
  Function * FOrig = FClone;
  bool isClone = false;
  if (!(paPass->getFuncInfo(*FClone))) {
    FOrig = paPass->getOrigFunctionFromClone(FClone);
    assert (FOrig && "No Function Information from Pool Allocation!\n");
    isClone = true;
  }

  // Get the pool handle for this DSNode...
  //  assert(!Node->isUnknownNode() && "Unknown node \n");
  const Type *PoolDescType = paPass->getPoolType();
  const Type *PoolDescPtrTy = PointerType::getUnqual(PoolDescType);
  if (Node->isUnknownNode()) {
    //
    // FIXME:
    //  This should be in a top down pass or propagated like collapsed pools
    //  below .
    //
    if (!collapsed) {
#if 0
      assert(!getDSNodeOffset(V, FClone) && " we don't handle middle of structs yet\n");
#else
      if (getDSNodeOffset(V, FOrig))
        std::cerr << "ERROR: we don't handle middle of structs yet"
                  << std::endl;
#endif
std::cerr << "JTC: PH: Null 1: " << *V << std::endl;
      return Constant::getNullValue(PoolDescPtrTy);
    }
  }

  //
  // Get the pool handle from the pool allocation pass.  Use the original
  // function because we want to ensure that whatever pool handle we get back
  // is accessible from the function.
  //
  Value * PH = paPass->getPool (Node, *FClone);

#if 0
  map <Function *, set<Value *> > &
    CollapsedPoolPtrs = efPass->CollapsedPoolPtrs;
#endif
  
#if 0
  if (PH) {
    // Check that the node pointed to by V in the TD DS graph is not
    // collapsed
    if (!collapsed && CollapsedPoolPtrs.count(FOrig)) {
      Value *v = PH;
      if (CollapsedPoolPtrs[FOrig].find(PH) != CollapsedPoolPtrs[FOrig].end()) {
#ifdef DEBUG
        std::cerr << "Collapsed pools \n";
#endif
        return Constant::getNullValue(PoolDescPtrTy);
      } else {
        if (Argument * Arg = dyn_cast<Argument>(v))
          if ((Arg->getParent()) != FOrig)
            return Constant::getNullValue(PoolDescPtrTy);
        return v;
      }
    } else {
      if (Argument * Arg = dyn_cast<Argument>(PH))
        if ((Arg->getParent()) != FOrig)
          return Constant::getNullValue(PoolDescPtrTy);
      return PH;
    } 
  }
#else
  //
  // If we found the pool handle, then return it to the caller.
  //
  if (PH) return PH;
#endif

  if (isClone)
    std::cerr << "JTC: No Pool: " << FClone->getName() << ": "
              << *V << std::endl;
  return 0;
}
#else
Value *
DSNodePass::getPoolHandle(const Value *V, Function *F) {
  DSGraph &TDG =  TDPass->getDSGraph(*F);
  const DSNode *Node = TDG.getNodeForValue((Value *)V).getNode();
  // Get the pool handle for this DSNode...
  //  assert(!Node->isUnknownNode() && "Unknown node \n");
  //  if (Node->isUnknownNode()) {
  //    return 0;
  //  }
  if ((TDG.getPoolDescriptorsMap()).count(Node)) 
    return (TDG.getPoolDescriptorsMap()[Node]->getMetaPoolValue());
  return 0;
}
#endif

DSNode* DSNodePass::getDSNode (const Value *VOrig, Function *F) {
#ifndef LLVA_KERNEL
  //
  // JTC:
  //  If this function has a clone, then try to grab the original.
  //
  bool isClone = false;
  Value * Vnew = (Value *)(VOrig);
  if (!(paPass->getFuncInfo(*F))) {
    isClone = true;
    F = paPass->getOrigFunctionFromClone(F);
    PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*F);
    if (!FI->NewToOldValueMap.empty()) {
      Vnew = FI->MapValueToOriginal (Vnew);
    }
    assert (F && "No Function Information from Pool Allocation!\n");
  }

  // Ensure that the function has a DSGraph
  assert (paPass->hasDSGraph(*F) && "No DSGraph for function!\n");

  //
  // Lookup the DSNode for the value in the function's DSGraph.
  //
  const Value * V = (Vnew) ? Vnew : VOrig;
  DSGraph * TDG = paPass->getDSGraph(*F);
#else  
  DSGraph * TDG = TDPass->getDSGraph(*F);
#endif  
  DSNode *DSN = TDG->getNodeForValue(V).getNode();

  //
  // If the value wasn't found in the function's DSGraph, then maybe we can
  // find the value in the globals graph.
  //
  DSGraph * GlobalsGraph;
  if (!DSN) {
#ifndef LLVA_KERNEL
    GlobalsGraph = paPass->getGlobalsGraph ();
#else
    GlobalsGraph = TDPass->getGlobalsGraph ();
#endif
    DSN = GlobalsGraph->getNodeForValue(V).getNode();
  }

  // FIXME: Debugging code.  Please remove when finished debugging.
#if 0
  if (!DSN) {
    if (const GetElementPtrInst * GEP = dyn_cast<GetElementPtrInst>(V)) {
      DSN = TDG->getNodeForValue(GEP->getPointerOperand()).getNode();
      if (!DSN) {
        DSN = GlobalsGraph->getNodeForValue(GEP->getPointerOperand()).getNode();
      }

      if (!DSN)
        std::cerr << "JTC: No GEP DSNode: " << isClone << ": " << *V << std::endl;
    }
  }

  if (!DSN) {
    std::cerr << "JTC: Pausing: No DSNode: " << isClone << std::endl;
    kill (getpid(), SIGTSTP);
    std::cerr << "JTC: Resuming\n";
  }
#endif

  return DSN;
}

unsigned DSNodePass::getDSNodeOffset(const Value *V, Function *F) {
#ifndef LLVA_KERNEL
  DSGraph *TDG = paPass->getDSGraph(*F);
#else  
  DSGraph *TDG = TDPass->getDSGraph(*F);
#endif  
  return TDG->getNodeForValue((Value *)V).getOffset();
}

/// We don't need to maintian the checked DS nodes and
/// checked values when we check every use of GEP.
void
DSNodePass::addCheckedDSNode(const DSNode * node) {
  if (!CheckEveryGEPUse) {
    CheckedDSNodes.insert(node);
  }
}

void
DSNodePass::addCheckedValue(const Value * value) {
  if (!CheckEveryGEPUse) {
   CheckedValues.insert(value);
  }
}

bool
DSNodePass::isDSNodeChecked(const DSNode * node) const {
  return CheckedDSNodes.find(node) != CheckedDSNodes.end();
}

bool
DSNodePass::isValueChecked(const Value * val) const {
  return CheckedValues.find(val) != CheckedValues.end();
}

}
