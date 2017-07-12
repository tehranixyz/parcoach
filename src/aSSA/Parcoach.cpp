#include "andersen/Andersen.h"
#include "DepGraph.h"
#include "ExtInfo.h"
#include "MemoryRegion.h"
#include "MemorySSA.h"
#include "ModRefAnalysis.h"
#include "Options.h"
#include "Parcoach.h"
#include "ParcoachAnalysisInter.h"
#include "PTACallGraph.h"
#include "Collectives.h"
#include "Utils.h"

#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/Analysis/LazyCallGraph.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"
#include "llvm/Transforms/Scalar/LowerAtomic.h"
#include <llvm/Analysis/LoopInfo.h>
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;
using namespace std;

ParcoachInstr::ParcoachInstr() : ModulePass(ID) {}

void
ParcoachInstr::getAnalysisUsage(AnalysisUsage &au) const {
  au.setPreservesAll();
  au.addRequiredID(UnifyFunctionExitNodes::ID);
  au.addRequired<DominanceFrontierWrapperPass>();
  au.addRequired<DominatorTreeWrapperPass>();
  au.addRequired<PostDominatorTreeWrapperPass>();
  au.addRequired<CallGraphWrapperPass>();
}

bool
ParcoachInstr::doInitialization(Module &M) {
  PAInter = NULL;
  PAIntra = NULL;

  getOptions();
  initCollectives();

  tstart = gettime();

  return true;
}

