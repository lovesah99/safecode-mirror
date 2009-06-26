//===- Intrinsic.cpp - Insert declaration of SAFECode intrinsics -------------//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements a module pass to insert declarations of the SAFECode
// intrinsics into the bitcode file. It also provides interfaces for later
// passes which use these intrinsics.
//
//===----------------------------------------------------------------------===//

#include "safecode/Intrinsic.h"
#include "safecode/VectorListHelper.h"
#include "safecode/SAFECodeConfig.h"
#include "safecode/Support/AllocatorInfo.h"

#include "llvm/Module.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Constants.h"

#include <set>
#include <queue>

using namespace llvm;

NAMESPACE_SC_BEGIN

//
// Method: runOnModule()
//
// Description:
//  This is the entry point for this Module Pass.  It will insert the necessary
//  SAFECode run-time functions into the Module.
//
// Inputs:
//  M - A reference to the Module to modify.
//
// Return value:
//  true  - The module was modified.
//  false - The module was not modified.
//
bool
InsertSCIntrinsic::runOnModule(Module & M) {
  currentModule = &M;
  TD = &getAnalysis<TargetData>();
  const Type * VoidTy = Type::VoidTy;
  const Type * Int32Ty = Type::Int32Ty;
  const Type * vpTy = PointerType::getUnqual(Type::Int8Ty);

  FunctionType * LSCheckTy = FunctionType::get
    (VoidTy, args<const Type*>::list(vpTy, vpTy), false);

  FunctionType * LSCheckAlignTy = FunctionType::get
    (VoidTy, args<const Type*>::list(vpTy, vpTy, Int32Ty), false);

  FunctionType * BoundsCheckTy = FunctionType::get
    (vpTy, args<const Type*>::list(vpTy, vpTy, vpTy), false);

  FunctionType * ExactCheck2Ty = FunctionType::get
    (vpTy, args<const Type*>::list(vpTy, vpTy, Int32Ty), false);

  FunctionType * FuncCheckTy = FunctionType::get
    (VoidTy, args<const Type*>::list(Int32Ty, vpTy, vpTy), false);

  FunctionType * GetActualValTy = FunctionType::get
    (vpTy, args<const Type*>::list(vpTy, vpTy), false);

  FunctionType * PoolRegTy = FunctionType::get
    (VoidTy, args<const Type*>::list(vpTy, vpTy, Int32Ty), false);

  FunctionType * PoolUnregTy = FunctionType::get
    (VoidTy, args<const Type*>::list(vpTy, vpTy), false);

  FunctionType * PoolArgRegTy = FunctionType::get
    (VoidTy, args<const Type*>::list(Int32Ty, PointerType::getUnqual(vpTy)), false);

  FunctionType * RegisterGlobalsTy = FunctionType::get
    (VoidTy, args<const Type*>::list(), false);

  FunctionType * InitRuntimeTy = RegisterGlobalsTy;

  FunctionType * InitPoolRuntimeTy = FunctionType::get
    (VoidTy, args<const Type*>::list(Int32Ty, Int32Ty, Int32Ty), false);

  addIntrinsic
    ("sc.lscheck",
    SC_INTRINSIC_HAS_POOL_HANDLE | SC_INTRINSIC_HAS_VALUE_POINTER
    | SC_INTRINSIC_CHECK | SC_INTRINSIC_MEMCHECK,
     LSCheckTy, 1);

  addIntrinsic
    ("sc.lscheckui",
    SC_INTRINSIC_HAS_POOL_HANDLE | SC_INTRINSIC_HAS_VALUE_POINTER
    | SC_INTRINSIC_CHECK | SC_INTRINSIC_MEMCHECK,
     LSCheckTy, 1);

  addIntrinsic
    ("sc.lscheckalign",
    SC_INTRINSIC_HAS_POOL_HANDLE | SC_INTRINSIC_HAS_VALUE_POINTER
    | SC_INTRINSIC_CHECK | SC_INTRINSIC_MEMCHECK,
     LSCheckAlignTy, 1);

  addIntrinsic
    ("sc.lscheckalignui",
    SC_INTRINSIC_HAS_POOL_HANDLE | SC_INTRINSIC_HAS_VALUE_POINTER
    | SC_INTRINSIC_CHECK | SC_INTRINSIC_MEMCHECK,
     LSCheckAlignTy, 1);

  addIntrinsic
    ("sc.boundscheck",
    SC_INTRINSIC_HAS_POOL_HANDLE | SC_INTRINSIC_HAS_VALUE_POINTER
    | SC_INTRINSIC_CHECK | SC_INTRINSIC_BOUNDSCHECK,
     BoundsCheckTy, 2);
  
  addIntrinsic
    ("sc.boundscheckui",
    SC_INTRINSIC_HAS_POOL_HANDLE | SC_INTRINSIC_HAS_VALUE_POINTER
    | SC_INTRINSIC_CHECK | SC_INTRINSIC_BOUNDSCHECK,
     BoundsCheckTy, 2);

  addIntrinsic
    ("sc.exactcheck2",
    SC_INTRINSIC_HAS_VALUE_POINTER
    | SC_INTRINSIC_CHECK | SC_INTRINSIC_BOUNDSCHECK,
     ExactCheck2Ty, 1);

  addIntrinsic
    ("sc.funccheck",
     SC_INTRINSIC_HAS_VALUE_POINTER |
     SC_INTRINSIC_CHECK | SC_INTRINSIC_MEMCHECK,
     FuncCheckTy, 1);

  addIntrinsic
    ("sc.get_actual_val",
     SC_INTRINSIC_OOB,
     GetActualValTy, 1);

  addIntrinsic
    ("sc.pool_register",
     SC_INTRINSIC_HAS_POOL_HANDLE | SC_INTRINSIC_HAS_VALUE_POINTER
     | SC_INTRINSIC_REGISTRATION,
     PoolRegTy, 1);

  addIntrinsic
    ("sc.pool_unregister",
     SC_INTRINSIC_HAS_POOL_HANDLE | SC_INTRINSIC_HAS_VALUE_POINTER
     | SC_INTRINSIC_REGISTRATION,
     PoolUnregTy, 1);

  addIntrinsic
    ("sc.pool_argvregister",
     SC_INTRINSIC_REGISTRATION,
     PoolArgRegTy, 1);

  addIntrinsic
    ("sc.register_globals",
     SC_INTRINSIC_REGISTRATION,
     RegisterGlobalsTy);

  addIntrinsic
    ("sc.init_runtime",
     SC_INTRINSIC_MISC,
     InitRuntimeTy);

  addIntrinsic
    ("sc.init_pool_runtime",
     SC_INTRINSIC_MISC,
     InitPoolRuntimeTy);

  // We always change the module.
  return true;
}

