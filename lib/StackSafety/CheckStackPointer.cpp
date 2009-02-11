//===- StackSafety.cpp: - Analysis for Ensuring Stack Safety --------------===//
//
//                          The SAFECode Compiler
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of StackSafety.h
//
//===----------------------------------------------------------------------===//

#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/BasicBlock.h"
#include "llvm/Type.h"
#include "llvm/Pass.h"
#include "llvm/Support/InstIterator.h"
#include "StackSafety.h"
#include <iostream>

using namespace llvm;
 
using namespace CSS;

char checkStackSafety::ID = 0;

RegisterPass<checkStackSafety> css("css1", "check stack safety");



bool checkStackSafety::markReachableAllocas(DSNode *DSN, bool start) {
  reachableAllocaNodes.clear();
  return   markReachableAllocasInt(DSN,start);
}

bool checkStackSafety::markReachableAllocasInt(DSNode *DSN, bool start) {
  bool returnValue = false;
  reachableAllocaNodes.insert(DSN);
  if (!start && DSN->isAllocaNode()) {
    returnValue =  true;
    AllocaNodes.push_back(DSN);
  }
  for (unsigned i = 0, e = DSN->getSize(); i < e; i += DS::PointerSize)
    if (DSNode *DSNchild = DSN->getLink(i).getNode()) {
      if (reachableAllocaNodes.find(DSNchild) != reachableAllocaNodes.end()) {
        continue;
      } else if (markReachableAllocasInt(DSNchild)) {
        returnValue = returnValue || true;
      }
    }
  return returnValue;
}

bool checkStackSafety::runOnModule(Module &M) {
  //  TDDataStructures *TDDS;
  //  TDDS = &getAnalysis<TDDataStructures>();
  EQTDDataStructures *BUDS;
  BUDS = &getAnalysis<EQTDDataStructures>();
  Function *MainFunc = M.getFunction("main") ? M.getFunction("main")
                                             : M.getFunction ("MAIN__");
  for (Module::iterator MI = M.begin(), ME = M.end(); MI != ME; ++MI) {
    Function &F = *MI;
    if (&F != MainFunc) {
      if (!F.isDeclaration()) {
        DSGraph * BUG = BUDS->getDSGraph(F);
	
        // check if return value is a  pointers
        if (isa<PointerType>(F.getReturnType())) {
          //return value type is a pointer
          for (inst_iterator ii = inst_begin(F), ie = inst_end(&F);
                             ii != ie;
                             ++ii) {
            if (ReturnInst *RI = dyn_cast<ReturnInst>(&*ii)) {
              DSNode *DSN = BUG->getNodeForValue(RI).getNode();
              if (DSN && markReachableAllocas(DSN)) {
                std::cerr << "Instruction : \n" << RI
                          << "points to a stack location\n";
                std::cerr << "In Function " << F.getName() << "\n";
                return false;
              }
            }
          }
        }
    
        Function::arg_iterator AI = F.arg_begin(), AE = F.arg_end();
        for (; AI != AE; ++AI) {
          if (isa<PointerType>(AI->getType())) {
            DSNode *DSN = BUG->getNodeForValue(AI).getNode();
            if (markReachableAllocas(DSN,true)) {
              std::cerr << "Instruction : \n" << AI
                        << "points to a stack location\n";
              std::cerr << "In Function " << F.getName() << "\n";
            }
          }
        }
	
        // Also, mark allocas pointed to by globals
        DSGraph::node_iterator DSNI = BUG->node_begin(), DSNE = BUG->node_end();
	
        for (; DSNI != DSNE; ++DSNI) {
          if (DSNI->isGlobalNode()) {
            if (markReachableAllocas(DSNI)) {
              std::cerr << "Global points to a stack location\n";
              std::cerr << "In Function " << F.getName() << "\n";
            }
          }
        }
      }
    }
  }
  return false;
}


Pass *createStackSafetyPass() { return new CSS::checkStackSafety(); }