bool
ParcoachInstr::doFinalization(Module &M) {
  tend = gettime();

  if (PAInter) {
    if (!optNoDataFlow) {
      errs() << "\n\033[0;36m==========================================\033[0;0m\n";
      errs() << "\033[0;36m===  PARCOACH INTER WITH DEP ANALYSIS  ===\033[0;0m\n";
      errs() << "\033[0;36m==========================================\033[0;0m\n";
      errs() << "Module name: " << M.getModuleIdentifier() << "\n";
      errs() << PAInter->getNbCollectivesFound() << " collective(s) found\n";
      errs() << PAInter->getNbWarnings() << " warning(s) issued\n";
      errs() << PAInter->getNbConds() << " cond(s) \n";
      errs() << PAInter->getConditionSet().size() << " different cond(s)\n";

      unsigned intersectionSize;
      int nbAdded;
      int nbRemoved;
      intersectionSize
	= getBBSetIntersectionSize(PAInter->getConditionSet(),
				   PAInter->getConditionSetParcoachOnly());

      nbAdded = PAInter->getConditionSet().size() - intersectionSize;
      nbRemoved = PAInter->getConditionSetParcoachOnly().size()
	- intersectionSize;
      errs() << nbAdded << " condition(s) added and " << nbRemoved
	     << " condition(s) removed with dep analysis.\n";

      intersectionSize
	= getInstSetIntersectionSize(PAInter->getWarningSet(),
				     PAInter->getWarningSetParcoachOnly());

      nbAdded = PAInter->getWarningSet().size() - intersectionSize;
      nbRemoved = PAInter->getWarningSetParcoachOnly().size()
	- intersectionSize;
      errs() << nbAdded << " warning(s) added and " << nbRemoved
	     << " warning(s) removed with dep analysis.\n";
    }

    errs() << "\n\033[0;36m==========================================\033[0;0m\n";
    errs() << "\033[0;36m============== PARCOACH INTER ONLY =============\033[0;0m\n";
    errs() << "\033[0;36m==========================================\033[0;0m\n";
    errs() << PAInter->getNbWarningsParcoachOnly() << " warning(s) issued\n";
    errs() << PAInter->getNbCondsParcoachOnly() << " cond(s) \n";
    errs() << PAInter->getConditionSetParcoachOnly().size() << " different cond(s)\n";
    errs() << PAInter->getNbCC() << " CC functions inserted \n";
    errs() << PAInter->getConditionSetParcoachOnly().size() << " different cond(s)\n";

    if (PAIntra) {
      unsigned intersectionSize;
      int nbAdded;
      int nbRemoved;
      intersectionSize
	= getBBSetIntersectionSize(PAInter->getConditionSetParcoachOnly(),
				   PAIntra->getConditionSetParcoachOnly());

      nbAdded = PAInter->getConditionSetParcoachOnly().size() -
	intersectionSize;
      nbRemoved = PAIntra->getConditionSetParcoachOnly().size()
	- intersectionSize;
      errs() << nbAdded << " condition(s) added and " << nbRemoved
	     << " condition(s) removed compared to intra analysis.\n";

      intersectionSize
	= getInstSetIntersectionSize(PAInter->getWarningSetParcoachOnly(),
				     PAIntra->getWarningSetParcoachOnly());

      nbAdded = PAInter->getWarningSetParcoachOnly().size() - intersectionSize;
      nbRemoved = PAIntra->getWarningSetParcoachOnly().size()
	- intersectionSize;
      errs() << nbAdded << " warning(s) added and " << nbRemoved
	     << " warning(s) removed compared to intra analysis.\n";
    }

    errs() << "\033[0;36m==========================================\033[0;0m\n";
  }

  if (PAIntra) {
    errs() << "\n\033[0;36m==========================================\033[0;0m\n";
    errs() << "\033[0;36m============== PARCOACH INTRA ONLY =============\033[0;0m\n";
    errs() << "\033[0;36m==========================================\033[0;0m\n";
    errs() << PAIntra->getNbWarningsParcoachOnly() << " warning(s) issued\n";
    errs() << PAIntra->getNbCondsParcoachOnly() << " cond(s) \n";
    errs() << PAIntra->getNbCC() << " CC functions inserted \n";
    errs() << PAIntra->getConditionSetParcoachOnly().size() << " different cond(s)\n";
    errs() << "\033[0;36m==========================================\033[0;0m\n";
  }

  if (optTimeStats) {
    errs()  << "AA time : "
    	    << format("%.3f", (tend_aa - tstart_aa)*1.0e3) << " ms\n";
    errs() << "Dep Analysis time : "
    	   << format("%.3f", (tend_flooding - tstart_pta)*1.0e3)
    	   << " ms\n";
    errs() << "Parcoach time : "
    	   << format("%.3f", (tend_parcoach - tstart_parcoach)*1.0e3)
    	   << " ms\n";
    errs() << "Total time : "
    	   << format("%.3f", (tend - tstart)*1.0e3) << " ms\n\n";

    errs() << "detailed timers:\n";
    errs() << "PTA time : "
    	   << format("%.3f", (tend_pta - tstart_pta)*1.0e3) << " ms\n";
    errs() << "Region creation time : "
    	   << format("%.3f", (tend_regcreation - tstart_regcreation)*1.0e3)
    	   << " ms\n";
    errs() << "Modref time : "
    	   << format("%.3f", (tend_modref - tstart_modref)*1.0e3)
    	   << " ms\n";
    errs() << "ASSA generation time : "
    	   << format("%.3f", (tend_assa- tstart_assa)*1.0e3)
    	   << " ms\n";
    errs() << "Dep graph generation time : "
    	   << format("%.3f", (tend_depgraph - tstart_depgraph)*1.0e3)
    	   << " ms\n";
    errs() << "Flooding time : "
    	   << format("%.3f", (tend_flooding - tstart_flooding)*1.0e3)
    	   << " ms\n";
  }

  return true;
}

