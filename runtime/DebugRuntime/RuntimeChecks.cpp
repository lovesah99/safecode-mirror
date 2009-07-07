//===- PoolAllocatorBitMask.cpp - Implementation of poolallocator runtime -===//
// 
//                          The SAFECode Compiler
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements various runtime checks used by SAFECode.
//
//===----------------------------------------------------------------------===//
// NOTES:
//  1) Some of the bounds checking code may appear strange.  The reason is that
//     it is manually inlined to squeeze out some more performance.  Please
//     don't change it.
//
//  2) This run-time performs MMU re-mapping of pages to perform dangling
//     pointer detection.  A "shadow" address is the address of a memory block
//     that has been remapped to a new virtal address; the shadow address is
//     returned to the caller on allocation and is unmapped on deallocation.
//     A "canonical" address is the virtual address of memory as it is mapped
//     in the pool slabs; the canonical address is remapped to different shadow
//     addresses each time that particular piece of memory is allocated.
//
//     In normal operation, the shadow address and canonical address are
//     identical.
//
//===----------------------------------------------------------------------===//

#include "SafeCodeRuntime.h"
#include "DebugReport.h"
#include "PoolAllocator.h"
#include "PageManager.h"
#include "ConfigData.h"

#include <map>
#include <cstdarg>
#include <cassert>

extern FILE * ReportLog;

using namespace NAMESPACE_SC;

//
// Function: _barebone_poolcheck()
//
// Description:
//  Perform an accurate load/store check for the given pointer.  This function
//  encapsulates the logic necessary to do the check.
//
// Return value:
//  true  - The pointer was found within a valid object within the pool.
//  false - The pointer was not found within a valid object within the pool.
//
static inline bool
_barebone_poolcheck (DebugPoolTy * Pool, void * Node) {
  void * S, * end;

  //
  // If the pool handle is NULL, return successful.
  //
  if (!Pool) return true;

  //
  // Look through the splay trees for an object in which the pointer points.
  //
  bool fs = Pool->Objects.find(Node, S, end);
  if ((fs) && (S <= Node) && (Node <= end)) {
    return true;
  }

  //
  // The node is not found or is not within bounds; fail!
  //
  return false;
}


//
// Function: isRewritePtr()
//
// Description:
//  Determines whether the specified pointer value is a rewritten value for an
//  Out-of-Bounds pointer value.
//
// Return value:
//  true  - The pointer value is an OOB pointer rewrite value.
//  false - The pointer value is the actual value of the pointer.
//
static bool
isRewritePtr (void * p) {
  unsigned ptr = (unsigned) p;

  if ((InvalidLower < ptr ) && (ptr < InvalidUpper))
    return true;
  return false;
}


void
poolcheck_debug (DebugPoolTy *Pool, void *Node, const char * SourceFilep, unsigned lineno) {
  if (_barebone_poolcheck (Pool, Node))
    return;
 
  //
  // Look for the object within the splay tree of external objects.
  //
  int fs = 0;
  void * S, *end;
	if (1) {
		S = Node;
		fs = ExternalObjects.find (Node, S, end);
		if ((fs) && (S <= Node) && (Node <= end)) {
			return;
		}
	}

  //
  // If it's a rewrite pointer, convert it back into its original value so
  // that we can print the real faulting address.
  //
  if (isRewritePtr (Node)) {
    Node = pchk_getActualValue (Pool, Node);
  }

  DebugViolationInfo v;
  v.type = ViolationInfo::FAULT_OUT_OF_BOUNDS,
    v.faultPC = __builtin_return_address(0),
    v.faultPtr = Node,
    v.SourceFile = SourceFilep,
    v.lineNo = lineno;
  
  ReportMemoryViolation(&v);
  return;
}

void
poolcheckui (DebugPoolTy *Pool, void *Node) {
  if (_barebone_poolcheck (Pool, Node))
    return;

  //
  // Look for the object within the splay tree of external objects.
  //
  int fs = 0;
  void * S, *end = 0;
	if (ConfigData.TrackExternalMallocs) {
		S = Node;
		fs = ExternalObjects.find (Node, S, end);
		if ((fs) && (S <= Node) && (Node <= end)) {
			return;
		}
	}

  //
  // The node is not found or is not within bounds.  Report a warning but keep
  // going.
  //
  fprintf (stderr, "PoolcheckUI failed(%p:%x): %p %p from %p\n", 
      (void*)Pool, fs, (void*)Node, end, __builtin_return_address(0));
  fflush (stderr);
  return;
}