//
// Method: addIntrinsic()
//
// Description:
//  Create and register a new function as a SAFECode intrinsic function.
//
// Inputs:
//  type     - The type of intrinsic check.
//  name     - The name of the function.
//  FTy      - The LLVM type of the intrinsic function.
//  ptrindex - The index of the operand to the function which is used to take
//             the pointer which the intrinsic checks.  This is unused for
//             non-run-time checking intrinsics.
//
//
void
InsertSCIntrinsic::addIntrinsic (const char * name,
                                 unsigned int flag,
                                 FunctionType * FTy,
                                 unsigned ptrindex /* = 0 */) {
  //
  // Check that this pass has already analyzed an LLVM Module.
  //
  assert (currentModule && "No LLVM Module!");

  // Structure describing the new intrinsic function
  IntrinsicInfoTy info;

  //
  // Create the new intrinsic function and configure its SAFECode attributes.
  //
  info.flag = flag;
  info.F = dyn_cast<Function> (currentModule->getOrInsertFunction(name, FTy));
  info.ptrindex = ptrindex;

  //
  // Map the function name and LLVM Function pointer to its SAFECode attributes.
  //
  intrinsics.push_back (info);
  intrinsicNameMap.insert
    (StringMapEntry<uint32_t>::Create(name, name + strlen(name), intrinsics.size() - 1));
}

const InsertSCIntrinsic::IntrinsicInfoTy &
InsertSCIntrinsic::getIntrinsic(const std::string & name) const {
  StringMap<uint32_t>::const_iterator it = intrinsicNameMap.find(name);
  assert(it != intrinsicNameMap.end() && "Intrinsic should be defined before uses!");
  return intrinsics[it->second];
}

