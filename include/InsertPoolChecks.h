#ifndef INSERT_BOUNDS_H
#define INSERT_BOUNDS_H

#include "safecode/Config/config.h"
#include "llvm/Pass.h"
#include "ConvertUnsafeAllocas.h"

#ifndef LLVA_KERNEL
#include "SafeDynMemAlloc.h"
#include "poolalloc/PoolAllocate.h"
#endif

namespace llvm {

ModulePass *creatInsertPoolChecks();
using namespace CUA;

struct InsertPoolChecks : public ModulePass {
    public :
    const char *getPassName() const { return "Inserting pool checks for array bounds "; }
    virtual bool runOnModule(Module &M);
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<ConvertUnsafeAllocas>();
//      AU.addRequired<CompleteBUDataStructures>();
//      AU.addRequired<TDDataStructures>();
#ifndef LLVA_KERNEL      
      AU.addRequired<EquivClassGraphs>();
      AU.addRequired<PoolAllocate>();
      AU.addRequired<EmbeCFreeRemoval>();
      AU.addRequired<TargetData>();
#else 
      AU.addRequired<TDDataStructures>();
#endif
      
    };
    private :
      CUA::ConvertUnsafeAllocas * cuaPass;
#ifndef  LLVA_KERNEL
  PoolAllocate * paPass;
  EquivClassGraphs *equivPass;
  EmbeCFreeRemoval *efPass;
  TargetData * TD;
#else
  TDDataStructures * TDPass;
#endif  
  Function *PoolCheck;
  Function *PoolCheckArray;
  Function *ExactCheck;
  Function *FunctionCheck;
  void addPoolCheckProto(Module &M);
  void addPoolChecks(Module &M);
  void addGetElementPtrChecks(Module &M);
  DSNode* getDSNode(const Value *V, Function *F);
  unsigned getDSNodeOffset(const Value *V, Function *F);
  void addLoadStoreChecks(Module &M);
#ifndef LLVA_KERNEL  
  void addLSChecks(Value *Vnew, const Value *V, Instruction *I, Function *F);
  Value * getPoolHandle(const Value *V, Function *F, PA::FuncInfo &FI, bool collapsed = false);
  void registerGlobalArraysWithGlobalPools(Module &M);
#else
  void addLSChecks(Value *V, Instruction *I, Function *F);
  Value * getPoolHandle(const Value *V, Function *F);
#endif  

};
}
#endif
