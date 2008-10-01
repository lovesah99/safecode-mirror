#include <iostream>
#include "safecode/Config/config.h"
#include "SCUtils.h"
#include "InsertPoolChecks.h"
#include "llvm/Instruction.h"
#include "llvm/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/ADT/VectorExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include "VectorListHelper.h"

#define REG_FUNC(var, ret, name, ...)do { var = M.getOrInsertFunction(name, FunctionType::get(ret, args<const Type*>::list(__VA_ARGS__), false)); } while (0);

char llvm::InsertPoolChecks::ID = 0;

static llvm::RegisterPass<InsertPoolChecks> ipcPass ("safecode", "insert runtime checks");

// Options for Enabling/Disabling the Insertion of Various Checks
cl::opt<bool> EnableIncompleteChecks  ("enable-incompletechecks", cl::Hidden,
                                cl::init(false),
                                cl::desc("Enable Checks on Incomplete Nodes"));

cl::opt<bool> EnableNullChecks  ("enable-nullchecks", cl::Hidden,
                                cl::init(false),
                                cl::desc("Enable Checks on NULL Pools"));


cl::opt<bool> DisableLSChecks  ("disable-lschecks", cl::Hidden,
                                cl::init(false),
                                cl::desc("Disable Load/Store Checks"));

cl::opt<bool> DisableGEPChecks ("disable-gepchecks", cl::Hidden,
                                cl::init(false),
                                cl::desc("Disable GetElementPtr(GEP) Checks"));

cl::opt<bool> DisableIntrinsicChecks ("disable-intrinchecks", cl::Hidden,
                                      cl::init(false),
                                      cl::desc("Disable Intrinsic Checks"));

// Options for where to insert various initialization code
cl::opt<string> InitFunctionName ("initfunc",
                                  cl::desc("Specify name of initialization "
                                           "function"),
                                  cl::value_desc("function name"));

// Pass Statistics
namespace {
  STATISTIC (NullChecks ,
                             "Poolchecks with NULL pool descriptor");
  STATISTIC (FullChecks ,
                             "Poolchecks with non-NULL pool descriptor");

  STATISTIC (PoolChecks , "Poolchecks Added");
#ifdef LLVA_KERNEL
  STATISTIC (MissChecks ,
                             "Poolchecks omitted due to bad pool descriptor");
  STATISTIC (BoundChecks,
                             "Bounds checks inserted");

  STATISTIC (MissedIncompleteChecks ,
                               "Poolchecks missed because of incompleteness");
  STATISTIC (MissedMultDimArrayChecks ,
                                           "Multi-dimensional array checks");

  STATISTIC (MissedStackChecks  , "Missed stack checks");
  STATISTIC (MissedGlobalChecks , "Missed global checks");
  STATISTIC (MissedNullChecks   , "Missed PD checks");
#endif
}