//
// Method: isSCIntrinsic()
//
// Description:
//  Determine whether the specified LLVM value is a call to a SAFECode
//  intrinsic.
//
// Inputs:
//  inst - The LLVM Value to check.  It can be any value, including
//         non-instruction values.
//  flag - Indicate the property in the desired intrinsic
//
// Return value:
//  true  - The LLVM value is a call to a SAFECode run-time function.
//  false - The LLVM value is not a call to a SAFECode run-time function.
//
bool
InsertSCIntrinsic::isSCIntrinsicWithFlags(Value * inst, unsigned flag) const {
  if (!isa<CallInst>(inst)) 
    return false;

  CallInst * CI = cast<CallInst>(inst);
  Function * F= CI->getCalledFunction();
  if (!F)
    return false;

  const std::string & name = F->getName();


  StringMap<uint32_t>::const_iterator it = intrinsicNameMap.find(name);

  if (it == intrinsicNameMap.end())
    return false;

  const IntrinsicInfoTy & info = intrinsics[it->getValue()];
  return info.flag && flag;
}

Value *
InsertSCIntrinsic::getValuePointer (CallInst * CI) {
  if (isSCIntrinsicWithFlags (CI, SC_INTRINSIC_HAS_VALUE_POINTER)) {
    const IntrinsicInfoTy & info = intrinsics[intrinsicNameMap[CI->getCalledFunction()->getName()]];

    //
    // Return the checked pointer in the call.  We use ptrindex + 1 because the
    // index is the index in the function signature, but in a CallInst, the
    // first argument is the function pointer.
    //
    return (CI->getOperand(info.ptrindex+1));
  }

  return NULL;
}


//
// Attempt to look for the originally allocated object by scanning the data
// flow up.
//
static Value * findObject(Value * obj) {
  std::set<Value *> exploredObjects;
  std::set<Value *> objects;
  std::queue<Value *> queue;
  queue.push(obj);
  while (!queue.empty()) {
    Value * o = queue.front();
    queue.pop();
    if (exploredObjects.count(o)) continue;

    exploredObjects.insert(o);

    if (isa<CastInst>(o)) {
      queue.push(cast<CastInst>(o)->getOperand(0));
    } else if (isa<GetElementPtrInst>(o)) {
      queue.push(cast<GetElementPtrInst>(o)->getPointerOperand());
    } else if (isa<PHINode>(o)) {
      PHINode * p = cast<PHINode>(o);
      for(unsigned int i = 0; i < p->getNumIncomingValues(); ++i) {
        queue.push(p->getIncomingValue(i));
      }
    } else {
      objects.insert(o);
    }
  }
  return objects.size() == 1 ? *(objects.begin()) : NULL;
}

//
// Check to see if we're indexing off the beginning of a known object.  If
// so, then find the size of the object.  Otherwise, return NULL.
//
Value *
InsertSCIntrinsic::getObjectSize(Value * V) {
  V = findObject(V);
  if (!V) {
    return NULL;
  }

  if (GlobalVariable * GV = dyn_cast<GlobalVariable>(V)) {
    return ConstantInt::get(Type::Int32Ty, TD->getTypeAllocSize (GV->getType()->getElementType()));
  }

  if (AllocationInst * AI = dyn_cast<AllocationInst>(V)) {
    unsigned int type_size = TD->getTypeAllocSize (AI->getAllocatedType());
    if (AI->isArrayAllocation()) {
      if (ConstantInt * CI = dyn_cast<ConstantInt>(AI->getArraySize())) {
        if (CI->getSExtValue() > 0) {
          type_size *= CI->getSExtValue();
        } else {
          return NULL;
        }
      } else {
        return NULL;
      }
    }
    return ConstantInt::get(Type::Int32Ty, type_size);
  }

  // Customized allocators

  if (CallInst * CI = dyn_cast<CallInst>(V)) {
    Function * F = CI->getCalledFunction();
    if (!F)
      return NULL;

    const std::string & name = F->getName();
    for (SAFECodeConfiguration::alloc_iterator it = SCConfig->alloc_begin(),
           end = SCConfig->alloc_end(); it != end; ++it) {
      if ((*it)->isAllocSizeMayConstant(CI) && (*it)->getAllocCallName() == name) {
        return (*it)->getAllocSize(CI);
      }
    }
  }

  return NULL;
}

char InsertSCIntrinsic::ID = 0;
static llvm::RegisterPass<InsertSCIntrinsic> X ("sc-insert-intrinsic", "insert SAFECode's intrinsic");

NAMESPACE_SC_END
