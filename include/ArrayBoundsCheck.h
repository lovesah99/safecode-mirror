//Assumes that ABCPreprocess is run before 

#ifndef ARRAY_BOUNDS_CHECK
#define ARRAY_BOUNDS_CHECK

#include "llvm/Instruction.h"
#include "llvm/Function.h"
#include "AffineExpressions.h"
#include "llvm/Analysis/DataStructure.h"
namespace llvm {

Pass *createArrayBoundsCheckPass();


namespace ABC {

struct ArrayBoundsCheck : public Pass {
  public :
    const char *getPassName() const { return "Array Bounds Check"; }
    virtual bool run(Module &M);
    std::vector<Instruction*> UnsafeGetElemPtrs;
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<CompleteBUDataStructures>();
      AU.setPreservesAll();
    }
  private :
  CompleteBUDataStructures *budsPass;
  typedef std::map<const Function *,FuncLocalInfo*> InfoMap;
  typedef std::map<Function*, int> FuncIntMap;
    
    //This is required for getting the names/unique identifiers for variables.
    SlotCalculator *Table;

    //for storing local information about a function
    InfoMap fMap; 

    //Known Func Database
    std::set<string> KnownFuncDB;
    
    //for storing info about the functions which are already proven safe
    FuncIntMap provenSafe;

    //for storing what control dependent blocks are already dealt with for the current
    //array access
    std::set<BasicBlock *> DoneList;

    //Initializes the KnownFuncDB
    void initialize(Module &M);
    
    void outputDeclsForOmega(Module &M);

    //Interface for collecting constraints for different array access
    // in a function
    void collectSafetyConstraints(Function &F);

    //This function collects from the branch which controls the current block
    //the Successor tells the path 
    void addBranchConstraints(BranchInst *BI, BasicBlock *Successor, ABCExprTree **rootp);
    //This method adds constraints for known trusted functions
    void addConstraintsForKnownFunctions(CallInst *CI, ABCExprTree **rootp);

    
    //Interface for getting constraints for a particular value
    void getConstraintsInternal( Value *v, ABCExprTree **rootp);
    void getConstraints( Value *v, ABCExprTree **rootp);

    //adds all the conditions on which the currentblock is control dependent on.
    void addControlDependentConditions(BasicBlock *currentBlock, ABCExprTree **rootp); 
    
    //Gives the return value constraints interms of its arguments 
    void getReturnValueConstraints( const CallInst *CI,ABCExprTree **rootp);

    //Checks if the function is safe (produces output for omega consumption)
    void checkSafety(Function &F);

    //Get the constraints on the arguments
    //This goes and looks at all call sites and ors the corresponding
    //constraints
    ABCExprTree* getArgumentConstraints(Function &F);

    //for simplifying the constraints 
    LinearExpr* SimplifyExpression( Value *Expr, ABCExprTree **rootp);

    string getValueName(const Value *V);
    void generateArrayTypeConstraintsGlobal(string var, const ArrayType *T, ABCExprTree **rootp, unsigned int numElem);
    void generateArrayTypeConstraints(string var, const ArrayType *T, ABCExprTree **rootp);
    void printarraytype(string var,const ArrayType  *T);
    void printSymbolicStandardArguments(const Module *M, ostream & out);
    void printStandardArguments(const Module *M, ostream & out);
    void Omega(Instruction *maI, ABCExprTree *root );

  };
}

}
#endif