namespace llvm {
////////////////////////////////////////////////////////////////////////////
// InsertPoolChecks Methods
////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////
// Static Functions
////////////////////////////////////////////////////////////////////////////

//
// Function: getNextInst()
//
// Description:
//  Get the next instruction following this instruction.
//
// Return value:
//  0 - There is no instruction after this instruction in the Basic Block.
//  Otherwise, a pointer to the next instruction is returned.
//
static Instruction *
getNextInst (Instruction * Inst) {
  BasicBlock::iterator i(Inst);
  ++i;
  if ((i) == Inst->getParent()->getInstList().end())
    return 0;
  return i;
}

//
// Function: isEligableForExactCheck()
//
// Return value:
//  true  - This value is eligable for an exactcheck.
//  false - This value is not eligable for an exactcheck.
//
static inline bool
isEligableForExactCheck (Value * Pointer, bool IOOkay) {
  if ((isa<AllocaInst>(Pointer)) ||
      (isa<MallocInst>(Pointer)) ||
      (isa<GlobalVariable>(Pointer)))
    return true;

  if (CallInst* CI = dyn_cast<CallInst>(Pointer)) {
    if (CI->getCalledFunction()) {
      if ((CI->getCalledFunction()->getName() == "__vmalloc" ||
           CI->getCalledFunction()->getName() == "malloc" ||
           CI->getCalledFunction()->getName() == "kmalloc" ||
           CI->getCalledFunction()->getName() == "kmem_cache_alloc" ||
           CI->getCalledFunction()->getName() == "__alloc_bootmem")) {
        return true;
      }

      if (IOOkay && (CI->getCalledFunction()->getName() == "__ioremap")) {
        return true;
      }
    }
  }

  return false;
}

//
// Function: findSourcePointer()
//
// Description:
//  Given a pointer value, attempt to find a source of the pointer that can
//  be used in an exactcheck().
//
// Outputs:
//  indexed - Flags whether the data flow went through a indexing operation
//            (i.e. a GEP).  This value is always written.
//
static Value *
findSourcePointer (Value * PointerOperand, bool & indexed, bool IOOkay = true) {
  //
  // Attempt to look for the originally allocated object by scanning the data
  // flow up.
  //
  indexed = false;
  Value * SourcePointer = PointerOperand;
  Value * OldSourcePointer = 0;
  while (!isEligableForExactCheck (SourcePointer, IOOkay)) {
    assert (OldSourcePointer != SourcePointer);
    OldSourcePointer = SourcePointer;

    // Check for GEP and cast constant expressions
    if (ConstantExpr * cExpr = dyn_cast<ConstantExpr>(SourcePointer)) {
      if ((cExpr->isCast()) ||
          (cExpr->getOpcode() == Instruction::GetElementPtr)) {
        if (isa<PointerType>(cExpr->getOperand(0)->getType())) {
          SourcePointer = cExpr->getOperand(0);
          continue;
        }
      }
      // We cannot handle this expression; break out of the loop
      break;
    }

    // Check for GEP and cast instructions
    if (GetElementPtrInst * G = dyn_cast<GetElementPtrInst>(SourcePointer)) {
      SourcePointer = G->getPointerOperand();
      indexed = true;
      continue;
    }

    if (CastInst * CastI = dyn_cast<CastInst>(SourcePointer)) {
      if (isa<PointerType>(CastI->getOperand(0)->getType())) {
        SourcePointer = CastI->getOperand(0);
        continue;
      }
      break;
    }

    // Check for call instructions to exact checks.
    CallInst * CI1;
    if ((CI1 = dyn_cast<CallInst>(SourcePointer)) &&
        (CI1->getCalledFunction()) &&
        (CI1->getCalledFunction()->getName() == "exactcheck3")) {
      SourcePointer = CI1->getOperand (2);
      continue;
    }

    // We can't scan through any more instructions; give up
    break;
  }

  if (isEligableForExactCheck (SourcePointer, IOOkay))
    PointerOperand = SourcePointer;

  return PointerOperand;
}

//
// Function: addExactCheck()
//
// Description:
//  Utility routine that inserts a call to exactcheck().  This function can
//  perform some optimization be determining if the arguments are constant.
//  If they are, we can forego inserting the call.
//
// Inputs:
//  Index - An LLVM Value representing the index of the access.
//  Bounds - An LLVM Value representing the bounds of the check.
//
void
InsertPoolChecks::addExactCheck (Value * Pointer,
                                 Value * Index, Value * Bounds,
                                 Instruction * InsertPt) {
  //
  // Record that this value was checked.
  //
  dsnPass->CheckedValues.insert (Pointer);

  //
  // Attempt to determine statically if this check will always pass; if so,
  // then don't bother doing it at run-time.
  //
  ConstantInt * CIndex  = dyn_cast<ConstantInt>(Index);
  ConstantInt * CBounds = dyn_cast<ConstantInt>(Bounds);
  if (CIndex && CBounds) {
    int index  = CIndex->getSExtValue();
    int bounds = CBounds->getSExtValue();
    assert ((index >= 0) && "exactcheck: const negative index");
    assert ((index < bounds) && "exactcheck: const out of range");

    return;
  }

  //
  // Second, cast the operands to the correct type.
  //
  Value * CastIndex = Index;
  if (Index->getType() != Type::Int32Ty)
    CastIndex = castTo (Index, Type::Int32Ty,
                             Index->getName()+".ec.casted", InsertPt);

  Value * CastBounds = Bounds;
  if (Bounds->getType() != Type::Int32Ty)
    CastBounds = castTo (Bounds, Type::Int32Ty,
                              Bounds->getName()+".ec.casted", InsertPt);

  const Type *VoidPtrType = PointerType::getUnqual(Type::Int8Ty); 
  Value * CastResult = Pointer;
  if (CastResult->getType() != VoidPtrType)
    CastResult = castTo (CastResult, VoidPtrType,
                              CastResult->getName()+".ec.casted", InsertPt);

  std::vector<Value *> args(1, CastIndex);
  args.push_back(CastBounds);
  args.push_back(CastResult);
      CallInst::Create (ExactCheck, args.begin(), args.end(), "ec", InsertPt);

#if 0
  //
  // Replace the old index with the return value of exactcheck(); this
  // prevents GCC from removing it completely.
  //
  Value * CastCI = CI;
  if (CI->getType() != GEP->getType())
    CastCI = castTo  (CI, GEP->getType(), GEP->getName(), InsertPt);

  Value::use_iterator UI = GEP->use_begin();
  for (; UI != GEP->use_end(); ++UI) {
    if (((*UI) != CI) && ((*UI) != CastResult))
      UI->replaceUsesOfWith (GEP, CastCI);
  }
#endif

  return;
}

//
// Function: addExactCheck2()
//
// Description:
//  Utility routine that inserts a call to exactcheck2().
//
// Inputs:
//  BasePointer   - An LLVM Value representing the base of the object to check.
//  Result        - An LLVM Value representing the pointer to check.
//  Bounds        - An LLVM Value representing the bounds of the check.
//  InsertPt      - The instruction before which to insert the check.
//
void
InsertPoolChecks::addExactCheck2 (Value * BasePointer,
                                  Value * Result,
                                  Value * Bounds,
                                  Instruction * InsertPt) {
  Value * ResultPointer = Result;

  // The LLVM type for a void *
  Type *VoidPtrType = PointerType::getUnqual(Type::Int8Ty); 

  //
  // Cast the operands to the correct type.
  //
  if (BasePointer->getType() != VoidPtrType)
    BasePointer = castTo (BasePointer, VoidPtrType,
                          BasePointer->getName()+".ec2.casted",
                          InsertPt);

  if (ResultPointer->getType() != VoidPtrType)
    ResultPointer = castTo (ResultPointer, VoidPtrType,
                            ResultPointer->getName()+".ec2.casted",
                            InsertPt);

  Value * CastBounds = Bounds;
  if (Bounds->getType() != Type::Int32Ty)
    CastBounds = castTo (Bounds, Type::Int32Ty,
                         Bounds->getName()+".ec.casted", InsertPt);

  //
  // Create the call to exactcheck2().
  //
  std::vector<Value *> args(1, BasePointer);
  args.push_back(ResultPointer);
  args.push_back(CastBounds);
  Instruction * CI;
  CI = CallInst::Create (ExactCheck2, args.begin(), args.end(), "", InsertPt);

  //
  // Record that this value was checked.
  //
  dsnPass->CheckedValues.insert (Result);

#if 0
  //
  // Replace the old pointer with the return value of exactcheck2(); this
  // prevents GCC from removing it completely.
  //
  if (CI->getType() != GEP->getType())
    CI = castTo (CI, GEP->getType(), GEP->getName(), InsertPt);

  Value::use_iterator UI = GEP->use_begin();
  for (; UI != GEP->use_end(); ++UI) {
    if (((*UI) != CI) && ((*UI) != ResultPointer))
      UI->replaceUsesOfWith (GEP, CI);
  }
#endif

  return;
}


//
// Function: insertExactCheck()
//
// Description:
//  Attepts to insert an efficient, accurate array bounds check for the given
//  GEP instruction; this check will not use Pools are MetaPools.
//
// Return value:
//  true  - An exactcheck() was successfully added.
//  false - An exactcheck() could not be added; a more extensive check will be
//          needed.
//
bool
InsertPoolChecks::insertExactCheck (GetElementPtrInst * GEP) {
  // The pointer operand of the GEP expression
  Value * PointerOperand = GEP->getPointerOperand();

  //
  // Get the DSNode for the instruction
  //
  Function *F   = GEP->getParent()->getParent();
  DSNode * Node = dsnPass->getDSNode(GEP, F);
  assert (Node && "boundscheck: DSNode is NULL!");

#if 0
  //
  // Determine whether an alignment check is needed.  This occurs when a DSNode
  // is type unknown (collapsed) but has pointers to type known (uncollapsed)
  // DSNodes.
  //
  if (preSCPass->nodeNeedsAlignment (Node)) {
    ++AlignChecks;
  }
#endif

  //
  // Attempt to find the object which we need to check.
  //
  bool WasIndexed = true;
  PointerOperand = findSourcePointer (PointerOperand, WasIndexed);

  //
  // Ensure the pointer operand really is a pointer.
  //
  if (!isa<PointerType>(PointerOperand->getType()))
    return false;

  //
  // Find the insertion point for the run-time check.
  //
  //BasicBlock::iterator InsertPt = AI->getParent()->begin();
  BasicBlock::iterator InsertPt = GEP;
  ++InsertPt;

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(PointerOperand)) {
    //
    // Attempt to remove checks on GEPs that only index into structures.
    // These criteria must be met:
    //  1) The pool must be Type-Homogoneous.
    //
#if 0
    if ((!(Node->isNodeCompletelyFolded())) &&
        (indexesStructsOnly (GEP))) {
      ++StructGEPsRemoved;
      return true;
    }
#endif

    //
    // Attempt to use a call to exactcheck() to check this value if it is a
    // global variable and, if it is a global array, it has a non-zero size.
    // We do not check zero length arrays because in C they are often used to
    // declare an external array of unknown size as follows:
    //        extern struct foo the_array[];
    //
    const Type * GlobalType = GV->getType()->getElementType();
    const ArrayType *AT = dyn_cast<ArrayType>(GlobalType);
    if ((!AT) || (AT->getNumElements())) {
      Value* Size = ConstantInt::get (Type::Int32Ty,
                                      TD->getABITypeSize(GlobalType));
      addExactCheck2 (PointerOperand, GEP, Size, InsertPt);
      return true;
    }
  }

