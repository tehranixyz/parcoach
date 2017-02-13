#include "DepGraph.h"
#include "Utils.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <queue>

using namespace llvm;
using namespace std;

DepGraph::DepGraph(MemorySSA *mssa) : mssa(mssa), buildGraphTime(0),
				      phiElimTime(0), floodDepTime(0),
				      floodCallTime(0), dotTime(0) {}
DepGraph::~DepGraph() {}

void
DepGraph::buildFunction(const llvm::Function *F, PostDominatorTree *PDT) {
  double t1 = gettime();

  curFunc = F;
  curPDT = PDT;

  visit(*const_cast<Function *>(F));

  // Add entry chi nodes to the graph.
  for (MSSAChi *chi : mssa->funToEntryChiMap[F]) {
    assert(chi && chi->var);
    funcToSSANodesMap[F].insert(chi->var);
    if (chi->opVar) {
      funcToSSANodesMap[F].insert(chi->opVar);
      ssaToSSAEdges[chi->opVar].insert(chi->var);
    }
  }

  // External functions
  if (F->isDeclaration()) {
    // Add var arg entry and exit chi nodes.
    if (F->isVarArg()) {
      MSSAChi *entryChi = mssa->extVarArgEntryChi[F];
      assert(entryChi && entryChi->var);
      funcToSSANodesMap[F].insert(entryChi->var);
      MSSAChi *exitChi =  mssa->extVarArgExitChi[F];
      assert(exitChi && exitChi->var);
      funcToSSANodesMap[F].insert(exitChi->var);
      ssaToSSAEdges[exitChi->opVar].insert(exitChi->var);
    }

    // Add args entry and exit chi nodes for external functions.
    unsigned argNo = 0;
    for (const Argument &arg : F->getArgumentList()) {
      if (!arg.getType()->isPointerTy()) {
	argNo++;
	continue;
      }

      MSSAChi *entryChi = mssa->extArgEntryChi[F][argNo];
      assert(entryChi && entryChi->var);
      funcToSSANodesMap[F].insert(entryChi->var);
      MSSAChi *exitChi =  mssa->extArgExitChi[F][argNo];
      assert(exitChi && exitChi->var);
      funcToSSANodesMap[F].insert(exitChi->var);
      ssaToSSAEdges[exitChi->opVar].insert(exitChi->var);

      argNo++;
    }

    // Add retval chi node for external functions
    if (F->getReturnType()->isPointerTy()) {
      MSSAChi *retChi = mssa->extRetChi[F];
      assert(retChi && retChi->var);
      funcToSSANodesMap[F].insert(retChi->var);
    }

    // If the function is MPI_Comm_rank set the address-taken ssa of the
    // second argument as a contamination source.
    if (F->getName().equals("MPI_Comm_rank")) {
      assert(mssa->extArgExitChi[F][1]);
      ssaSources.insert(mssa->extArgExitChi[F][1]->var);
    }

    // If the function is MPI_Group_rank set the address-taken ssa of the
    // second argument as a contamination source.
    if (F->getName().equals("MPI_Group_rank")) {
      assert(mssa->extArgExitChi[F][1]);
      ssaSources.insert(mssa->extArgExitChi[F][1]->var);
    }

    // memcpy
    if (F->getName().find("memcpy") != StringRef::npos) {
      MSSAChi *srcEntryChi = mssa->extArgEntryChi[F][1];
      MSSAChi *dstExitChi = mssa->extArgExitChi[F][0];

      ssaToSSAEdges[srcEntryChi->var].insert(dstExitChi->var);

      // llvm.mempcy instrinsic returns void whereas memcpy returns dst
      if (F->getReturnType()->isPointerTy()) {
	MSSAChi *retChi = mssa->extRetChi[F];
	ssaToSSAEdges[dstExitChi->var].insert(retChi->var);
      }
    }

    // memmove
    if (F->getName().find("memmove") != StringRef::npos) {
      MSSAChi *srcEntryChi = mssa->extArgEntryChi[F][1];
      MSSAChi *dstExitChi = mssa->extArgExitChi[F][0];

      ssaToSSAEdges[srcEntryChi->var].insert(dstExitChi->var);

      // llvm.memmove instrinsic returns void whereas memmove returns dst
      if (F->getReturnType()->isPointerTy()) {
	MSSAChi *retChi = mssa->extRetChi[F];
	ssaToSSAEdges[dstExitChi->var].insert(retChi->var);
      }
    }

    // memset
    if (F->getName().find("memset") != StringRef::npos) {
      MSSAChi *argExitChi = mssa->extArgExitChi[F][0];
      const Argument *cArg = getFunctionArgument(F, 1);
      assert(cArg);

      llvmToSSAEdges[cArg].insert(argExitChi->var);

      // llvm.memset instrinsic returns void whereas memset returns dst
      if (F->getReturnType()->isPointerTy()) {
	MSSAChi *retChi = mssa->extRetChi[F];
	ssaToSSAEdges[argExitChi->var].insert(retChi->var);
      }
    }
  }

  double t2 = gettime();

  buildGraphTime += t2 - t1;
}

