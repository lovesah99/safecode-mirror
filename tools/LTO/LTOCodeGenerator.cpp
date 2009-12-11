//===-LTOCodeGenerator.cpp - LLVM Link Time Optimizer ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements the Link Time Optimization library. This library is 
// intended to be used by linker to optimize code at link time.
//
//===----------------------------------------------------------------------===//

#include "LTOModule.h"
#include "LTOCodeGenerator.h"

#include "safecode/SAFECode.h"
#include "safecode/SAFECodeConfig.h"
#include "safecode/InsertChecks/RegisterBounds.h"
#include "safecode/InsertChecks/RegisterRuntimeInitializer.h"
#include "safecode/Support/AllocatorInfo.h"

#include "poolalloc/PoolAllocate.h"

#include "ABCPreProcess.h"
#include "InsertPoolChecks.h"
#include "ArrayBoundsCheck.h"
#include "safecode/BreakConstantGEPs.h"
#include "safecode/BreakConstantStrings.h"
#include "safecode/CStdLib.h"
#include "safecode/DebugInstrumentation.h"
#include "safecode/DetectDanglingPointers.h"
#include "safecode/DummyUse.h"
#include "safecode/OptimizeChecks.h"
#include "safecode/RewriteOOB.h"
#include "safecode/SpeculativeChecking.h"
#include "safecode/LowerSafecodeIntrinsic.h"
#include "safecode/FaultInjector.h"
#include "safecode/CodeDuplication.h"

#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Linker.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/ModuleProvider.h"
#include "llvm/PassManager.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/CodeGen/FileWriters.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Mangler.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/StandardPasses.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/System/Host.h"
#include "llvm/System/Signals.h"
#include "llvm/Target/SubtargetFeature.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetAsmInfo.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegistry.h"
#include "llvm/Target/TargetSelect.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"


#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <fcntl.h>


using namespace llvm;
using namespace NAMESPACE_SC;

static cl::opt<bool> DisableInline("disable-inlining",
  cl::desc("Do not run the inliner pass"));

enum CheckingRuntimeType {
  RUNTIME_PA, RUNTIME_DEBUG, RUNTIME_SINGLETHREAD, RUNTIME_PARALLEL, RUNTIME_QUEUE_OP, RUNTIME_SVA 
};

const char* LTOCodeGenerator::getVersionString()
{
#ifdef LLVM_VERSION_INFO
    return PACKAGE_NAME " version " PACKAGE_VERSION ", " LLVM_VERSION_INFO;
#else
    return PACKAGE_NAME " version " PACKAGE_VERSION;
#endif
}


LTOCodeGenerator::LTOCodeGenerator() 
    : _context(getGlobalContext()),
      _linker("LinkTimeOptimizer", "ld-temp.o", _context), _target(NULL),
      _emitDwarfDebugInfo(false), _scopeRestrictionsDone(false),
      _codeModel(LTO_CODEGEN_PIC_MODEL_DYNAMIC),
      _nativeObjectFile(NULL), _assemblerPath(NULL)
{
    std::cerr << "JTC" << std::endl;
    InitializeAllTargets();
    InitializeAllAsmPrinters();
}

LTOCodeGenerator::~LTOCodeGenerator()
{
    delete _target;
    delete _nativeObjectFile;
}



bool LTOCodeGenerator::addModule(LTOModule* mod, std::string& errMsg)
{
    return _linker.LinkInModule(mod->getLLVVMModule(), &errMsg);
}
    

bool LTOCodeGenerator::setDebugInfo(lto_debug_model debug, std::string& errMsg)
{
    switch (debug) {
        case LTO_DEBUG_MODEL_NONE:
            _emitDwarfDebugInfo = false;
            return false;
            
        case LTO_DEBUG_MODEL_DWARF:
            _emitDwarfDebugInfo = true;
            return false;
    }
    errMsg = "unknown debug format";
    return true;
}


