//===- insert.cpp - Insert run-time checks -------------------------------- --//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass instruments a program with the necessary run-time checks for
// SAFECode.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "safecode"

#include "safecode/SAFECode.h"

#include <iostream>

#include "llvm/Instruction.h"
#include "llvm/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/ADT/VectorExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

#include "SCUtils.h"
#include "InsertPoolChecks.h"
#include "safecode/VectorListHelper.h"

NAMESPACE_SC_BEGIN

char InsertPoolChecks::ID = 0;

static RegisterPass<InsertPoolChecks> ipcPass ("safecode", "insert runtime checks");

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
  STATISTIC (StaticChecks , "GEP Checks Done Statically");
  STATISTIC (TotalStatic , "GEP Checks Examined Statically");
  STATISTIC (NullChecks ,
                             "Poolchecks with NULL pool descriptor");
  STATISTIC (FullChecks ,
                             "Poolchecks with non-NULL pool descriptor");

  STATISTIC (PoolChecks , "Poolchecks Added");
  STATISTIC (AlignLSChecks,  "Number of alignment checks on loads/stores");
  STATISTIC (MissedVarArgs , "Vararg functions not processed");
}

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
  if ((isa<AllocationInst>(Pointer)) || (isa<GlobalVariable>(Pointer)))
    return true;

  if (CallInst* CI = dyn_cast<CallInst>(Pointer)) {
    if (CI->getCalledFunction()) {
      if (CI->getCalledFunction()->getName() == "poolalloc") {
        return true;
      }
      if (CI->getCalledFunction()->getName() == "malloc") {
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
// Function: addExactCheck2()
//
// Description:
//  Utility routine that inserts a call to exactcheck2().
//  It also records checked pointers in dsNodePass::checkedValues.
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
  dsnPass->addCheckedValue(Result);
  return;
}

//
// Function: isSafe()
//
// Description:
//  Determine whether the GEP will always generate a pointer that lands within
//  the bounds of the object.
//
// Inputs:
//  TD  - The TargetData pass which should be used for finding type-sizes and
//        offsets of elements within a derived type.
//  GEP - The getelementptr instruction to check.
//
// Return value:
//  true  - The GEP never generates a pointer outside the bounds of the object.
//  false - The GEP may generate a pointer outside the bounds of the object.
//          There may also be cases where we know that the GEP *will* return an
//          out-of-bounds pointer; we let pointer rewriting take care of those
//          cases.
//
static bool
isSafe (TargetData * TD, GetElementPtrInst * GEP) {
  //
  // Update the total number of GEPs that we're trying to check statically.
  //
  ++TotalStatic;

  //
  // Determine whether the indices of the GEP are all constant.  If not, then
  // we don't bother to prove anything.
  //
  for (unsigned index = 1; index < GEP->getNumOperands(); ++index) {
    if (ConstantInt * CI = dyn_cast<ConstantInt>(GEP->getOperand(index))) {
      if (CI->getSExtValue() < 0) {
        return false;
      }
    } else {
      return false;
    }
  }

  //
  // Check to see if we're indexing off the beginning of a known object.  If
  // so, then find the size of the object.  Otherwise, assume the size is zero.
  //
  Value * PointerOperand = GEP->getPointerOperand();
  unsigned int type_size = 0;
  if (GlobalVariable * GV = dyn_cast<GlobalVariable>(PointerOperand)) {
    type_size = TD->getTypeAllocSize (GV->getType()->getElementType());
  } else if (AllocationInst * AI = dyn_cast<AllocationInst>(PointerOperand)) {
    type_size = TD->getTypeAllocSize (AI->getAllocatedType());
    if (AI->isArrayAllocation()) {
      if (ConstantInt * CI = dyn_cast<ConstantInt>(AI->getArraySize())) {
        if (CI->getSExtValue() > 0) {
          type_size *= CI->getSExtValue();
        } else {
          return false;
        }
      }
    }
  }

  //
  // If the type size is non-zero, then we did, in fact, find an object off of
  // which the GEP is indexing.  Statically determine if the indexing operation
  // is always within bounds.
  //
  if (type_size) {
    Value ** Indices = new Value *[GEP->getNumOperands() - 1];

    for (unsigned index = 1; index < GEP->getNumOperands(); ++index) {
      Indices[index - 1] = GEP->getOperand(index);
    }

    unsigned offset = TD->getIndexedOffset (PointerOperand->getType(),
                                            Indices,
                                            GEP->getNumOperands() - 1);

    if (offset < type_size) {
      ++StaticChecks;
      return true;
    }
  }

  //
  // We cannot statically prove that the GEP is safe.
  //
  return false;
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
  // First, attempt to prove that the GEP is safe.
  //
  if (isSafe (TD, GEP))
    return true;

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
                                      TD->getTypeAllocSize(GlobalType));
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
    Value *AllocSize=ConstantInt::get(Type::Int32Ty, TD->getTypeAllocSize(AllocaType));

    if (AI->isArrayAllocation())
      AllocSize = BinaryOperator::Create(Instruction::Mul,
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
    } else if (CI->getCalledFunction()->getName() == "poolalloc") {
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


void
InsertPoolChecks::addCheckProto(Module &M) {
  intrinsic = &getAnalysis<InsertSCIntrinsic>();

  PoolCheck 		= intrinsic->getIntrinsic("sc.lscheck").F;
  PoolCheckUI 		= intrinsic->getIntrinsic("sc.lscheckui").F;
  PoolCheckAlign 	= intrinsic->getIntrinsic("sc.lscheckalign").F;
  PoolCheckAlignUI 	= intrinsic->getIntrinsic("sc.lscheckalignui").F;
  PoolCheckArray 	= intrinsic->getIntrinsic("sc.boundscheck").F;
  PoolCheckArrayUI 	= intrinsic->getIntrinsic("sc.boundscheckui").F;
  ExactCheck		= intrinsic->getIntrinsic("sc.exactcheck").F;
  ExactCheck2 		= intrinsic->getIntrinsic("sc.exactcheck2").F;
  FunctionCheck 	= intrinsic->getIntrinsic("sc.funccheck").F;

  //
  // Mark poolcheck() as only reading memory.
  //
  PoolCheck->setOnlyReadsMemory();
  PoolCheckUI->setOnlyReadsMemory();
  PoolCheckAlign->setOnlyReadsMemory();
  PoolCheckAlignUI->setOnlyReadsMemory();

  // Special cases for var args
}

bool
InsertPoolChecks::runOnFunction(Function &F) {
  static bool uninitialized = true;
  if (uninitialized) {
    addCheckProto(*F.getParent());
    uninitialized = false;
  }

  abcPass = &getAnalysis<ArrayBoundsCheckGroup>();
  paPass = &getAnalysis<PoolAllocateGroup>();
  // paPass = getAnalysisIfAvailable<PoolAllocateGroup>();
  assert (paPass && "Pool Allocation Transform *must* be run first!");
  TD  = &getAnalysis<TargetData>();
  dsnPass = &getAnalysis<DSNodePass>();

  //std::cerr << "Running on Function " << F.getName() << std::endl;

  //
  // FIXME:
  //  We need to insert checks for variadic functions, too.
  //
  if (F.isVarArg())
    ++MissedVarArgs;
  else
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
    for (inst_iterator I = inst_begin(&F), E = inst_end(&F); I != E; ++I) {
      if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&*I))
        addGetElementPtrChecks (GEP);
    }
  }
  if (!DisableLSChecks)  addLoadStoreChecks(F);
}