void
DepGraph::visitBasicBlock(llvm::BasicBlock &BB) {
  // Add MSSA Phi nodes and edges to the graph.
  for (MSSAPhi *phi : mssa->bbToPhiMap[&BB]) {
    assert(phi && phi->var);
    funcToSSANodesMap[curFunc].insert(phi->var);
    for (auto I : phi->opsVar) {
      assert(I.second);
      funcToSSANodesMap[curFunc].insert(I.second);
      ssaToSSAEdges[I.second].insert(phi->var);
    }

    for (const Value *pred : phi->preds) {
      funcToLLVMNodesMap[curFunc].insert(pred);
      llvmToSSAEdges[pred].insert(phi->var);
    }
  }
}

void
DepGraph::visitAllocaInst(llvm::AllocaInst &I) {
  // Do nothing
}

void
DepGraph::visitTerminatorInst(llvm::TerminatorInst &I) {
  // Do nothing
}

void
DepGraph::visitCmpInst(llvm::CmpInst &I) {
  // Cmp instruction is a value, connect the result to its operands.
  funcToLLVMNodesMap[curFunc].insert(&I);

  for (const Value *v : I.operands()) {
    llvmToLLVMEdges[v].insert(&I);
    funcToLLVMNodesMap[curFunc].insert(v);
  }
}

void
DepGraph::visitLoadInst(llvm::LoadInst &I) {
  // Load inst, connect MSSA mus and the pointer loaded.
  funcToLLVMNodesMap[curFunc].insert(&I);
  funcToLLVMNodesMap[curFunc].insert(I.getPointerOperand());

  for (MSSAMu *mu : mssa->loadToMuMap[&I]) {
    assert(mu && mu->var);
    funcToSSANodesMap[curFunc].insert(mu->var);
    ssaToLLVMEdges[mu->var].insert(&I);
  }

  llvmToLLVMEdges[I.getPointerOperand()].insert(&I);
}

void
DepGraph::visitStoreInst(llvm::StoreInst &I) {
  // Store inst
  // For each chi, connect the pointer, the value stored and the MSSA operand.
  for (MSSAChi *chi : mssa->storeToChiMap[&I]) {
    assert(chi && chi->var && chi->opVar);
    funcToSSANodesMap[curFunc].insert(chi->var);
    funcToSSANodesMap[curFunc].insert(chi->opVar);
    funcToLLVMNodesMap[curFunc].insert(I.getPointerOperand());
    funcToLLVMNodesMap[curFunc].insert(I.getValueOperand());

    ssaToSSAEdges[chi->opVar].insert(chi->var);
    llvmToSSAEdges[I.getValueOperand()].insert(chi->var);
    llvmToSSAEdges[I.getPointerOperand()].insert(chi->var);
  }
}

