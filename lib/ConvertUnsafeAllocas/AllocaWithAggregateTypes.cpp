#include "llvm/Pass.h"
#include "llvm/BasicBlock.h"
#include "llvm/Type.h"
#include "ConvertUnsafeAllocas.h"
#include "llvm/Function.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/ADT/VectorExtras.h"

#include <iostream>

using namespace llvm;

namespace llvm
{

  // Create the command line option for the pass
  //  RegisterOpt<MallocPass> X ("malloc", "Alloca to Malloc Pass");

  inline bool MallocPass::TypeContainsPointer(const Type *Ty) {
    if (Ty->getTypeID() == Type::PointerTyID)
      return true;
    else if (Ty->getTypeID() == Type::StructTyID) {
      const StructType * structTy = cast<StructType>(Ty);
      unsigned numE = structTy->getNumElements();
      for (unsigned index = 0; index < numE; index++) {
	if (TypeContainsPointer(structTy->getElementType(index)))
	  return true;
      }
    } else if (Ty->getTypeID() == Type::ArrayTyID) {
      const ArrayType *arrayTy = cast<ArrayType>(Ty);
      if (TypeContainsPointer(arrayTy->getElementType()))
	return true;
    }
    return false;
  }

  inline bool
  MallocPass::changeType (Instruction * Inst)
  {
    //
    // Check to see if the instruction is an alloca.
    //
    if (Inst->getOpcode() == Instruction::Alloca) {
      AllocationInst * AllocInst = cast<AllocationInst>(Inst);
      
      //
      // Get the type of object allocated.
      //
      const Type * TypeCreated = AllocInst->getAllocatedType ();
      
      if (TypeContainsPointer(TypeCreated))
	return true;
      
    }
    return false;
  }

  bool
  MallocPass::runOnFunction (Function &F)
  {
    bool modified = false;
    TargetData &TD = getAnalysis<TargetData>();
    Module *theM = F.getParent();  
    Constant *memsetF = 
      theM->getOrInsertFunction("memset", Type::VoidTy, 
				PointerType::getUnqual(Type::Int8Ty), Type::Int32Ty , 
				Type::Int32Ty, 0);
    for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
      {
	for (BasicBlock::iterator IAddrBegin=I->begin(), IAddrEnd = I->end();
	     IAddrBegin != IAddrEnd;
	     ++IAddrBegin)
	  {
	    //
	    // Determine if the instruction needs to be changed.
	    //
	    if (changeType (IAddrBegin))
	      {
		AllocationInst * AllocInst = cast<AllocationInst>(IAddrBegin);
		/*
		//
		// Get the type of object allocated.
		//
		const Type * TypeCreated = AllocInst->getAllocatedType ();
		
		MallocInst * TheMalloc = 
		new MallocInst(TypeCreated, 0, AllocInst->getName(), 
		IAddrBegin);
		std::cerr << "Found alloca that is struct or array" << std::endl;

		//
		// Remove old uses of the old instructions.
		//
		AllocInst->replaceAllUsesWith (TheMalloc);

		//
		// Remove the old instruction.
		//
		AllocInst->getParent()->getInstList().erase(AllocInst);
		modified = true;
		*/

    Instruction * InsertPt = ++IAddrBegin;
    --IAddrBegin;
		CastInst *CastI = 
		  CastInst::createPointerCast (AllocInst, 
			       PointerType::getUnqual(Type::Int8Ty), "casted", InsertPt);
		std::vector<Value *> args(1, CastI);
		args.push_back(ConstantInt::get(Type::Int32Ty,204));
		args.push_back(ConstantInt::get(Type::Int32Ty,
						 TD.getABITypeSize(AllocInst->getType())));
		  new CallInst(memsetF, args.begin(), args.end(), "", InsertPt);
	      }
	  }
      }
    return modified;
  }

  RegisterPass<MallocPass> Z("convallocai", "converts unsafe allocas to mallocs");
} // end of namespace

