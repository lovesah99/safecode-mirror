//===- ArrayBoundCheck.cpp - Static Array Bounds Checking --------------------//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass uses a constraint solver (Omega) to verify that array indexing
// operations are safe.  This pass uses control dependence and post dominance
// frontier to generate constraints.
//
// FIXME:
//  The interface for this pass should probably be changed so that multiple
//  implementations can be plugged in providing different tradeoffs between
//  accuracy and efficiency.  For example, a FunctionPass interface could be
//  used to query whether a GEP requires a run-time check.  This implementation
//  would have said FunctionPass query a ModulePass that performs the
//  constraint solving; other intraprocedural implementations could be
//  simple FunctionPass'es.
//
//===----------------------------------------------------------------------===//

#include <unistd.h>

#define DEBUG_TYPE "static-abc"

#include "dsa/DSGraph.h"
#include "utils/fdstream.h"
#include "llvm/Pass.h"
#include "llvm/Module.h"
#include "llvm/BasicBlock.h"
#include "ArrayBoundsCheck.h"
#include "SCUtils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Constants.h"
#include "llvm/Analysis/LoopInfo.h"
#include "omega.h"
#include "llvm/ADT/VectorExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Support/CommandLine.h"
#include "safecode/Config/config.h"
#include <fstream>
#include <iostream>
#include <stdio.h>
#include <fcntl.h>
#include <sys/wait.h>

// Pathname to the Omega compiler
#define OMEGA "/home/vadve/dhurjati/bin/oc"

// Pathname of include file generated for input into the Omega compiler
#define OMEGA_TMP_INCLUDE_FILE "omega_include.ip"

using namespace llvm;

NAMESPACE_SC_BEGIN

char ArrayBoundsCheck::ID = 0;

namespace {
  STATISTIC(SafeGEPs,    "GEPs proved safe via Omega");
  STATISTIC(SafeStructs, "Structures in GEPs that are deemed safe");
  STATISTIC(TotalStructs, "Total structures used in GEPs");

  cl::opt<bool> NoStructChecks ("disable-structchecks", cl::Hidden,
                                cl::init(true),
                                cl::desc("Disable Checks on Structure Indices"));

  cl::opt<string> OmegaFilename("omegafile",
                                cl::desc("Specify omega include filename"),
                                cl::init(OMEGA_TMP_INCLUDE_FILE),
                                cl::value_desc("filename"));
}
std::ostream &Out = std::cerr;
std::ofstream includeOut;

// The following are filled from the preprocess pass, since they require
// function passes.
extern IndVarMap indMap; 

// Count the number of problems given to Omega
static  unsigned countA = 0;

// This will tell us whether the collection of constraints
// depends on the incoming args or not
// Do we need this to be global?
static bool reqArgs = false;

// A hack for LLVM's malloc instruction which concerts all ints to uints
// This is not really necessary, as it is checked in the pool allocation
// run-time library 
static bool fromMalloc = false;
/*
static RegisterPass<ArrayBoundsCheck> X ("abc-omega", "Interprocedural Array Bounds Check pass");
static RegisterAnalysisGroup<ArrayBoundsCheckGroup, true> ABCGroup(X);
*/

//
// Method: initialize()
//
// Description:
//  Perform some preliminary initialization.
//
void
ArrayBoundsCheck::initialize () {
  KnownFuncDB.insert("snprintf"); //added the format string & string check
  KnownFuncDB.insert("strcpy"); //need to add the extra checks 
  KnownFuncDB.insert("memcpy"); //need to add the extra checks 
  KnownFuncDB.insert("llvm.memcpy"); //need to add the extra checks 
  KnownFuncDB.insert("strlen"); //Gives return value constraints 
  KnownFuncDB.insert("read"); // read requires checks and return value constraints
  KnownFuncDB.insert("fread"); //need to add the extra checks 

  KnownFuncDB.insert("fprintf"); //need to check if it is not format string
  KnownFuncDB.insert("printf"); //need to check if it is not format string 
  KnownFuncDB.insert("vfprintf"); //need to check if it is not format string 
  KnownFuncDB.insert("syslog"); //need to check if it is not format string 

  KnownFuncDB.insert("memset"); //need to check if we are not setting outside
  KnownFuncDB.insert("llvm.memset"); //need to check if we are not setting outside
  KnownFuncDB.insert("gets"); // need to check if the char array is greater than 80
  KnownFuncDB.insert("strchr"); //FIXME check has not been added yet 
  KnownFuncDB.insert("sprintf"); //FIXME to add extra checks
  KnownFuncDB.insert("fscanf"); //Not sure if it requires a check

  //Not sure if the following require any checks. 
  KnownFuncDB.insert("llvm.va_start");
  KnownFuncDB.insert("llvm.va_end");
  
  //The following doesnt require checks
  KnownFuncDB.insert("random");
  KnownFuncDB.insert("rand");
  KnownFuncDB.insert("clock");
  KnownFuncDB.insert("exp");
  KnownFuncDB.insert("fork");
  KnownFuncDB.insert("wait");
  KnownFuncDB.insert("fflush");
  KnownFuncDB.insert("fclose");
  KnownFuncDB.insert("alarm");
  KnownFuncDB.insert("signal");
  KnownFuncDB.insert("setuid");
  KnownFuncDB.insert("__errno_location");
  KnownFuncDB.insert("log");
  KnownFuncDB.insert("srand48");
  KnownFuncDB.insert("drand48");
  KnownFuncDB.insert("lrand48");
  KnownFuncDB.insert("times"); 
  KnownFuncDB.insert("puts");
  KnownFuncDB.insert("putchar");
  KnownFuncDB.insert("strcmp");
  KnownFuncDB.insert("strtol");
  KnownFuncDB.insert("fopen");
  KnownFuncDB.insert("fwrite");
  KnownFuncDB.insert("fgetc");
  KnownFuncDB.insert("getc");
  KnownFuncDB.insert("open");
  KnownFuncDB.insert("feof");
  KnownFuncDB.insert("fputc");
  KnownFuncDB.insert("atol");
  KnownFuncDB.insert("atoi");
  KnownFuncDB.insert("atof");
  KnownFuncDB.insert("exit");
  KnownFuncDB.insert("perror");
  KnownFuncDB.insert("sqrt");
  KnownFuncDB.insert("floor");
  KnownFuncDB.insert("pow");
  KnownFuncDB.insert("abort");
  KnownFuncDB.insert("srand");
  KnownFuncDB.insert("perror");
  KnownFuncDB.insert("__isnan");
  KnownFuncDB.insert("__main");
  KnownFuncDB.insert("ceil");
}

//
// Method: outputDeclsForOmega()
//
// Description:
//  Output the variables from the module that will be included in every call to
//  the Omega compiler.
//
void
ArrayBoundsCheck::outputDeclsForOmega (Module& M) {
  //
  // Generate Omega variables for argv and argc.
  //
  // FIXME:
  //  For what purpose is the Unknown variable?
  //
  includeOut << "symbolic   Unknown;\n"
             << "symbolic   argc;\n"
             << "symbolic   argv;\n";

  //
  // Create an Omega variable for each global variable.
  //
  Module::global_iterator gI = M.global_begin(), gE = M.global_end();
  for (; gI != gE; ++gI) {
    includeOut << "symbolic   " << getValueName((gI)) << ";\n";
    if (const ArrayType *AT = dyn_cast<ArrayType>(gI->getType()->getElementType())) {
      printarraytype(getValueName(gI), AT);
    }
  }

  for (Module::iterator FI = M.begin(), FE = M.end(); FI != FE; ++FI) {
    // For sanity, change the iterator into an actual Function pointer
    Function *F = FI;

    //
    // Create an Omega variable for the function's name.
    //
    includeOut << "symbolic " << getValueName(F) <<"; \n";

    //
    // Create an Omega variable for each parameter of the function.
    //
    Function::ArgumentListType::iterator aI=F->getArgumentList().begin(),
                                         aE=F->getArgumentList().end();
    for (; aI != aE; ++aI) {
      includeOut << "symbolic   " << getValueName((aI)) << ";\n";
    }

    //
    // Create an Omega variable for each non-void instruction within the
    // function.
    //
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      if ((&*I)->getType() != Type::VoidTy) {
        includeOut << "symbolic   "
                   << getValueName(&*I)
                   << ";\n";

        if (AllocationInst *AI = dyn_cast<AllocationInst>(&*I)) {
          // We have to see the dimension of the array that this alloca is
          // pointing to
          // If the allocation is done by constant, then its a constant array
          // else its a normal alloca which we already have taken care of  
          if (const ArrayType *AT = dyn_cast<ArrayType>(AI->getType()->getElementType())) {
            printarraytype(getValueName(&*I), AT);
          }
        }
      }
    }
  }
}

