#ifndef DEPGRAPH_H
#define DEPGRAPH_H

#include "MSSAMuChi.h"
#include "MemorySSA.h"
#include "PTACallGraph.h"

#include "llvm/IR/InstVisitor.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

class DepGraph : public llvm::InstVisitor<DepGraph> {
public:
  typedef std::set<MSSAVar *> VarSet;
  typedef std::set<const MSSAVar *> ConstVarSet;
  typedef std::set<const llvm::Value *> ValueSet;

  DepGraph(MemorySSA *mssa, PTACallGraph *CG, llvm::Pass *pass);
  virtual ~DepGraph();

  void buildFunction(const llvm::Function *F);
  void toDot(std::string filename);
  void dotTaintPath(const llvm::Value *v, std::string filename,
		    const llvm::Instruction *collective);


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
  void visitExtractValueInst(llvm::ExtractValueInst &I);
  void visitExtractElementInst(llvm::ExtractElementInst &I);
  void visitInsertElementInst(llvm::InsertElementInst &I);
  void visitInsertValueInst(llvm::InsertValueInst &I);
  void visitShuffleVectorInst(llvm::ShuffleVectorInst &I);
  void visitInstruction(llvm::Instruction &I);

  // Phi elimination pass.
  // A ssa Phi function can be eliminated if its operands are equivalent.
  // In this case operands are merged into a single node and the phi is replaced
  // with this single node. The phi elimination pass allows us to break the
  // dependency with phi predicates when its operands are the same, e.g.:
  // if (rank)
  //   a = 0;
  // else
  //   a = 0;
  void phiElimination();

  void computeTaintedValues();
  void computeTaintedCalls();

  bool isTaintedCall(const llvm::CallInst *CI);
  bool isTaintedValue(const llvm::Value *v);
  void getTaintedCallConditions(const llvm::CallInst *call,
				std::set<const llvm::Value *> &conditions);
  void getTaintedCallInterIPDF(const llvm::CallInst *call,
			       std::set<const llvm::BasicBlock *> &ipdf);

  void printTimers() const;

private:
  MemorySSA *mssa;
  PTACallGraph *CG;
  llvm::Pass *pass;

  const llvm::Function *curFunc;
  llvm::PostDominatorTree *curPDT;

  /* Graph nodes */

  // Map from a function to all its top-level variables nodes.
  llvm::DenseMap<const llvm::Function *, ValueSet> funcToLLVMNodesMap;
  // Map from a function to all its address taken ssa nodes.
  llvm::DenseMap<const llvm::Function *, VarSet> funcToSSANodesMap;
  std::set<const llvm::Function *> varArgNodes;

  /* Graph edges */

  // top-level to top-level edges
  llvm::DenseMap<const llvm::Value *, ValueSet> llvmToLLVMChildren;
  llvm::DenseMap<const llvm::Value *, ValueSet> llvmToLLVMParents;

  // top-level to address-taken ssa edges
  llvm::DenseMap<const llvm::Value *, VarSet> llvmToSSAChildren;
  llvm::DenseMap<const llvm::Value *, VarSet> llvmToSSAParents;
  // address-taken ssa to top-level edges
  llvm::DenseMap<MSSAVar *, ValueSet> ssaToLLVMChildren;
  llvm::DenseMap<MSSAVar *, ValueSet> ssaToLLVMParents;

  // address-top ssa to address-taken ssa edges
  llvm::DenseMap<MSSAVar *, VarSet> ssaToSSAChildren;
  llvm::DenseMap<MSSAVar *, VarSet> ssaToSSAParents;

  void addEdge(const llvm::Value *s, const llvm::Value *d);
  void addEdge(const llvm::Value *s, MSSAVar *d);
  void addEdge(MSSAVar *s, const llvm::Value *d);
  void addEdge(MSSAVar *s, MSSAVar *d);
  void removeEdge(const llvm::Value *s, const llvm::Value *d);
  void removeEdge(const llvm::Value *s, MSSAVar *d);
  void removeEdge(MSSAVar *s, const llvm::Value *d);
  void removeEdge(MSSAVar *s, MSSAVar *d);

  /* PDF+ call nodes and edges */

  // map from a function to all its call instructions
  llvm::DenseMap<const llvm::Function *, ValueSet> funcToCallNodes;
  // map from call instructions to called functions
  llvm::DenseMap<const llvm::Value *, const llvm::Function *> callToFuncEdges;
  // map from a condition to call instructions depending on that condition.
  llvm::DenseMap<const llvm::Value *, ValueSet> condToCallEdges;

  // map from a function to all the call sites calling this function.
  llvm::DenseMap<const llvm::Function *, ValueSet> funcToCallSites;
  // map from a callsite to all its conditions.
  llvm::DenseMap<const llvm::Value *, ValueSet> callsiteToConds;

  /* tainted nodes */
  ValueSet taintedLLVMNodes;
  ValueSet taintedCallNodes;
  std::set<const llvm::Function *> taintedFunctions;
  ConstVarSet taintedSSANodes;
  ConstVarSet taintResetSSANodes;
  ConstVarSet ssaSources;

  /* Graph construction for call sites*/
  void connectCSMus(llvm::CallInst &I);
  void connectCSChis(llvm::CallInst &I);
  void connectCSEffectiveParameters(llvm::CallInst &I);
  void connectCSEffectiveParametersExt(llvm::CallInst &I,
				       const llvm::Function *callee);
  void connectCSCalledReturnValue(llvm::CallInst &I);
  void connectCSRetChi(llvm::CallInst &I);

  // Two nodes are equivalent if they have exactly the same incoming and
  // outgoing edges and if none of them are phi nodes.
  bool areSSANodesEquivalent(MSSAVar *var1, MSSAVar *var2);

  // This function replaces phi with op1 and removes op2.
  void eliminatePhi(MSSAPhi *phi, std::vector<MSSAVar *> ops);

  void dotFunction(llvm::raw_fd_ostream &stream, const llvm::Function *F);
  void dotExtFunction(llvm::raw_fd_ostream &stream, const llvm::Function *F);
  std::string getNodeStyle(const llvm::Value *v);
  std::string getNodeStyle(const MSSAVar *v);
  std::string getNodeStyle(const llvm::Function *f);
  std::string getCallNodeStyle(const llvm::Value *v);

  struct DGDebugLoc {
    const llvm::Function *F;
    std::string filename;
    int line;

    bool operator < (const DGDebugLoc &o) const {
      return line < o.line;
    }
  };

  bool getDGDebugLoc(const llvm::Value *v, DGDebugLoc &DL);
  bool getDGDebugLoc(MSSAVar *v, DGDebugLoc &DL);
  std::string getStringMsg(const llvm::Value *v);
  std::string getStringMsg(MSSAVar *v);
  bool getDebugTrace(std::vector<DGDebugLoc> &DLs, std::string &trace,
		     const llvm::Instruction *collective);
  void reorderAndRemoveDup(std::vector<DGDebugLoc> &DLs);

  /* stats */
  double buildGraphTime;
  double phiElimTime;
  double floodDepTime;
  double floodCallTime;
  double dotTime;
};

#endif /* DEPGRAPH_H */