//
// Method: insertAlignmentCheck()
//
// Description:
//  Insert an alignment check for the specified value.
//
void
InsertPoolChecks::insertAlignmentCheck (LoadInst * LI) {
  // Get the function containing the load instruction
  Function * F = LI->getParent()->getParent();

  // Get the DSNode for the result of the load instruction.  If it is type
  // unknown, then no alignment check is needed.
  DSNode * LoadResultNode = dsnPass->getDSNode (LI,F);
  if (!(LoadResultNode && (!(LoadResultNode->isNodeCompletelyFolded())))) {
    return;
  }

  //
  // Get the pool handle for the node.
  //
  PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*F);
  Value *PH = dsnPass->getPoolHandle(LI, F, *FI);
  if (!PH) return;

  //
  // If the node is incomplete or unknown, then only perform the check if
  // checks to incomplete or unknown are allowed.
  //
  Constant * ThePoolCheckFunction = PoolCheckAlign;
  if ((LoadResultNode->isUnknownNode()) ||
      (LoadResultNode->isIncompleteNode())) {
#if 0
    if (EnableUnknownChecks) {
      ThePoolCheckFunction = PoolCheckAlignUI;
    } else {
      ++MissedIncompleteChecks;
      return;
    }
#else
    ThePoolCheckFunction = PoolCheckAlignUI;
    return;
#endif
  }

  //
  // A check is needed.  Scan through the links of the DSNode of the load's
  // pointer operand; we need to determine the offset for the alignment check.
  //
  DSNode * Node = dsnPass->getDSNode (LI->getPointerOperand(), F);
  if (!Node) return;
  for (unsigned i = 0 ; i < Node->getNumLinks(); i+=4) {
    DSNodeHandle & LinkNode = Node->getLink(i);
    if (LinkNode.getNode() == LoadResultNode) {
      // Insertion point for this check is *after* the load.
      BasicBlock::iterator InsertPt = LI;
      ++InsertPt;

      // Create instructions to cast the checked pointer and the checked pool
      // into sbyte pointers.
      Value *CastVI  = castTo (LI, PointerType::getUnqual(Type::Int8Ty), InsertPt);
      Value *CastPHI = castTo (PH, PointerType::getUnqual(Type::Int8Ty), InsertPt);

      // Create the call to poolcheck
      std::vector<Value *> args(1,CastPHI);
      args.push_back(CastVI);
      args.push_back (ConstantInt::get(Type::Int32Ty, LinkNode.getOffset()));
      CallInst::Create (ThePoolCheckFunction,args.begin(), args.end(), "", InsertPt);

      // Update the statistics
      ++AlignLSChecks;

      break;
    }
  }
}

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
  //
  //
  // FIXME:
  //  This optimization is not safe.  We need to ensure that the memory is
  //  not freed between the previous check and this check.
  //
  // If we've already checked this pointer, don't bother checking it again.
  //
  if (dsnPass->isValueChecked (Vnew))
    return;

  //
  // This may be a load instruction that loads a pointer that:
  //  1) Points to a type known pool, and
  //  2) Loaded from a type unknown pool
  //
  // If this is the case, we need to perform an alignment check on the result
  // of the load.  Do that here.
  //
  if (LoadInst * LI = dyn_cast<LoadInst>(I)) {
    insertAlignmentCheck (LI);
  }

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
  if (Node && (Node->isExternalNode())) return;

  //
  // We need to check two types of pointers:
  //  1) All Type-Unknown pointers.  These can be pointing anywhere.
  //  2) Type-Known pointers into an array.  If we reach this point in the
  //     code, then no previous GEP check has verified that this pointer is
  //     within bounds.  Therefore, a load/store check is needed to ensure that
  //     the pointer is within bounds.
  //
  if (Node && (Node->isNodeCompletelyFolded() || Node->isArray())) {
    if (dyn_cast<CallInst>(I)) {
      // Do not perform function checks on incomplete nodes
      if (Node->isIncompleteNode()) return;

      // Get the globals list corresponding to the node
      std::vector<const Function *> FuncList;
      Node->addFullFunctionList(FuncList);
      std::vector<const Function *>::iterator flI= FuncList.begin(), flE = FuncList.end();
      unsigned num = FuncList.size();
      if (flI != flE) {
        const Type* csiType = Type::Int32Ty;
        Value *NumArg = ConstantInt::get(csiType, num);	
               
        CastInst *CastVI = 
          CastInst::CreatePointerCast (Vnew, 
           PointerType::getUnqual(Type::Int8Ty), "casted", I);

        std::vector<Value *> args(1, NumArg);
        args.push_back(CastVI);
        for (; flI != flE ; ++flI) {
          Function *func = (Function *)(*flI);
          CastInst *CastfuncI = 
            CastInst::CreatePointerCast (func, 
             PointerType::getUnqual(Type::Int8Ty), "casted", I);
          args.push_back(CastfuncI);
        }
        CallInst::Create(FunctionCheck, args.begin(), args.end(), "", I);
      }
    } else {
      //
      // FIXME:
      //  The next two lines should ensure that the allocation size is large
      //  enough for whatever value is being loaded/stored.
      //
      // If the pointer used for the load/store check is trivially seen to be
      // valid (load/store to allocated memory or a global variable), don't
      // bother doing a check.
      //
      if ((isa<AllocationInst>(Vnew)) || (isa<GlobalVariable>(Vnew)))
        return;

      bool indexed = true;
      Value * SourcePointer = findSourcePointer (Vnew, indexed, false);
      if (isEligableForExactCheck (SourcePointer, false)) {
        Value * AllocSize;
        if (AllocationInst * AI = dyn_cast<AllocationInst>(SourcePointer)) {
          AllocSize = ConstantInt::get (Type::Int32Ty,
                                        TD->getTypeAllocSize(AI->getAllocatedType()));
          if (AI->isArrayAllocation()) {
            AllocSize = BinaryOperator::Create (Instruction::Mul,
                                               AllocSize,
                                               AI->getArraySize(), "sizetmp", I);
          }
        } else if (GlobalVariable * GV = dyn_cast<GlobalVariable>(SourcePointer)) {
          AllocSize = ConstantInt::get (Type::Int32Ty,
                                        TD->getTypeAllocSize(GV->getType()->getElementType()));
        } else if (CallInst * CI = dyn_cast<CallInst>(SourcePointer)) {
          assert (CI->getCalledFunction() && "Indirect call!\n");
          if (CI->getCalledFunction()->getName() == "poolalloc") {
            AllocSize = CI->getOperand(2);
          } else {
            assert (0 && "Cannot recognize allocator for source pointer!\n");
          }
        } else {
          assert (0 && "Cannot handle source pointer!\n");
        }

        addExactCheck2 (SourcePointer, Vnew, AllocSize, I);
      } else {
        CastInst *CastVI = 
          CastInst::CreatePointerCast (Vnew, 
                 PointerType::getUnqual(Type::Int8Ty), "casted", I);
        CastInst *CastPHI = 
          CastInst::CreatePointerCast (PH, 
                 PointerType::getUnqual(Type::Int8Ty), "casted", I);
        std::vector<Value *> args(1,CastPHI);
        args.push_back(CastVI);

        dsnPass->addCheckedDSNode(Node);
        dsnPass->addCheckedValue(Vnew);
        Constant * PoolCheckFunc = (Node->isIncompleteNode()) ? PoolCheckUI
                                                              : PoolCheck;
        CallInst::Create (PoolCheckFunc, args.begin(), args.end(), "", I);
      }
    }
  }
}