//
// Method: getValueName()
//
// Description:
//  Provide a name for the specified LLVM Value that is acceptable as a
//  variable name for the Omega Calculator.
//
// Inputs:
//  V - The LLVM value for which to generate a name.
//
// Return value:
//  A managled name that can be used as a variable name in the Omega
//  Calculator.
//
std::string
ArrayBoundsCheck::getValueName(const Value *V) {
  //
  // Mangle the name.  Let the mangler handle the conversion of the name into
  // a format acceptable for the constraint solver.
  //
  return (Mang->getValueName(V));
}

void ArrayBoundsCheck::printarraytype(string var,const ArrayType  *T) {
  string var1 = var + "_i";
  includeOut << "symbolic   " << var1 << ";\n";
  if (const ArrayType *AT = dyn_cast<ArrayType>(T->getElementType())) {
    printarraytype(var1,AT);
  }
}

//
// Method: getReturnValueConstraints()
//
// Description:
//  Get the constraints for the return value of the specified function.
//
ABCExprTree*
ArrayBoundsCheck::getReturnValueConstraints (Function *f) {
  bool localSave = reqArgs;
  const Type* csiType = Type::Int32Ty;
  const Constant * signedzero = ConstantInt::get(csiType,0);
  string var = "0";
  Constraint *c = new Constraint(var, new LinearExpr(signedzero, Mang),"=");
  ABCExprTree *root = new ABCExprTree(c); //dummy constraint 
  Function::iterator bI = f->begin(), bE = f->end();
  for (;bI != bE; ++bI) {
    BasicBlock *bb = bI;
    if (ReturnInst *RI = dyn_cast<ReturnInst>(bb->getTerminator()))  
      getConstraints(RI,&root);
  }
  reqArgs = localSave ; //restore to the original
  return root;
}

//
// Method: addFormalToActual()
//
// Description:
//  Add constraints to the constraint expression stating that the actual
//  parameters in the call site match the formal arguments in the function
//  definition.
//
void
ArrayBoundsCheck::addFormalToActual (Function *Fn,
                                     CallInst *CI,
                                     ABCExprTree **rootp) {
  LinearExpr *le1 = new LinearExpr(CI,Mang);
  Constraint *c1 = new Constraint(getValueName(Fn),le1,"=");
  *rootp = new ABCExprTree(*rootp,new ABCExprTree(c1),"&&");
  
  Function::arg_iterator formalArgCurrent = Fn->arg_begin(),
                         formalArgEnd     = Fn->arg_end();
  for (unsigned i = 1;
       formalArgCurrent != formalArgEnd;
       ++formalArgCurrent, ++i) {
    string varName = getValueName(formalArgCurrent);
    Value *OperandVal = CI->getOperand(i);
    LinearExpr *le = new LinearExpr(OperandVal,Mang);
    Constraint* c1 = new Constraint(varName,le,"=");
    ABCExprTree *temp = new ABCExprTree(c1);
    *rootp = new ABCExprTree(*rootp, temp, "&&"); //and of all arguments
  }
}

//
// Method: getConstraintsAtCallSite()
//
// Description:
//  This is an auxillary function used by getConstraints().  It gets the
//  constraints on the return value in terms of its arguments and creates a
//  conjunction with these new constraints and the constraints specified in
//  rootp.
//
// Inputs:
//  CI - The Call instruction
//  rootp - A reference to the pre-defined constraints.
//
// Outputs:
//  rootp - This constraint expression is updated to have the new constraints
//          discovered by this method.
//
// Notes:
//  FIXME: The astute observer will notice that the code always assumes that
//         direct function calls to functions with no body are in the set of
//         pre-defined known functions while the code for indirect function
//         calls checks to see that the target function is also in KnownFuncDB.
//         This is probably a bug.
//
//  FIXME: Is ignoring recursively called functions safe?
//
void
ArrayBoundsCheck::getConstraintsAtCallSite (CallInst *CI, ABCExprTree **rootp) {
  //
  // Process direct and indirect calls differently.
  //
  if (Function *pf = dyn_cast<Function>(CI->getOperand(0))) {
    //
    // If the target function is not defined, it may be a function for which
    // pre-defined constraints already exist.
    //
    if (pf->isDeclaration()) {
      *rootp = new ABCExprTree (*rootp,
                                addConstraintsForKnownFunctions(pf, CI),
                                "&&");
      addFormalToActual (pf, CI, rootp);
    } else {
      //
      // FIXME:
      //  Do not process functions that are called recursively i.e., are in
      //  an SCC.
      //
      if (buCG->isInSCC(pf)) {
        std::cerr << "Ignoring return values on function in recursion\n";
        return; 
      }

      //
      // Create new constraints for the return value of the function.
      //
      *rootp = new ABCExprTree(*rootp,getReturnValueConstraints(pf), "&&");
      addFormalToActual(pf, CI, rootp);
    }

    //
    // Now get the constraints on the actual arguments for the original call
    // site.
    //
    for (unsigned i =1; i < CI->getNumOperands(); ++i) 
      getConstraints (CI->getOperand(i), rootp);
  } else {
    // Handle Indirect Calls

    ABCExprTree *temproot = 0;

    // Loop over all of the possible targets of the call instruction
    EQTDDataStructures::callee_iterator I = cbudsPass->callee_begin(CI),
                                        E = cbudsPass->callee_end(CI);
    //    assert((I != E) && "Indirect Call site doesn't have targets ???? ");
    //Actually thats fine, we ignore the return value constraints ;)

    for (; I != E; ++I) {
      // Get the function called by the indirect function call
      Function * Target = (Function *)(*I);

      //
      // If the function is externally declared or if it is a specially
      // recognized function, then handle it specially.
      //
      if ((Target->isDeclaration()) ||
          (KnownFuncDB.find(Target->getName()) != KnownFuncDB.end()) ) {
        ABCExprTree * temp = addConstraintsForKnownFunctions(Target, CI);
        addFormalToActual(Target, CI, &temp);
        if (temproot) {
          // We need to or them 
          temproot = new ABCExprTree(temproot, temp, "||");
        } else {
          temproot = temp;
        }
      } else {
        if (buCG->isInSCC(Target)) {
          std::cerr << "Ignoring return values on function in recursion\n";
          return;
        }
        ABCExprTree * temp = getReturnValueConstraints(Target);
        addFormalToActual(Target, CI, &temp);
        if (temproot) {
          temproot = new ABCExprTree(temproot, temp, "||");
        } else {
          temproot = temp;
        }
      }
    }
    if (temproot) {
      *rootp = new ABCExprTree(*rootp, temproot, "&&");
      //
      // Now get the constraints on the actual arguments for the original
      // call site 
      //
      for (unsigned i =1; i < CI->getNumOperands(); ++i) {
        getConstraints(CI->getOperand(i),rootp);
      }
    }
  }
}

void
ArrayBoundsCheck::addControlDependentConditions (BasicBlock *currentBlock,
                                                 ABCExprTree **rootp) {
  PostDominanceFrontier::const_iterator it = postdomFrontier->find(currentBlock);
  if (it != postdomFrontier->end()) {
    const PostDominanceFrontier::DomSetType &S = it->second;
    if (S.size() > 0) {
      PostDominanceFrontier::DomSetType::iterator pCurrent = S.begin(),
                                                  pEnd     = S.end();
      //check if it is control dependent on only one node.
      //If it is control dependent on only one node.
      //If it not, then there must be only one that dominates this node and
      //the rest should be dominated by this node.
      //or this must dominate every other node (incase of do while)
      bool dominated = false; 
      bool rdominated = true; //to check if this dominates every other node
      for (; pCurrent != pEnd; ++pCurrent) {
        if (*pCurrent == currentBlock) {
          rdominated = rdominated & true;
          continue;
        }
        if (!dominated) {
          if (domTree->dominates(*pCurrent, currentBlock)) {
            dominated = true;
            rdominated = false;
            continue;
          }
        }
        if (domTree->dominates(currentBlock, *pCurrent)) {
          rdominated = rdominated & true;
          continue;
        } else {
#if 0
          out << "In function " << currentBlock->getParent()->getName();
          out << "for basic block " << currentBlock->getName();
          out << "Something wrong .. non affine or unstructured control flow ??\n";
#endif
          dominated = false;
          break;
        }
      }
      if ((dominated) || (rdominated)) {
        // Now we are sure that the control dominance is proper
        // i.e. it doesn't have unstructured control flow 
        
        PostDominanceFrontier::DomSetType::iterator pdCurrent = S.begin(),
                                                    pdEnd     = S.end();
        for (; pdCurrent != pdEnd; ++pdCurrent) {
          BasicBlock *CBB = *pdCurrent;
          if (DoneList.find(CBB) == DoneList.end()) {
            TerminatorInst *TI = CBB->getTerminator();
            if (BranchInst *BI = dyn_cast<BranchInst>(TI)) {
              for (unsigned index = 0; index < BI->getNumSuccessors(); ++index) {
                BasicBlock * succBlock = BI->getSuccessor(index);
                if (postdomTree->properlyDominates(currentBlock, succBlock)) {
                  DoneList.insert(CBB);
                  addControlDependentConditions(CBB,rootp);
                  addBranchConstraints(BI, BI->getSuccessor(index), rootp);
                  break;
                }
              }
            }
          }
        }
      }
    }
  }
}

