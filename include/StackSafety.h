//===- StackSafety.h                                      -*- C++ -*---------=//
//
// This file defines checks for stack safety
// 
//===----------------------------------------------------------------------===//

#ifndef LLVM_STACKSAFETY_H
#define LLVM_STACKSAFETY_H

#include "dsa/DataStructure.h"
#include "dsa/DSGraph.h"
#include "dsa/DSNode.h"
#include "llvm/Pass.h"

#include <set>

namespace llvm {

  ModulePass* createStackSafetyPass();
  
  namespace CSS {
    
    struct checkStackSafety : public ModulePass {
      
    public :
      static char ID;
      checkStackSafety() : ModulePass ((intptr_t) &ID) {}
      std::vector<DSNode *> AllocaNodes;
      const char *getPassName() const { return "Stack Safety Check";}
      virtual bool runOnModule(Module &M);
      virtual void getAnalysisUsage(AnalysisUsage &AU) const {
        AU.addRequired<EQTDDataStructures>();
        AU.setPreservesAll();
      }
    private :
      std::set<DSNode *> reachableAllocaNodes; 
      bool markReachableAllocas(DSNode *DSN, bool start=false);
      bool markReachableAllocasInt(DSNode *DSN, bool start=false);
    };
  }
}
#endif
