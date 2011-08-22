//===- CFIChecks.cpp - Insert indirect function call checks --------------- --//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass instruments loads and stores with run-time checks to ensure memory
// safety.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "safecode"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Constants.h"

#include "safecode/CFIChecks.h"
#include "safecode/Utility.h"

namespace llvm {

char CFIChecks::ID = 0;

static RegisterPass<CFIChecks>
X ("cfichecks", "Insert control-flow integrity run-time checks");

// Pass Statistics
namespace {
  STATISTIC (Checks, "CFI Checks Added");
}

//
// Method: createTargetTable()
//
// Description:
//  Create a global variable that contains the targets of the specified
//  function call.
//
// Inputs:
//  CI - A call instruction.
//
// Outputs:
//  isComplete - Flag indicating whether all targets of the call are known.
//
// Return value:
//  A global variable pointing to an array of call targets.
//
GlobalVariable *
CFIChecks::createTargetTable (CallInst & CI, bool & isComplete) {
  //
  // Get the call graph.
  //
  CallGraph & CG = getAnalysis<CallGraph>();

  //
  // Get the call graph node for the function containing the call.
  //
  CallGraphNode * CGN = CG[CI.getParent()->getParent()];

  //
  // Iterate through all of the target call nodes and add them to the list of
  // targets to use in the global variable.
  //
  isComplete = true;
  PointerType * VoidPtrType = getVoidPtrType(CI.getContext());
  SmallVector<Constant *, 20> Targets;
  for (CallGraphNode::iterator ti = CGN->begin(); ti != CGN->end(); ++ti) {
    //
    // See if this call record corresponds to the call site in question.
    //
    const Value * V = ti->first;
    if (V != &CI)
      continue;

    //
    // Get the target function.
    //
    Function * Target = ti->second->getFunction();

    //
    // If there is no target function, then this call can call code external
    // to the module.  In that case, mark the call as incomplete.
    //
    if (!Target) {
      isComplete = false;
      continue;
    }

    //
    // Add the target to the set of targets.  Cast it to a void pointer first.
    //
    Targets.push_back (ConstantExpr::getZExtOrBitCast (Target, VoidPtrType));
  }

  //
  // Truncate the list with a null pointer.
  //
  Targets.push_back(ConstantPointerNull::get (VoidPtrType));

  //
  // Create the constant array initializer containing all of the targets.
  //
  ArrayType * AT = ArrayType::get (VoidPtrType, Targets.size());
  Constant * TargetArray = ConstantArray::get (AT, Targets);
  return new GlobalVariable (*(CI.getParent()->getParent()->getParent()),
                             AT,
                             true,
                             GlobalValue::InternalLinkage,
                             TargetArray,
                             "TargetList");
}

//
// Method: visitCallInst()
//
// Description:
//  Place a run-time check on a call instruction.
//
void
CFIChecks::visitCallInst (CallInst & CI) {
  //
  // If the call is inline assembly code or a direct call, then don't insert a
  // check.
  //
  Value * CalledValue = CI.getCalledValue()->stripPointerCasts();
  if ((isa<Function>(CalledValue)) || (isa<InlineAsm>(CalledValue)))
    return;

  //
  // Create the call to the run-time check.
  // The first argument is the function pointer used in the call.
  // The second argument is the pointer to check.
  //
  Value * args[2];
  LLVMContext & Context = CI.getContext();
  bool isComplete = false;
  GlobalVariable * Targets = createTargetTable (CI, isComplete);
  args[0] = castTo (CI.getCalledValue(), getVoidPtrType(Context), &CI);
  args[1] = castTo (Targets, getVoidPtrType(Context), &CI);
  CallInst * Check = CallInst::Create (FunctionCheckUI, args, "", &CI);

  //
  // If there's debug information on the load instruction, add it to the
  // run-time check.
  //
  if (MDNode * MD = CI.getMetadata ("dbg"))
    Check->setMetadata ("dbg", MD);

  //
  // Update the statistics.
  //
  ++Checks;
  return;
}

bool
CFIChecks::runOnModule (Module & M) {
  //
  // Create a function prototype for the function that performs incomplete
  // function call checks.
  //
  Type *VoidTy    = Type::getVoidTy (M.getContext());
  Type *VoidPtrTy = getVoidPtrType (M.getContext());
  FunctionCheckUI = cast<Function>(M.getOrInsertFunction ("funccheckui",
                                                          VoidTy,
                                                          VoidPtrTy,
                                                          VoidPtrTy,
                                                          NULL));
  assert (FunctionCheckUI && "Function Check function has disappeared!\n");

  //
  // Visit all of the instructions in the function.
  //
  visit (M);
  return true;
}

}