bool LTOCodeGenerator::setCodePICModel(lto_codegen_model model, 
                                       std::string& errMsg)
{
    switch (model) {
        case LTO_CODEGEN_PIC_MODEL_STATIC:
        case LTO_CODEGEN_PIC_MODEL_DYNAMIC:
        case LTO_CODEGEN_PIC_MODEL_DYNAMIC_NO_PIC:
            _codeModel = model;
            return false;
    }
    errMsg = "unknown pic model";
    return true;
}

void LTOCodeGenerator::setAssemblerPath(const char* path)
{
    if ( _assemblerPath )
        delete _assemblerPath;
    _assemblerPath = new sys::Path(path);
}

void LTOCodeGenerator::addMustPreserveSymbol(const char* sym)
{
    _mustPreserveSymbols[sym] = 1;
}


bool LTOCodeGenerator::writeMergedModules(const char* path, std::string& errMsg)
{
    if ( this->determineTarget(errMsg) ) 
        return true;

    // mark which symbols can not be internalized 
    this->applyScopeRestrictions();

    // create output file
    std::ofstream out(path, std::ios_base::out|std::ios::trunc|std::ios::binary);
    if ( out.fail() ) {
        errMsg = "could not open bitcode file for writing: ";
        errMsg += path;
        return true;
    }
    
    // write bitcode to it
    WriteBitcodeToFile(_linker.getModule(), out);
    if ( out.fail() ) {
        errMsg = "could not write bitcode file: ";
        errMsg += path;
        return true;
    }
    
    return false;
}


const void* LTOCodeGenerator::compile(size_t* length, std::string& errMsg)
{
    // make unique temp .s file to put generated assembly code
    sys::Path uniqueAsmPath("lto-llvm.s");
    if ( uniqueAsmPath.createTemporaryFileOnDisk(true, &errMsg) )
        return NULL;
    sys::RemoveFileOnSignal(uniqueAsmPath);
       
    // generate assembly code
    bool genResult = false;
    {
      raw_fd_ostream asmFD(uniqueAsmPath.c_str(),
                           /*Binary=*/false, /*Force=*/true,
                           errMsg);
      formatted_raw_ostream asmFile(asmFD);
      if (!errMsg.empty())
        return NULL;
      genResult = this->generateAssemblyCode(asmFile, errMsg);
    }
    if ( genResult ) {
        if ( uniqueAsmPath.exists() )
            uniqueAsmPath.eraseFromDisk();
        return NULL;
    }
    
    // make unique temp .o file to put generated object file
    sys::PathWithStatus uniqueObjPath("lto-llvm.o");
    if ( uniqueObjPath.createTemporaryFileOnDisk(true, &errMsg) ) {
        if ( uniqueAsmPath.exists() )
            uniqueAsmPath.eraseFromDisk();
        return NULL;
    }
    sys::RemoveFileOnSignal(uniqueObjPath);

    // assemble the assembly code
    const std::string& uniqueObjStr = uniqueObjPath.toString();
    bool asmResult = this->assemble(uniqueAsmPath.toString(), 
                                                        uniqueObjStr, errMsg);
    if ( !asmResult ) {
        // remove old buffer if compile() called twice
        delete _nativeObjectFile;
        
        // read .o file into memory buffer
        _nativeObjectFile = MemoryBuffer::getFile(uniqueObjStr.c_str(),&errMsg);
    }

    // remove temp files
    uniqueAsmPath.eraseFromDisk();
    uniqueObjPath.eraseFromDisk();

    // return buffer, unless error
    if ( _nativeObjectFile == NULL )
        return NULL;
    *length = _nativeObjectFile->getBufferSize();
    return _nativeObjectFile->getBufferStart();
}