void InsertPoolChecks::addLoadStoreChecks(Function &F){
  //here we check that we only do this on original functions
  //and not the cloned functions, the cloned functions may not have the
  //DSG

  bool isClonedFunc = false;
  Function *Forig = &F;
  PA::FuncInfo *FI = NULL;

  if (!SCConfig->SVAEnabled) {
    if (paPass->getFuncInfo(F))
      isClonedFunc = false;
    else
      isClonedFunc = true;
    
    FI = paPass->getFuncInfoOrClone(F);
    if (isClonedFunc) {
      Forig = paPass->getOrigFunctionFromClone(&F);
    }
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
        if (!SCConfig->SVAEnabled) {
          // The ASM Writer does not handle inline assembly very well
          llvm::cerr << "JTC: Indirect Function Call Check: "
                     << F.getName() << " : " << *(CI->getOperand(0)) << std::endl;
        }
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

void
InsertPoolChecks::addGetElementPtrChecks (GetElementPtrInst * GEP) {
  if (abcPass->isGEPSafe(GEP))
    return;

  if (dsnPass->isValueChecked(GEP))
    return;

  Instruction * iCurrent = GEP;

    // We have the GetElementPtr
    if (!isa<GetElementPtrInst>(*iCurrent)) {
      //Then this must be a function call
      //FIXME, get strcpy and others from the backup dir and adjust them for LLVA
      //Right now I just add memset &llva_memcpy for LLVA
      //      std::cerr << " function call \n";
      return;
    }
    Function *F = GEP->getParent()->getParent();
    // Now we need to decide if we need to pass in the alignmnet
    //for the poolcheck
    //     if (getDSNodeOffset(GEP->getPointerOperand(), F)) {
    //       std::cerr << " we don't handle middle of structs yet\n";
    //assert(!getDSNodeOffset(GEP->getPointerOperand(), F) && " we don't handle middle of structs yet\n");
    //       ++MissChecks;
    //       continue;
    //     }
    
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
      if (PH && isa<ConstantPointerNull>(PH)) return;
      if (insertExactCheck (GEPNew)) {
        DSNode * Node = dsnPass->getDSNode (GEP, F);
        dsnPass->addCheckedDSNode(Node);
        // checked value is inserted by addExactCheck2(), which is called by insertExactCheck()
        return;
      }

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
                secOp = CastInst::CreateSExtOrBitCast(secOp, Type::Int32Ty,
                                     secOp->getName()+".ec.casted", Casted);
              }

              const Type* csiType = Type::Int32Ty;
              std::vector<Value *> args(1,secOp);
              args.push_back(ConstantInt::get(csiType,AT->getNumElements()));
              CallInst::Create(ExactCheck,args.begin(), args.end(), "", Casted);
              DEBUG(std::cerr << "Inserted exact check call Instruction \n");
              return;
            } else if (GEPNew->getNumOperands() == 3) {
              if (ConstantInt *COP = dyn_cast<ConstantInt>(GEPNew->getOperand(1))) {
                // FIXME: assuming that the first array index is 0
                assert((COP->getZExtValue() == 0) && "non zero array index\n");
                Value * secOp = GEPNew->getOperand(2);
                if (secOp->getType() != Type::Int32Ty) {
                  secOp = CastInst::CreateSExtOrBitCast(secOp, Type::Int32Ty,
                                       secOp->getName()+".ec2.casted", Casted);
                }
                std::vector<Value *> args(1,secOp);
                const Type* csiType = Type::Int32Ty;
                args.push_back(ConstantInt::get(csiType,AT->getNumElements()));
                CallInst::Create(ExactCheck, args.begin(), args.end(), "", getNextInst(Casted));
                return;
              } else {
                // Handle non constant index two dimensional arrays later
                abort();
              }
            } else {
              // Handle Multi dimensional cases later
              DEBUG(std::cerr << "WARNING: Handle multi dimensional globals later\n");
              GEP->dump();
            }
          }
          DEBUG(std::cerr << " Global variable ok \n");
        }

        //      These must be real unknowns and they will be handled anyway
        //      std::cerr << " WARNING, DID NOT HANDLE   \n";
        //      (*iCurrent)->dump();
        return ;
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
        dsnPass->addCheckedDSNode(Node);
        dsnPass->addCheckedValue(GEPNew);

        Instruction * CI;
        if (Node->isIncompleteNode())
          CI = CallInst::Create(PoolCheckArrayUI, args.begin(), args.end(),
                                "", InsertPt);
        else
          CI = CallInst::Create(PoolCheckArray, args.begin(), args.end(),
                                "", InsertPt);

        DEBUG(std::cerr << "inserted instrcution \n");
      }
    }
}

NAMESPACE_SC_END