  //
  // If the pointer was generated by a dominating alloca instruction, we can
  // do an exactcheck on it, too.
  //
  if (AllocationInst *AI = dyn_cast<AllocationInst>(PointerOperand)) {
    //
    // Attempt to remove checks on GEPs that only index into structures.
    // These criteria must be met:
    //  1) The pool must be Type-Homogoneous.
    //
#if 0
    if ((!(Node->isNodeCompletelyFolded())) &&
        (indexesStructsOnly (GEP))) {
      ++StructGEPsRemoved;
      return true;
    }
#endif

    const Type * AllocaType = AI->getAllocatedType();
    Value *AllocSize=ConstantInt::get(Type::Int32Ty, TD->getABITypeSize(AllocaType));

    if (AI->isArrayAllocation())
      AllocSize = BinaryOperator::create(Instruction::Mul,
                                         AllocSize,
                                         AI->getOperand(0), "sizetmp", GEP);

    addExactCheck2 (PointerOperand, GEP, AllocSize, InsertPt);
    return true;
  }

  //
  // If the pointer was an allocation, we should be able to do exact checks
  //
  CallInst* CI = dyn_cast<CallInst>(PointerOperand);
  if (CI && (CI->getCalledFunction())) {
    if ((CI->getCalledFunction()->getName() == "__vmalloc") || 
        (CI->getCalledFunction()->getName() == "kmalloc") || 
        (CI->getCalledFunction()->getName() == "malloc") || 
        (CI->getCalledFunction()->getName() == "__alloc_bootmem")) {
      //
      // Attempt to remove checks on GEPs that only index into structures.
      // These criteria must be met:
      //  1) The pool must be Type-Homogoneous.
      //
#if 0
      if ((!(Node->isNodeCompletelyFolded())) &&
          (indexesStructsOnly (GEP))) {
        ++StructGEPsRemoved;
        return true;
      }
#endif

      Value* Cast = castTo (CI->getOperand(1), Type::Int32Ty, "", GEP);
      addExactCheck2(PointerOperand, GEP, Cast, InsertPt);
      return true;
    } else if (CI->getCalledFunction()->getName() == "__ioremap") {
      Value* Cast = castTo (CI->getOperand(2), Type::Int32Ty, "", GEP);
      addExactCheck2(PointerOperand, GEP, Cast, InsertPt);
      return true;
    }
  }

  //
  // If the pointer is to a structure, we may be able to perform a simple
  // exactcheck on it, too, unless the array is at the end of the structure.
  // Then, we assume it's a variable length array and must be full checked.
  //
#if 0
  if (const PointerType * PT = dyn_cast<PointerType>(PointerOperand->getType()))
    if (const StructType *ST = dyn_cast<StructType>(PT->getElementType())) {
      const Type * CurrentType = ST;
      ConstantInt * C;
      for (unsigned index = 2; index < GEP->getNumOperands() - 1; ++index) {
        //
        // If this GEP operand is a constant, index down into the next type.
        //
        if (C = dyn_cast<ConstantInt>(GEP->getOperand(index))) {
          if (const StructType * ST2 = dyn_cast<StructType>(CurrentType)) {
            CurrentType = ST2->getElementType(C->getZExtValue());
            continue;
          }

          if (const ArrayType * AT = dyn_cast<ArrayType>(CurrentType)) {
            CurrentType = AT->getElementType();
            continue;
          }

          // We don't know how to handle this type of element
          break;
        }

        //
        // If the GEP operand is not constant and points to an array type,
        // then try to insert an exactcheck().
        //
        const ArrayType * AT;
        if ((AT = dyn_cast<ArrayType>(CurrentType)) && (AT->getNumElements())) {
          const Type* csiType = Type::getPrimitiveType(Type::Int32Ty);
          ConstantInt * Bounds = ConstantInt::get(csiType,AT->getNumElements());
          addExactCheck (GEP, GEP->getOperand (index), Bounds);
          return true;
        }
      }
    }
#endif

  /*
   * We were not able to insert a call to exactcheck().
   */
  return false;
}

//
// Function: insertExactCheck()
//
// Description:
//  Attepts to insert an efficient, accurate array bounds check for the given
//  GEP instruction; this check will not use Pools are MetaPools.
//
// Inputs:
//  I        - The instruction for which we are adding the check.
//  Src      - The pointer that needs to be checked.
//  Size     - The size, in bytes, that will be read/written by instruction I.
//  InsertPt - The instruction before which the check should be inserted.
//
// Return value:
//  true  - An exactcheck() was successfully added.
//  false - An exactcheck() could not be added; a more extensive check will be
//          needed.
//
bool
InsertPoolChecks::insertExactCheck (Instruction * I,
                                    Value * Src,
                                    Value * Size,
                                    Instruction * InsertPt) {
  // The pointer operand of the GEP expression
  Value * PointerOperand = Src;

  //
  // Get the DSNode for the instruction
  //
#if 1
  Function *F   = I->getParent()->getParent();
  DSGraph & TDG = dsnPass->getDSGraph(*F);
  DSNode * Node = TDG.getNodeForValue(I).getNode();
  if (!Node)
    return false;
#endif

  //
  // Determine whether an alignment check is needed.  This occurs when a DSNode
  // is type unknown (collapsed) but has pointers to type known (uncollapsed)
  // DSNodes.
  //
#if 0
  if (preSCPass->nodeNeedsAlignment (Node)) {
    ++AlignChecks;
  }
#endif

  //
  // Attempt to find the original object for which this check applies.
  // This involves unpeeling casts, GEPs, etc.
  //
  bool WasIndexed = true;
  PointerOperand = findSourcePointer (PointerOperand, WasIndexed);

  //
  // Ensure the pointer operand really is a pointer.
  //
  if (!isa<PointerType>(PointerOperand->getType()))
  {
    return false;
  }

  //
  // Attempt to use a call to exactcheck() to check this value if it is a
  // global array with a non-zero size.  We do not check zero length arrays
  // because in C they are often used to declare an external array of unknown
  // size as follows:
  //        extern struct foo the_array[];
  //
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(PointerOperand)) {
    const Type* csiType = Type::Int32Ty;
    unsigned int arraysize = TD->getABITypeSize(GV->getType()->getElementType());
    ConstantInt * Bounds = ConstantInt::get(csiType, arraysize);
    if (WasIndexed)
      addExactCheck2 (PointerOperand, Src, Bounds, InsertPt);
    else
      addExactCheck (Src, Size, Bounds, InsertPt);
    return true;
  }

  //
  // If the pointer was generated by a dominating alloca instruction, we can
  // do an exactcheck on it, too.
  //
  if (AllocaInst *AI = dyn_cast<AllocaInst>(PointerOperand)) {
    const Type * AllocaType = AI->getAllocatedType();
    Value *AllocSize=ConstantInt::get(Type::Int32Ty, TD->getABITypeSize(AllocaType));

    if (AI->isArrayAllocation())
      AllocSize = BinaryOperator::create(Instruction::Mul,
                                         AllocSize,
                                         AI->getOperand(0), "allocsize", InsertPt);

    if (WasIndexed)
      addExactCheck2 (PointerOperand, Src, AllocSize, InsertPt);
    else
      addExactCheck (Src, Size, AllocSize, InsertPt);
    return true;
  }

  //
  // If the pointer was an allocation, we should be able to do exact checks
  //
  if(CallInst* CI = dyn_cast<CallInst>(PointerOperand)) {
    if (CI->getCalledFunction() && (
                              CI->getCalledFunction()->getName() == "__vmalloc" || 
                              CI->getCalledFunction()->getName() == "malloc" || 
                              CI->getCalledFunction()->getName() == "kmalloc")) {
      Value* Cast = castTo (CI->getOperand(1), Type::Int32Ty, "allocsize", InsertPt);
      if (WasIndexed)
        addExactCheck2 (PointerOperand, Src, Cast, InsertPt);
      else
        addExactCheck (Src, Size, Cast, InsertPt);
      return true;
    }
  }

  //
  // We were not able to insert a call to exactcheck().
  //
  return false;
}