void
DepGraph::visitGetElementPtrInst(llvm::GetElementPtrInst &I) {
  // GetElementPtr, connect operands.
  funcToLLVMNodesMap[curFunc].insert(&I);

  for (const Value *v : I.operands()) {
    llvmToLLVMEdges[v].insert(&I);
    funcToLLVMNodesMap[curFunc].insert(v);
  }
}
void
DepGraph::visitPHINode(llvm::PHINode &I) {
  // Connect LLVM Phi, connect operands and predicates.
  funcToLLVMNodesMap[curFunc].insert(&I);

  for (const Value *v : I.operands()) {
    llvmToLLVMEdges[v].insert(&I);
    funcToLLVMNodesMap[curFunc].insert(v);
  }

  for (const Value *v : mssa->llvmPhiToPredMap[&I]) {
    llvmToLLVMEdges[v].insert(&I);
    funcToLLVMNodesMap[curFunc].insert(v);
  }
}
void
DepGraph::visitCastInst(llvm::CastInst &I) {
  // Cast inst, connect operands
  funcToLLVMNodesMap[curFunc].insert(&I);

  for (const Value *v : I.operands()) {
    llvmToLLVMEdges[v].insert(&I);
    funcToLLVMNodesMap[curFunc].insert(v);
  }
}
void
DepGraph::visitSelectInst(llvm::SelectInst &I) {
  // Select inst, connect operands
  funcToLLVMNodesMap[curFunc].insert(&I);

  for (const Value *v : I.operands()) {
    llvmToLLVMEdges[v].insert(&I);
    funcToLLVMNodesMap[curFunc].insert(v);
  }
}
void
DepGraph::visitBinaryOperator(llvm::BinaryOperator &I) {
  // Binary op, connect operands
  funcToLLVMNodesMap[curFunc].insert(&I);

  for (const Value *v : I.operands()) {
    llvmToLLVMEdges[v].insert(&I);
    funcToLLVMNodesMap[curFunc].insert(v);
  }
}