//
// Method: addConstraintsForKnownFunctions()
//
// Description:
//  Generates constraints for a call instruction to a pre-defined function
//  (this function is usually either an LLVM intrinsic or a standard libc
//  function).
//
// Inputs:
//  kf - The called function (this could be the target of an indirect function
//       call).
//  CI - The call instruction (can be an indirect call; hence the need for kf).
//
// Return value:
//  The constraint expression for the call instruction is returned.
//
ABCExprTree*
ArrayBoundsCheck::addConstraintsForKnownFunctions (Function *kf, CallInst *CI) {
  const Type* csiType = Type::Int32Ty;
  const Constant * signedzero = ConstantInt::get(csiType,0);
  string var = "0";
  Constraint *c = new Constraint(var, new LinearExpr(signedzero, Mang),"=");
  ABCExprTree *root = new ABCExprTree(c); //dummy constraint 
  ABCExprTree **rootp = &root;
  string funcName = kf->getName();
  if (funcName == "memcpy") {
    string var = getValueName(CI->getOperand(1));
    LinearExpr *l1 = new LinearExpr(CI->getOperand(2),Mang);
    Constraint *c1 = new Constraint(var,l1,">=");
    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c1),"||");
    getConstraints(CI->getOperand(1), rootp);
    getConstraints(CI->getOperand(2), rootp);
  } else if (funcName == "llvm.memcpy") {
    string var = getValueName(CI->getOperand(1));
    LinearExpr *l1 = new LinearExpr(CI->getOperand(2),Mang);
    Constraint *c1 = new Constraint(var,l1,">=");
    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c1),"||");
    getConstraints(CI->getOperand(1), rootp);
    getConstraints(CI->getOperand(2), rootp);
  } else if (funcName == "strlen") {
    string var = getValueName(CI);
    const Type* csiType = Type::Int32Ty;
    const Constant * signedzero = ConstantInt::get(csiType,0);
    
    Constraint *c = new Constraint(var, new LinearExpr(signedzero, Mang),">=");
    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
    LinearExpr *l1 = new LinearExpr(CI->getOperand(1),Mang);
    Constraint *c1 = new Constraint(var,l1,"<");
    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c1),"&&");
    getConstraints(CI->getOperand(1), rootp);
  } else if (funcName == "read") {
    string var = getValueName(CI);
    LinearExpr *l1 = new LinearExpr(CI->getOperand(3),Mang);
    Constraint *c1 = new Constraint(var,l1,"<=");
    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c1),"&&");
    getConstraints(CI->getOperand(3), rootp);
    
  } else if (funcName == "fread") {
    string var = getValueName(CI);
    LinearExpr *l1 = new LinearExpr(CI->getOperand(2),Mang);
    LinearExpr *l2 = new LinearExpr(CI->getOperand(3),Mang);
    l2->mulLinearExpr(l1);
    Constraint *c1 = new Constraint(var,l2,"<=");
    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c1),"&&");
    getConstraints(CI->getOperand(3), rootp);
    getConstraints(CI->getOperand(2), rootp);
    
  } else {
    //      out << funcName << " is not supported yet \n";
    // Ignoring some functions is okay as long as they are not part of the
    //one of the multiple indirect calls
    assert((CI->getOperand(0) == kf) && "Need to handle this properly \n");
  }
  return root;
}

//
// Method: getConstraints()
//
// Description:
//  Get constraints on an LLVM Value.  This code sets up the call to the
//  getConstraintsInternal() method (which does all the real work).
//
void
ArrayBoundsCheck::getConstraints(Value *v, ABCExprTree **rootp) {
  string tempName1 = getValueName(v);
  LinearExpr *letemp1 = new LinearExpr(v,Mang);
  Constraint* ctemp1 = new Constraint(tempName1,letemp1,"=");
  ABCExprTree* abctemp1 = new ABCExprTree(ctemp1);
  getConstraintsInternal(v,&abctemp1);
  *rootp = new ABCExprTree(*rootp, abctemp1, "&&");
}

