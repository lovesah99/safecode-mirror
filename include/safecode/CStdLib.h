//===------------ CStdLib.h - Secure C standard library calls -------------===//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass finds all calls to functions in the C standard library and
// transforms them to a more secure form.
//
//===----------------------------------------------------------------------===//

#ifndef CSTDLIB_H
#define CSTDLIB_H

#include "llvm/Analysis/Passes.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/InstVisitor.h"

#include "safecode/PoolHandles.h"
#include "safecode/SAFECode.h"

#include <algorithm>
#include <vector>

using namespace llvm;

NAMESPACE_SC_BEGIN

// Statistics counters
STATISTIC(stat_transform_strcpy, "Total strcpy() calls transformed");

/**
 * Pass that secures C standard library string calls via transforms
 */
class StringTransform : public ModulePass {
private:
  bool strcpyTransform(Module &M);

public:
  static char ID;
  StringTransform() : ModulePass((intptr_t)&ID) {}
  virtual bool runOnModule(Module &M);

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    // Pretend that we don't modify anything
    AU.setPreservesAll();

    // We require these passes to get information on pool handles
    AU.addRequired<DSNodePass>();
    AU.addRequired<PoolAllocateGroup>();
    AU.addRequired<TDDataStructures>();
  }

  virtual void print(std::ostream &O, const Module *M) const {}
};

NAMESPACE_SC_END

#endif