void
DepGraph::visitCallInst(llvm::CallInst &I) {
  /* Building rules for call sites :
   *
   * %c = call f (..., %a, ...)
   * [ mu(..., o1, ...) ]
   * [ ...
   *  o2 = chi(o1)
   *  ... ]
   *
   * define f (..., %b, ...) {
   *  [ ..., o0 = X(o), ... ]
   *
   *  ...
   *
   *  [ ...
   *    mu(on)
   *    ... ]
   *  ret %r
   * }
   *
   * Top-level variables
   *
   * rule1: %a -----> %b
   * rule2: %c <----- %r
   *
   * Address-taken variables
   *
   * rule3: o1 ------> o0 in f
   * rule4: o1 ------> o2
   * rule5: o2 <------ on in f
   */

  if (isIntrinsicDbgInst(&I))
    return;

  // Chi of the callsite.
  for (MSSAChi *chi : mssa->callSiteToChiMap[CallSite(&I)]) {
    assert(chi && chi->var && chi->opVar);
    funcToSSANodesMap[curFunc].insert(chi->opVar);
    funcToSSANodesMap[curFunc].insert(chi->var);
    ssaToSSAEdges[chi->opVar].insert(chi->var); // rule4

    MSSACallChi *callChi = cast<MSSACallChi>(chi);
    const Function *called = callChi->called;

    // External Function, we connect call chi to artifical chi of the external
    // function for each argument.
    if (called->isDeclaration()) {
      MSSAExtCallChi *extCallChi = cast<MSSAExtCallChi>(callChi);
      unsigned argNo = extCallChi->argNo;

      // Case where this is a var arg parameter.
      if (argNo >= called->arg_size()) {
	assert(called->isVarArg());

	assert(mssa->extVarArgExitChi[called]);
	MSSAVar *var = mssa->extVarArgExitChi[called]->var;
	assert(var);
	funcToSSANodesMap[called].insert(var);
	ssaToSSAEdges[var].insert(chi->var); // rule5
      }

      else {
	// rule5
	assert(mssa->extArgExitChi[called][argNo]);
	ssaToSSAEdges[mssa->extArgExitChi[called][argNo]->var].insert(chi->var);
      }

      continue;
    }

    auto it = mssa->funRegToReturnMuMap.find(called);
    if (it != mssa->funRegToReturnMuMap.end()) {
      MSSAMu *returnMu = it->second[chi->region];
      assert(returnMu && returnMu->var);
      funcToSSANodesMap[called].insert(returnMu->var);
      ssaToSSAEdges[returnMu->var].insert(chi->var); // rule5
    }
  }

  // Mu of the call site.
  for (MSSAMu *mu : mssa->callSiteToMuMap[CallSite(&I)]) {
    assert(mu && mu->var);
    funcToSSANodesMap[curFunc].insert(mu->var);
    MSSACallMu *callMu = cast<MSSACallMu>(mu);
    const Function *called = callMu->called;

    // External Function, we connect call mu to artifical chi of the external
    // function for each argument.
    if (called->isDeclaration()) {
      MSSAExtCallMu *extCallMu = cast<MSSAExtCallMu>(callMu);
      unsigned argNo = extCallMu->argNo;

      // Case where this is a var arg parameter
      if (argNo >= called->arg_size()) {
	assert(called->isVarArg());

	assert(mssa->extVarArgEntryChi[called]);
	MSSAVar *var = mssa->extVarArgEntryChi[called]->var;
	assert(var);
	funcToSSANodesMap[called].insert(var);
	ssaToSSAEdges[mu->var].insert(var); // rule3
      }

      else {
	// rule3
	assert(mssa->extArgEntryChi[called][argNo]);
	ssaToSSAEdges[mu->var].insert(mssa->extArgEntryChi[called][argNo]->var);
      }

      continue;
    }

    auto it = mssa->funRegToEntryChiMap.find(called);
    if (it != mssa->funRegToEntryChiMap.end()) {
      MSSAChi *entryChi = it->second[mu->region];
      assert(entryChi && entryChi->var);
      funcToSSANodesMap[called].insert(entryChi->var);
      ssaToSSAEdges[callMu->var].insert(entryChi->var); // rule3
    }
  }

  // Connect effective parameters to formal parameters.
  const Function *called = I.getCalledFunction();
  unsigned argIdx = 0;
  for (const Argument &arg : called->getArgumentList()) {
    funcToLLVMNodesMap[curFunc].insert(I.getArgOperand(argIdx));
    funcToLLVMNodesMap[called].insert(&arg);

    llvmToLLVMEdges[I.getArgOperand(argIdx)].insert(&arg); // rule1

    argIdx++;
  }

  // If the function called returns a value, connect the return value to the
  // call value.
  if (!called->isDeclaration() && !I.getType()->isVoidTy()) {
    funcToLLVMNodesMap[curFunc].insert(&I);
    llvmToLLVMEdges[getReturnValue(called)].insert(&I); // rule2
  }

  // External function, if the function called returns a pointer, connect the
  // artifical ret chi to the retcallchi
  // return chi of the caller.
  if (called->isDeclaration() && called->getReturnType()->isPointerTy()) {
    for (MSSAChi *chi : mssa->extCallSiteToRetChiMap[CallSite(&I)]) {
      assert(chi && chi->var && chi->opVar);
      funcToSSANodesMap[curFunc].insert(chi->var);
      funcToSSANodesMap[curFunc].insert(chi->opVar);

      ssaToSSAEdges[chi->opVar].insert(chi->var);
      ssaToSSAEdges[mssa->extRetChi[called]->var].insert(chi->var);
    }
  }

  // Add call node
  funcToCallNodes[curFunc].insert(&I);

  // Add pred to call edges
  set<const Value *> preds = computeIPDFPredicates(*curPDT, I.getParent());
  for (const Value *pred : preds) {
    condToCallEdges[pred].insert(&I);
    callsiteToConds[&I].insert(pred);
  }

  // Add call to func edge
  callToFuncEdges[&I] = I.getCalledFunction();

  funcToCallSites[called].insert(&I);
}

void
DepGraph::visitInstruction(llvm::Instruction &I) {
  errs() << "Error: Unhandled instruction " << I << "\n";
}