bool LTOCodeGenerator::assemble(const std::string& asmPath, 
                                const std::string& objPath, std::string& errMsg)
{
    sys::Path tool;
    bool needsCompilerOptions = true;
    if ( _assemblerPath ) {
        tool = *_assemblerPath;
        needsCompilerOptions = false;
    } else {
        // find compiler driver
        tool = sys::Program::FindProgramByName("gcc");
        if ( tool.isEmpty() ) {
            errMsg = "can't locate gcc";
            return true;
        }
    }

    // build argument list
    std::vector<const char*> args;
    std::string targetTriple = _linker.getModule()->getTargetTriple();
    args.push_back(tool.c_str());
    if ( targetTriple.find("darwin") != std::string::npos ) {
        // darwin specific command line options
        if (strncmp(targetTriple.c_str(), "i386-apple-", 11) == 0) {
            args.push_back("-arch");
            args.push_back("i386");
        }
        else if (strncmp(targetTriple.c_str(), "x86_64-apple-", 13) == 0) {
            args.push_back("-arch");
            args.push_back("x86_64");
        }
        else if (strncmp(targetTriple.c_str(), "powerpc-apple-", 14) == 0) {
            args.push_back("-arch");
            args.push_back("ppc");
        }
        else if (strncmp(targetTriple.c_str(), "powerpc64-apple-", 16) == 0) {
            args.push_back("-arch");
            args.push_back("ppc64");
        }
        else if (strncmp(targetTriple.c_str(), "arm-apple-", 10) == 0) {
            args.push_back("-arch");
            args.push_back("arm");
        }
        else if ((strncmp(targetTriple.c_str(), "armv4t-apple-", 13) == 0) ||
                 (strncmp(targetTriple.c_str(), "thumbv4t-apple-", 15) == 0)) {
            args.push_back("-arch");
            args.push_back("armv4t");
        }
        else if ((strncmp(targetTriple.c_str(), "armv5-apple-", 12) == 0) ||
                 (strncmp(targetTriple.c_str(), "armv5e-apple-", 13) == 0) ||
                 (strncmp(targetTriple.c_str(), "thumbv5-apple-", 14) == 0) ||
                 (strncmp(targetTriple.c_str(), "thumbv5e-apple-", 15) == 0)) {
            args.push_back("-arch");
            args.push_back("armv5");
        }
        else if ((strncmp(targetTriple.c_str(), "armv6-apple-", 12) == 0) ||
                 (strncmp(targetTriple.c_str(), "thumbv6-apple-", 14) == 0)) {
            args.push_back("-arch");
            args.push_back("armv6");
        }
        else if ((strncmp(targetTriple.c_str(), "armv7-apple-", 12) == 0) ||
                 (strncmp(targetTriple.c_str(), "thumbv7-apple-", 14) == 0)) {
            args.push_back("-arch");
            args.push_back("armv7");
        }
        // add -static to assembler command line when code model requires
        if ( (_assemblerPath != NULL) && (_codeModel == LTO_CODEGEN_PIC_MODEL_STATIC) )
            args.push_back("-static");
    }
    if ( needsCompilerOptions ) {
        args.push_back("-c");
        args.push_back("-x");
        args.push_back("assembler");
    }
    args.push_back("-o");
    args.push_back(objPath.c_str());
    args.push_back(asmPath.c_str());
    args.push_back(0);

    // invoke assembler
    if ( sys::Program::ExecuteAndWait(tool, &args[0], 0, 0, 0, 0, &errMsg) ) {
        errMsg = "error in assembly";    
        return true;
    }
    return false; // success
}



bool LTOCodeGenerator::determineTarget(std::string& errMsg)
{
    if ( _target == NULL ) {
        std::string Triple = _linker.getModule()->getTargetTriple();
        if (Triple.empty())
          Triple = sys::getHostTriple();

        // create target machine from info for merged modules
        const Target *march = TargetRegistry::lookupTarget(Triple, errMsg);
        if ( march == NULL )
            return true;

        // The relocation model is actually a static member of TargetMachine
        // and needs to be set before the TargetMachine is instantiated.
        switch( _codeModel ) {
        case LTO_CODEGEN_PIC_MODEL_STATIC:
            TargetMachine::setRelocationModel(Reloc::Static);
            break;
        case LTO_CODEGEN_PIC_MODEL_DYNAMIC:
            TargetMachine::setRelocationModel(Reloc::PIC_);
            break;
        case LTO_CODEGEN_PIC_MODEL_DYNAMIC_NO_PIC:
            TargetMachine::setRelocationModel(Reloc::DynamicNoPIC);
            break;
        }

        // construct LTModule, hand over ownership of module and target
        std::string FeatureStr = getFeatureString(Triple.c_str());
        _target = march->createTargetMachine(Triple, FeatureStr);
    }
    return false;
}