void
InsertPoolChecks::addCheckProto(Module &M) {
  static const Type * VoidTy = Type::VoidTy;
  static const Type * Int32Ty = Type::Int32Ty;
  static const Type * vpTy = PointerType::getUnqual(Type::Int8Ty);

  REG_FUNC (PoolCheck,        VoidTy, "poolcheck",          vpTy, vpTy)
  REG_FUNC (PoolCheckUI,      VoidTy, "poolcheckui",        vpTy, vpTy)
  REG_FUNC (PoolCheckArray,   VoidTy, "boundscheck",        vpTy, vpTy, vpTy)
  REG_FUNC (PoolCheckArrayUI, VoidTy, "boundscheckui",      vpTy, vpTy, vpTy)
  REG_FUNC (ExactCheck,       VoidTy, "exactcheck",         Int32Ty, Int32Ty)
  REG_FUNC (ExactCheck2,      vpTy,   "exactcheck2",         vpTy, vpTy, Int32Ty)
  std::vector<const Type *> FArg3(1, Type::Int32Ty);
  FArg3.push_back(vpTy);
  FArg3.push_back(vpTy);
  FunctionType *FunctionCheckTy = FunctionType::get(Type::VoidTy, FArg3, true);
  FunctionCheck = M.getOrInsertFunction("funccheck", FunctionCheckTy);
  REG_FUNC (GetActualValue,   vpTy,   "pchk_getActualValue",vpTy, vpTy)

  // Special cases for var args

}
  
bool
InsertPoolChecks::doInitialization(Module &M) {
  addCheckProto(M);
  return true;
}

bool
InsertPoolChecks::runOnFunction(Function &F) {
  abcPass = &getAnalysis<ArrayBoundsCheck>();
#ifndef LLVA_KERNEL
  paPass = &getAnalysis<PoolAllocateGroup>();
  // paPass = getAnalysisToUpdate<PoolAllocateGroup>();
  assert (paPass && "Pool Allocation Transform *must* be run first!");
  TD  = &getAnalysis<TargetData>();
#endif
  dsnPass = &getAnalysis<DSNodePass>();
  addPoolChecks(F);
  return true;
}

bool
InsertPoolChecks::doFinalization(Module &M) {
  //
	// Update the statistics.
  //
	PoolChecks = NullChecks + FullChecks;
	return true;
}

void
InsertPoolChecks::addPoolChecks(Function &F) {
  if (!DisableGEPChecks) {
    Function::iterator fI = F.begin(), fE = F.end();
    for ( ; fI != fE; ++fI) {
      BasicBlock * BB = fI;
      addGetElementPtrChecks (BB);
    }
  }
  if (!DisableLSChecks)  addLoadStoreChecks(F);
}

void
InsertPoolChecks::addGetActualValue (ICmpInst *SCI, unsigned operand) {
#if 1
  // We know that the operand is a pointer type 
  Value *op   = SCI->getOperand(operand);
  Function *F = SCI->getParent()->getParent();

#ifndef LLVA_KERNEL    
#if 0
  // Some times the ECGraphs doesnt contain F for newly created cloned
  // functions
  if (!equivPass->ContainsDSGraphFor(*F)) {
    PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*F);
    op = FI->MapValueToOriginal(op);
    if (!op) return; //abort();
  }
#endif
#endif    

  Function *Fnew = F;
  Value *PH = 0;
  if (Argument *arg = dyn_cast<Argument>(op)) {
    Fnew = arg->getParent();
    PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*Fnew);
    PH = dsnPass->getPoolHandle(op, Fnew, *FI);
  } else if (Instruction *Inst = dyn_cast<Instruction>(op)) {
    Fnew = Inst->getParent()->getParent();
    PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*Fnew);
    PH = dsnPass->getPoolHandle(op, Fnew, *FI);
  } else if (isa<Constant>(op)) {
    return;
    //      abort();
  } else if (!isa<ConstantPointerNull>(op)) {
    //has to be a global
    abort();
  }
  op = SCI->getOperand(operand);
  if (!isa<ConstantPointerNull>(op)) {
    if (PH) {
      if (1) { //HACK fixed
        const Type * VoidPtrType = PointerType::getUnqual(Type::Int8Ty);
        Value * PHVptr = castTo (PH, VoidPtrType, PH->getName()+".casted", SCI);
        Value * OpVptr = castTo (op, VoidPtrType, op->getName()+".casted", SCI);

        std::vector<Value *> args = make_vector(PHVptr, OpVptr,0);
        CallInst *CI = CallInst::Create (GetActualValue, args.begin(), args.end(), "getval", SCI);
        Instruction *CastBack = castTo (CI, op->getType(),
                                         op->getName()+".castback", SCI);
        SCI->setOperand (operand, CastBack);
      }
    } else {
      //It shouldn't work if PH is not null
    }
  }
#endif
}