void
DepGraph::toDot(string filename) {
  errs() << "Writing '" << filename << "' ...\n";

  double t1 = gettime();

  error_code EC;
  raw_fd_ostream stream(filename, EC, sys::fs::F_Text);

  stream << "digraph F {\n";
  stream << "compound=true;\n";
  stream << "rankdir=LR;\n";


  // dot global LLVM values in a separate cluster
  stream << "\tsubgraph cluster_globalsvar {\n";
  stream << "style=filled;\ncolor=lightgrey;\n";
  stream << "label=< <B>  Global Values </B> >;\n";
  stream << "node [style=filled,color=white];\n";
  for (const Value &g : mssa->m->globals()) {
    stream << "Node" << ((void *) &g) << " [label=\""
	   << getValueLabel(&g) << "\" "
	   << getNodeStyle(&g) << "];\n";
  }
  stream << "}\n;";

  for (auto I = mssa->m->begin(), E = mssa->m->end(); I != E; ++I) {
    const Function *F = &*I;
    if (isIntrinsicDbgFunction(F))
      continue;

    if (F->isDeclaration())
      dotExtFunction(stream, F);
    else
      dotFunction(stream, F);
  }

  // Edges
  for (auto I : llvmToLLVMEdges) {
    const Value *s = I.first;
    for (const Value *d : I.second) {
      stream << "Node" << ((void *) s) << " -> "
      	     << "Node" << ((void *) d) << "\n";
    }
  }

  for (auto I : llvmToSSAEdges) {
    const Value *s = I.first;
    for (MSSAVar *d : I.second) {
      stream << "Node" << ((void *) s) << " -> "
  	     << "Node" << ((void *) d) << "\n";
    }
  }

  for (auto I : ssaToSSAEdges) {
    MSSAVar *s = I.first;
    for (MSSAVar *d : I.second) {
      stream << "Node" << ((void *) s) << " -> "
  	     << "Node" << ((void *) d) << "\n";
    }
  }

  for (auto I : ssaToLLVMEdges) {
    MSSAVar *s = I.first;
    for (const Value *d : I.second) {
      stream << "Node" << ((void *) s) << " -> "
  	     << "Node" << ((void *) d) << "\n";
    }
  }

  for (auto I : callToFuncEdges) {
    const Value *call = I.first;
    const Function *f = I.second;
    stream << "NodeCall" << ((void *) call) << " -> "
	   << "Node" << ((void *) f)
      	   << " [lhead=cluster_" << ((void *) f)
	   <<"]\n";
  }

  for (auto I : condToCallEdges) {
    const Value *s = I.first;
    for (const Value *call : I.second) {
      stream << "Node" << ((void *) s) << " -> "
  	     << "NodeCall" << ((void *) call) << "\n";
    }
  }

  stream << "}\n";

  double t2 = gettime();

  dotTime += t2 - t1;
}

void
DepGraph::dotFunction(raw_fd_ostream &stream, const Function *F) {
  stream << "\tsubgraph cluster_" << ((void *) F) << " {\n";
  stream << "style=filled;\ncolor=lightgrey;\n";
  stream << "label=< <B>" << F->getName() << "</B> >;\n";
  stream << "node [style=filled,color=white];\n";


  // Nodes with label
  for (const Value *v : funcToLLVMNodesMap[F]) {
    if (isa<GlobalValue>(v))
      continue;
    stream << "Node" << ((void *) v) << " [label=\""
	   << getValueLabel(v) << "\" "
	   << getNodeStyle(v) << "];\n";

  }

  for (const MSSAVar *v : funcToSSANodesMap[F]) {
    stream << "Node" << ((void *) v) << " [label=\""
	   << v->getName()
	   <<  "\" shape=diamond "
	   << getNodeStyle(v) << "];\n";
  }

  for (const Value *v : funcToCallNodes[F]) {
    stream << "NodeCall" << ((void *) v) << " [label=\""
	   << getCallValueLabel(v)
	   <<  "\" shape=rectangle "
	   << getCallNodeStyle(v) << "];\n";
  }

  stream << "Node" << ((void *) F) << " [style=invisible];\n";

  stream << "}\n";
}

