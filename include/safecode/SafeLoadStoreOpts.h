//===- SafeLoadStoreOpts.cpp - Optimize Safe Load/Store Checks ----*- C++ -*--//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass removes load/store checks that are known to be safe statically.
//
//===----------------------------------------------------------------------===//

#ifndef SAFECODE_SAFELOADSTOREOPTS_H
#define SAFECODE_SAFELOADSTOREOPTS_H

#include "safecode/SAFECode.h"
#include "safecode/Intrinsic.h"

#include "poolalloc/PoolAllocate.h"

#include "llvm/Pass.h"

using namespace llvm;

NAMESPACE_SC_BEGIN

//
// Pass: OptimizeSafeLoadStore
//
// Description:
//  This pass removes run-time checks on loads and stores that are statically
//  known to be safe.  It does this for loads and stores on type-safe memory
//  objects as well as loads and stores that are trivially safe (e.g., loads to
//  the first byte of a global variable).
//
struct OptimizeSafeLoadStore : public ModulePass {
  protected:
    DSNodeHandle getDSNodeHandle (const Value * V, const Function * F);

  public:
    static char ID;
    OptimizeSafeLoadStore() : ModulePass((intptr_t)(&ID)) {}
    virtual bool runOnModule (Module & M);

    const char *getPassName() const {
      return "Optimize SAFECode Load/Store Checks";
    }

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<InsertSCIntrinsic>();
      AU.addRequired<EQTDDataStructures>();
      AU.setPreservesCFG();
    }
};

NAMESPACE_SC_END
#endif