#ifdef LLVA_KERNEL
//
// Method: addLSChecks()
//
// Description:
//  Insert a poolcheck() into the code for a load or store instruction.
//
void InsertPoolChecks::addLSChecks(Value *V, Instruction *I, Function *F) {
  DSGraph & TDG = TDPass->getDSGraph(*F);
  DSNode * Node = TDG.getNodeForValue(V).getNode();
  
  if (Node && Node->isNodeCompletelyFolded()) {
    if (!EnableIncompleteChecks) {
      if (Node->isIncomplete()) {
        ++MissedIncompleteChecks;
        return;
      }
    }
    // Get the pool handle associated with this pointer.  If there is no pool
    // handle, use a NULL pointer value and let the runtime deal with it.
    PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*F);
    Value *PH = dsnPass->getPoolHandle(V, F, *FI);
#ifdef DEBUG
std::cerr << "LLVA: addLSChecks: Pool " << PH << " Node " << Node << std::endl;
#endif
    // FIXME: We cannot handle checks to global or stack positions right now.
    if ((!PH) || (Node->isAllocaNode()) || (Node->isGlobalNode())) {
      ++NullChecks;
      if (!PH) ++MissedNullChecks;
      if (Node->isAllocaNode()) ++MissedStackChecks;
      if (Node->isGlobalNode()) ++MissedGlobalChecks;

      // Don't bother to insert the NULL check unless the user asked
      if (!EnableNullChecks)
        return;
      PH = Constant::getNullValue(PointerType::getUnqual(Type::Int8Ty));
    } else {
      //
      // Only add the pool check if the pool is a global value or it
      // belongs to the same basic block.
      //
      if (isa<GlobalValue>(PH)) {
        ++FullChecks;
      } else if (isa<Instruction>(PH)) {
        Instruction * IPH = (Instruction *)(PH);
        if (IPH->getParent() == I->getParent()) {
          //
          // If the instructions belong to the same basic block, ensure that
          // the pool dominates the load/store.
          //
          Instruction * IP = IPH;
          for (IP=IPH; (IP->isTerminator()) || (IP == I); IP=IP->getNext()) {
            ;
          }
          if (IP == I)
            ++FullChecks;
          else {
            ++MissChecks;
            return;
          }
        } else {
          ++MissChecks;
          return;
        }
      } else {
        ++MissChecks;
        return;
      }
    }      
    // Create instructions to cast the checked pointer and the checked pool
    // into sbyte pointers.
    CastInst *CastVI = 
      CastInst::createPointerCast(V, 
		   PointerType::getUnqual(Type::Int8Ty), "node.lscasted", I);
    CastInst *CastPHI = 
      CastInst::createPointerCast(PH, 
		   PointerType::getUnqual(Type::Int8Ty), "poolhandle.lscasted", I);

    // Create the call to poolcheck
    std::vector<Value *> args(1,CastPHI);
    args.push_back(CastVI);
    CallInst::Create(PoolCheck,args,"", I);
  }
}

void
InsertPoolChecks::addLoadStoreChecks(Function &F) {
    for (inst_iterator I = inst_begin(&F), E = inst_end(&F); I != E; ++I) {
    if (LoadInst *LI = dyn_cast<LoadInst>(&*I)) {
      Value *P = LI->getPointerOperand();
      addLSChecks(P, LI, F);
    } else if (StoreInst *SI = dyn_cast<StoreInst>(&*I)) {
      Value *P = SI->getPointerOperand();
      addLSChecks(P, SI, F);
    } else if (ICmpInst *CmpI = dyn_cast<ICmpInst>(&*I)) {
      switch (CmpI->getPredicate()) {
        ICmpInst::Predicate::ICMP_EQ:
        ICmpInst::Predicate::ICMP_NE:
          // Replace all pointer operands with the getActualValue() call
          assert ((CmpI->getNumOperands() == 2) &&
                   "nmber of operands for CmpI different from 2 ");
          if (isa<PointerType>(CmpI->getOperand(0)->getType())) {
            // we need to insert a call to getactualvalue
            // First get the poolhandle for the pointer
            // TODO: We don't have a working getactualvalue(), so don't waste
            // time calling it.
            if ((!isa<ConstantPointerNull>(CmpI->getOperand(0))) &&
                (!isa<ConstantPointerNull>(CmpI->getOperand(1)))) {
              addGetActualValue(CmpI, 0);
              addGetActualValue(CmpI, 1);
            }
          }
          break;

        default:
          break;
      }
    }
  }
}
#else
//
// Method: addLSChecks()
//
// Inputs:
//  Vnew        - The pointer operand of the load/store instruction.
//  V           - ?
//  Instruction - The load or store instruction
//  F           - The parent function of the instruction
//
void
InsertPoolChecks::addLSChecks (Value *Vnew,
                               const Value *V,
                               Instruction *I,
                               Function *F) {

  PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*F);
  Value *PH = dsnPass->getPoolHandle(V, F, *FI );
  DSNode* Node = dsnPass->getDSNode(V, F);
  if (!PH) {
    return;
  }

  if (isa<ConstantPointerNull>(PH)) {
    //we have a collapsed/Unknown pool
    Value *PH = dsnPass->getPoolHandle(V, F, *FI, true); 
#if 0
    assert (PH && "Null pool handle!\n");
#else
    if (!PH) return;
#endif
  }

  //
  // Do not perform checks on incomplete nodes.  While external heap
  // allocations can be recorded via hooking functionality in the system's
  // original allocator routines, external globals and stack allocations remain
  // invisible.
  //
  if (Node && (Node->isIncompleteNode())) return;

  //
  // Only check pointers to type-unknown objects.
  //
  if (Node && Node->isNodeCompletelyFolded()) {
    if (dyn_cast<CallInst>(I)) {
      // Do not perform function checks on incomplete nodes
      if (Node->isIncompleteNode()) return;

      // Get the globals list corresponding to the node
      std::vector<Function *> FuncList;
      Node->addFullFunctionList(FuncList);
      std::vector<Function *>::iterator flI= FuncList.begin(), flE = FuncList.end();
      unsigned num = FuncList.size();
      if (flI != flE) {
        const Type* csiType = Type::Int32Ty;
        Value *NumArg = ConstantInt::get(csiType, num);	
               
        CastInst *CastVI = 
          CastInst::createPointerCast (Vnew, 
           PointerType::getUnqual(Type::Int8Ty), "casted", I);

        std::vector<Value *> args(1, NumArg);
        args.push_back(CastVI);
        for (; flI != flE ; ++flI) {
          Function *func = *flI;
          CastInst *CastfuncI = 
            CastInst::createPointerCast (func, 
             PointerType::getUnqual(Type::Int8Ty), "casted", I);
          args.push_back(CastfuncI);
        }
        CallInst::Create(FunctionCheck, args.begin(), args.end(), "", I);
      }
    } else {
      //
      // If the pointer used for the load/store check is trivially seen to be
      // valid (load/store to allocated memory or a global variable), don't
      // bother doing a check.
      //
      if ((isa<AllocationInst>(Vnew)) || (isa<GlobalVariable>(Vnew)))
        return;

      //
      // If we've already checked this pointer, don't bother checking it again.
      //
      if (dsnPass->CheckedValues.find (Vnew) != dsnPass->CheckedValues.end())
        return;

      bool indexed = true;
      Value * SourcePointer = findSourcePointer (Vnew, indexed, false);
      if (isEligableForExactCheck (SourcePointer, false)) {
        Value * AllocSize;
        if (AllocationInst * AI = dyn_cast<AllocationInst>(SourcePointer)) {
          AllocSize = ConstantInt::get (Type::Int32Ty,
                                        TD->getABITypeSize(AI->getAllocatedType()));
          if (AI->isArrayAllocation()) {
            AllocSize = BinaryOperator::create (Instruction::Mul,
                                               AllocSize,
                                               AI->getArraySize(), "sizetmp", I);
          }
        } else if (GlobalVariable * GV = dyn_cast<GlobalVariable>(SourcePointer)) {
          AllocSize = ConstantInt::get (Type::Int32Ty,
                                        TD->getABITypeSize(GV->getType()->getElementType()));
        } else {
          assert (0 && "Cannot handle source pointer!\n");
        }
        addExactCheck2 (SourcePointer, Vnew, AllocSize, I);
      } else {
        CastInst *CastVI = 
          CastInst::createPointerCast (Vnew, 
                 PointerType::getUnqual(Type::Int8Ty), "casted", I);
        CastInst *CastPHI = 
          CastInst::createPointerCast (PH, 
                 PointerType::getUnqual(Type::Int8Ty), "casted", I);
        std::vector<Value *> args(1,CastPHI);
        args.push_back(CastVI);

        dsnPass->CheckedDSNodes.insert (Node);
        dsnPass->CheckedValues.insert (Vnew);
        if (Node->isIncompleteNode())
          CallInst::Create(PoolCheckUI,args.begin(), args.end(), "", I);
        else
          CallInst::Create(PoolCheck,args.begin(), args.end(), "", I);
      }
    }
  }
}