//
// Method: getConstraintsInternal()
//
// Description:
//  Get constraints on an LLVM Value.  This code assumes that the Table is
//  correctly set for the function that is calling this.
//
void
ArrayBoundsCheck::getConstraintsInternal (Value *v, ABCExprTree **rootp) {
  string var;

  if (Instruction *I = dyn_cast<Instruction>(v)) {
    // The basic block in which the instruction is located
    BasicBlock * currentBlock = I->getParent();

    // The function in which the instruction is located
    Function* func = currentBlock->getParent();

    // Here we need to add the post dominator stuff if necessary
    addControlDependentConditions (currentBlock, rootp);

    //
    // Set the name of the variable.  If it is a return instruction, set the
    // name to the name of the function (as a ReturnInst is not a Value).
    //
    if (!isa<ReturnInst>(I)) {
      var = getValueName(I);
    } else {
      var = getValueName(func);
    }

    //
    // Check to see whether we have already created a constraint expression for
    // this instruction.  If so, then the constraint of the input Value is the
    // conjuntion of the given constraint and the constraint already computed
    // for this Value. 
    //
    if (fMap.count(func)) {
      if (fMap[func]->inLocalConstraints(I)) { //checking the cache
        if (fMap[func]->getLocalConstraint(I) != 0) {
          *rootp = new ABCExprTree(*rootp,
                                   fMap[func]->getLocalConstraint(I),
                                   "&&");
        }
        return;
      }
    } else {
      fMap[func] = new FuncLocalInfo();
    }

    //
    // No previous constraints exist for the instruction.  Create a new
    // constraint and record it in the function constraint map (fMap).
    //
    fMap[func]->addLocalConstraint (I,0);
    if (isa<SwitchInst>(I)) {
      // TODO later
    } else if (ReturnInst * ri = dyn_cast<ReturnInst>(I)) {
      if (ri->getNumOperands() > 0) {
        // Constraints on return values 
        LinearExpr *l1 = new LinearExpr(ri->getOperand(0),Mang);
        Constraint *c1 = new Constraint(var,l1,"=");
        *rootp = new ABCExprTree(*rootp,new ABCExprTree(c1),"&&");
        getConstraints(ri->getOperand(0), rootp);
      }
    } else if (PHINode *p = dyn_cast<PHINode>(I)) {
      // Constraints on normal PhiNodes
      if (indMap.count(p) > 0) {
        // We know that this is the canonical induction variable
        // First get the upper bound
        Value *UBound = indMap[p];
        LinearExpr *l1 = new LinearExpr(UBound, Mang);
        Constraint *c1 = new Constraint(var, l1, "<");
        *rootp = new ABCExprTree(*rootp,new ABCExprTree(c1),"&&");

        const Type* csiType = Type::Int32Ty;
        const Constant * signedzero = ConstantInt::get(csiType,0);
        LinearExpr *l2 = new LinearExpr(signedzero, Mang);
        Constraint *c2 = new Constraint(var, l2, ">=");
        *rootp = new ABCExprTree(*rootp,new ABCExprTree(c2),"&&");
        
        getConstraints(UBound, rootp);
      }
    } else if (CallInst * CI = dyn_cast<CallInst>(I)) {
      // Constraints on a calls to the RMalloc function

      //
      // FIXME: What is RMalloc and why is it important?
      //
      if (CI->getOperand(0)->getName() == "RMalloc") {
        // It is an RMalloc, we know it has only one argument 
        Constraint *c = new Constraint(var, SimplifyExpression(I->getOperand(1),rootp),"=");
        *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
      } else {
        if (fMap.count(func) == 0) {
          fMap[func] = new FuncLocalInfo();
        }
        // This also get constraints for arguments of CI
        getConstraintsAtCallSite(CI, rootp);
      }
    } else if (AllocationInst * AI = dyn_cast<AllocationInst>(I)) {
      //
      // Note that this is for local variables which are converted into
      // allocas and mallocs.  We take care of the RMallocs (CASES work) in the
      // CallInst case
      //
      if (const ArrayType *AT = dyn_cast<ArrayType>(AI->getType()->getElementType())) {
        // Sometimes allocas have some array as their allocating constant !!
        // We then have to generate constraints for all the dimensions
        const Type* csiType = Type::Int32Ty;
        const Constant * signedOne = ConstantInt::get(csiType,1);

        Constraint *c=new Constraint(var, new LinearExpr(signedOne, Mang),"=");
        *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
        generateArrayTypeConstraints(var, AT, rootp);
      } else {
        // This is the general case, where the allocas/mallocs are allocated by
        // some variable.
        // Note:
        //  Ugly hack because of the LLVM front end's cast of argument of
        //  malloc to uint
        fromMalloc = true;
        Value *sizeVal = I->getOperand(0) ;
        //          if (CastInst *csI = dyn_cast<CastInst>(I->getOperand(0))) {
        //            const Type *toType = csI->getType();
        //            const Type *fromType = csI->getOperand(0)->getType();
        //            if ((toType->isPrimitiveType()) && (toType->getPrimitiveID() == Type::UIntTyID)) {
        //              sizeVal = csI->getOperand(0);
        //          }
        //          }
        Constraint *c = new Constraint(var,
                            SimplifyExpression(sizeVal,rootp),
                            "=");
        fromMalloc = false;
        *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
      }
    } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(I)) {
      Value *PointerOperand = I->getOperand(0);
      if (const PointerType *pType = dyn_cast<PointerType>(PointerOperand->getType()) ){
        // This is for arrays inside structs 
        if (const StructType *stype = dyn_cast<StructType>(pType->getElementType())) {
          // getelementptr *key, long 0, ubyte 0, long 18
          if (GEP->getNumOperands() == 4) {
            if (const ArrayType *aType = dyn_cast<ArrayType>(stype->getContainedType(0))) {
              int elSize = aType->getNumElements();
              if (const ConstantInt *CSI = dyn_cast<ConstantInt>(I->getOperand(3))) {
                elSize = elSize - CSI->getSExtValue();
                if (elSize == 0) {
                  //
                  // FIXME:
                  //  Dirty HACK.  This doesn't work for more than 2 arrays in
                  //  a struct!!
                  //
                  if (const ArrayType *aType2 = dyn_cast<ArrayType>(stype->getContainedType(1))) {
                    elSize = aType2->getNumElements();
                  }
                }
                const Type* csiType = Type::Int32Ty;
                const Constant * signedOne = ConstantInt::get(csiType,elSize);
                Constraint *c = new Constraint(var,
                                               new LinearExpr(signedOne, Mang),
                                               "=");
                *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
              }
            }
          }
        }
      }

      // Dunno if this is a special case or needs to be generalized
      // FIXME for now it is a special case.
      if (I->getNumOperands() == 2) {
        getConstraints(PointerOperand,rootp);
        getConstraints(GEP->getOperand(1),rootp);
        LinearExpr *L1 = new LinearExpr(GEP->getOperand(1), Mang);
        LinearExpr *L2 = new LinearExpr(PointerOperand, Mang);
        L1->negate();
        L1->addLinearExpr(L2);
        Constraint *c = new Constraint(var, L1,"=");
        *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
      }

      //
      // This is added for the special case found in the embedded bench marks
      // Normally GetElementPtrInst is taken care by the getSafetyConstraints
      // But sometimes you get a pointer to an array x = &x[0]
      // z = getelementptr x 0 0
      // getlelementptr z is equivalent to getelementptr x !
      //
      if (I->getNumOperands() == 3) {
        if (const PointerType *PT = dyn_cast<PointerType>(PointerOperand->getType())) {
          if (const ArrayType *AT = dyn_cast<ArrayType>(PT->getElementType())) {
            if (const ConstantInt *CSI = dyn_cast<ConstantInt>(I->getOperand(1))) {
              if (CSI->getSExtValue() == 0) {
                if (const ConstantInt *CSI2 = dyn_cast<ConstantInt>(I->getOperand(2))) {
                  if (CSI2->getSExtValue() == 0) {
                    //Now add the constraint

                    const Type* csiType = Type::Int32Ty;
                    const Constant * signedOne = ConstantInt::get(csiType,AT->getNumElements());
                    Constraint *c = new Constraint(var, new LinearExpr(signedOne, Mang),"=");
                    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
                    
                  }
                }
              }
            }
          }
        }
      }
    } else {
      Constraint *c = new Constraint(var, SimplifyExpression(I,rootp),"=");
      *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
    }

    //
    // Store the new constraint in the function map cache.
    //
    fMap[func]->addLocalConstraint(I,*rootp);
  } else if (GlobalVariable *GV = dyn_cast<GlobalVariable>(v)) {
    //
    // Generate a constraint if the global variable is a global array.
    //
    var = getValueName(GV);
    if (const ArrayType *AT = dyn_cast<ArrayType>(GV->getType()
                                                    ->getElementType())) {
      const Type* csiType = Type::Int32Ty;
      const Constant * signedOne = ConstantInt::get(csiType,1);
      
      Constraint *c = new Constraint(var, new LinearExpr(signedOne, Mang),"=");
      *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
      generateArrayTypeConstraintsGlobal(var, AT, rootp, 1);          
    }
  }
}

void
ArrayBoundsCheck::generateArrayTypeConstraintsGlobal (string var,
                                                      const ArrayType *T,
                                                      ABCExprTree **rootp,
                                                      unsigned int numElem) {
  string var1 = var + "_i";
  const Type* csiType = Type::Int32Ty;
  if (const ArrayType *AT = dyn_cast<ArrayType>(T->getElementType())) {
    //
    // If this is a multi-dimensional array, call this method recursively to
    // get the constraints of the array inside of this array.
    //
    const Constant * signedOne = ConstantInt::get(csiType,1);
    Constraint *c = new Constraint(var1, new LinearExpr(signedOne, Mang),"=");
    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
    generateArrayTypeConstraintsGlobal(var1,
                                       AT,
                                       rootp,
                                       T->getNumElements() * numElem);
  } else {
    //
    // If this is a single dimension array, create a constraint for it.
    //
    const Constant * signedOne = ConstantInt::get (csiType, numElem * T->getNumElements());
    Constraint *c = new Constraint(var1, new LinearExpr(signedOne, Mang),"=");
    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
  }
}


void ArrayBoundsCheck::generateArrayTypeConstraints(string var, const ArrayType *T, ABCExprTree **rootp) {
  string var1 = var + "_i";
  const Type* csiType = Type::Int32Ty;
  const Constant * signedOne = ConstantInt::get(csiType,T->getNumElements());
  Constraint *c = new Constraint(var1, new LinearExpr(signedOne, Mang),"=");
  *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
  if (const ArrayType *AT = dyn_cast<ArrayType>(T->getElementType())) {
    generateArrayTypeConstraints(var1,AT, rootp);
  } else if (const StructType *ST = dyn_cast<StructType>(T->getElementType())) {
    //This will only work one level of arrays and structs
    //If there are arrays inside a struct then this will
    //not help us prove the safety of the access ....
    unsigned Size = getAnalysis<TargetData>().getTypeAllocSize(ST);
    string var2 = var1 + "_i";
    const Type* csiType = Type::Int32Ty;
    const Constant * signedOne = ConstantInt::get(csiType,Size);
    Constraint *c = new Constraint(var2, new LinearExpr(signedOne, Mang),"=");
    *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
  }
}
  
