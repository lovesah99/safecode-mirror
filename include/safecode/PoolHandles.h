//===- PoolHandles.h - Find DSNodes and Pool Handles for SAFECode Passes -----//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements a pass that interfaces with the DSA and Pool Allocation
// passes to lookup both DSNode and Pool Handle information.  This
// functionality was moved to another pass as what it needs to do is different
// for different configurations of SAFECode.
//
//===----------------------------------------------------------------------===//

#ifndef POOLHANDLES_H
#define POOLHANDLES_H

#include "llvm/Instructions.h"
#include "llvm/Pass.h"
#include "safecode/Config/config.h"
#include "safecode/SAFECodeConfig.h"

#include "poolalloc/PoolAllocate.h"

#include "dsa/DSNode.h"

using namespace llvm;

NAMESPACE_SC_BEGIN

/// Passes that holds DSNode and Pool Handle information
  struct DSNodePass : public ModulePass {
	public :
    static char ID;
    DSNodePass () : ModulePass ((intptr_t) &ID) { }
    DSNodePass (intptr_t id) : ModulePass (id) { }
    const char *getPassName() const {
      return "DS Node And Pool Allocation Handle Pass";
    }
    virtual bool runOnModule(Module &M);
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequiredTransitive<PoolAllocateGroup>();
      if (SCConfig->DSAType == SAFECodeConfiguration::DSA_BASIC) {
        AU.addRequired<BasicDataStructures>();
      } else {
        AU.addRequired<EQTDDataStructures>();
      }
      AU.setPreservesAll();
    };

    //
    // Method: releaseMemory()
    //
    // Description:
    //  This method is called by the pass manager when the analysis results of
    //  this pass are either invalidated or no longer needed.  This method
    //  should free up memory and prepare the analysis pass for its next
    //  execution by the pass manager.
    //
    void releaseMemory () {
      //
      // Clear out the set of checked values and checked DSNodes.
      //
      CheckedValues.clear();
      CheckedDSNodes.clear();
    }

  public:
    // FIXME: Provide better interfaces
    PoolAllocateGroup * paPass;
    DSGraph * getDSGraph (Function & F);
    DSNode* getDSNode(const Value *V, Function *F);
    unsigned getDSNodeOffset(const Value *V, Function *F);
    Value * getPoolHandle(const Value *V, Function *F, PA::FuncInfo &FI, bool collapsed = true);

    void addCheckedDSNode(const DSNode * node);
    void addCheckedValue(const Value * value);
    bool isDSNodeChecked(const DSNode * node) const;
    bool isValueChecked(const Value * val) const;

  private:
    // Set of checked DSNodes
    std::set<const DSNode *> CheckedDSNodes;

    // The set of values that already have run-time checks
    std::set<const Value *> CheckedValues;
};

NAMESPACE_SC_END
#endif
