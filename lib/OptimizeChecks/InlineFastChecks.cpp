//===- InlineFastChecks.cpp - Inline Fast Checks -------------------------- --//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass replaces calls to fastlscheck within inline code to perform the
// check.  It is designed to provide the advantage of libLTO without libLTO.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "inline-fastchecks"

#include "llvm/ADT/Statistic.h"
#include "llvm/Constants.h"
#include "llvm/Instruction.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <vector>

namespace {
  STATISTIC (Inlined, "Number of Fast Checks Inlined");
}

namespace llvm {
  //
  // Pass: InlineFastChecks
  //
  // Description:
  //  This pass inlines fast checks to make them faster.
  //
  struct InlineFastChecks : public ModulePass {
   public:
    static char ID;
    InlineFastChecks() : ModulePass(ID) {}
     virtual bool runOnModule (Module & M);
     const char *getPassName() const {
       return "Inline fast checks transform";
     }
    
     virtual void getAnalysisUsage(AnalysisUsage &AU) const {
       AU.addRequired<TargetData>();
       return;
     }

   private:
     // Private methods
     bool inlineCheck (Function * F);
     bool createBodyFor (Function * F);
  };
}

using namespace llvm;

//
// Method: findChecks()
//
// Description:
//  Find the checks that need to be inlined.
//
// Return value:
//  true  - One or more calls to the check were inlined.
//  false - No calls to the check were inlined.
//
bool
llvm::InlineFastChecks::inlineCheck (Function * F) {
  //
  // Get the runtime function in the code.  If no calls to the run-time
  // function were added to the code, do nothing.
  //
  if (!F) return false;

  //
  // Iterate though all calls to the function and search for pointers that are
  // checked but only used in comparisons.  If so, then schedule the check
  // (i.e., the call) for removal.
  //
  bool modified = false;
  std::vector<CallInst *> CallsToInline;
  for (Value::use_iterator FU = F->use_begin(); FU != F->use_end(); ++FU) {
    //
    // We are only concerned about call instructions; any other use is of
    // no interest to the organization.
    //
    if (CallInst * CI = dyn_cast<CallInst>(*FU)) {
      //
      // If the call instruction has no uses, we can remove it.
      //
      if (CI->use_begin() == CI->use_end())
        CallsToInline.push_back (CI);
    }
  }

  //
  // Update the statistics and determine if we will modify anything.
  //
  if (CallsToInline.size()) {
    modified = true;
    Inlined += CallsToInline.size();
  }

  //
  // Inline all of the fast calls we found.
  //
  TargetData & TD = getAnalysis<TargetData>();
  InlineFunctionInfo IFI (0, &TD);
  for (unsigned index = 0; index < CallsToInline.size(); ++index) {
    InlineFunction (CallsToInline[index], IFI);
  }

  return modified;
}

//
// Function: createFaultBlock()
//
// Description:
//  Create a basic block which will cause the program to terminate.
//
static BasicBlock *
createFaultBlock (Function & F) {
  //
  // Create the basic block.
  //
  BasicBlock * faultBB = BasicBlock::Create (F.getContext(), "fault", &F);

  //
  // Terminate the basic block with an unreachable instruction.
  //
  Instruction * UI = new UnreachableInst (F.getContext(), faultBB);

  //
  // Add an instruction that will generate a trap.
  //
  LLVMContext & Context = F.getContext();
  Module * M = F.getParent();
  M->getOrInsertFunction ("abort", Type::getVoidTy (Context), 0);
  CallInst::Create (M->getFunction ("abort"), "", UI);

  return faultBB;
}


// fastlscheck (const char *base, const char *result, unsigned size,
//              unsigned lslen) {
bool
llvm::InlineFastChecks::createBodyFor (Function * F) {
  //
  // If the function does not exist, do nothing.
  //
  if (!F) return false;

  //
  // If the function has a body, do nothing.
  //
  if (!(F->isDeclaration())) return false;

  //
  // Create an entry block that will perform the comparisons and branch either
  // to the success block or the fault block.
  //
  LLVMContext & Context = F->getContext();
  BasicBlock * entryBB = BasicBlock::Create (Context, "entry", F);

  //
  // Create a basic block that just returns.
  //
  BasicBlock * goodBB = BasicBlock::Create (Context, "pass", F);
  ReturnInst::Create (F->getContext(), goodBB);

  //
  // Create a basic block that handles the run-time check failures.
  //
  BasicBlock * faultBB = createFaultBlock (*F);

  //
  // Add instructions to the entry block to perform the pointer comparisons
  // and to branch to the good or fault blocks, respectively.
  // 
  Function::arg_iterator arg = F->arg_begin();
  Value * Base = arg++;
  Value * Result = arg++;
  Value * Size = arg++;
  ICmpInst * Compare1 = new ICmpInst (*entryBB,
                                   CmpInst::ICMP_ULE,
                                   Base,
                                   Result,
                                   "cmp1");

  TargetData & TD = getAnalysis<TargetData>();
  Value * BaseInt = new PtrToIntInst (Base,
                                    TD.getIntPtrType(Context), "tmp", entryBB);
  Value * SizeInt = new ZExtInst (Size, TD.getIntPtrType(Context), "size", entryBB);
  Value * LastByte = BinaryOperator::Create (Instruction::Add,
                                             BaseInt,
                                             SizeInt,
                                             "lastbyte",
                                             entryBB);
  Value * PtrInt = new PtrToIntInst (Result,
                                    TD.getIntPtrType(Context), "tmp", entryBB);
  Value * Compare2 = new ICmpInst (*entryBB,
                                   CmpInst::ICMP_ULT,
                                   PtrInt,
                                   LastByte,
                                   "cmp2");

  Value * Sum = BinaryOperator::Create (Instruction::And, Compare1, Compare2, "and", entryBB);

  BranchInst::Create (goodBB, faultBB, Sum, entryBB);

  //
  // Make the function internal.
  //
  F->setLinkage (GlobalValue::InternalLinkage);
  return true;
}

bool
llvm::InlineFastChecks::runOnModule (Module & M) {
  //
  // Create a function body for the fastlscheck call.
  //
  createBodyFor (M.getFunction ("fastlscheck"));

  //
  // Search for call sites to the function and forcibly inline them.
  //
  inlineCheck (M.getFunction ("fastlscheck"));
  return true;
}

namespace llvm {
  char InlineFastChecks::ID = 0;

  static RegisterPass<InlineFastChecks>
  X ("inline-fastchecks", "Inline fast run-time checks", true);

  ModulePass * createInlineFastChecksPass (void) {
    return new InlineFastChecks();
  }
}