void
DepGraph::dotExtFunction(raw_fd_ostream &stream, const Function *F) {
  stream << "\tsubgraph cluster_" << ((void *)F) << " {\n";
  stream << "style=filled;\ncolor=lightgrey;\n";
  stream << "label=< <B>" << F->getName() << "</B> >;\n";
  stream << "node [style=filled,color=white];\n";


  // Nodes with label
  for (const Value *v : funcToLLVMNodesMap[F]) {
    stream << "Node" << ((void *) v) << " [label=\""
	   << getValueLabel(v) << "\" "
	   << getNodeStyle(v) << "];\n";
  }

  for (const MSSAVar *v : funcToSSANodesMap[F]) {
    stream << "Node" << ((void *) v) << " [label=\""
	   << v->getName()
	   <<  "\" shape=diamond "
	   << getNodeStyle(v) << "];\n";
  }

  stream << "Node" << ((void *) F) << " [style=invisible];\n";

  stream << "}\n";
}

std::string
DepGraph::getNodeStyle(const llvm::Value *v) {
  if (taintedLLVMNodes.count(v) != 0)
    return "style=filled, color=red";
  return "style=filled, color=white";
}

std::string
DepGraph::getNodeStyle(const MSSAVar *v) {
  if (taintedSSANodes.count(v) != 0)
    return "style=filled, color=red";
  return "style=filled, color=white";
}

std::string
DepGraph::getNodeStyle(const Function *f) {
  if (taintedFunctions.count(f) != 0)
    return "style=filled, color=red";
  return "style=filled, color=white";
}

std::string
DepGraph::getCallNodeStyle(const llvm::Value *v) {
  if (taintedCallNodes.count(v) != 0)
    return "style=filled, color=red";
  return "style=filled, color=white";
}


void
DepGraph::computeTaintedValues() {
  double t1 = gettime();

  std::queue<MSSAVar *> varToVisit;
  std::queue<const Value *> valueToVisit;

  for(const MSSAVar *src : ssaSources) {
    taintedSSANodes.insert(src);
    varToVisit.push(const_cast<MSSAVar *>(src));
  }

  while (varToVisit.size() > 0 || valueToVisit.size() > 0) {
    if (varToVisit.size() > 0) {
      MSSAVar *s = varToVisit.front();
      varToVisit.pop();

      for (MSSAVar *d : ssaToSSAEdges[s]) {
	if (taintedSSANodes.count(d) != 0)
	  continue;

	taintedSSANodes.insert(d);
	varToVisit.push(d);
      }

      for (const Value *d : ssaToLLVMEdges[s]) {
	if (taintedLLVMNodes.count(d) != 0)
	  continue;

	taintedLLVMNodes.insert(d);
	valueToVisit.push(d);
      }
    }

    if (valueToVisit.size() > 0) {
      const Value *s = valueToVisit.front();
      valueToVisit.pop();

      for (const Value *d : llvmToLLVMEdges[s]) {
	if (taintedLLVMNodes.count(d) != 0)
	  continue;

	taintedLLVMNodes.insert(d);
	valueToVisit.push(d);
      }

      for (MSSAVar *d : llvmToSSAEdges[s]) {
	if (taintedSSANodes.count(d) != 0)
	  continue;
	taintedSSANodes.insert(d);
	varToVisit.push(d);
      }
    }
  }

  double t2 = gettime();

  floodDepTime += t2 - t1;
}

void
DepGraph::computeTaintedCalls() {
  double t1 = gettime();

  queue<const Function *> funcToVisit;

  for (auto I : condToCallEdges) {
    const Value *cond = I.first;
    if (taintedLLVMNodes.count(cond) == 0)
      continue;

    for (const Value *call : I.second) {
      taintedCallNodes.insert(call);
      funcToVisit.push(callToFuncEdges[call]);
    }
  }

 while (funcToVisit.size() > 0) {
   const Function *s = funcToVisit.front();
   funcToVisit.pop();

   for (const Value *d : funcToCallNodes[s]) {
     if (taintedCallNodes.count(d) != 0)
       continue;
     taintedCallNodes.insert(d);
     funcToVisit.push(callToFuncEdges[d]);
   }
 }

 double t2 = gettime();

 floodCallTime += t2 - t1;
}

