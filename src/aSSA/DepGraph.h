#ifndef DEPGRAPH_H
#define DEPGRAPH_H

#include "MSSAMuChi.h"
#include "MemorySSA.h"

#include "llvm/IR/InstVisitor.h"
#include "llvm/Support/raw_ostream.h"

class DepGraph : public llvm::InstVisitor<DepGraph> {
public:
  typedef std::set<MSSAVar *> VarSet;
  typedef std::set<const MSSAVar *> ConstVarSet;
  typedef std::set<const llvm::Value *> ValueSet;

  DepGraph(MemorySSA *mssa);
  virtual ~DepGraph();

  void buildFunction(const llvm::Function *F, llvm::PostDominatorTree *PDT);
  void toDot(std::string filename);

  void visitBasicBlock(llvm::BasicBlock &BB);
  void visitAllocaInst(llvm::AllocaInst &I);
  void visitTerminatorInst(llvm::TerminatorInst &I);
  void visitCmpInst(llvm::CmpInst &I);
  void visitLoadInst(llvm::LoadInst &I);
  void visitStoreInst(llvm::StoreInst &I);
  void visitGetElementPtrInst(llvm::GetElementPtrInst &I);
  void visitPHINode(llvm::PHINode &I);
  void visitCastInst(llvm::CastInst &I);
  void visitSelectInst(llvm::SelectInst &I);
  void visitBinaryOperator(llvm::BinaryOperator &I);
  void visitCallInst(llvm::CallInst &I);
  void visitInstruction(llvm::Instruction &I);

  void isTaintedCalls(const llvm::Function *F);
  bool isTainted(const llvm::Value *v);

private:
  MemorySSA *mssa;

  const llvm::Function *curFunc;
  llvm::PostDominatorTree *curPDT;

  /* Graph nodes */

  // Map from a function to all its top-level variables nodes.
  llvm::DenseMap<const llvm::Function *, ValueSet> funcToLLVMNodesMap;
  // Map from a function to all its address taken ssa nodes.
  llvm::DenseMap<const llvm::Function *, VarSet> funcToSSANodesMap;

  /* Graph edges */

  // top-level to top-level edges
  llvm::DenseMap<const llvm::Value *, ValueSet> llvmToLLVMEdges;
  // top-level to address-taken ssa edges
  llvm::DenseMap<const llvm::Value *, VarSet> llvmToSSAEdges;
  // address-taken ssa to top-level edges
  llvm::DenseMap<MSSAVar *, ValueSet> ssaToLLVMEdges;
  // address-top ssa to address-taken ssa edges
  llvm::DenseMap<MSSAVar *, VarSet> ssaToSSAEdges;

  /* PDF+ call nodes and edges */

  std::set<const llvm::Function *> funcNodes;
  // map from a function to all its call instructions
  llvm::DenseMap<const llvm::Function *, ValueSet> funcToCallNodes;
  // map from call instructions to called functions
  llvm::DenseMap<const llvm::Value *, const llvm::Function *> callToFuncEdges;
  // map from a condition to call instructions depending on that condition.
  llvm::DenseMap<const llvm::Value *, ValueSet> condToCallEdges;

  /* tainted nodes */
  ValueSet taintedLLVMNodes;
  ValueSet taintedCallNodes;
  std::set<const llvm::Function *> taintedFunctions;
  ConstVarSet taintedSSANodes;
  ConstVarSet ssaSources;

  void computeTaintedValues();
  void computeTaintedValuesRec(const llvm::Value *v);
  void computeTaintedValuesRec(MSSAVar *v);
  void computeTaintedCalls();
  void computeTaintedCalls(const llvm::Value *v);
  void computeTaintedCalls(const llvm::Function *F);

  void dotFunction(llvm::raw_fd_ostream &stream, const llvm::Function *F);
  void dotExtFunction(llvm::raw_fd_ostream &stream, const llvm::Function *F);
  std::string getNodeStyle(const llvm::Value *v);
  std::string getNodeStyle(const MSSAVar *v);
  std::string getNodeStyle(const llvm::Function *f);
  std::string getCallNodeStyle(const llvm::Value *v);
};

#endif /* DEPGRAPH_H */