//
// Function: boundscheck_lookup()
//
// Description:
//  Perform the lookup for a bounds check.
//
// Inputs:
//  Source - The pointer to look up within the set of valid objects.
//
// Outputs:
//  Source - If the object is found within the pool, this is the address of the
//           first valid byte of the object.
//
//  End    - If the object is found within the pool, this is the address of the
//           last valid byte of the object.
//
// Return value:
//  Returns true if the object is found.
//
static bool 
boundscheck_lookup (DebugPoolTy * Pool, void * & Source, void * & End ) {
  // Search for object for Source in splay tree, return length 
  return Pool->Objects.find(Source, Source, End);
}

//
// Function: boundscheck_check()
//
// Description:
//  This is the slow path for a boundscheck() and boundcheckui() calls.
//
// Inputs:
//  ObjStart - The address of the first valid byte of the object.
//  ObjEnd   - The address of the last valid byte of the object.
//  Pool     - The pool in which the pointer belong.
//  Source   - The source pointer used in the indexing operation (the GEP).
//  Dest     - The result pointer of the indexing operation (the GEP).
//  CanFail  - Flags whether the check can fail (for complete DSNodes).
//
// Note:
//  If ObjLen is zero, then the lookup says that Source was not found within
//  any valid object.
//
static void *
boundscheck_check (bool found, void * ObjStart, void * ObjEnd, DebugPoolTy * Pool,
                   void * Source, void * Dest, bool CanFail,
                   const char * SourceFile, unsigned int lineno) {
  //
  // Determine if this is a rewrite pointer that is being indexed.  If so,
  // compute the original value, re-do the indexing operation, and rewrite the
  // value back.
  //
  if (isRewritePtr (Source)) {
    //
    // Get the real pointer value (which is outside the bounds of a valid
    // object.
    //
    void * RealSrc = pchk_getActualValue (Pool, Source);

    //
    // Compute the real result pointer (the value the GEP would really have on
    // the original pointer value).
    //
    Dest = (void *)((unsigned) RealSrc + ((unsigned) Dest - (unsigned) Source));

    //
    // Retrieve the original bounds of the object.
    //
    // FIXME: the casts are hacks to deal with C++ type systems
    //
    ObjStart = const_cast<void*>(RewrittenObjs[Source].first);
    ObjEnd   = const_cast<void*>(RewrittenObjs[Source].second);

    //
    // Redo the bounds check.  If it succeeds, return the real value.
    // Otherwise, just continue on with the rest of the failed bounds check
    // processing as before.
    //
    if (__builtin_expect (((ObjStart <= Dest) && ((Dest <= ObjEnd))), 1)) {
      if (logregs) {
        fprintf (stderr, "unrewrite(1): (0x%p) -> (0x%p, 0x%p) \n", Source, RealSrc, Dest);
        fflush (stderr);
      }
      return Dest;
    }

    //
    // Pretend this was an index off of the original out of bounds pointer
    // value and continue processing
    //
    if (logregs) {
      fprintf (stderr, "unrewrite(2): (0x%p) -> (0x%p, 0x%p) \n", Source, RealSrc, Dest);
      fflush (stderr);
    }

    found = true;
    Source = RealSrc;
  }

  //
  // Now, we know that the pointer is out of bounds.  If we indexed off the
  // beginning or end of a valid object, determine if we can rewrite the
  // pointer into an OOB pointer.  Whether we can or not depends upon the
  // SAFECode configuration.
  //
  if (found) {
    if ((ConfigData.StrictIndexing == false) ||
        (((char *) Dest) == (((char *)ObjEnd)+1))) {
      void * ptr = rewrite_ptr (Pool, Dest, ObjStart, ObjEnd, SourceFile, lineno);
      if (logregs) {
        fprintf (ReportLog, "boundscheck: rewrite(1): %p %p %p %p at pc=%p to %p at %s (%d)\n",
                 ObjStart, ObjEnd, Source, Dest, (void*)__builtin_return_address(0), ptr, SourceFile, lineno);
        fflush (ReportLog);
      }
      return ptr;
    } else {
      unsigned allocPC = 0;
      unsigned allocID = 0;
      unsigned char * allocSF = (unsigned char *) "<Unknown>";
      unsigned allocLN = 0;
      PDebugMetaData debugmetadataptr = NULL;
      void * S , * end;
      if (dummyPool.DPTree.find(ObjStart, S, end, debugmetadataptr)) {
        allocPC = ((unsigned) (debugmetadataptr->allocPC)) - 5;
        allocID  = debugmetadataptr->allocID;
        allocSF  = (unsigned char *) debugmetadataptr->SourceFile;
        allocLN  = debugmetadataptr->lineno;
      }

      OutOfBoundsViolation v;
      v.type = ViolationInfo::FAULT_OUT_OF_BOUNDS,
        v.faultPC = __builtin_return_address(0),
        v.faultPtr = Dest,
        v.dbgMetaData = debugmetadataptr,
        v.SourceFile = SourceFile,
        v.lineNo = lineno,
        v.objStart = ObjStart,
        v.objLen = (unsigned)((char*) ObjEnd - (char*)(ObjStart)) + 1;

      ReportMemoryViolation(&v);
      return Dest;
    }
  }

  /*
   * Allow pointers to the first page in memory provided that they remain
   * within that page.  Loads and stores using such pointers will fault.  This
   * allows indexing of NULL pointers without error.
   */
  if ((((unsigned char *)0) <= Source) && (Source < (unsigned char *)(4096))) {
    if ((((unsigned char *)0) <= Dest) && (Dest < (unsigned char *)(4096))) {
      if (logregs) {
        fprintf (ReportLog, "boundscheck: NULL Index: %x %x %p %p at pc=%p at %s (%d)\n",
                 0, 4096, (void*)Source, (void*)Dest, (void*)__builtin_return_address(0), SourceFile, lineno);
        fflush (ReportLog);
      }
      return Dest;
    } else {
      if ((ConfigData.StrictIndexing == false) ||
          (((uintptr_t) Dest) == 4096)) {
        if (logregs) {
          fprintf (ReportLog, "boundscheck: rewrite(3): %x %x %p %p at pc=%p at %s (%d)\n",
                   0, 4096, (void*)Source, (void*)Dest, (void*)__builtin_return_address(0), SourceFile, lineno);
          fflush (ReportLog);
        }
        return rewrite_ptr (Pool,
                            Dest,
                            (void *)0,
                            (void *)4096,
                            SourceFile,
                            lineno);
      } else {
        OutOfBoundsViolation v;
        v.type = ViolationInfo::FAULT_OUT_OF_BOUNDS,
          v.faultPC = __builtin_return_address(0),
          v.faultPtr = Dest,
          v.dbgMetaData = NULL,
          v.SourceFile = NULL,
          v.lineNo = 0,
          v.objStart = 0,
          v.objLen = 4096;

        ReportMemoryViolation(&v);
      }
    }
  }

  //
  // Attempt to look for the object in the external object splay tree.
  // Do this even if we're not tracking external allocations because a few
  // other objects without associated pools (e.g., argv pointers) may be
  // registered in here.
  //
  if (1) {
    void * S, * end;
    bool fs = ExternalObjects.find(Source, S, end);
    if (fs) {
      if ((S <= Dest) && (Dest <= end)) {
        return Dest;
      } else {
        if ((ConfigData.StrictIndexing == false) ||
            (((char *) Dest) == (((char *)end)+1))) {
          void * ptr = rewrite_ptr (Pool, Dest, S, end, SourceFile, lineno);
          if (logregs)
            fprintf (ReportLog,
                     "boundscheck: rewrite(2): %p %p %p %p at pc=%p to %p at %s (%d)\n",
                     S, end, Source, Dest, (void*)__builtin_return_address(0),
                     ptr, SourceFile, lineno);
          fflush (ReportLog);
          return ptr;
        }
        
        OutOfBoundsViolation v;
        v.type = ViolationInfo::FAULT_OUT_OF_BOUNDS,
          v.faultPC = __builtin_return_address(0),
          v.faultPtr = Dest,
          v.dbgMetaData = NULL,
          v.SourceFile = SourceFile,
          v.lineNo = lineno,
          v.objStart = ObjStart,
          v.objLen = (unsigned)((char*) end - (char*)(S)) + 1;

        ReportMemoryViolation(&v);
      }
    }
  }

  //
  // We cannot find the object.  Continue execution.
  //
  if (CanFail) {
    OutOfBoundsViolation v;
    v.type = ViolationInfo::FAULT_OUT_OF_BOUNDS,
      v.faultPC = __builtin_return_address(0),
      v.faultPtr = Dest,
      v.dbgMetaData = NULL,
      v.SourceFile = SourceFile,
      v.lineNo = lineno,
      v.objStart = 0,
      v.objLen = 0;
    
    ReportMemoryViolation(&v);

  }

  return Dest;
}