void
DepGraph::printTimers() const {
  errs() << "Build graph time : " << buildGraphTime*1.0e3 << " ms\n";
  errs() << "Phi elimination time : " << phiElimTime*1.0e3 << " ms\n";
  errs() << "Flood dependencies time : " << floodDepTime*1.0e3 << " ms\n";
  errs() << "Flood calls PDF+ time : " << floodCallTime*1.0e3 << " ms\n";
  errs() << "Dot graph time : " << dotTime*1.0e3 << " ms\n";
}

bool
DepGraph::isTaintedCall(const CallInst *CI) {
  return taintedCallNodes.count(CI) != 0;
}

bool
DepGraph::isTaintedValue(const Value *v){
	if (taintedLLVMNodes.count(v) != 0)
		return true;
	return false;
}

void
DepGraph::getTaintedCallConditions(const llvm::CallInst *call,
				   std::set<const llvm::Value *> &conditions) {
  std::set<const llvm::CallInst *> visitedCallSites;
  queue<const CallInst *> callsitesToVisit;
  callsitesToVisit.push(call);

  while (callsitesToVisit.size() > 0) {
    const CallInst *CS = callsitesToVisit.front();
    const Function *F = CS->getParent()->getParent();
    callsitesToVisit.pop();
    visitedCallSites.insert(CS);

    for (const Value *cond : callsiteToConds[CS])
      conditions.insert(cond);

    for (const Value *v : funcToCallSites[F]) {
      const CallInst *CS2 = cast<CallInst>(v);
      if (visitedCallSites.count(CS2) != 0)
	continue;
      callsitesToVisit.push(CS2);
    }
  }
}

bool
DepGraph::areSSANodesEquivalent(MSSAVar *var1, MSSAVar *var2) {
  assert(var1);
  assert(var2);

  if (var1->def->type == MSSADef::PHI || var2->def->type == MSSADef::PHI)
    return false;

  VarSet incomingSSAsVar1;
  VarSet incomingSSAsVar2;

  ValueSet incomingValuesVar1;
  ValueSet incomingValuesVar2;

  // Check whether outgoing edges are the same for both nodes.
  if (ssaToSSAEdges[var1].size() != ssaToSSAEdges[var2].size())
    return false;

  if (ssaToLLVMEdges[var1].size() != ssaToLLVMEdges[var2].size())
    return false;

  for (MSSAVar *v : ssaToSSAEdges[var1]) {
    if (ssaToSSAEdges[var2].find(v) == ssaToSSAEdges[var2].end())
      return false;
  }
  for (const Value *v : ssaToLLVMEdges[var1]) {
    if (ssaToLLVMEdges[var2].find(v) == ssaToLLVMEdges[var2].end())
      return false;
  }

  // Check whether incoming edges are the same for both nodes.
  for (auto I : ssaToSSAEdges) {
    auto it1 = I.second.find(var1);
    auto it2 = I.second.find(var2);

    if ((it1 == I.second.end() && it2 != I.second.end()) ||
	(it1 != I.second.end() && it2 == I.second.end()))
      return false;
  }
  for (auto I : llvmToSSAEdges) {
    auto it1 = I.second.find(var1);
    auto it2 = I.second.find(var2);

    if ((it1 == I.second.end() && it2 != I.second.end()) ||
	(it1 != I.second.end() && it2 == I.second.end()))
      return false;
  }

  return true;
}

