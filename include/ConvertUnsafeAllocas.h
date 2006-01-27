#ifndef CONVERT_ALLOCA_H
#define CONVERT_ALLOCA_H

#include "llvm/Pass.h"
#include "ArrayBoundsCheck.h"
#include "StackSafety.h"
#include "llvm/Target/TargetData.h"
namespace llvm {


ModulePass *createConvertUnsafeAllocas();

using namespace ABC;
using namespace CSS;
#define LLVA_KERNEL
 struct MallocPass : public FunctionPass
 {
   private:
inline bool changeType (Instruction * Inst);
   
   inline bool TypeContainsPointer(const Type *Ty);
   
   public:
   virtual bool runOnFunction (Function &F);
   virtual void getAnalysisUsage(AnalysisUsage &AU) const {
     AU.addRequired<TargetData>();
//     AU.setPreservesAll();
   }
 };
 
 
namespace CUA {
struct ConvertUnsafeAllocas : public ModulePass {
    public :
    const char *getPassName() const { return "Array Bounds Check"; }
    virtual bool runOnModule(Module &M);
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<ArrayBoundsCheck>();
      AU.addRequired<checkStackSafety>();
      AU.addRequired<CompleteBUDataStructures>();
      AU.addRequired<TDDataStructures>();
      AU.addRequired<TargetData>();
      // Does not preserve the BU or TD graphs
      //      AU.setPreservesAll();
    }

  DSNode * getDSNode(const Value *I, Function *F);
  DSNode * getTDDSNode(const Value *I, Function *F);

  std::vector<Instruction *>  & getUnsafeGetElementPtrsFromABC() {
    assert(abcPass != 0 && "First run the array bounds pass correctly");
    return abcPass->UnsafeGetElemPtrs;
  }  

  // The set of Malloc Instructions that are a result of conversion from
  // alloca's due to static array bounds detection failure
  std::set<const MallocInst *>  ArrayMallocs;

    private :
      TDDataStructures * tddsPass;
      BUDataStructures * budsPass;
      ArrayBoundsCheck * abcPass;
      checkStackSafety * cssPass;
  TargetData *TD;
  
#ifdef LLVA_KERNEL
Function *kmalloc;
#endif
    std::list<DSNode *> unsafeAllocaNodes;
    std::set<DSNode *> reachableAllocaNodes; 
    bool markReachableAllocas(DSNode *DSN);
    bool markReachableAllocasInt(DSNode *DSN);
    void TransformAllocasToMallocs(std::list<DSNode *> & unsafeAllocaNodes);
    void TransformCSSAllocasToMallocs(std::vector<DSNode *> & cssAllocaNodes);
    void getUnsafeAllocsFromABC();
    void TransformCollapsedAllocas(Module &M);

};
}
} 
#endif