void LTOCodeGenerator::applyScopeRestrictions()
{
    if ( !_scopeRestrictionsDone ) {
        Module* mergedModule = _linker.getModule();

        // Start off with a verification pass.
        PassManager passes;
        passes.add(createVerifierPass());

        // mark which symbols can not be internalized 
        if ( !_mustPreserveSymbols.empty() ) {
            Mangler mangler(*mergedModule, 
                                _target->getTargetAsmInfo()->getGlobalPrefix());
            std::vector<const char*> mustPreserveList;
            for (Module::iterator f = mergedModule->begin(), 
                                        e = mergedModule->end(); f != e; ++f) {
                if ( !f->isDeclaration() 
                  && _mustPreserveSymbols.count(mangler.getMangledName(f)) )
                  mustPreserveList.push_back(::strdup(f->getNameStr().c_str()));
            }
            for (Module::global_iterator v = mergedModule->global_begin(), 
                                 e = mergedModule->global_end(); v !=  e; ++v) {
                if ( !v->isDeclaration()
                  && _mustPreserveSymbols.count(mangler.getMangledName(v)) )
                  mustPreserveList.push_back(::strdup(v->getNameStr().c_str()));
            }
            passes.add(createInternalizePass(mustPreserveList));
        }
        // apply scope restrictions
        passes.run(*mergedModule);
        
        _scopeRestrictionsDone = true;
    }
}

static void
addPoolAllocationPass(PassManager & Passes) {
  Passes.add(new PoolAllocateSimple(true, true, true));
}

static void
addStaticGEPCheckingPass(PassManager & Passes) {
  Passes.add(new ArrayBoundsCheckStruct());
  Passes.add(new ArrayBoundsCheckLocal());
}