void
DepGraph::eliminatePhi(MSSAPhi *phi, MSSAVar *op1, MSSAVar *op2) {
  // Remove links from predicates to PHI
  for (const Value *v : phi->preds) {
    auto it = llvmToSSAEdges[v].find(phi->var);
    assert(it != llvmToSSAEdges[v].end());
    llvmToSSAEdges[v].erase(it);
  }

  // Remove links from op1,op2 to PHI
  auto it1 = ssaToSSAEdges[op1].find(phi->var);
  assert(it1 != ssaToSSAEdges[op1].end());
  ssaToSSAEdges[op1].erase(it1);
  auto it2 = ssaToSSAEdges[op2].find(phi->var);
  assert(it2 != ssaToSSAEdges[op2].end());
  ssaToSSAEdges[op2].erase(it2);

  // For each outgoing edge from PHI to a SSA node N, connect
  // op1 to N and remove the link from PHI to N.
  for (MSSAVar *v : ssaToSSAEdges[phi->var]) {
    ssaToSSAEdges[op1].insert(v);

    // If N is a phi replace the phi operand of N with
    // PHI operand 0
    if (v->def->type == MSSADef::PHI) {
      MSSAPhi *outPHI = cast<MSSAPhi>(v->def);

      bool found = false;
      for (auto I = outPHI->opsVar.begin(), E = outPHI->opsVar.end(); I != E;
	   ++I) {
	if (I->second == phi->var) {
	  found = true;
	  I->second = op1;
	  break;
	}
      }
      assert(found);
    }

    auto it = ssaToSSAEdges[phi->var].find(v);
    assert(it != ssaToSSAEdges[phi->var].end());
    ssaToSSAEdges[phi->var].erase(it);
  }

  // For each outgoing edge from PHI to a LLVM node N, connect
  // connect PHI operand 0 to N and remove the link from PHI to N.
  for (const Value *v : ssaToLLVMEdges[phi->var]) {
    ssaToLLVMEdges[op1].insert(v);
    // ssaToLLVMEdges[op2].insert(v);

    auto it = ssaToLLVMEdges[phi->var].find(v);
    assert(it != ssaToLLVMEdges[phi->var].end());
    ssaToLLVMEdges[phi->var].erase(it);
  }

  // Remove PHI Node
  const Function *F = phi->var->bb->getParent();
  assert(F);
  auto it3 = funcToSSANodesMap[F].find(phi->var);
  assert(it3 != funcToSSANodesMap[F].end());
  funcToSSANodesMap[F].erase(it3);

  // Remove edges connected to PHI operand 1
  for (auto I = ssaToSSAEdges.begin(), E = ssaToSSAEdges.end(); I != E;
       ++I) {
    set<MSSAVar *>::iterator J = I->second.find(op2);
    if (J != I->second.end()) {
      I->second.erase(J);
      assert(I->second.find(op2) == I->second.end());
    }
  }
  for (auto I : ssaToSSAEdges) {
    set<MSSAVar *>::iterator J = I.second.find(op2);
    assert (J == I.second.end());
  }
  for (auto I = llvmToSSAEdges.begin(), E = llvmToSSAEdges.end(); I != E;
       ++I) {
    auto J = I->second.find(op2);
    if (J != I->second.end()) {
      I->second.erase(J);
      assert(I->second.find(op2) == I->second.end());
    }
  }

  // Remove PHI operand 1
  auto it4 = funcToSSANodesMap[F].find(op2);
  assert(it4 != funcToSSANodesMap[F].end());
  funcToSSANodesMap[F].erase(it4);
}


void
DepGraph::phiElimination() {
  double t1 = gettime();

  // For each function, iterate through its basic block and try to eliminate phi
  // function until reaching a fixed point.
  for (const Function &F : *mssa->m) {
    bool changed = true;

    while (changed) {
      changed = false;

      for (const BasicBlock &BB : F) {
	for (MSSAPhi *phi : mssa->bbToPhiMap[&BB]) {
	  if (funcToSSANodesMap[&F].count(phi->var) == 0)
	    continue;

	  // For each phi we test if its operands (chi) are not PHI and
	  // are equivalent
	  vector<MSSAVar *> phiOperands;
	  for (auto J : phi->opsVar)
	    phiOperands.push_back(J.second);

	  if (phiOperands.size() != 2)
	    continue;

	  assert(phiOperands[0] != phiOperands[1]);

	  if (!areSSANodesEquivalent(phiOperands[0], phiOperands[1]))
	    continue;

	  // PHI Node can be eliminated !
	  changed = true;
	  eliminatePhi(phi, phiOperands[0], phiOperands[1]);
	}
      }
    }
  }

  double t2 = gettime();
  phiElimTime += t2 - t1;
}