void InsertPoolChecks::addLoadStoreChecks(Function &F){
  //here we check that we only do this on original functions
  //and not the cloned functions, the cloned functions may not have the
  //DSG
  bool isClonedFunc = false;
  if (paPass->getFuncInfo(F))
    isClonedFunc = false;
  else
    isClonedFunc = true;
  Function *Forig = &F;
  PA::FuncInfo *FI = paPass->getFuncInfoOrClone(F);
  if (isClonedFunc) {
      Forig = paPass->getOrigFunctionFromClone(&F);
  }
  //we got the original function

  for (inst_iterator I = inst_begin(&F), E = inst_end(&F); I != E; ++I) {
    if (LoadInst *LI = dyn_cast<LoadInst>(&*I)) {
      // We need to get the LI from the original function
      Value *P = LI->getPointerOperand();
      if (isClonedFunc) {
        assert (FI && "No FuncInfo for this function\n");
        assert((FI->MapValueToOriginal(LI)) && " not in the value map \n");
        const LoadInst *temp = dyn_cast<LoadInst>(FI->MapValueToOriginal(LI));
        assert(temp && " Instruction  not there in the NewToOldValue map");
        const Value *Ptr = temp->getPointerOperand();
        addLSChecks(P, Ptr, LI, Forig);
      } else {
        addLSChecks(P, P, LI, Forig);
      }
    } else if (StoreInst *SI = dyn_cast<StoreInst>(&*I)) {
      Value *P = SI->getPointerOperand();
      if (isClonedFunc) {
        std::cerr << *(SI) << std::endl;
#if 0
        assert(FI->NewToOldValueMap.count(SI) && " not in the value map \n");
#else
        assert((FI->MapValueToOriginal(SI)) && " not in the value map \n");
#endif
#if 0
        const StoreInst *temp = dyn_cast<StoreInst>(FI->NewToOldValueMap[SI]);
#else
        const StoreInst *temp = dyn_cast<StoreInst>(FI->MapValueToOriginal(SI));
#endif
        assert(temp && " Instruction  not there in the NewToOldValue map");
        const Value *Ptr = temp->getPointerOperand();
        addLSChecks(P, Ptr, SI, Forig);
      } else {
        addLSChecks(P, P, SI, Forig);
      }
    } else if (CallInst *CI = dyn_cast<CallInst>(&*I)) {
      Value *FunctionOp = CI->getOperand(0);
      if (!isa<Function>(FunctionOp)) {
        std::cerr << "JTC: LIC: " << F.getName() << " : " << *(CI->getOperand(0)) << std::endl;
        if (isClonedFunc) {
          assert(FI->MapValueToOriginal(CI) && " not in the value map \n");
          const CallInst *temp = dyn_cast<CallInst>(FI->MapValueToOriginal(CI));
          assert(temp && " Instruction  not there in the NewToOldValue map");
          const Value* FunctionOp1 = temp->getOperand(0);
          addLSChecks(FunctionOp, FunctionOp1, CI, Forig);
        } else {
          addLSChecks(FunctionOp, FunctionOp, CI, Forig);
        }
      }
    } 
  }
}

#endif

