//===-------- String.cpp - Secure C standard string library calls ---------===//
//
//                          The SAFECode Compiler
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass finds all calls to functions in the C standard string library and
// transforms them to a more secure form.
//
//===----------------------------------------------------------------------===//

//
// To add a new function to the CStdLib checks, the following modifications are
// necessary:
// 
// In SAFECode:
//
//   - Add the pool_* prototype of the function to
//     runtime/include/CStdLibSupport.h.
//
//   - Implement the pool_* version of the function in the relevant file in
//     runtime/DebugRuntime.
//
//   - Add debug instrumentation information to
//     lib/DebugInstrumentation/DebugInstrumentation.cpp.
//
//   - Update the StringTransform pass to transform calls of the library
//     function into its pool_* version in lib/CStdLib/String.cpp.
//
// In poolalloc:
//
//   - Add an entry for the pool_* version of the function containing the
//     number of initial pool arguments to the structure in
//     include/dsa/CStdLib.h.
//
//   - Add an entry to lib/DSA/StdLibPass.cpp for the pool_* version of the
//     function to allow DSA to recognize it.
//


#include "safecode/CStdLib.h"
#include "safecode/Config/config.h"

#include <cstdarg>
#include <string>

using std::string;

using namespace llvm;

namespace llvm
{

// Identifier variable for the pass
char StringTransform::ID = 0;

// Statistics counters

STATISTIC(st_xform_vprintf,   "Total vprintf() calls transformed");
STATISTIC(st_xform_vfprintf,  "Total vfprintf() calls transformed");
STATISTIC(st_xform_vsprintf,  "Total vsprintf() calls transformed");
STATISTIC(st_xform_vsnprintf, "Total vsnprintf() calls transformed");
STATISTIC(st_xform_vscanf,    "Total vscanf() calls transformed");
STATISTIC(st_xform_vsscanf,   "Total vsscanf() calls transformed");
STATISTIC(st_xform_vfscanf,   "Total vfscanf() calls transformed");
STATISTIC(st_xform_vsyslog,   "Total vsyslog() calls transformed");
STATISTIC(st_xform_memccpy, "Total memccpy() calls transformed");
STATISTIC(st_xform_memchr,  "Total memchr() calls transformed");
STATISTIC(st_xform_memcmp,  "Total memcmp() calls transformed");
STATISTIC(st_xform_memcpy,  "Total memcpy() calls transformed");
STATISTIC(st_xform_memmove, "Total memmove() calls transformed");
STATISTIC(st_xform_memset,  "Total memset() calls transformed");
STATISTIC(st_xform_strcat,  "Total strcat() calls transformed");
STATISTIC(st_xform_strchr,  "Total strchr() calls transformed");
STATISTIC(st_xform_strcmp,  "Total strcmp() calls transformed");
STATISTIC(st_xform_strcoll, "Total strcoll() calls transformed");
STATISTIC(st_xform_strcpy,  "Total strcpy() calls transformed");
STATISTIC(st_xform_strcspn, "Total strcspn() calls transformed");
// strerror_r
STATISTIC(st_xform_strlen,  "Total strlen() calls transformed");
STATISTIC(st_xform_strncat, "Total strncat() calls transformed");
STATISTIC(st_xform_strncmp, "Total strncmp() calls transformed");
STATISTIC(st_xform_strncpy, "Total strncpy() calls transformed");
STATISTIC(st_xform_strpbrk, "Total strpbrk() calls transformed");
STATISTIC(st_xform_strrchr, "Total strrchr() calls transformed");
STATISTIC(st_xform_strspn,  "Total strspn() calls transformed");
STATISTIC(st_xform_strstr,  "Total strstr() calls transformed");
STATISTIC(st_xform_strxfrm, "Total strxfrm() calls transformed");
// strtok, strtok_r, strxfrm

#ifdef HAVE_MEMPCPY
STATISTIC(st_xform_mempcpy,  "Total mempcpy() calls transformed");
#endif
#ifdef HAVE_STRCASESTR
STATISTIC(st_xform_strcasestr,  "Total strcasestr() calls transformed");
#endif
#ifdef HAVE_STPCPY
STATISTIC(st_xform_stpcpy,  "Total stpcpy() calls transformed");
#endif
#ifdef HAVE_STRNLEN
STATISTIC(st_xform_strnlen, "Total strnlen() calls transformed");
#endif

STATISTIC(st_xform_bcmp,    "Total bcmp() calls transformed");
STATISTIC(st_xform_bcopy,   "Total bcopy() calls transformed");
STATISTIC(st_xform_bzero,   "Total bzero() calls transformed");
STATISTIC(st_xform_index,   "Total index() calls transformed");
STATISTIC(st_xform_rindex,  "Total rindex() calls transformed");
STATISTIC(st_xform_strcasecmp,  "Total strcasecmp() calls transformed");
STATISTIC(st_xform_strncasecmp, "Total strncasecmp() calls transformed");

STATISTIC(st_xform_fgets, "Total fgets() calls transformed");
STATISTIC(st_xform_fputs, "Total fputs() calls transformed");
STATISTIC(st_xform_fwrite, "Total fwrite() calls transformed");
STATISTIC(st_xform_fread, "Total fread() calls transformed");
STATISTIC(st_xform_gets, "Total gets() calls transformed");
STATISTIC(st_xform_puts, "Total puts() calls transformed");

//
// Functions that aren't handled (yet...):
//  - stpncpy and __stpncpy_chk
//  - setbuf
//  - setvbuf
//

static RegisterPass<StringTransform>
ST("string_transform", "Secure C standard string library calls");

//
// Entry point for the LLVM pass that transforms C standard string library calls
//
bool
StringTransform::runOnModule(Module &M)
{
  // Flags whether we modified the module.
  bool chgd = false;

  tdata = &getAnalysis<TargetData>();

  // Create needed pointer types (char * == i8 * == VoidPtrTy).
  Type *VoidPtrTy = IntegerType::getInt8PtrTy(M.getContext());
  // Determine the type of size_t for functions that return this result.
  Type *SizeTTy = tdata->getIntPtrType(M.getContext());
  // Create other return types (int, void).
  Type *Int32Ty = IntegerType::getInt32Ty(M.getContext());
  Type *VoidTy  = Type::getVoidTy(M.getContext());

  // Functions from <stdio.h>, <syslog.h>
  chgd |= transform(M, "vprintf",  2, 1, Int32Ty, st_xform_vprintf);
  chgd |= transform(M, "vfprintf", 3, 2, Int32Ty, st_xform_vfprintf);
  chgd |= transform(M, "vsprintf", 3, 2, Int32Ty, st_xform_vsprintf);
  chgd |= transform(M, "vscanf",   2, 1, Int32Ty, st_xform_vscanf);
  chgd |= transform(M, "vsscanf",  3, 2, Int32Ty, st_xform_vsscanf);
  chgd |= transform(M, "vfscanf",  3, 2, Int32Ty, st_xform_vfscanf);
  SourceFunction VSNPf   = { "vsnprintf", Int32Ty, 4 };
  SourceFunction VSyslog = { "vsyslog", VoidTy, 3 };
  DestFunction PVSNPf   = { "pool_vsnprintf", 4, 2 };
  DestFunction PVSyslog = { "pool_vsyslog", 3, 1 };
  // Note that we need to switch the order of arguments to the instrumented
  // calls of vsnprintf() and vsyslog(), because the CStdLib pass convention is
  // to place all the interesting pointer arguments at the start of the
  // parameter list, but these functions have initial arguments that are
  // non-pointers.
  chgd |= vtransform(M, VSNPf, PVSNPf, st_xform_vsnprintf, 1u, 3u, 2u, 4u);
  chgd |= vtransform(M, VSyslog, PVSyslog, st_xform_vsyslog, 2u, 1u, 3u);
  // __isoc99_vscanf family: these are found in glibc
  SourceFunction IsoC99Vscanf  = { "__isoc99_vscanf", Int32Ty, 2 };
  SourceFunction IsoC99Vsscanf = { "__isoc99_vsscanf", Int32Ty, 3 };
  SourceFunction IsoC99Vfscanf = { "__isoc99_vfscanf", Int32Ty, 3 };
  DestFunction PVscanf  = { "pool_vscanf", 2, 1 };
  DestFunction PVsscanf = { "pool_vsscanf", 3, 2 };
  DestFunction PVfscanf = { "pool_vfscanf", 3, 2 };
  chgd |= vtransform(M, IsoC99Vscanf, PVscanf, st_xform_vscanf, 1u, 2u);
  chgd |= vtransform(M, IsoC99Vsscanf, PVsscanf, st_xform_vsscanf, 1u, 2u);
  chgd |= vtransform(M, IsoC99Vfscanf, PVfscanf, st_xform_vfscanf, 1u, 2u);
  // __vsprintf_chk, __vsnprintf_chk
  SourceFunction VSPfC  = { "__vsprintf_chk", Int32Ty, 5 };
  SourceFunction VSNPfC = { "__vsnprintf_chk", Int32Ty, 6 };
  DestFunction PVSPf = { "pool_vsprintf", 3, 2 };
  chgd |= vtransform(M, VSPfC, PVSPf, st_xform_vsprintf, 1u, 4u, 5u);
  chgd |= vtransform(M, VSNPfC, PVSNPf, st_xform_vsnprintf, 1u, 5u, 2u, 6u);
  // Functions from <string.h>
  chgd |= transform(M, "memccpy", 4, 2, VoidPtrTy, st_xform_memccpy);
  chgd |= transform(M, "memchr",  3, 1, VoidPtrTy, st_xform_memchr);
  chgd |= transform(M, "memcmp",  3, 2, Int32Ty,   st_xform_memcmp);
  chgd |= transform(M, "memcpy",  3, 2, Int32Ty,   st_xform_memcpy);
  chgd |= transform(M, "memmove", 3, 2, VoidPtrTy, st_xform_memmove);
  chgd |= transform(M, "memset",  2, 1, VoidPtrTy, st_xform_memset);
  chgd |= transform(M, "strcat",  2, 2, VoidPtrTy, st_xform_strcat);
  chgd |= transform(M, "strchr",  2, 1, VoidPtrTy, st_xform_strchr);
  chgd |= transform(M, "strcmp",  2, 2, Int32Ty,   st_xform_strcmp);
  chgd |= transform(M, "strcoll", 2, 2, Int32Ty,   st_xform_strcoll);
  chgd |= transform(M, "strcpy",  2, 2, VoidPtrTy, st_xform_strcpy);
  chgd |= transform(M, "strcspn", 2, 2, SizeTTy,   st_xform_strcspn);
  // chgd |= handle_strerror_r(M);
  chgd |= transform(M, "strlen",  1, 1, SizeTTy,   st_xform_strlen);
  chgd |= transform(M, "strncat", 3, 2, VoidPtrTy, st_xform_strncat);
  chgd |= transform(M, "strncmp", 3, 2, Int32Ty,   st_xform_strncmp);
  chgd |= transform(M, "strncpy", 3, 2, VoidPtrTy, st_xform_strncpy);
  chgd |= transform(M, "strpbrk", 2, 2, VoidPtrTy, st_xform_strpbrk);
  chgd |= transform(M, "strrchr", 2, 1, VoidPtrTy, st_xform_strrchr);
  chgd |= transform(M, "strspn",  2, 2, SizeTTy,   st_xform_strspn);
  chgd |= transform(M, "strstr",  2, 2, VoidPtrTy, st_xform_strstr);
  chgd |= transform(M, "strxfrm", 3, 2, SizeTTy,   st_xform_strxfrm);
  // Extensions to <string.h>
#ifdef HAVE_MEMPCPY
  chgd |= transform(M, "mempcpy", 3, 2, VoidPtrTy, st_xform_mempcpy);
#endif
#ifdef HAVE_STRCASESTR
  chgd |= transform(M, "strcasestr", 2, 2, VoidPtrTy, st_xform_strcasestr);
#endif
#ifdef HAVE_STPCPY
  chgd |= transform(M, "stpcpy",  2, 2, VoidPtrTy, st_xform_stpcpy);
#endif
#ifdef HAVE_STRNLEN
  chgd |= transform(M, "strnlen", 2, 1, SizeTTy,   st_xform_strnlen);
#endif
  // Functions from <strings.h>
  chgd |= transform(M, "bcmp",    3, 2, Int32Ty,   st_xform_bcmp);
  chgd |= transform(M, "bcopy",   3, 2, VoidTy,    st_xform_bcopy);
  chgd |= transform(M, "bzero",   2, 1, VoidTy,    st_xform_bzero);
  chgd |= transform(M, "index",   2, 1, VoidPtrTy, st_xform_index);
  chgd |= transform(M, "rindex",  2, 1, VoidPtrTy, st_xform_rindex);
  chgd |= transform(M, "strcasecmp", 2, 2, Int32Ty, st_xform_strcasecmp);
  chgd |= transform(M, "strncasecmp", 3, 2, Int32Ty, st_xform_strncasecmp);
  // Darwin-specific secure extensions to <string.h>
  SourceFunction MemcpyChk  = { "__memcpy_chk", VoidPtrTy, 4 };
  SourceFunction MemmoveChk = { "__memmove_chk", VoidPtrTy, 4 };
  SourceFunction MemsetChk  = { "__memset_chk", VoidPtrTy, 4 };
  SourceFunction StrcpyChk  = { "__strcpy_chk", VoidPtrTy, 3 };
  SourceFunction StrcatChk  = { "__strcat_chk", VoidPtrTy, 3 };
  SourceFunction StrncatChk = { "__strncat_chk", VoidPtrTy, 4 };
  SourceFunction StrncpyChk = { "__strncpy_chk", VoidPtrTy, 4 };
  DestFunction PoolMemcpy  = { "pool_memcpy", 3, 2 };
  DestFunction PoolMemmove = { "pool_memmove", 3, 2 };
  DestFunction PoolMemset  = { "pool_memset", 3, 1 };
  DestFunction PoolStrcpy  = { "pool_strcpy", 2, 2 };
  DestFunction PoolStrcat  = { "pool_strcat", 2, 2 };
  DestFunction PoolStrncat = { "pool_strncat", 3, 2 };
  DestFunction PoolStrncpy = { "pool_strncpy", 3, 2 };
  chgd |= vtransform(M, MemcpyChk, PoolMemcpy, st_xform_memcpy, 1u, 2u, 3u);
  chgd |= vtransform(M, MemmoveChk, PoolMemmove, st_xform_memmove, 1u, 2u, 3u);
  chgd |= vtransform(M, MemsetChk, PoolMemset, st_xform_memset, 1u, 2u, 3u);
  chgd |= vtransform(M, StrcpyChk, PoolStrcpy, st_xform_strcpy, 1u, 2u);
  chgd |= vtransform(M, StrcatChk, PoolStrcat, st_xform_strcat, 1u, 2u);
  chgd |= vtransform(M, StrncatChk, PoolStrncat, st_xform_strncat, 1u, 2u, 3u);
  chgd |= vtransform(M, StrncpyChk, PoolStrncpy, st_xform_strncpy, 1, 2u, 3u);
#ifdef HAVE_STPCPY
  SourceFunction StpcpyChk = { "__stpcpy_chk", VoidPtrTy, 3 };
  DestFunction PoolStpcpy = { "pool_stpcpy", 2, 2 };
  chgd |= vtransform(M, StpcpyChk, PoolStpcpy, st_xform_stpcpy, 1u, 2u);
#endif

  // Functions from <stdio.h> 
  chgd |= transform(M, "fgets", 3, 1, VoidPtrTy, st_xform_fgets);
  chgd |= transform(M, "fputs", 2, 1, Int32Ty, st_xform_fputs);
  chgd |= transform(M, "fwrite", 4, 1, SizeTTy, st_xform_fwrite);
  chgd |= transform(M, "fread", 4, 1, SizeTTy, st_xform_fread);
  chgd |= transform(M, "gets", 1, 1, VoidPtrTy, st_xform_gets);
  chgd |= transform(M, "puts", 1, 1, Int32Ty, st_xform_puts);
  return chgd;
}

//
// Simple wrapper to gtransform() for when
//   1) the transformed function is named "pool_" + original name.
//   2) the order and number of arguments is preserved from the original to the
//      transformed function.
//
// Parameters:
//   M         - the module to scan
//   argc      - the expected number of arguments to the original function
//   pool_argc - the number of initial pool parameters to add to the transformed
//               function
//   ReturnTy  - the expected return type of the original function
//   statistic - a reference to a relevant Statistic for the number of
//               transformation
//
// Returns:
//   This function returns true if the module was modified and false otherwise.
//
bool
StringTransform::transform(Module &M,
                           const StringRef FunctionName,
                           const unsigned argc,
                           const unsigned pool_argc,
                           Type *ReturnTy,
                           Statistic &statistic)
{
  SourceFunction src = { FunctionName.data(), ReturnTy, argc };
  string dst_name  = "pool_" + FunctionName.str();
  DestFunction dst = { dst_name.c_str(), argc, pool_argc };
  vector<unsigned> args;
  for (unsigned i = 1; i <= argc; i++)
    args.push_back(i);
  return gtransform(M, src, dst, statistic, args);
}

//
// vtransform() - wrapper to gtransform() that takes variable arguments
// instead of a vector as the final parameter
//
bool
StringTransform::vtransform(Module &M,
                            const SourceFunction &from,
                            const DestFunction &to,
                            Statistic &stat,
                            ...)
{
  vector<unsigned> args;
  va_list ap;
  va_start(ap, stat);
  // Read the positions to append as vararg parameters.
  for (unsigned i = 1; i <= to.source_argc; i++) {
    unsigned position = va_arg(ap, unsigned);
    args.push_back(position);
  }
  va_end(ap);
  return gtransform(M, from, to, stat, args);
}

//
// Secures C standard string library calls by transforming them into
// their corresponding runtime wrapper functions.
//
// The 'from' parameter describes a function to transform. It is struct of
// the form
//   struct { const char *name; Type *return_type; unsigned argc };
// where
//   - 'name' is the name of the function to transform
//   - 'return_type' is its expected return type
//   - 'argc' is its expected number of arguments.
//
// The 'to' parameter describes the function to transform into. It is a struct
// of the form
//   struct { const char *name, unsigned source_argc, unsigned pool_argc };
// where
//   - 'name' is the name of the resulting function
//   - 'source_argc' is the number of parameters the function takes from the
//     original function
//   - 'pool_argc' is the number of initial pool parameters to add.
//
// The 'append_order' vector describes how to move the parameters of the
// original function into the transformed function call.
//
// @param M              Module from runOnModule() to scan for functions to
//                       transform.
// @param from           SourceFunction structure reference described above
// @param to             DestFunction structure reference described above.
// @param stat           A reference to the relevant transform statistic.
// @param append_order   A vector describing the order to add the arguments from
//                       the source function into the destination function.
// @return               Returns true if any calls were transformed, and
//                       false if no changes were made.
//
bool
StringTransform::gtransform(Module &M,
                            const SourceFunction &from,
                            const DestFunction &to,
                            Statistic &stat,
                            const vector<unsigned> &append_order)
{
  // Get the source function if it exists in the module.
  Function *src = M.getFunction(from.name);
  if (!src)
    return false;
  // Make sure the source function behaves as described, otherwise skip it.
  FunctionType *F_type = src->getFunctionType();
  if (F_type->getReturnType() != from.return_type || F_type->isVarArg() ||
      F_type->getNumParams() != from.argc)
    return false;
  // Make sure the append_order vector has the expected number of elements.
  assert(append_order.size() == to.source_argc &&
    "Unexpected number of parameter positions in vector!");
  // Check that each pool parameter has a corresponding original parameter.
  assert(to.pool_argc <= to.source_argc && "More pools than arguments?");
  // Check if the pool completeness information can be fit into a single 8 bit
  // quantity.
  assert(to.pool_argc <= 8 && "Only up to 8 pool parameters supported!");
  std::vector<Instruction *> toModify;
  std::vector<Instruction *>::iterator modifyIter, modifyEnd;
  // Scan through the module for uses of the function to transform.
  for (Value::use_iterator UI = src->use_begin(), UE = src->use_end();
       UI != UE;
       ++UI) {
    CallSite CS(*UI);
    // Ensure the use is an instruction and that the instruction calls the
    // source function (as opposed to passing it as a parameter or other
    // possible uses).
    if (!CS || CS.getCalledValue() != src)
      continue;
    toModify.push_back(CS.getInstruction());
  }
  // Return early if we've found nothing to modify.
  if (toModify.empty())
    return false;
  // The pool handle type is a void pointer (i8 *).
  PointerType *VoidPtrTy = IntegerType::getInt8PtrTy(M.getContext());
  Type *Int8Ty = IntegerType::getInt8Ty(M.getContext());
  // Build the type of the parameters to the transformed function. This function
  // has to.pool_argc initial arguments of type i8 *.
  std::vector<Type *> ParamTy(to.pool_argc, VoidPtrTy);
  // After the initial pool arguments, parameters from the original function go
  // into the type.
  for (unsigned i = 0; i < to.source_argc; i++) {
    unsigned position = append_order[i];
    assert(0 < position && position <= from.argc && "Parameter out of bounds!");
    Type *ParamType = F_type->getParamType(position - 1);
    if (i < to.pool_argc)
      assert(
        isa<PointerType>(ParamType) && "Pointer type expected for parameter!"
      );
    ParamTy.push_back(ParamType);
  }
  // The completeness bitvector goes at the end.
  ParamTy.push_back(Int8Ty);
  // Build the type of the transformed function.
  FunctionType *FT = FunctionType::get(F_type->getReturnType(), ParamTy, false);
#ifndef NDEBUG
  Function *PoolFInModule = M.getFunction(to.name);
  // Make sure that the function declarations don't conflict.
  assert((PoolFInModule == 0 ||
    PoolFInModule->getFunctionType() == FT ||
    PoolFInModule->hasLocalLinkage()) &&
    "Replacement function already declared with wrong type!");
#endif
  // Build the actual transformed function.
  Constant *PoolF = M.getOrInsertFunction(to.name, FT);
  // This is a placeholder value for the pool handles (to be "filled in" later
  // by poolalloc).
  Value *PH = ConstantPointerNull::get(VoidPtrTy);
  // Transform every valid use of the function that was found.
  for (modifyIter = toModify.begin(), modifyEnd = toModify.end();
       modifyIter != modifyEnd;
       ++modifyIter) {
    Instruction *I = *modifyIter;
    // Construct vector of parameters to transformed function call. Also insert
    // NULL pool handles.
    std::vector<Value *> Params(to.pool_argc, PH);
    // Insert the original parameters.
    for (unsigned i = 0; i < to.source_argc; i++) {
      Value *f = I->getOperand(append_order[i] - 1);
      Params.push_back(f);
    }
    // Add the DSA completeness bitvector. Set it to 0 (= incomplete).
    Params.push_back(ConstantInt::get(Int8Ty, 0));
    // Create the call instruction for the transformed function and insert it
    // before the current instruction.
    CallInst *C = CallInst::Create(PoolF, Params, "", I);
    // Transfer debugging metadata if it exists from the old call into the new
    // one.
    if (MDNode *DebugNode = I->getMetadata("dbg"))
      C->setMetadata("dbg", DebugNode);
    // Replace all uses of the function with its transformed equivalent.
    I->replaceAllUsesWith(C);
    I->eraseFromParent();
    // Record the transformation.
    ++stat;
  }
  // Reaching here means some call has been modified;
  return true;
}

}