ABCExprTree *ArrayBoundsCheck::getArgumentConstraints(Function & F) {
    if (buCG->isInSCC(&F)) return 0; //Ignore recursion for now
    std::set<Function *> reqArgFList;
    bool localSave = reqArgs;
   //First check if it is in cache
    ABCExprTree *root = fMap[&F]->getArgumentConstraints();
    if (root) {
      return root;
    } else {
      //Its not there in cache, so we compute it
      if (buCG->FuncCallSiteMap.count(&F)) {
        std::vector<CallSite> &cslist = buCG->FuncCallSiteMap[&F];
        for (unsigned idx = 0, sz = cslist.size(); idx != sz; ++idx) {
          ABCExprTree *rootCallInst = 0;
          if (CallInst *CI = dyn_cast<CallInst>(cslist[idx].getInstruction())) {
            //we need to AND the constraints on the arguments
            reqArgs = false;
            Function::arg_iterator formalArgCurrent = F.arg_begin(), formalArgEnd = F.arg_end();
            for (unsigned i = 1; formalArgCurrent != formalArgEnd; ++formalArgCurrent, ++i) {
              if (i < CI->getNumOperands()) {
                string varName = getValueName(formalArgCurrent);
                Value *OperandVal = CI->getOperand(i);
                LinearExpr *le = new LinearExpr(OperandVal,Mang);
                Constraint* c1 = new Constraint(varName,le,"=");
                ABCExprTree *temp = new ABCExprTree(c1);
                if (!isa<Constant>(OperandVal)) {
                  getConstraints(OperandVal,&temp);
                }
                if (!rootCallInst)  {
                  rootCallInst = temp;
                } else {
                  rootCallInst = new ABCExprTree(rootCallInst, temp, "&&");
                }
              }
            }
            if (reqArgs) {
              //This Call site requires args better to maintain a set
              //and get the argument constraints once for all
              //since there could be multiple call sites from the same function
              reqArgFList.insert(CI->getParent()->getParent());
            }
          }
          if (!root) {
            root = rootCallInst;
          } else {
            root = new ABCExprTree(root, rootCallInst, "||");
          }
        }
        std::set<Function *>::iterator sI = reqArgFList.begin(), sE= reqArgFList.end();
        for (; sI != sE ; ++sI) {
          ABCExprTree * argConstraints = getArgumentConstraints(**sI);
          if (argConstraints) root = new ABCExprTree(root, argConstraints, "&&");
        }
        fMap[&F]->addArgumentConstraints(root);  //store it in cache
      }
    }
    reqArgs = localSave;
    return root;
  }


void ArrayBoundsCheck::printStandardArguments(const Module *M, ostream & out) {
  for (Module::const_iterator fI = M->begin(), fE = M->end(); fI != fE; ++fI) {
    if (fI->getName() == "main") {
      Function::const_arg_iterator formalArgCurrent = fI->arg_begin(), formalArgEnd = fI->arg_end();
      if (formalArgCurrent != formalArgEnd) {
        //relyingon front end's ability to get two arguments
        string argcname = getValueName(formalArgCurrent);
        ++formalArgCurrent;
        string argvname = getValueName(formalArgCurrent);
        out << " && " << argcname << " = " << argvname ;
        break;
      }
    }
  }
}

void ArrayBoundsCheck::printSymbolicStandardArguments(const Module *M, ostream & out) {
  for (Module::const_iterator fI = M->begin(), fE = M->end(); fI != fE; ++fI) {
    if (fI->getName() == "main") {
      Function::const_arg_iterator formalArgCurrent = fI->arg_begin(), formalArgEnd = fI->arg_end();
      if (formalArgCurrent != formalArgEnd) {
        //relyingon front end's ability to get two arguments
        string argcname = getValueName(formalArgCurrent);//->getName();
        ++formalArgCurrent;
        string argvname = getValueName(formalArgCurrent);//->getName();
        out << "symbolic " << argcname << ";\n";
        out << "symbolic " << argvname  << ";\n";
        break;
      }
    }
  }
}


//
// Method: checkSafety()
//
// Notes:
//  FIXME: This method doesn't handle any kind of recursion.
//
void
ArrayBoundsCheck::checkSafety(Function &F) {
  //
  // Do not process functions that have no body.
  //
  if (F.isDeclaration()) return;

  if (fMap[&F] != 0) {
    MemAccessInstListType MemAccessInstList = fMap[&F]->getMemAccessInstList();
    MemAccessInstListIt maI = MemAccessInstList.begin(),
                        maE = MemAccessInstList.end();
    for (; maI != maE; ++maI) {
      ABCExprTree *root = fMap[&F]->getSafetyConstraint(maI->first);
      ABCExprTree * argConstraints = 0;
      if (maI->second) {
        argConstraints = getArgumentConstraints(F);
      }
      if (argConstraints) {
        root = new ABCExprTree(root,argConstraints,"&&");
      }
      //omega stuff should go in here.
      Omega(maI->first,root);
    }
  }
}

#define parentR p2cdes[0]  
#define childW p2cdes[1]  
#define childR c2pdes[0]  
#define parentW c2pdes[1]
  
void ArrayBoundsCheck::Omega(Instruction *maI, ABCExprTree *root ) {
  int p2cdes[2];
  int c2pdes[2];
  pid_t pid, perlpid;
  pipe(p2cdes);
  pipe(c2pdes);

  
  if ((pid = fork())) {
    //this is the parent
    close(childR);
    close(childW);
    fcntl(parentW, F_SETFL, O_NONBLOCK);
    boost::fdostream out(parentW);
    const Module *M = (maI)->getParent()->getParent()->getParent();
    if (root != 0) {
      root->printOmegaSymbols(out);
      DEBUG(root->printOmegaSymbols(std::cerr));
    }
    printSymbolicStandardArguments(M, out);

    //Dinakar Debug 
    DEBUG(printSymbolicStandardArguments(M,std::cerr));

    out << " P" <<countA << " := {[i] : \n";

    //Dinakar Debug 
    DEBUG(std::cerr << " P" << countA << " := {[i] : \n");
    
    if (root != 0)root->print(out);
    //Dinakar Debug
    DEBUG(if (root != 0)root->print(std::cerr));
    
    printStandardArguments(M, out);
    //Dinakar Debug
    DEBUG(printStandardArguments(M, std::cerr));
    //    out << " && (argv = argc) ";
    out << "};\n Hull P"<<countA++ << ";\n" ;
    
    //Dinakar Debug
    DEBUG(std::cerr << "};\n Hull P"<<countA-1 << ";\n");
    close(parentW);
    int perl2parent[2];
    pipe(perl2parent);
    if (!(perlpid = fork())){
      //child
      close(perl2parent[0]); //perl doesn't read anything from parent
      close(fileno(stdout));
      dup(perl2parent[1]); 
      close(fileno(stdin)); 
      dup(parentR); //this for reading from omega calculator
       if (execvp(OMEGASCRIPT,NULL) == -1) {
        perror("execve error \n");
        exit(-1); 
      }
    } else {
      int result;
      close(perl2parent[1]);
      boost::fdistream inp(perl2parent[0]);
      std::cerr << "waiting for output " << countA << "\n";
      inp >> result;
      close(perl2parent[0]);
      //      read(perl2parent[0],&result,4);
      if (result == 1) {
        std::cerr << "proved safe \n";
        std::cerr << maI;

        // Update the statistics
        ++SafeGEPs;

        //        MarkGEPUnsafe(maI);        
        //Omega proved SAFE 
      } else {
        std::cerr << "cannot prove safe " << countA;
        std::cerr << maI;
        MarkGEPUnsafe(maI);
      }
    }
  } else if (pid < 0) {
    perror("fork error \n");
    exit(-1);
  } else {
    //pid == 0
    // this is child
    close(parentW);
    close(parentR);
    close(fileno(stdin));
    dup(childR);
    close(fileno(stdout));
    dup(childW);
    if (execvp(OMEGA,NULL) == -1) {
      perror("execve error \n");
      exit(-1);
    }
  }

  //
  // Wait for child processes.
  //
  waitpid (pid, NULL, 0);
  waitpid (perlpid, NULL, 0);
}

//
// Method: runOnModule()
//
// Description:
//  This is the entry point for this LLVM analysis pass.
//
// Inputs:
//  M - A reference to the LLVM module to analyze.
//
// Return value:
//  true  - The module was modified.
//  false - The module was not modified.
//
bool
ArrayBoundsCheck::runOnModule(Module &M) {
  cbudsPass = &getAnalysis<EQTDDataStructures>();
  buCG      = &getAnalysis<BottomUpCallGraph>();

  //
  // Create a new name mangler.
  //
  Mang = new OmegaMangler(M);

  //
  // Do some preliminary initialization of data structures.
  //
  initialize();

  //
  // Create the include file that will be passed to the Omega constraint
  // solver.
  //
  includeOut.open (OmegaFilename.c_str());
  outputDeclsForOmega(M);
  includeOut.close();
  
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    Function &F = *I;

    //
    // Skip functions that have no body.
    //
    if (F.isDeclaration())
      continue;

    //
    // Retrieve dominator information about the function we're processing
    //
    DominatorTree &DTDT = getAnalysis<DominatorTree>(F);
    PostDominatorTree &PDTDT = getAnalysis<PostDominatorTree>(F);
    PostDominanceFrontier & PDF = getAnalysis<PostDominanceFrontier>(F);
    domTree = &DTDT;
    postdomTree = &PDTDT;
    postdomFrontier = & PDF;

    //
    // Create constraints for the function.
    //
    if (!(F.hasName()) || (KnownFuncDB.find(F.getName()) == KnownFuncDB.end()))
      collectSafetyConstraints(F);
  }

  //
  // Check the constraints.
  //
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    Function &F = *I;
    if (I->isDeclaration())
      continue;

    //
    // Retrieve dominator information about the function we're processing
    //
    DominatorTree &DTDT = getAnalysis<DominatorTree>(F);
    PostDominatorTree &PDTDT = getAnalysis<PostDominatorTree>(F);
    domTree = &DTDT;
    postdomTree = &PDTDT;

    //
    // Run the constraint solver on the function.
    //
    if (!(provenSafe.count(&F) != 0)) checkSafety(F);
  }

  //
  // Free the name mangler.
  //
  delete Mang;

  return false;
}