void
InsertPoolChecks::addGetElementPtrChecks (BasicBlock * BB) {
  std::set<Instruction *> * UnsafeGetElemPtrs = abcPass->getUnsafeGEPs (BB);
  if (!UnsafeGetElemPtrs)
    return;
  std::set<Instruction *>::const_iterator iCurrent = UnsafeGetElemPtrs->begin(),
                                          iEnd     = UnsafeGetElemPtrs->end();
  for (; iCurrent != iEnd; ++iCurrent) {
    // We have the GetElementPtr
    if (!isa<GetElementPtrInst>(*iCurrent)) {
      //Then this must be a function call
      //FIXME, get strcpy and others from the backup dir and adjust them for LLVA
      //Right now I just add memset &llva_memcpy for LLVA
      //      std::cerr << " function call \n";
#ifdef LLVA_KERNEL
      CallInst *CI = dyn_cast<CallInst>(*iCurrent);
      if (CI && (!DisableIntrinsicChecks)) {
        Value *Fop = CI->getOperand(0);
        Function *F = CI->getParent()->getParent();
        if (Fop->getName() == "llva_memcpy") {
          Value *PH = dsnPass->getPoolHandle(CI->getOperand(1), F); 
          Instruction *InsertPt = CI;
          if (!PH) {
            ++NullChecks;
            ++MissedNullChecks;

            // Don't bother to insert the NULL check unless the user asked
            if (!EnableNullChecks)
              continue;
            PH = Constant::getNullValue(PointerType::getUnqual(Type::Int8Ty));
          }
          CastInst *CastCIUint = 
            CastInst::createPointerCast(CI->getOperand(1), Type::Int32Ty, "node.lscasted", InsertPt);
          CastInst *CastCIOp3 = 
            CastInst::createZExtOrBitCast(CI->getOperand(3), Type::Int32Ty, "node.lscasted", InsertPt);
          Instruction *Bop = BinaryOperator::create(Instruction::Add, CastCIUint,
                          CastCIOp3, "memcpyadd",InsertPt);
          
          // Create instructions to cast the checked pointer and the checked pool
          // into sbyte pointers.
          CastInst *CastSourcePointer = 
            CastInst::createPointerCast(CI->getOperand(1), 
                         PointerType::getUnqual(Type::Int8Ty), "memcpy.1.casted", InsertPt);
          CastInst *CastCI = 
            CastInst::createPointerCast(Bop, 
                         PointerType::getUnqual(Type::Int8Ty), "mempcy.2.casted", InsertPt);
          CastInst *CastPHI = 
            CastInst::createPointerCast(PH, 
                         PointerType::getUnqual(Type::Int8Ty), "poolhandle.lscasted", InsertPt);
          
          // Create the call to poolcheck
          std::vector<Value *> args(1,CastPHI);
          args.push_back(CastSourcePointer);
          args.push_back(CastCI);
          CallInst::Create(PoolCheckArray,args.begin(), args.end(),"", InsertPt);
#if 0
        } else if (Fop->getName() == "memset") {
          Value *PH = getPoolHandle(CI->getOperand(1), F); 
          Instruction *InsertPt = CI->getNext();
          if (!PH) {
            NullChecks++;
            // Don't bother to insert the NULL check unless the user asked
            if (!EnableNullChecks)
              continue;
            PH = Constant::getNullValue(PointerType::getUnqual(Type::Int8Ty));
          }
          CastInst *CastCIUint = 
            CastInst::createPointerCast(CI, Type::Int32Ty, "node.lscasted", InsertPt);
          CastInst *CastCIOp3 = 
            CastInst::createZExtOrBitCast(CI->getOperand(3), Type::Int32Ty, "node.lscasted", InsertPt);
          Instruction *Bop = BinaryOperator::create(Instruction::Add, CastCIUint,
                          CastCIOp3, "memsetadd",InsertPt);
          
          // Create instructions to cast the checked pointer and the checked pool
          // into sbyte pointers.
          CastInst *CastSourcePointer = 
            CastInst::createPointerCast(CI->getOperand(1), 
                         PointerType::getUnqual(Type::Int8Ty), "memset.1.casted", InsertPt);
          CastInst *CastCI = 
            CastInst::createPointerCast(Bop, 
                         PointerType::getUnqual(Type::Int8Ty), "memset.2.casted", InsertPt);
          CastInst *CastPHI = 
            CastInst::createPointerCast(PH, 
                         PointerType::getUnqual(Type::Int8Ty), "poolhandle.lscasted", InsertPt);
          
          // Create the call to poolcheck
          std::vector<Value *> args(1,CastPHI);
          args.push_back(CastSourcePointer);
          args.push_back(CastCI);
          CallInst::Create(PoolCheckArray,args,"", InsertPt);
#endif
        }
      }
#endif
      continue;
    }
    GetElementPtrInst *GEP = cast<GetElementPtrInst>(*iCurrent);
    Function *F = GEP->getParent()->getParent();
    // Now we need to decide if we need to pass in the alignmnet
    //for the poolcheck
    //     if (getDSNodeOffset(GEP->getPointerOperand(), F)) {
    //       std::cerr << " we don't handle middle of structs yet\n";
    //assert(!getDSNodeOffset(GEP->getPointerOperand(), F) && " we don't handle middle of structs yet\n");
    //       ++MissChecks;
    //       continue;
    //     }
    
#ifndef LLVA_KERNEL    
    PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*F);
    Instruction *Casted = GEP;
#if 0
    //
    // JTC: Disabled.  I'm not sure why we would look up a cloned value when
    //                 processing an old value.
    //
std::cerr << "Parent: " << GEP->getParent()->getParent()->getName() << std::endl;
std::cerr << "Ins   : " << *GEP << std::endl;
    if (!FI->ValueMap.empty()) {
      assert(FI->ValueMap.count(GEP) && "Instruction not in the value map \n");
      Instruction *temp = dyn_cast<Instruction>(FI->ValueMap[GEP]);
      assert(temp && " Instruction  not there in the Value map");
      Casted  = temp;
    }
#endif
    if (GetElementPtrInst *GEPNew = dyn_cast<GetElementPtrInst>(Casted)) {
      Value *PH = dsnPass->getPoolHandle(GEP, F, *FI);
      if (PH && isa<ConstantPointerNull>(PH)) continue;
      if (insertExactCheck (GEPNew)) continue;
      if (!PH) {
        Value *PointerOperand = GEPNew->getPointerOperand();
        if (ConstantExpr *cExpr = dyn_cast<ConstantExpr>(PointerOperand)) {
          if ((cExpr->getOpcode() == Instruction::Trunc) ||
              (cExpr->getOpcode() == Instruction::ZExt) ||
              (cExpr->getOpcode() == Instruction::SExt) ||
              (cExpr->getOpcode() == Instruction::FPToUI) ||
              (cExpr->getOpcode() == Instruction::FPToSI) ||
              (cExpr->getOpcode() == Instruction::UIToFP) ||
              (cExpr->getOpcode() == Instruction::SIToFP) ||
              (cExpr->getOpcode() == Instruction::FPTrunc) ||
              (cExpr->getOpcode() == Instruction::FPExt) ||
              (cExpr->getOpcode() == Instruction::PtrToInt) ||
              (cExpr->getOpcode() == Instruction::IntToPtr) ||
              (cExpr->getOpcode() == Instruction::BitCast))
            PointerOperand = cExpr->getOperand(0);
        }
        if (GlobalVariable *GV = dyn_cast<GlobalVariable>(PointerOperand)) {
          if (const ArrayType *AT = dyn_cast<ArrayType>(GV->getType()->getElementType())) {
            // We need to insert an actual check.  It could be a select
            // instruction.
            // First get the size.
            // This only works for one or two dimensional arrays.
            if (GEPNew->getNumOperands() == 2) {
              Value *secOp = GEPNew->getOperand(1);
              if (secOp->getType() != Type::Int32Ty) {
                secOp = CastInst::createSExtOrBitCast(secOp, Type::Int32Ty,
                                     secOp->getName()+".ec.casted", Casted);
              }

              const Type* csiType = Type::Int32Ty;
              std::vector<Value *> args(1,secOp);
              args.push_back(ConstantInt::get(csiType,AT->getNumElements()));
              CallInst::Create(ExactCheck,args.begin(), args.end(), "", Casted);
              DEBUG(std::cerr << "Inserted exact check call Instruction \n");
              continue;
            } else if (GEPNew->getNumOperands() == 3) {
              if (ConstantInt *COP = dyn_cast<ConstantInt>(GEPNew->getOperand(1))) {
                // FIXME: assuming that the first array index is 0
                assert((COP->getZExtValue() == 0) && "non zero array index\n");
                Value * secOp = GEPNew->getOperand(2);
                if (secOp->getType() != Type::Int32Ty) {
                  secOp = CastInst::createSExtOrBitCast(secOp, Type::Int32Ty,
                                       secOp->getName()+".ec2.casted", Casted);
                }
                std::vector<Value *> args(1,secOp);
                const Type* csiType = Type::Int32Ty;
                args.push_back(ConstantInt::get(csiType,AT->getNumElements()));
                CallInst::Create(ExactCheck, args.begin(), args.end(), "", getNextInst(Casted));
                continue;
              } else {
                // Handle non constant index two dimensional arrays later
                abort();
              }
            } else {
              // Handle Multi dimensional cases later
              DEBUG(std::cerr << "WARNING: Handle multi dimensional globals later\n");
              (*iCurrent)->dump();
            }
          }
          DEBUG(std::cerr << " Global variable ok \n");
        }

        //      These must be real unknowns and they will be handled anyway
        //      std::cerr << " WARNING, DID NOT HANDLE   \n";
        //      (*iCurrent)->dump();
        continue ;
      } else {
        //
        // Determine if this is a pool belonging to a cloned version of the
        // function.  If so, do not add a pool check.
        //
        if (Instruction * InsPH = dyn_cast<Instruction>(PH)) {
          if ((InsPH->getParent()->getParent()) !=
              (Casted->getParent()->getParent()))
            return;
        }

        BasicBlock::iterator InsertPt = Casted;
        ++InsertPt;
        Casted            = castTo (Casted,
                                    PointerType::getUnqual(Type::Int8Ty),
                                    (Casted)->getName()+".pc.casted",
                                    InsertPt);

        Value * CastedSrc = castTo (GEP->getPointerOperand(),
                                    PointerType::getUnqual(Type::Int8Ty),
                                    (Casted)->getName()+".pcsrc.casted",
                                    InsertPt);

        Value *CastedPH = castTo (PH,
                                  PointerType::getUnqual(Type::Int8Ty),
                                  "jtcph",
                                  InsertPt);
        std::vector<Value *> args(1, CastedPH);
        args.push_back(CastedSrc);
        args.push_back(Casted);

        // Insert it
        DSNode * Node = dsnPass->getDSNode (GEP, F);
        dsnPass->CheckedDSNodes.insert (Node);
        dsnPass->CheckedValues.insert (Casted);
        if (Node->isIncompleteNode())
          CallInst::Create(PoolCheckArrayUI, args.begin(), args.end(),
                           "", InsertPt);
        else
          CallInst::Create(PoolCheckArray, args.begin(), args.end(),
                           "", InsertPt);
        DEBUG(std::cerr << "inserted instrcution \n");
      }
    }
#else
    //
    // Get the pool handle associated with the pointer operand.
    //
    Value *PH = dsnPass->getPoolHandle(GEP->getPointerOperand(), F);
    GetElementPtrInst *GEPNew = GEP;
    Instruction *Casted = GEP;

    DSGraph & TDG = TDPass->getDSGraph(*F);
    DSNode * Node = TDG.getNodeForValue(GEP).getNode();

    DEBUG(std::cerr << "LLVA: addGEPChecks: Pool " << PH << " Node ");
    DEBUG(std::cerr << Node << std::endl);

    Value *PointerOperand = GEPNew->getPointerOperand();
    if (ConstantExpr *cExpr = dyn_cast<ConstantExpr>(PointerOperand)) {
      if (cExpr->getOpcode() == Instruction::Cast)
        PointerOperand = cExpr->getOperand(0);
    }
    if (GlobalVariable *GV = dyn_cast<GlobalVariable>(PointerOperand)) {
      if (const ArrayType *AT = dyn_cast<ArrayType>(GV->getType()->getElementType())) {
        // we need to insert an actual check
        // It could be a select instruction
        // First get the size
        // This only works for one or two dimensional arrays
        if (GEPNew->getNumOperands() == 2) {
          Value *secOp = GEPNew->getOperand(1);
          if (secOp->getType() != Type::Int32Ty) {
            secOp = CastInst::createSExtOrBitCast(secOp, Type::Int32Ty,
                                 secOp->getName()+".ec3.casted", Casted);
          }
          
          std::vector<Value *> args(1,secOp);
          const Type* csiType = Type::getPrimitiveType(Type::Int32TyID);
          args.push_back(ConstantInt::get(csiType,AT->getNumElements()));
          CallInst *newCI = CallInst::Create(ExactCheck,args,"", Casted);
          ++BoundChecks;
          //	    DEBUG(std::cerr << "Inserted exact check call Instruction \n");
          continue;
        } else if (GEPNew->getNumOperands() == 3) {
          if (ConstantInt *COP = dyn_cast<ConstantInt>(GEPNew->getOperand(1))) {
            //FIXME assuming that the first array index is 0
            assert((COP->getZExtValue() == 0) && "non zero array index\n");
            Value * secOp = GEPNew->getOperand(2);
            if (secOp->getType() != Type::Int32Ty) {
              secOp = CastInst::createSExtOrBitCast(secOp, Type::Int32Ty,
                                   secOp->getName()+".ec4.casted", Casted);
            }
            std::vector<Value *> args(1,secOp);
            const Type* csiType = Type::getPrimitiveType(Type::Int32TyID);
            args.push_back(ConstantInt::get(csiType,AT->getNumElements()));
            CallInst *newCI = CallInst::Create(ExactCheck,args,"", Casted->getNext());
            ++BoundChecks;
            continue;
          } else {
            //Handle non constant index two dimensional arrays later
            abort();
          }
        } else {
          //Handle Multi dimensional cases later
          std::cerr << "WARNING: Handle multi dimensional globals later\n";
          (*iCurrent)->dump();
          ++MissedMultDimArrayChecks;
        }
        DEBUG(std::cerr << " Global variable ok \n");
      }
    }

#if 0
    //No checks for incomplete nodes 
    if (!EnableIncompleteChecks) {
      if (Node->isIncomplete()) {
        ++MissedNullChecks;
        continue;
      }
    }
#endif

    //
    // We cannot insert an exactcheck().  Insert a pool check.
    //
    if (!PH) {
      DEBUG(std::cerr << "missing GEP check: Null PH: " << GEP << "\n");
      ++NullChecks;
      if (!PH) ++MissedNullChecks;

      // Don't bother to insert the NULL check unless the user asked
      if (!EnableNullChecks)
      {
        continue;
      }
      PH = Constant::getNullValue(PointerType::getUnqual(Type::Int8Ty));
    } else {
      //
      // Determine whether the pool handle dominates the pool check.
      // If not, then don't insert it.
      //

      //
      // FIXME:
      //  This domination check is too restrictive; it eliminates pools that do
      //  dominate but are outside of the current basic block.
      //
      // Only add the pool check if the pool is a global value or it belongs
      // to the same basic block.
      //
      if (isa<GlobalValue>(PH)) {
        ++FullChecks;
      } else if (isa<Instruction>(PH)) {
        Instruction * IPH = (Instruction *)(PH);
        if (IPH->getParent() == Casted->getParent()) {
          //
          // If the instructions belong to the same basic block, ensure that
          // the pool dominates the load/store.
          //
          Instruction * IP = IPH;
          for (IP=IPH; (IP->isTerminator()) || (IP==Casted); IP=IP->getNext()) {
            ;
          }
          if (IP == Casted)
            ++FullChecks;
          else {
            ++MissChecks;
            continue;
          }
        } else {
          ++MissChecks;
          continue;
        }
      } else {
        ++MissChecks;
        continue;
      }
    }

    //
    // Regardless of the node type, always perform an accurate bounds check.
    //
    Instruction *InsertPt = Casted->getNext();
    if (Casted->getType() != PointerType::getUnqual(Type::Int8Ty)) {
      Casted = CastInst::createPointerCast(Casted,PointerType::getUnqual(Type::Int8Ty),
                            (Casted)->getName()+".pc2.casted",InsertPt);
    }
    Instruction *CastedPointerOperand = CastInst::createPointerCast(PointerOperand,
                                         PointerType::getUnqual(Type::Int8Ty),
                                         PointerOperand->getName()+".casted",InsertPt);
    Instruction *CastedPH = CastInst::createPointerCast(PH,
                                         PointerType::getUnqual(Type::Int8Ty),
                                         "ph",InsertPt);
    std::vector<Value *> args(1, CastedPH);
    args.push_back(CastedPointerOperand);
    args.push_back(Casted);
    CallInst * newCI = CallInst::Create(PoolCheckArray,args, "",InsertPt);
#endif    
  }
}

#undef REG_FUNC
}