void
ParcoachInstr::replaceOMPMicroFunctionCalls(Module &M) {
  // Get all calls to be replaced (only __kmpc_fork_call is handled for now).
  std::vector<CallInst *> callToReplace;

  for (const Function &F : M) {
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
  	const CallInst *ci = dyn_cast<CallInst>(&I);
  	if (!ci)
  	  continue;

  	const Function *called = ci->getCalledFunction();
  	if (!called)
  	  continue;

  	if (called->getName().equals("__kmpc_fork_call")) {
  	  callToReplace.push_back(const_cast<CallInst *>(ci));
  	}
      }
    }
  }

  // Replace each call to __kmpc_fork_call with a call to the outlined function.
  for (unsigned n=0; n<callToReplace.size(); n++) {
    CallInst *ci = callToReplace[n];

    // Operand 2 contains the outlined function
    Value *op2 = ci->getOperand(2);
    ConstantExpr *op2AsCE = dyn_cast<ConstantExpr>(op2);
    assert(op2AsCE);
    Instruction *op2AsInst = op2AsCE->getAsInstruction();
    Function *outlinedFunc = dyn_cast<Function>(op2AsInst->getOperand(0));
    assert(outlinedFunc);
    delete op2AsInst;

    errs() << outlinedFunc->getName() << "\n";

    unsigned callNbOps = ci->getNumOperands();

    SmallVector<Value *, 8> NewArgs;

    // map 2 firsts operands of CI to null
    for (unsigned i=0; i<2; i++) {
      Type *ArgTy = getFunctionArgument(outlinedFunc, i)->getType();
      Value *val = Constant::getNullValue(ArgTy);
      NewArgs.push_back(val);
    }

    //  op 3 to nbops-1
    for (unsigned i=3; i<callNbOps-1; i++) {
      NewArgs.push_back(ci->getOperand(i));
    }

    CallInst *NewCI = CallInst::Create(const_cast<Function *>(outlinedFunc), NewArgs);
    NewCI->setCallingConv(outlinedFunc->getCallingConv());
    ReplaceInstWithInst(const_cast<CallInst *>(ci), NewCI);
  }
}