//
// Function: boundscheck_debug()
//
// Description:
//  Identical to boundscheck() except that it takes additional debug info
//  parameters.
//
void *
boundscheck_debug (DebugPoolTy * Pool, void * Source, void * Dest, const char * SourceFile, unsigned lineno) {
  // This code is inlined at all boundscheck() calls

  // Search the splay for Source and return the bounds of the object
  void * ObjStart = Source, * ObjEnd = 0;
  bool ret = boundscheck_lookup (Pool, ObjStart, ObjEnd); 

  if (logregs) {
    fprintf (stderr, "boundscheck_debug: %p - %p\n", ObjStart, ObjEnd);
    fflush (stderr);
  }

  // Check if destination lies in the same object
  if (__builtin_expect ((ret && (ObjStart <= Dest) &&
                        ((Dest <= ObjEnd))), 1)) {
    return Dest;
  } else {
    //
    // Either:
    //  1) A valid object was not found in splay tree, or
    //  2) Dest is not within the valid object in which Source was found
    //
    return boundscheck_check (ret, ObjStart, ObjEnd, Pool, Source, Dest, true, SourceFile, lineno);  
  }
}

//
// Function: boundscheckui_debug()
//
// Description:
//  Identical to boundscheckui() but with debug information.
//
// Inputs:
//  Pool       - The pool to which the pointers (Source and Dest) should
//               belong.
//  Source     - The Source pointer of the indexing operation (the GEP).
//  Dest       - The result of the indexing operation (the GEP).
//  SourceFile - The source file in which the check was inserted.
//  lineno     - The line number of the instruction for which the check was
//               inserted.
//
void *
boundscheckui_debug (DebugPoolTy * Pool,
                     void * Source,
                     void * Dest,
                     const char * SourceFile,
                     unsigned int lineno) {
  // This code is inlined at all boundscheckui calls

  // Search the splay for Source and return the bounds of the object
  void * ObjStart = Source, * ObjEnd = 0;
  bool ret = boundscheck_lookup (Pool, ObjStart, ObjEnd); 

  if (logregs) {
    fprintf (stderr, "boundscheckui_debug: %p - %p\n", ObjStart, ObjEnd);
    fflush (stderr);
  }

  // Check if destination lies in the same object
  if (__builtin_expect ((ret && (ObjStart <= Dest) &&
                        ((Dest <= ObjEnd))), 1)) {
    return Dest;
  } else {
    //
    // Either:
    //  1) A valid object was not found in splay tree, or
    //  2) Dest is not within the valid object in which Source was found
    //
    return boundscheck_check (ret,
                              ObjStart,
                              ObjEnd,
                              Pool,
                              Source,
                              Dest,
                              false,
                              SourceFile,
                              lineno);
  }
}

void
poolcheck (DebugPoolTy *Pool, void *Node) {
  poolcheck_debug(Pool, Node, "<unknown>", 0);
}

//
// Function: boundscheck()
//
// Description:
//  Perform a precise bounds check.  Ensure that Source is within a valid
//  object within the pool and that Dest is within the bounds of the same
//  object.
//
void *
boundscheck (DebugPoolTy * Pool, void * Source, void * Dest) {
  return boundscheck_debug(Pool, Source, Dest, "<unknown>", 0);
}

//
// Function: boundscheckui()
//
// Description:
//  Perform a bounds check (with lookup) on the given pointers.
//
// Inputs:
//  Pool - The pool to which the pointers (Source and Dest) should belong.
//  Source - The Source pointer of the indexing operation (the GEP).
//  Dest   - The result of the indexing operation (the GEP).
//
void *
boundscheckui (DebugPoolTy * Pool, void * Source, void * Dest) {
  return boundscheckui_debug (Pool, Source, Dest, "<unknown>", 0);
}