static void
addLowerIntrinsicPass(PassManager & Passes, CheckingRuntimeType type) {
  /// Mapping between check intrinsics and implementation

  typedef LowerSafecodeIntrinsic::IntrinsicMappingEntry IntrinsicMappingEntry;
  static IntrinsicMappingEntry RuntimePA[] = 
    { {"sc.lscheck",         "__sc_no_op_poolcheck" },
      {"sc.lscheckui",       "__sc_no_op_poolcheck" },
      {"sc.lscheckalign",    "__sc_no_op_poolcheckalign" },
      {"sc.lscheckalignui",    "__sc_no_op_poolcheckalign" },
      {"sc.boundscheck",       "__sc_no_op_boundscheck" },
      {"sc.boundscheckui",     "__sc_no_op_boundscheck" },
      {"sc.exactcheck",       "__sc_no_op_exactcheck" },
      {"sc.exactcheck2",     "__sc_no_op_exactcheck2" },
      {"poolregister",      "__sc_no_op_poolregister" },
      {"poolunregister",    "__sc_no_op_poolunregister" },
      {"poolalloc",         "__sc_barebone_poolalloc"},
      {"poolfree",          "__sc_barebone_poolfree"},
      {"pooldestroy",       "__sc_barebone_pooldestroy"},
      {"pool_init_runtime", "__sc_barebone_pool_init_runtime"},
      {"poolinit",          "__sc_barebone_poolinit"},
      {"poolrealloc",       "__sc_barebone_poolrealloc"},
      {"poolcalloc",        "__sc_barebone_poolcalloc"},
      {"poolstrdup",        "__sc_barebone_poolstrdup"},
      {"sc.get_actual_val",  "pchk_getActualValue" },
    };

  static IntrinsicMappingEntry RuntimeSingleThread[] = 
    { {"sc.lscheck",         "sc.lscheck" },
      {"sc.lscheckui",       "__sc_no_op_poolcheck" },
      {"sc.lscheckalign",    "poolcheckalign" },
      {"sc.lscheckalignui",    "poolcheckalignui" },
      {"sc.boundscheck",       "boundscheck" },
      {"sc.boundscheckui",     "boundscheckui" },
      {"sc.exactcheck",       "exactcheck" },
      {"sc.exactcheck2",     "exactcheck2" },
      {"sc.pool_register",      "poolregister" },
      {"sc.pool_unregister",    "poolunregister" },
      {"sc.init_pool_runtime", "__sc_bc_pool_init_runtime"},
      {"poolalloc",         "__sc_bc_poolalloc"},
      {"poolfree",          "__sc_bc_poolfree"},
      {"pooldestroy",       "__sc_bc_pooldestroy"},
      {"poolinit",          "__sc_bc_poolinit"},
      {"poolrealloc",       "__sc_bc_poolrealloc"},
      {"poolcalloc",        "__sc_bc_poolcalloc"},
      {"poolstrdup",        "__sc_bc_poolstrdup"},
      {"sc.get_actual_val",  "pchk_getActualValue" },
    };

  static IntrinsicMappingEntry RuntimeDebug[] = 
    { {"sc.lscheck",         "poolcheck" },
      {"sc.lscheckui",       "poolcheckui" },
      {"sc.lscheckalign",    "poolcheckalign" },
      {"sc.lscheckalignui",  "poolcheckalignui" },
      {"sc.boundscheck",     "boundscheck" },
      {"sc.boundscheckui",   "boundscheckui" },
      {"sc.exactcheck",      "exactcheck" },
      {"sc.exactcheck2",     "exactcheck2" },
      {"sc.funccheck",       "__sc_dbg_funccheck" },
      {"sc.get_actual_val",  "pchk_getActualValue" },
      {"sc.pool_register",   "__sc_dbg_poolregister" },
      {"sc.pool_unregister", "__sc_dbg_poolunregister" },
      {"sc.pool_unregister_stack", "__sc_dbg_poolunregister_stack" },
      {"sc.pool_unregister_debug", "__sc_dbg_poolunregister_debug" },
      {"sc.pool_unregister_stack_debug", "__sc_dbg_poolunregister_stack_debug" },
      {"poolalloc",         "__pa_bitmap_poolalloc"},

      {"sc.init_pool_runtime", "pool_init_runtime"},
      {"sc.pool_register_debug", "__sc_dbg_src_poolregister"},
      {"sc.pool_register_stack_debug", "__sc_dbg_src_poolregister_stack"},
      {"sc.pool_register_stack", "__sc_dbg_poolregister_stack"},
      {"sc.pool_register_global", "__sc_dbg_poolregister_global"},
      {"sc.pool_register_global_debug", "__sc_dbg_poolregister_global_debug"},
      {"sc.lscheck_debug",      "poolcheck_debug"},
      {"sc.lscheckalign_debug", "poolcheckalign_debug"},
      {"sc.boundscheck_debug",  "boundscheck_debug"},
      {"sc.boundscheckui_debug","boundscheckui_debug"},
      {"sc.exactcheck2_debug",  "exactcheck2_debug"},
      {"sc.pool_argvregister",  "__sc_dbg_poolargvregister"},

      {"poolinit",              "__sc_dbg_poolinit"},
      {"poolalloc_debug",       "__sc_dbg_src_poolalloc"},
      {"poolfree_debug",        "__sc_dbg_src_poolfree"},

      // These functions register objects in the splay trees
      {"poolcalloc_debug",      "__sc_dbg_src_poolcalloc"},
      {"poolcalloc",            "__sc_dbg_poolcalloc"},
      {"poolstrdup",            "__sc_dbg_poolstrdup"},
      {"poolstrdup_debug",      "__sc_dbg_poolstrdup_debug"},
      {"poolrealloc",           "__sc_dbg_poolrealloc"},
    };

  static IntrinsicMappingEntry RuntimeParallel[] = 
    { {"sc.lscheck",         "__sc_par_poolcheck" },
      {"sc.lscheckui",       "__sc_no_op_poolcheck" },
      {"sc.lscheckalign",    "__sc_par_poolcheckalign" },
      {"sc.lscheckalignui",    "__sc_par_poolcheckalignui" },
      {"sc.boundscheck",       "__sc_par_boundscheck" },
      {"sc.boundscheckui",     "__sc_par_boundscheckui" },
      {"sc.exactcheck",       "exactcheck" },
      {"sc.exactcheck2",     "exactcheck2" },
      {"sc.lscheck.serial",     "__sc_bc_poolcheck" },
      {"sc.lscheckui.serial",   "__sc_no_op_poolcheck" },
      {"sc.lscheckalign.serial","poolcheckalign" },
      {"sc.lscheckalignui.serial","poolcheckalignui" },
      {"sc.boundscheck.serial",   "__sc_bc_boundscheck" },
      {"sc.boundscheckui.serial", "__sc_bc_boundscheckui" },
      {"sc.exactcheck.serial",       "exactcheck" },
      {"sc.exactcheck2.serial",     "exactcheck2" },
      {"poolargvregister",      "__sc_par_poolargvregister" },
      {"poolregister",      "__sc_par_poolregister" },
      {"poolunregister",    "__sc_par_poolunregister" },
      {"poolalloc",         "__sc_par_poolalloc"},
      {"poolfree",          "__sc_par_poolfree"},
      {"pooldestroy",       "__sc_par_pooldestroy"},
      {"pool_init_runtime", "__sc_par_pool_init_runtime"},
      {"poolinit",          "__sc_par_poolinit"},
      {"poolrealloc",       "__sc_par_poolrealloc"},
      {"poolcalloc",        "__sc_par_poolcalloc"},
      {"poolstrdup",        "__sc_par_poolstrdup"},
    };

  const char * queueOpFunction = "__sc_par_enqueue_1";

  static IntrinsicMappingEntry RuntimeQueuePerformance[] = 
    { {"sc.lscheck",        queueOpFunction}, 
      {"sc.lscheckui",      queueOpFunction},
      {"sc.lscheckalign",   queueOpFunction}, 
      {"sc.lscheckalignui", queueOpFunction},
      {"sc.boundscheck",    queueOpFunction}, 
      {"sc.boundscheckui",  queueOpFunction},
      {"sc.exactcheck",     "exactcheck" },
      {"sc.exactcheck2",    "exactcheck2" },
      {"poolregister",      queueOpFunction}, 
      {"poolunregister",    queueOpFunction},
      {"poolalloc",         "__sc_barebone_poolalloc"},
      {"poolfree",          "__sc_barebone_poolfree"},
      {"pooldestroy",       "__sc_barebone_pooldestroy"},
      {"pool_init_runtime", "__sc_par_pool_init_runtime"},
      {"poolinit",          "__sc_barebone_poolinit"},
      {"poolrealloc",       "__sc_barebone_poolrealloc"},
      {"poolcalloc",        "__sc_barebone_poolcalloc"},
      {"poolstrdup",        "__sc_barebone_poolstrdup"},
    };

  static IntrinsicMappingEntry RuntimeSVA[] = 
    { {"sc.lscheck",         "poolcheck" },
      {"sc.lscheckui",       "poolcheck_i" },
      {"sc.lscheckalign",    "poolcheckalign" },
      {"sc.lscheckalignui",  "poolcheckalign_i" },
      {"sc.boundscheck",     "pchk_bounds" },
      {"sc.boundscheckui",   "pchk_bounds_i" },
      {"sc.exactcheck",      "exactcheck" },
      {"sc.exactcheck2",     "exactcheck2" },
      {"sc.pool_register",   "pchk_reg_obj" },
      {"sc.pool_unregister", "pchk_drop_obj" },
      {"poolinit",           "__sva_pool_init" },
    };

  switch (type) {
  case RUNTIME_PA:
    Passes.add(new LowerSafecodeIntrinsic(RuntimePA, RuntimePA + sizeof(RuntimePA) / sizeof(IntrinsicMappingEntry)));
    break;
    
  case RUNTIME_DEBUG:
    Passes.add(new LowerSafecodeIntrinsic(RuntimeDebug, RuntimeDebug + sizeof(RuntimeDebug) / sizeof(IntrinsicMappingEntry)));
    break;

  case RUNTIME_SINGLETHREAD:
    Passes.add(new LowerSafecodeIntrinsic(RuntimeSingleThread, RuntimeSingleThread + sizeof(RuntimeSingleThread) / sizeof(IntrinsicMappingEntry)));
    break;

  case RUNTIME_PARALLEL:
    Passes.add(new LowerSafecodeIntrinsic(RuntimeParallel, RuntimeParallel + sizeof(RuntimeParallel) / sizeof(IntrinsicMappingEntry)));
    break;

  case RUNTIME_QUEUE_OP:
    Passes.add(new LowerSafecodeIntrinsic(RuntimeQueuePerformance, RuntimeQueuePerformance+ sizeof(RuntimeQueuePerformance) / sizeof(IntrinsicMappingEntry)));
    break;

  case RUNTIME_SVA:
    Passes.add(new LowerSafecodeIntrinsic(RuntimeSVA, RuntimeSVA + sizeof(RuntimeSVA) / sizeof(IntrinsicMappingEntry)));
    break;

  default:
    assert (0 && "Invalid Runtime!");
  }
}

