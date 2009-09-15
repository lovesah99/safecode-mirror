//===- ExactCheckOpt.cpp -------------------------------------------------- --//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
//  This pass tries to lower bounds checks and load/store checks to exact
//  checks, that is checks whose bounds information can be determined easily,
//  say, allocations inside a function or global variables. Therefore SAFECode
//  does not need to register stuffs in the meta-data.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "exactcheck-opt"
#include "safecode/OptimizeChecks.h"
#include "safecode/Support/AllocatorInfo.h"
#include "SCUtils.h"

#include "llvm/ADT/Statistic.h"

NAMESPACE_SC_BEGIN

static RegisterPass<ExactCheckOpt> X ("exactcheck-opt", "Exact check optimization", true);

// Pass Statistics
namespace {
  STATISTIC (ExactChecks ,    "The number of checks lowered to exactcheck");
}

char ExactCheckOpt::ID = 0;

//
// Method: runOnModule()
//
// Description:
//  This method is the entry point for the transform pass.
//
// Inputs:
//  M - The LLVM module to transform.
//
// Return value:
//  true  - The module was modified.
//  false - The module was not modified.
//
bool
ExactCheckOpt::runOnModule(Module & M) {
  ExactChecks = 0;
  intrinsic = &getAnalysis<InsertSCIntrinsic>();
  ExactCheck2 = intrinsic->getIntrinsic("sc.exactcheck2").F;

  InsertSCIntrinsic::intrinsic_const_iterator i = intrinsic->intrinsic_begin();
  InsertSCIntrinsic::intrinsic_const_iterator e = intrinsic->intrinsic_end();
  for (; i != e; ++i) {
    if (i->flag & (InsertSCIntrinsic::SC_INTRINSIC_BOUNDSCHECK
                   | InsertSCIntrinsic::SC_INTRINSIC_MEMCHECK)) {
      checkingIntrinsicsToBeRemoved.clear();
      Function * F = i->F;
      for (Value::use_iterator UI = F->use_begin(), E = F->use_end();
           UI != E;
           ++UI) {
        CallInst * CI = dyn_cast<CallInst>(*UI);
        if (CI) {
          visitCheckingIntrinsic(CI);
        }
      }

      ExactChecks += checkingIntrinsicsToBeRemoved.size();

      //
      // Remove checking intrinsics that have been optimized
      //
      for (std::vector<CallInst*>::const_iterator i = checkingIntrinsicsToBeRemoved.begin(), e = checkingIntrinsicsToBeRemoved.end(); i != e; ++i) {
        (*i)->eraseFromParent();
      }
    }
  }

  //
  // Conservatively assume that we have changed something in the module.
  //
  return true;
}

//
// Function: visitCheckingIntrinsic()
//
// Description:
//  Attempts to rewrite an extensive check into an efficient, accurate array
//  bounds check which will not use meta-data information
//
// Inputs:
//  CI - A pointer to the instruction that performs a run-time check.
//
// Return value:
//  true  - Successfully rewrite the check into an exact check.
//  false - Cannot perform the optimization.
//
bool
ExactCheckOpt::visitCheckingIntrinsic(CallInst * CI) {
  //
  // Get the pointer that is checked by this run-time check.
  //
  Value * CheckPtr = intrinsic->getValuePointer(CI);

  //
  // Strip off all casts and GEPs to try to find the source of the pointer.
  //
  Value * BasePtr = intrinsic->findObject (CheckPtr);
  if (!BasePtr) return false;

  //
  // Attempt to get the size of the pointer.  If a size is returned, we know
  // that the base pointer points to the beginning of an object, and we can do
  // a run-time check without a lookup.
  //
  if (Value * Size = intrinsic->getObjectSize(BasePtr)) {
    rewriteToExactCheck(CI, BasePtr, CheckPtr, Size);
    return true;
  }

  //
  // We were not able to insert a call to exactcheck().
  //
  return false;
}

//
// Function: rewriteToExactCheck()
//
// Description:
//  Rewrite a check into an exact check
//
// Inputs:
//  BasePointer   - An LLVM Value representing the base of the object to check.
//  Result        - An LLVM Value representing the pointer to check.
//  Bounds        - An LLVM Value representing the bounds of the check.
//
void
ExactCheckOpt::rewriteToExactCheck(CallInst * CI, Value * BasePointer, 
                                   Value * ResultPointer, Value * Bounds) {
  // The LLVM type for a void *
  const Type *VoidPtrType = getVoidPtrType(); 
  const Type * Int32Type = IntegerType::getInt32Ty(getGlobalContext());

  //
  // Cast the operands to the correct type.
  //
  if (BasePointer->getType() != VoidPtrType)
    BasePointer = castTo (BasePointer, VoidPtrType,
                          BasePointer->getName()+".ec.casted",
                          CI);

  if (ResultPointer->getType() != VoidPtrType)
    ResultPointer = castTo (ResultPointer, VoidPtrType,
                            ResultPointer->getName()+".ec.casted",
                            CI);

  Value * CastBounds = Bounds;
  if (Bounds->getType() != Int32Type)
    CastBounds = castTo (Bounds, Int32Type, Bounds->getName()+".ec.casted", CI);

  //
  // Create the call to exactcheck2().
  //
  std::vector<Value *> args(1, BasePointer);
  args.push_back(ResultPointer);
  args.push_back(CastBounds);

  CallInst * ExactCheckCI = CallInst::Create (ExactCheck2, args.begin(), args.end(), "", CI);
  // boundscheck / exactcheck return an out of bound pointer when REWRITE_OOB is
  // enabled. We need to replace all uses to make the optimization correct, but
  // we don't need do anything for load / store checks.
  //
  // We can test the condition above by simply testing the return types of the
  // checking functions.
  if (ExactCheckCI->getType() == CI->getType()) {
    CI->replaceAllUsesWith(ExactCheckCI);
  }

  checkingIntrinsicsToBeRemoved.push_back(CI);
}

NAMESPACE_SC_END
