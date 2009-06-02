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

#include "safecode/OptimizeChecks.h"
#include "safecode/Support/AllocatorInfo.h"
#include "SCUtils.h"

NAMESPACE_SC_BEGIN

char ExactCheckOpt::ID = 0;

bool
ExactCheckOpt::runOnModule(Module & M) {
  intrinsic = &getAnalysis<InsertSCIntrinsic>();
  ExactCheck2 = intrinsic->getIntrinsic("sc.exactcheck2").F;

  InsertSCIntrinsic::intrinsic_const_iterator i, e;
  for (i = intrinsic->intrinsic_begin(), e = intrinsic->intrinsic_end(); i != e; ++i) {
    // FIXME: the code is actually in Intrinsic.cpp, refactor required here.
    if (i->type == InsertSCIntrinsic::SC_INTRINSIC_GEPCHECK
        || i->type == InsertSCIntrinsic::SC_INTRINSIC_MEMCHECK) {
      checkingIntrinsicsToBeRemoved.clear();
      Function * F = i->F;
      for (Value::use_iterator UI = F->use_begin(), E = F->use_end(); UI != E; ++UI) {
        CallInst * CI = dyn_cast<CallInst>(*UI);
        if (CI) {
          visitCheckingIntrinsic(CI);
        }
      }

      // Remove checking intrinsics that have been optimized
      for (std::vector<CallInst*>::const_iterator i = checkingIntrinsicsToBeRemoved.begin(), e = checkingIntrinsicsToBeRemoved.end(); i != e; ++i) {
        (*i)->eraseFromParent();
      }
    }
  }

  return false;
}

//
// Function: visitCheckingIntrinsic()
//
// Description:
//  Attempts to rewrite an extensive check into an efficient, accurate array
//  bounds check which will not use meta-data information
//
// Return value:
//  true  - Successfully rewrite the check into an exact check.
//  false - Cannot perform the optimization.
//
bool
ExactCheckOpt::visitCheckingIntrinsic(CallInst * CI) {
  Value * CheckPtr = intrinsic->getCheckedPointer(CI);
  bool indexed;
  Value * BasePtr = getBasePtr(CheckPtr, indexed);
  Value * Size = intrinsic->getObjectSize(BasePtr);
  if (Size) {
    rewriteToExactCheck(CI, BasePtr, CheckPtr, Size);
  }
  /*
   * We were not able to insert a call to exactcheck().
   */
  return false;
}

//
// Function: rewriteToExactCheck()
//
// Description:
// Rewrite a check into an exact check
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
  Type *VoidPtrType = PointerType::getUnqual(Type::Int8Ty); 

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
  if (Bounds->getType() != Type::Int32Ty)
    CastBounds = castTo (Bounds, Type::Int32Ty,
                         Bounds->getName()+".ec.casted", CI);

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

//
// Function: getBasePtr()
//
// Description:
//  Given a pointer value, attempt to find a source of the pointer that can
//  be used in an exactcheck().
//
// Outputs:
//  indexed - Flags whether the data flow went through a indexing operation
//            (i.e. a GEP).  This value is always written.
//
Value *
ExactCheckOpt::getBasePtr (Value * PointerOperand, bool & indexed) {
  //
  // Attempt to look for the originally allocated object by scanning the data
  // flow up.
  //
  indexed = false;
  Value * SourcePointer = PointerOperand;
  Value * OldSourcePointer;
  do {
    OldSourcePointer = SourcePointer;
    SourcePointer = SourcePointer->stripPointerCasts();
    // Check for GEP and cast instructions
    if (GetElementPtrInst * G = dyn_cast<GetElementPtrInst>(SourcePointer)) {
      SourcePointer = G->getPointerOperand();
      indexed = true;
    }
  } while (SourcePointer != OldSourcePointer);
  return SourcePointer;
}

NAMESPACE_SC_END