//
// Method: collectSafetyConstraints()
//
void
ArrayBoundsCheck::collectSafetyConstraints (Function &F) {
  //
  // If we have not analyzed this function before, create a new entry for it
  // in the function map.
  //
  if (fMap.count(&F) == 0) {
    fMap[&F] = new FuncLocalInfo();
  }

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    Instruction *iLocal = &*I;

    //
    // Sometimes a GetElementPtr instruction is hidden within a cast
    // instruction.  Go fetch it out.
    //
    if (isa<CastInst>(iLocal)) {
      if (isa<GetElementPtrInst>(iLocal->getOperand(0))) {
        iLocal = cast<Instruction>(iLocal->getOperand(0));
      }
    }

    if (GetElementPtrInst *MAI = dyn_cast<GetElementPtrInst>(iLocal)) {
      if (const PointerType *PT = dyn_cast<PointerType>(MAI->getPointerOperand()->getType())) {
        if (!isa<StructType>(PT->getElementType())) {
          User::op_iterator mI = MAI->op_begin(), mE = MAI->op_end();
          if (mI == mE) {
            continue;
          }

          // Advance to the first index operand
          mI++;
          ABCExprTree *root;
          string varName = getValueName(MAI->getPointerOperand());
          LinearExpr *le = new LinearExpr(*mI,Mang);
          Constraint* c1 = new Constraint(varName,le,"<="); // length < index
          ABCExprTree* abctemp1 = new ABCExprTree(c1);
          Constraint* c2 = new Constraint("0",le,">",true); // 0 > index
          ABCExprTree* abctemp2 = new ABCExprTree(c2);
          root = new ABCExprTree(abctemp1, abctemp2, "||");

          //
          // Process the other indices in the GEP.
          //
          mI++;
          for (; mI != mE; ++mI) {
            LinearExpr *le = new LinearExpr(*mI,Mang);
            varName = varName+"_i" ;
            Constraint* c1 = new Constraint(varName,le,"<="); // length < index
            ABCExprTree* abctemp1 = new ABCExprTree(c1);
            Constraint* c2 = new Constraint("0",le,">",true); // 0 > index
            ABCExprTree* abctemp2 = new ABCExprTree(c2);
            ABCExprTree*abctempor = new ABCExprTree(abctemp1,abctemp2,"||"); // abctemp1 || abctemp2
            root = new ABCExprTree(root, abctempor, "||");
          }

          //reinitialize mI , now getting the constraints on the indices
          //We need to clear DoneList since we are getting constraints for a
          //new access. (DoneList is the list of basic blocks that are in the
          //post dominance frontier of this accesses basic block
          DoneList.clear();
          reqArgs = false;
          addControlDependentConditions(MAI->getParent(), &root);
          mI = MAI->idx_begin();
          for (; mI != mE; ++mI) {
            getConstraints(*mI,&root);
          }
          getConstraints(MAI->getPointerOperand(),&root);
          fMap[&F]->addSafetyConstraint(MAI,root);
          fMap[&F]->addMemAccessInst(MAI, reqArgs);
        } else {
          //
          // FIXME: TODO:
          //  We need to do some constraint solving for pure struct indexing.
          //
          if (NoStructChecks) {
            if (!indexesStructsOnly (MAI)) {
              MarkGEPUnsafe (MAI);
            } else {
              ++SafeStructs;
              ++TotalStructs;
            }
          } else {
            MarkGEPUnsafe (MAI);
          }
        }
      } else {
        std::cerr << "GEP to non-pointer: " << *iLocal << std::endl;
      }
    } else if (CallInst *CI = dyn_cast<CallInst>(iLocal)) {
      // Now we need to collect and add the constraints for trusted lib
      // functions like read , fread, memcpy 
      if (Function *FCI = dyn_cast<Function>(CI->getOperand(0))) {
        //Its a direct function call,
        string funcName = FCI->getName();
        DEBUG(std::cerr << "Adding constraints for " << funcName << "\n");
        reqArgs = false;

        if (funcName == "read") {
          LinearExpr *le = new LinearExpr(CI->getOperand(3),Mang);
          string varName = getValueName(CI->getOperand(2));
          Constraint* c1 = new Constraint(varName,le,">"); // buf.length >= size 
          ABCExprTree* root = new ABCExprTree(c1);

          Constraint* c2 = new Constraint("0",le,">",true); // 0 > size
          ABCExprTree* abctemp2 = new ABCExprTree(c2);
          root = new ABCExprTree(root,abctemp2,"||"); // abctemp1 || abctemp2      
          getConstraints(CI->getOperand(2), &root);
          getConstraints(CI->getOperand(3), &root);
          fMap[&F]->addMemAccessInst(CI, reqArgs);
          fMap[&F]->addSafetyConstraint(CI, root);
        } else if (funcName == "strchr") {
          std::cerr << " DID NOT HANDLE strchr\n";
          std::cerr << "Program may not be SAFE\n";
          //    exit(-1);
        } else if (funcName == "sprintf") {
          std::cerr << " DID NOT HANDLE sprintf\n";
          std::cerr << "Program may not be SAFE\n";
          //    abort();
        } else if (funcName == "fscanf") {
          std::cerr << " DID NOT HANDLE fscanf\n";
          std::cerr << "Program may not be SAFE\n";
          //    abort();
        } else if (funcName == "fread") {
          //FIXME, assumes reading only a byte 
          LinearExpr *le = new LinearExpr(CI->getOperand(3),Mang);
          string varName = getValueName(CI->getOperand(1));
          Constraint* c1 = new Constraint(varName,le,">"); // buf.length >= size 
          ABCExprTree* root = new ABCExprTree(c1);

          Constraint* c2 = new Constraint("0",le,">",true); // 0 > size
          ABCExprTree* abctemp2 = new ABCExprTree(c2);
          root = new ABCExprTree(root,abctemp2,"||"); // abctemp1 || abctemp2      
          getConstraints(CI->getOperand(1), &root);
          getConstraints(CI->getOperand(3), &root);
          fMap[&F]->addMemAccessInst(CI, reqArgs);
          fMap[&F]->addSafetyConstraint(CI, root);
        } else if (funcName == "memset") {
          LinearExpr *le = new LinearExpr(CI->getOperand(3),Mang);
          string varName = getValueName(CI->getOperand(1));
          Constraint* c1 = new Constraint(varName,le,">"); // buf.length >= size 
          ABCExprTree* root = new ABCExprTree(c1);
          Constraint* c2 = new Constraint("0",le,">",true); // 0 > size
          ABCExprTree* abctemp2 = new ABCExprTree(c2);
          root = new ABCExprTree(root,abctemp2,"||"); // abctemp1 || abctemp2      
          getConstraints(CI->getOperand(1), &root);
          getConstraints(CI->getOperand(3), &root);
          fMap[&F]->addMemAccessInst(CI, reqArgs);
          fMap[&F]->addSafetyConstraint(CI, root);
            
        } else if (funcName == "gets") {
          LinearExpr *le = new LinearExpr(CI->getOperand(1),Mang); //buf.length
          Constraint* c1 = new Constraint("80",le,"<"); // buf.length > 80  
          ABCExprTree* root = new ABCExprTree(c1);
          //    Constraint* c2 = new Constraint("0",le,">",true); // 0 > size
          //    ABCExprTree* abctemp2 = new ABCExprTree(c2);
          //    root = new ABCExprTree(root,abctemp2,"||"); // abctemp1 || abctemp2      
          getConstraints(CI->getOperand(1), &root);
          //    getConstraints(CI->getOperand(3), &root);
          fMap[&F]->addMemAccessInst(CI, reqArgs);
          fMap[&F]->addSafetyConstraint(CI, root);
            
            
        } else if (funcName == "llvm.memset") {
          LinearExpr *le = new LinearExpr(CI->getOperand(3),Mang);
          string varName = getValueName(CI->getOperand(1));
          Constraint* c1 = new Constraint(varName,le,">"); // buf.length >= size 
          ABCExprTree* root = new ABCExprTree(c1);
          Constraint* c2 = new Constraint("0",le,">",true); // 0 > size
          ABCExprTree* abctemp2 = new ABCExprTree(c2);
          root = new ABCExprTree(root,abctemp2,"||"); // abctemp1 || abctemp2      
          getConstraints(CI->getOperand(1), &root);
          getConstraints(CI->getOperand(3), &root);
          fMap[&F]->addMemAccessInst(CI, reqArgs);
          fMap[&F]->addSafetyConstraint(CI, root);
            
            
        } else if (funcName == "memcpy") {
          LinearExpr *le = new LinearExpr(CI->getOperand(3),Mang);
          string varName = getValueName(CI->getOperand(1));
          Constraint* c1 = new Constraint(varName,le,">"); // buf.length >= size 
          ABCExprTree* root = new ABCExprTree(c1);

          Constraint* c2 = new Constraint("0",le,">",true); // 0 > size
          ABCExprTree* abctemp2 = new ABCExprTree(c2);
          root = new ABCExprTree(root,abctemp2,"||"); // abctemp1 || abctemp2      
          getConstraints(CI->getOperand(1), &root);
          getConstraints(CI->getOperand(3), &root);
          fMap[&F]->addMemAccessInst(CI, reqArgs);
          fMap[&F]->addSafetyConstraint(CI, root);
            
        } else if (funcName == "llvm.memcpy") {
          LinearExpr *le = new LinearExpr(CI->getOperand(3),Mang);
          string varName = getValueName(CI->getOperand(1));
          Constraint* c1 = new Constraint(varName,le,">"); // buf.length >= size 
          ABCExprTree* root = new ABCExprTree(c1);

          Constraint* c2 = new Constraint("0",le,">",true); // 0 > size
          ABCExprTree* abctemp2 = new ABCExprTree(c2);
          root = new ABCExprTree(root,abctemp2,"||"); // abctemp1 || abctemp2      
          getConstraints(CI->getOperand(1), &root);
          getConstraints(CI->getOperand(3), &root);
          fMap[&F]->addMemAccessInst(CI, reqArgs);
          fMap[&F]->addSafetyConstraint(CI, root);
            
        } else if (funcName == "strcpy") {
          LinearExpr *le = new LinearExpr(CI->getOperand(2),Mang);
          string varName = getValueName(CI->getOperand(1));
          Constraint* c1 = new Constraint(varName,le,"<="); // buf.length >= size 
          ABCExprTree* root = new ABCExprTree(c1);

          getConstraints(CI->getOperand(2), &root);
          getConstraints(CI->getOperand(1), &root);
          fMap[&F]->addMemAccessInst(CI, reqArgs);
          fMap[&F]->addSafetyConstraint(CI, root);
            
        } else if (funcName == "snprintf") {
          LinearExpr *le = new LinearExpr(CI->getOperand(2),Mang);
          string varName = getValueName(CI->getOperand(1));
          Constraint* c1 = new Constraint(varName,le,">"); // buf.length >= size 
          ABCExprTree* root = new ABCExprTree(c1);

          Constraint* c2 = new Constraint("0",le,">",true); // 0 > size
          ABCExprTree* abctemp2 = new ABCExprTree(c2);
          root = new ABCExprTree(root,abctemp2,"||"); // abctemp1 || abctemp2      
          getConstraints(CI->getOperand(1), &root);
          getConstraints(CI->getOperand(2), &root);
          fMap[&F]->addMemAccessInst(CI, reqArgs);
          fMap[&F]->addSafetyConstraint(CI, root);

        } else if (funcName == "fprintf") {
          if (!isa<ConstantArray>(CI->getOperand(2))) {
            if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(CI->getOperand(2))) {
              if (!isa<ConstantArray>(GEPI->getPointerOperand())) {
                std::cerr << "Format string problem " << CI->getOperand(2);
                //exit(-1);
              }
            }
          }
        } else if (funcName == "vfprintf") {
          if (!isa<ConstantArray>(CI->getOperand(2))) {
            if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(CI->getOperand(2))) {
              if (!isa<ConstantArray>(GEPI->getPointerOperand())) {
                std::cerr << "Format string problem " << CI->getOperand(2);
                //    exit(-1);
              }
            }
          }
        } else if (funcName == "printf") {
          if (!isa<ConstantArray>(CI->getOperand(1))) {
            if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(CI->getOperand(1))) {
              if (!isa<ConstantArray>(GEPI->getPointerOperand())) {
                std::cerr << "Format string problem " << CI->getOperand(1);
                //exit(-1);
              }
            }
          }
        } else if (funcName == "syslog") {
          if (!isa<ConstantArray>(CI->getOperand(2))) {
            if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(CI->getOperand(1))) {
              if (!isa<ConstantArray>(GEPI->getPointerOperand())) {
                std::cerr << "Format string problem " << CI->getOperand(1);
                //exit(-1);
              }
            }
          }
        } else if (FCI->isDeclaration()) {
          if (KnownFuncDB.find(funcName) == KnownFuncDB.end()) {
                  //      std::cerr << "Don't know what constraints to add " << funcName << "\n";
            //      std::cerr << "Exiting \n";
            //      exit(-1);
          }
        }
      } else {
        //indirect function call doesn't call the known external functions
        EQTDDataStructures::callee_iterator cI = cbudsPass->callee_begin(CI),
                                            cE = cbudsPass->callee_end(CI);
        for (; cI != cE; ++cI) { 
          Function * Target = (Function *)(*cI);
          if ((Target->isDeclaration()) ||
              (KnownFuncDB.find(Target->getName()) != KnownFuncDB.end())) {
            assert(1 && " Assumption that indirect fn call doesnt call an externalfails");
          }
        }
      }
    }
  }
}

