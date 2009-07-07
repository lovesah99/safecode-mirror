//===- convert.h - Promote unsafe alloca instructions to heap allocations ----//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements a pass that promotes unsafe stack allocations to heap
// allocations.  It also updates the pointer analysis results accordingly.
//
// This pass relies upon the abcpre, abc, and checkstack safety passes.
//
//===----------------------------------------------------------------------===//

#ifndef CONVERT_ALLOCA_H
#define CONVERT_ALLOCA_H

#include "dsa/DataStructure.h"
#include "llvm/Pass.h"
#include "ArrayBoundsCheck.h"
#include "StackSafety.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Target/TargetData.h"
#include "safecode/Config/config.h"
#include "safecode/PoolHandles.h"

#include "poolalloc/PoolAllocate.h"

#include <set>

NAMESPACE_SC_BEGIN

ModulePass *createConvertUnsafeAllocas();

using namespace CSS;

//
// Pass: InitAllocas
//
// Description:
//  This pass ensures that uninitialized pointers within stack allocated
//  (i.e., alloca'ed) memory cannot be dereferenced to cause a memory error.
//  This can be done either by promoting the stack allocation to a heap
//  allocation (since the heap allocator must provide similar protection for
//  heap allocated memory) or be inserting special initialization code.
//
struct InitAllocas : public FunctionPass {
  private:
    // Private data
    Constant * memsetF;
    DominatorTree * domTree;
    DSNodePass * dsnPass;
    PoolAllocateGroup * paPass;

    // The type of a pool descriptor
    const Type * PoolType;

    // Private methods
    inline bool changeType (Instruction * Inst);
    inline bool TypeContainsPointer(const Type *Ty);

  public:
    static char ID;
    InitAllocas() : FunctionPass((intptr_t)(&ID)) {}
    const char *getPassName() const { return "Malloc Pass"; }
    virtual bool runOnFunction (Function &F);
    virtual bool doInitialization (Module &M);
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<TargetData>();
      AU.addRequired<PoolAllocateGroup>();
      AU.addRequired<DSNodePass>();
      DSNodePass::getAnalysisUsageForDSA(AU);
      AU.setPreservesCFG();
      AU.setPreservesAll();
    }
};

namespace CUA {
//
// Pass: ConvertUnsafeAllocas
//
// Description:
//  This pass promotes stack allocations to heap allocations if necessary to
//  provide memory safety. 
//
struct ConvertUnsafeAllocas : public ModulePass {
  public:
    static char ID;
    ConvertUnsafeAllocas (intptr_t IDp = (intptr_t) (&ID)) : ModulePass (IDp) {}
    const char *getPassName() const { return "Convert Unsafe Allocas"; }
    virtual bool runOnModule(Module &M);
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<TargetData>();
      AU.addRequired<DominatorTree>();
      AU.addRequired<DominanceFrontier>();

      AU.addRequired<ArrayBoundsCheckGroup>();
      AU.addRequired<checkStackSafety>();
      DSNodePass::getAnalysisUsageForDSA(AU);

      AU.addPreserved<ArrayBoundsCheckGroup>();
      AU.addPreserved<EQTDDataStructures>();
      AU.setPreservesCFG();

    }

    DSNode * getDSNode(const Value *I, Function *F);
    DSNode * getTDDSNode(const Value *I, Function *F);

    // The set of Malloc Instructions that are a result of conversion from
    // alloca's due to static array bounds detection failure
    std::set<const MallocInst *>  ArrayMallocs;

  protected:
    TargetData         * TD;
    EQTDDataStructures * budsPass;
    ArrayBoundsCheckGroup   * abcPass;
    checkStackSafety   * cssPass;

#ifdef LLVA_KERNEL
    Constant *kmalloc;
    Constant *StackPromote;
#endif

    std::list<DSNode *> unsafeAllocaNodes;
    std::set<DSNode *> reachableAllocaNodes; 

    bool markReachableAllocas(DSNode *DSN);
    bool markReachableAllocasInt(DSNode *DSN);
    void TransformAllocasToMallocs(std::list<DSNode *> & unsafeAllocaNodes);
    void TransformCSSAllocasToMallocs(Module & M, std::set<DSNode *> & cssAllocaNodes);
    void getUnsafeAllocsFromABC(Module &M);
    void TransformCollapsedAllocas(Module &M);
    virtual void InsertFreesAtEnd(MallocInst *MI);
    virtual Value * promoteAlloca(AllocaInst * AI, DSNode * Node);
};

//
// Struct: PAConvertUnsafeAllocas 
//
// Description:
//  This is an LLVM transform pass that is similar to the original
//  ConvertUnsafeAllocas pass.  However, instead of promoting unsafe stack
//  allocations to malloc instructions, it will promote them to use special
//  allocation functions within the pool allocator run-time.
//
// Notes:
//  o) By using the pool allocator run-time, this pass should generate faster
//     code than the original ConvertUnsafeAllocas pass.
//  o) This pass requires that a Pool Allocation pass be executed before this
//     transform is executed.
//
struct PAConvertUnsafeAllocas : public ConvertUnsafeAllocas {
  private:
    PoolAllocateGroup * paPass;

  protected:
    virtual void InsertFreesAtEndNew(Value * PH, Instruction  *MI);
    virtual Value * promoteAlloca(AllocaInst * AI, DSNode * Node);

  public:
    static char ID;
    PAConvertUnsafeAllocas () : ConvertUnsafeAllocas ((intptr_t)(&ID)) {}
    virtual bool runOnModule(Module &M);
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<ArrayBoundsCheckGroup>();
      AU.addRequired<checkStackSafety>();

      DSNodePass::getAnalysisUsageForDSA(AU);

      AU.addRequired<TargetData>();
      AU.addRequired<DominatorTree>();
      AU.addRequired<DominanceFrontier>();

      AU.addPreserved<ArrayBoundsCheckGroup>();
      AU.addPreserved<PoolAllocateGroup>();

    }
};

}

NAMESPACE_SC_END
 
#endif