bool
ParcoachInstr::runOnModule(Module &M) {
  if (optContextSensitive && optDotTaintPaths) {
    errs() << "Error: you cannot use -dot-taint-paths option in context "
	   << "sensitive mode.\n";
    exit(0);
  }

  if (optStats) {
    unsigned nbFunctions = 0;
    unsigned nbIndirectCalls = 0;
    unsigned nbDirectCalls = 0;
    for (const Function &F : M) {
      nbFunctions++;

      for (const BasicBlock &BB : F) {
	for( const Instruction &I : BB) {
	  const CallInst *ci = dyn_cast<CallInst>(&I);
	  if (!ci)
	    continue;
	  if (ci->getCalledFunction())
	    nbDirectCalls++;
	  else
	    nbIndirectCalls++;
	}
      }
    }

    errs() << "nb functions : " << nbFunctions << "\n";
    errs() << "nb direct calls : " << nbDirectCalls << "\n";
    errs() << "nb indirect calls : " << nbIndirectCalls << "\n";

    exit(0);
  }



  ExtInfo extInfo(M);

  // Replace OpenMP Micro Function Calls
  replaceOMPMicroFunctionCalls(M);

  // Run Andersen alias analysis.
  tstart_aa = gettime();
  Andersen AA(M);
  tend_aa = gettime();

  errs() << "* AA done\n";

  // Create PTA call graph
  tstart_pta = gettime();
  PTACallGraph PTACG(M, &AA);
  tend_pta = gettime();
  errs() << "* PTA Call graph creation done\n";

  // Create regions from allocation sites.
  tstart_regcreation = gettime();
  vector<const Value *> regions;
  AA.getAllAllocationSites(regions);



  errs() << regions.size() << " regions\n";
  unsigned regCounter = 0;
  for (const Value *r : regions) {
    if (regCounter%100 == 0) {
      errs() << regCounter << " regions created ("
	     << ((float) regCounter) / regions.size() * 100<< "%)\n";
      }
    regCounter++;
    MemReg::createRegion(r);
  }
  tend_regcreation = gettime();

  if (optDumpRegions)
    MemReg::dumpRegions();
  errs() << "* Regions creation done\n";

  // Compute MOD/REF analysis
  tstart_modref = gettime();
  ModRefAnalysis MRA(PTACG, &AA, &extInfo);
  tend_modref = gettime();
  if (optDumpModRef)
    MRA.dump();

  errs() << "* Mod/ref done\n";

  // Compute all-inclusive SSA.
  tstart_assa = gettime();
  MemorySSA MSSA(&M, &AA, &PTACG, &MRA, &extInfo);

  unsigned nbFunctions = M.getFunctionList().size();
  unsigned counter = 0;
  for (Function &F : M) {
    if (!PTACG.isReachableFromEntry(&F)) {
      errs() << F.getName() << " is not reachable from entry\n";


      continue;
    }

    if (counter % 100 == 0)
      errs() << "MSSA: visited " << counter << " functions over " << nbFunctions
	     << " (" << (((float) counter)/nbFunctions*100) << "%)\n";
    counter++;

    if (isIntrinsicDbgFunction(&F)) {
      continue;
    }

    if (F.isDeclaration())
      continue;

    DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
    DominanceFrontier &DF =
      getAnalysis<DominanceFrontierWrapperPass>(F).getDominanceFrontier();
    PostDominatorTree &PDT =
      getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();

    MSSA.buildSSA(&F, DT, DF, PDT);
    if (optDumpSSA)
      MSSA.dumpMSSA(&F);
    if (F.getName().equals(optDumpSSAFunc))
      MSSA.dumpMSSA(&F);
  }
  tend_assa = gettime();
  errs() << "* SSA done\n";

  // Compute dep graph.
  tstart_depgraph = gettime();
  DepGraph *DG = new DepGraph(&MSSA, &PTACG, this);

  counter = 0;
  for (Function &F : M) {
    if (!PTACG.isReachableFromEntry(&F))
      continue;

    if (counter % 100 == 0)
      errs() << "DepGraph: visited " << counter << " functions over " << nbFunctions
	     << " (" << (((float) counter)/nbFunctions*100) << "%)\n";

    counter++;

    if (isIntrinsicDbgFunction(&F))
      continue;

    DG->buildFunction(&F);
  }

  errs() << "* Dep graph done\n";

  // Phi elimination pass.
  if (!optDisablePhiElim)
    DG->phiElimination();

  errs() << "* phi elimination done\n";
  tend_depgraph = gettime();

  tstart_flooding = gettime();

  // Compute tainted values
  if (optContextSensitive)
    DG->computeTaintedValuesContextSensitive();
  else
    DG->computeTaintedValuesContextInsensitive();
  tend_flooding = gettime();
  errs() << "* value contamination  done\n";

  // Dot dep graph.
  if (optDotGraph)
    DG->toDot("dg.dot");

  errs() << "* Starting Parcoach analysis ...\n";

  tstart_parcoach = gettime();
  // Parcoach analysis

  PAInter = new ParcoachAnalysisInter(M, DG, PTACG, optNoInstrum);
  PAInter->run();

  PAIntra = new ParcoachAnalysisIntra(M, NULL, this, optNoInstrum);
  PAIntra->run();


  tend_parcoach = gettime();

  return false;
}

char ParcoachInstr::ID = 0;

double ParcoachInstr::tstart = 0;
double ParcoachInstr::tend = 0;
double ParcoachInstr::tstart_aa = 0;
double ParcoachInstr::tend_aa = 0;
double ParcoachInstr::tstart_pta = 0;
double ParcoachInstr::tend_pta = 0;
double ParcoachInstr::tstart_regcreation = 0;
double ParcoachInstr::tend_regcreation = 0;
double ParcoachInstr::tstart_modref = 0;
double ParcoachInstr::tend_modref = 0;
double ParcoachInstr::tstart_assa = 0;
double ParcoachInstr::tend_assa = 0;
double ParcoachInstr::tstart_depgraph = 0;
double ParcoachInstr::tend_depgraph = 0;
double ParcoachInstr::tstart_flooding = 0;
double ParcoachInstr::tend_flooding = 0;
double ParcoachInstr::tstart_parcoach = 0;
double ParcoachInstr::tend_parcoach = 0;

static RegisterPass<ParcoachInstr> Z("parcoach", "Module pass");
