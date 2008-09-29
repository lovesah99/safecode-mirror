//===-- sc - SAFECode Compiler Tool -------------------------------------===//
//
//                     The SAFECode Project
//
// This file was developed by the LLVM research group and is distributed
// under the University of Illinois Open Source License. See LICENSE.TXT for
// details.
//
//===--------------------------------------------------------------------===//
//
// This program is a tool to run the SAFECode passes on a bytecode input file.
//
//===--------------------------------------------------------------------===//

#include "llvm/Module.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/System/Signals.h"

#include "ABCPreProcess.h"
#include "InsertPoolChecks.h"
#include "IndirectCallChecks.h"
#include "SpeculativeChecking.h"

#include <fstream>
#include <iostream>
#include <memory>

using namespace llvm;

// General options for sc.
static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bytecode>"), cl::init("-"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));

static cl::opt<bool>
Force("f", cl::desc("Overwrite output files"));

static cl::opt<bool>
FullPA("pa", cl::init(false), cl::desc("Use pool allocation"));

static cl::opt<bool>
DanglingPointerChecks("dpchecks", cl::init(false), cl::desc("Perform Dangling Pointer Checks"));

static cl::opt<bool>
EnableFastCallChecks("enable-fastcallchecks", cl::init(false),
                     cl::desc("Enable fast indirect call checks"));

static cl::opt<bool>
DisableMonotonicLoopOpt("disable-monotonic-loop-opt", cl::init(false), cl::desc("Disable optimization for checking monotonic loops"));

static cl::opt<bool>
EnableSpecChecking("spec-checking", cl::init(false), cl::desc("Use speculative checking"));


// GetFileNameRoot - Helper function to get the basename of a filename.
static inline std::string
GetFileNameRoot(const std::string &InputFilename) {
  std::string IFN = InputFilename;
  std::string outputFilename;
  int Len = IFN.length();
  if ((Len > 2) &&
      IFN[Len-3] == '.' && IFN[Len-2] == 'b' && IFN[Len-1] == 'c') {
    outputFilename = std::string(IFN.begin(), IFN.end()-3); // s/.bc/.s/
  } else {
    outputFilename = IFN;
  }
  return outputFilename;
}


// main - Entry point for the sc compiler.
//
int main(int argc, char **argv) {
  std::string mt;
  std::string & msg = mt;
  llvm_shutdown_obj ShutdownObject;

  try {
    cl::ParseCommandLineOptions(argc, argv, " llvm system compiler\n");
    sys::PrintStackTraceOnErrorSignal();

    // Load the module to be compiled...
    std::auto_ptr<Module> M;
    std::string ErrorMessage;
    if (MemoryBuffer *Buffer
          = MemoryBuffer::getFileOrSTDIN(InputFilename, &ErrorMessage)) {
      M.reset(ParseBitcodeFile(Buffer, &ErrorMessage));
      delete Buffer;
    }

    if (M.get() == 0) {
      std::cerr << argv[0] << ": bytecode didn't read correctly.\n";
      return 1;
    }

    // Build up all of the passes that we want to do to the module...
    PassManager Passes;

    Passes.add(new TargetData(M.get()));

    // Convert Unsafe alloc instructions first.  This does not rely upon
    // pool allocation and has problems dealing with cloned functions.
    Passes.add(new ConvertUnsafeAllocas());

    // Remove indirect calls to malloc and free functions
    Passes.add(createIndMemRemPass());

    // Ensure that all malloc/free calls are changed into LLVM instructions
    Passes.add(createRaiseAllocationsPass());

    // Schedule the Equivalence Class and Bottom-Up call graph analyses
    // before pool allocation.  They don't work after pool allocation has
    // been run, and PassManager schedules them after pool allocation for
    // some reason.
    Passes.add(new EquivClassGraphs());
    Passes.add(new BottomUpCallGraph());

    if (FullPA)
      Passes.add(new PoolAllocate(true, true));
    else
      Passes.add(new PoolAllocateSimple(true, true));

#if 0
    // Convert Unsafe alloc instructions first.  This version relies upon
    // pool allocation,
    Passes.add(new PAConvertUnsafeAllocas());
#endif
    Passes.add(new ABCPreProcess());
    Passes.add(new EmbeCFreeRemoval());
    Passes.add(new PreInsertPoolChecks());
    Passes.add(new InsertPoolChecks());
    Passes.add(new RegisterStackObjPass());
    Passes.add(new MallocPass());
    if (EnableFastCallChecks)
      Passes.add(createIndirectCallChecksPass());


    Passes.add(createLICMPass());
    if (!DisableMonotonicLoopOpt)
      Passes.add(new MonotonicLoopOpt());

    if(EnableSpecChecking) {
      Passes.add(new SpeculativeCheckingPass());
      Passes.add(new SpeculativeCheckingInsertSyncPoints());
    }

    // Verify the final result
    Passes.add(createVerifierPass());

    // Figure out where we are going to send the output...
    std::ostream *Out = 0;
    if (OutputFilename != "") {
      if (OutputFilename != "-") {
        // Specified an output filename?
        if (!Force && std::ifstream(OutputFilename.c_str())) {
          // If force is not specified, make sure not to overwrite a file!
          std::cerr << argv[0] << ": error opening '" << OutputFilename
                    << "': file exists!\n"
                    << "Use -f command line argument to force output\n";
          return 1;
        }
        Out = new std::ofstream(OutputFilename.c_str());

        // Make sure that the Out file gets unlinked from the disk if we get a
        // SIGINT
        sys::RemoveFileOnSignal(sys::Path(OutputFilename));
      } else {
        Out = &std::cout;
      }
    } else {
      if (InputFilename == "-") {
        OutputFilename = "-";
        Out = &std::cout;
      } else {
        OutputFilename = GetFileNameRoot(InputFilename);

        OutputFilename += ".sc.bc";
      }

      if (!Force && std::ifstream(OutputFilename.c_str())) {
        // If force is not specified, make sure not to overwrite a file!
          std::cerr << argv[0] << ": error opening '" << OutputFilename
                    << "': file exists!\n"
                    << "Use -f command line argument to force output\n";
          return 1;
      }
      
      Out = new std::ofstream(OutputFilename.c_str());
      if (!Out->good()) {
        std::cerr << argv[0] << ": error opening " << OutputFilename << "!\n";
        delete Out;
        return 1;
      }

      // Make sure that the Out file gets unlinked from the disk if we get a
      // SIGINT
      sys::RemoveFileOnSignal(sys::Path(OutputFilename));
    }
    
    // Add the writing of the output file to the list of passes
    Passes.add (CreateBitcodeWriterPass(*Out));

    // Run our queue of passes all at once now, efficiently.
    Passes.run(*M.get());

    

    // Delete the ostream if it's not a stdout stream
    if (Out != &std::cout) delete Out;
  
    return 0;
  } catch (msg) {
    std::cerr << argv[0] << ": " << msg << "\n";
  } catch (...) {
    std::cerr << argv[0] << ": Unexpected unknown exception occurred.\n";
  }
  llvm_shutdown();
  return 1;
}