//
// TODO:
//  Need to handle the new floating point compare instructions
//
void
ArrayBoundsCheck::addBranchConstraints (BranchInst *BI,
                                        BasicBlock *Successor,
                                        ABCExprTree **rootp) {
  //this has to be a conditional branch, otherwise we wouldnt have been here
  assert((BI->isConditional()) && "abcd wrong branch constraint");
  if (CmpInst *SCI = dyn_cast<CmpInst>(BI->getCondition())) {

    //SCI now has the conditional statement
    Value *operand0 = SCI->getOperand(0);
    Value *operand1 = SCI->getOperand(1);

    getConstraints(operand0,rootp);
    getConstraints(operand1,rootp);
      

    LinearExpr *l1 = new LinearExpr(operand1,Mang);

    string var0 = getValueName(operand0);
    Constraint *ct = 0;

    switch (SCI->getPredicate()) {
      case ICmpInst::ICMP_ULE: 
      case ICmpInst::ICMP_SLE: 
        //there are 2 cases for each opcode!
        //its the true branch or the false branch
        if (BI->getSuccessor(0) == Successor) {
          //true branch 
          ct = new Constraint(var0,l1,"<=");
        } else {
          ct = new Constraint(var0,l1,">");
        }
        break;
      case ICmpInst::ICMP_UGE: 
      case ICmpInst::ICMP_SGE: 
        if (BI->getSuccessor(0) == Successor) {
          //true branch 
          ct = new Constraint(var0,l1,">=");
        } else {
          //false branch
          ct = new Constraint(var0,l1,"<");
        }
        break;
      case ICmpInst::ICMP_ULT: 
      case ICmpInst::ICMP_SLT: 
        if (BI->getSuccessor(0) == Successor) {
          //true branch 
          ct = new Constraint(var0,l1,"<");
        } else {
          //false branch
          ct = new Constraint(var0,l1,">=");
        }
        break;
      case ICmpInst::ICMP_UGT:
      case ICmpInst::ICMP_SGT:
        if (BI->getSuccessor(0) == Successor) {
          //true branch 
          ct = new Constraint(var0,l1,">");
        } else {
          //false branch
          ct = new Constraint(var0,l1,"<=");
        }
        break;
      default:
        break;
    }
    if (ct != 0) {
      ct->print(std::cerr);
      *rootp = new ABCExprTree(*rootp,new ABCExprTree(ct),"&&");
    }
  }
}