/// Optimize merged modules using various IPO passes
bool LTOCodeGenerator::generateAssemblyCode(formatted_raw_ostream& out,
                                            std::string& errMsg)
{
    if ( this->determineTarget(errMsg) ) 
        return true;

    // mark which symbols can not be internalized 
    this->applyScopeRestrictions();

    Module* mergedModule = _linker.getModule();

    // If target supports exception handling then enable it now.
    switch (_target->getTargetAsmInfo()->getExceptionHandlingType()) {
    case ExceptionHandling::Dwarf:
      llvm::DwarfExceptionHandling = true;
      break;
    case ExceptionHandling::SjLj:
      llvm::SjLjExceptionHandling = true;
      break;
    case ExceptionHandling::None:
      break;
    default:
      assert (0 && "Unknown exception handling model!");
    }

    // if options were requested, set them
    if ( !_codegenOptions.empty() )
        cl::ParseCommandLineOptions(_codegenOptions.size(), 
                                                (char**)&_codegenOptions[0]);

    // Instantiate the pass manager to organize the passes.
    PassManager passes;

    // Start off with a verification pass.
    passes.add(createVerifierPass());

    // Add an appropriate TargetData instance for this module...
    passes.add(new TargetData(*_target->getTargetData()));
    
    createStandardLTOPasses(&passes, /*Internalize=*/ false, !DisableInline,
                            /*VerifyEach=*/ false);

    ///////////////////////////////////////////////////////////////////////////
    // Add the SAFECode transform passes
    ///////////////////////////////////////////////////////////////////////////

    //
    // Merge constants.  We do this here because merging constants *after*
    // running SAFECode may cause incorrect registration of global objects
    // (e.g., two global object registrations may register the same object
    // because the globals are identical constant strings).
    //
    passes.add (createConstantMergePass());

    // Remove all constant GEP expressions
    (passes.add(new BreakConstantGEPs()));

    // Ensure that all malloc/free calls are changed into LLVM instructions
    (passes.add(createRaiseAllocationsPass()));

    //
    // Remove indirect calls to malloc and free functions.  This can be done
    // here because none of the SAFECode transforms will add indirect calls to
    // malloc() and free().
    //
    (passes.add(createIndMemRemPass()));

    // Ensure that all malloc/free calls are changed into LLVM instructions
    (passes.add(createRaiseAllocationsPass()));

    //
    // Ensure that all functions have only a single return instruction.  We do
    // this to make stack-to-heap promotion easier (with a single return
    // instruction, we know where to free all of the promoted alloca's).
    //
    (passes.add(createUnifyFunctionExitNodesPass()));

    //
    // Convert Unsafe alloc instructions first.  This does not rely upon
    // pool allocation and has problems dealing with cloned functions.
    //
    passes.add(new ArrayBoundsCheckLocal());
    (passes.add(new ConvertUnsafeAllocas()));

    //
    // Run pool allocation.
    //
	 	addPoolAllocationPass(passes);

    //
    // Instrument the code so that memory objects are registered into the
    // correct pools.  Note that user-space SAFECode requires a few additional
    // transforms to do this.
    //
    passes.add(new RegisterGlobalVariables());

    passes.add(new RegisterMainArgs());
    passes.add(new RegisterRuntimeInitializer());
    passes.add(new RegisterFunctionByvalArguments());

    // Register all customized allocators, such as vmalloc() / kmalloc() in
    // kernel, or poolalloc() in pool allocation
    passes.add(new RegisterCustomizedAllocation());      

    //
    // Use static analysis to determine which indexing operations (GEPs) do not
    // require run-time checks.  This is scheduled right before the check
    // insertion pass because it seems that the PassManager will invalidate the
    // results if they are not consumed immediently.
    //
    std::cerr << "JTC" << std::endl;
    addStaticGEPCheckingPass(passes);

    passes.add(new InsertPoolChecks());

    passes.add(new ExactCheckOpt());

    (passes.add(new RegisterStackObjPass()));
    (passes.add(new InitAllocas()));
    
    (passes.add(new StringTransform()));

    passes.add(new MonotonicLoopOpt());

    //
    // Do post processing required for Out of Bounds pointer rewriting.
    // Note that the RewriteOOB pass is always required for user-space
    // SAFECode because it is how we handle the C standard allowing pointers to
    // move one beyond the end of an object as long as the pointer is not
    // dereferenced.
    //
    // Try to optimize the checks first as the RewriteOOB pass may make
    // optimization impossible.
    //
    passes.add (new OptimizeChecks());
    passes.add(new RewriteOOB());

    passes.add (new DummyUse());

    //
    // Remove special attributes for loop hoisting that were added by previous
    // SAFECode passes.
    //
    passes.add (createClearCheckAttributesPass());

    //
    // Attempt to optimize the checks.
    //
    passes.add (new OptimizeChecks());
    passes.add (new PoolRegisterElimination());

    passes.add(new UnusedCheckElimination());

    //
    // Instrument the code so that dangling pointers are detected.
    //
    passes.add(new DetectDanglingPointers());

    passes.add (new DebugInstrument());

    // Lower the checking intrinsics into appropriate runtime function calls.
    // It should be the last pass
    addLowerIntrinsicPass(passes, RUNTIME_DEBUG);

    // Make all strings non-constant so that the linker doesn't try to merge
    // them together.
    passes.add(new BreakConstantStrings());


    // Make sure everything is still good.
    passes.add(createVerifierPass());

    FunctionPassManager* codeGenPasses =
            new FunctionPassManager(new ExistingModuleProvider(mergedModule));

    codeGenPasses->add(new TargetData(*_target->getTargetData()));

    ObjectCodeEmitter* oce = NULL;

    switch (_target->addPassesToEmitFile(*codeGenPasses, out,
                                         TargetMachine::AssemblyFile,
                                         CodeGenOpt::Aggressive)) {
        case FileModel::MachOFile:
            oce = AddMachOWriter(*codeGenPasses, out, *_target);
            break;
        case FileModel::ElfFile:
            oce = AddELFWriter(*codeGenPasses, out, *_target);
            break;
        case FileModel::AsmFile:
            break;
        case FileModel::Error:
        case FileModel::None:
            errMsg = "target file type not supported";
            return true;
    }

    if (_target->addPassesToEmitFileFinish(*codeGenPasses, oce,
                                           CodeGenOpt::Aggressive)) {
        errMsg = "target does not support generation of this file type";
        return true;
    }

    // Run our queue of passes all at once now, efficiently.
    passes.run(*mergedModule);

    // Run the code generator, and write assembly file
    codeGenPasses->doInitialization();

    for (Module::iterator
           it = mergedModule->begin(), e = mergedModule->end(); it != e; ++it)
      if (!it->isDeclaration())
        codeGenPasses->run(*it);

    codeGenPasses->doFinalization();

    out.flush();

    return false; // success
}


/// Optimize merged modules using various IPO passes
void LTOCodeGenerator::setCodeGenDebugOptions(const char* options)
{
    std::string ops(options);
    for (std::string o = getToken(ops); !o.empty(); o = getToken(ops)) {
        // ParseCommandLineOptions() expects argv[0] to be program name.
        // Lazily add that.
        if ( _codegenOptions.empty() ) 
            _codegenOptions.push_back("libLTO");
        _codegenOptions.push_back(strdup(o.c_str()));
    }
}