// SimplifyExpression: Simplify a Value and return it as an affine expressions
LinearExpr*
ArrayBoundsCheck::SimplifyExpression (Value *Expr, ABCExprTree **rootp) {
  assert(Expr != 0 && "Can't classify a null expression!");

  // nothing  known return variable itself
  if (Expr->getType() == Type::FloatTy || Expr->getType() == Type::DoubleTy)
    return new LinearExpr(Expr, Mang);

  if ((isa<BasicBlock>(Expr)) || (isa<Function>(Expr)))
    assert(1 && "Unexpected expression type to classify!");
  if ((isa<GlobalVariable>(Expr)) || (isa<Argument>(Expr))) {
    reqArgs = true; //I know using global is ugly, fix this later 
    return new LinearExpr(Expr, Mang);
  }
  if (isa<Constant>(Expr)) {
    Constant *CPV = cast<Constant>(Expr);
    if (CPV->getType()->isInteger()) { // It's an integral constant!
      if (dyn_cast<ConstantArray>(CPV)) {
        assert(1 && "Constant Array don't know how to get the values ");
      } else if (ConstantInt *CPI = dyn_cast<ConstantInt>(Expr)) {
        return new LinearExpr(CPI, Mang);
      }
    }
    return new LinearExpr(Expr, Mang); //nothing known, just return itself
  }
  if (isa<Instruction>(Expr)) {
    Instruction *I = cast<Instruction>(Expr);

    switch (I->getOpcode()) {       // Handle each instruction type seperately
    case Instruction::Add: {
      LinearExpr* Left =  (SimplifyExpression(I->getOperand(0), rootp));
      if (Left == 0) {
        Left = new LinearExpr(I->getOperand(0), Mang);
      }
      LinearExpr* Right = (SimplifyExpression(I->getOperand(1), rootp));
      if (Right == 0) {
        Right = new LinearExpr(I->getOperand(1), Mang);
      }
      Left->addLinearExpr(Right);
      return Left;
    }
    case Instruction::Sub: {
      LinearExpr* Left =  (SimplifyExpression(I->getOperand(0), rootp));
      if (Left == 0) {
        Left = new LinearExpr(I->getOperand(0), Mang);
      }
      LinearExpr* Right = (SimplifyExpression(I->getOperand(1), rootp));
      if (Right == 0) {
        Right = new LinearExpr(I->getOperand(1), Mang);
      }
      Right->negate();
      Left->addLinearExpr(Right);
      return Left;
    }
    case Instruction::FCmp :
    case Instruction::ICmp : {
      LinearExpr* L = new LinearExpr(I->getOperand(1),Mang);
      return L;
    };
    case Instruction::Mul :
    
      LinearExpr* Left =  (SimplifyExpression(I->getOperand(0),rootp));
      if (Left == 0) {
        Left = new LinearExpr(I->getOperand(0), Mang);
      }
      LinearExpr* Right = (SimplifyExpression(I->getOperand(1),rootp));
      if (Right == 0) {
        Right = new LinearExpr(I->getOperand(1), Mang);
      }
      return Left->mulLinearExpr(Right);
    }  // end switch
    if (isa<CastInst>(I)) {
      DEBUG(std::cerr << "dealing with cast instruction ");
      const Type *fromType = I->getOperand(0)->getType();
      const Type *toType = I->getType();
      string number1;
      string number2;
      bool addC = false;
      if (toType->isPrimitiveType() && fromType->isPrimitiveType()) {
        //Here we have to give constraints for 
        //FIXME .. this should be for all types not just sbyte 
        // FIXME: JTC: I am pretty sure this is overly conservative; I'm just
        //             not sure how to handle the lack of signed-ness on LLVM
        //             types.
        if (toType == Type::Int32Ty) {
          if (fromType == Type::Int8Ty) {
            number1 = "0";
            number2 = "127";
            addC = true;
          } else if (fromType == Type::Int8Ty) {
            //in llvm front end the malloc argument is always casted to
            //uint! so we hack it here
            //This hack works incorrectly for
            //some programs so moved it to malloc itself
            //FIXME FIXME This might give incorrect results in some cases!!!!
            addC = true;
          }
        }

#if 0
        switch(toType->getTypeID()) {
        case Type::Int32TyID :
          switch (fromType->getTypeID()) {
          case Type::Int8TyID :
            number1 = "-128";
            number2 = "127";
            addC = true;
            break;
          case Type::Int8TyID :
            number1 = "0";
            number2 = "255";
            addC = true;
          default:
            break;
          }
        case Type::Int32TyID :
          switch(fromType->getTypeID()) {
          case Type::Int32TyID :
            //in llvm front end the malloc argument is always casted to
            //uint! so we hack it here
            //This hack works incorrectly for
            //some programs so moved it to malloc itself
            //FIXME FIXME This might give incorrect results in some cases!!!!
            addC = true;
            break;
          case Type::Int8TyID :
          case Type::Int8TyID :
            number1 = "0";
            number2 = "255";
            addC = true;
            break;
          default :
            break;
          }
        default:
          break;
        }
#endif
        if (addC) {
          string var = getValueName(I);
          LinearExpr *l1 = new LinearExpr(I,Mang);
          if (number1 != "") {
            Constraint *c1 = new Constraint(number1,l1,">=",true);
            *rootp = new ABCExprTree(*rootp,new ABCExprTree(c1),"&&");
          }
          if (number2 != "") {
            Constraint *c2 = new Constraint(number2,l1,"<=",true);
            *rootp = new ABCExprTree(*rootp,new ABCExprTree(c2),"&&");
          }
          Constraint *c = new Constraint(var, SimplifyExpression(I->getOperand(0),rootp),"=");
          *rootp = new ABCExprTree(*rootp,new ABCExprTree(c),"&&");
          return l1;
        }
      } else {
        if (const PointerType *pType = dyn_cast<PointerType>(I->getType())) {
          const Type *eltype = pType->getElementType();
          if (eltype->isPrimitiveType()) {
            unsigned numbytes = 0;
            if (eltype == Type::Int8Ty) {
              //FIXME: this should make use of Target!!!!
              numbytes = 1;
            } else if (eltype == Type::Int8Ty) {
              numbytes = 4;
            } else if (eltype == Type::Int32Ty) {
              numbytes = 4;
            } else if (eltype == Type::Int32Ty) {
              numbytes = 4;
            } else if (eltype == Type::Int16Ty) {
              numbytes = 2;
            } else if (eltype == Type::Int16Ty) {
              numbytes = 2;
            } else if (eltype == Type::Int64Ty) {
              numbytes = 8;
            } else if (eltype == Type::Int64Ty) {
              numbytes = 8;
            } 

            if (numbytes != 0) {
              if (const PointerType *opPType = dyn_cast<PointerType>(fromType)){
                const Type *opEltype = opPType->getElementType();
                if (const StructType *stype = dyn_cast<StructType>(opEltype)) {
                  // special case for casts to beginning of structs
                  // the case is (sbyte*) (Struct with the first element arrauy)
                  // If it is a cast from struct to something else * ...
                  if (const ArrayType *aType = dyn_cast<ArrayType>(stype->getContainedType(0))) {
                    if (aType->getElementType()->isPrimitiveType()) {
                      int elSize = aType->getNumElements();
                      if ((aType->getElementType()) == Type::Int16Ty) {
                        elSize = (elSize/numbytes)*2;
                      } else if ((aType->getElementType()) == Type::Int32Ty) {
                        elSize = (elSize/numbytes)*4;
                      } else if ((aType->getElementType()) == Type::Int64Ty) {
                        elSize = (elSize/numbytes)*8;
                      }
                      string varName = getValueName(I);
                      const Type* csiType = Type::Int32Ty;
                      const Constant * signedOne = ConstantInt::get(csiType,elSize);
                      LinearExpr *l1 = new LinearExpr(signedOne, Mang);
                      return l1;
                    }
                  }
                } else if (const ArrayType *aType = dyn_cast<ArrayType>(opEltype)) {
                  if (aType->getElementType()->isPrimitiveType()) {
                    int elSize = aType->getNumElements();
                    if ((aType->getElementType()) == Type::Int8Ty) {
                      elSize = elSize / numbytes;
                    } else if ((aType->getElementType()) == Type::Int16Ty) {
                      elSize = (elSize/numbytes) *2;
                    } else if ((aType->getElementType()) == Type::Int32Ty) {
                      elSize = (elSize/numbytes)*4;
                    } else if ((aType->getElementType()) == Type::Int64Ty) {
                      elSize = (elSize/numbytes)*8;
                    }
                    string varName = getValueName(I);
                    const Type* csiType = Type::Int32Ty;
                    const Constant * signedOne = ConstantInt::get(csiType,elSize);
                    LinearExpr *l1 = new LinearExpr(signedOne, Mang);
                    return l1;
                  }
                }
              }
            }
          }
        }
      } 
      return SimplifyExpression(I->getOperand(0),rootp);
    } else {
      getConstraints(I,rootp);
      LinearExpr* ret = new LinearExpr(I,Mang);
      return ret;
    }
  }

  // Otherwise, I don't know anything about this value!
  return 0;
}

NAMESPACE_SC_END
