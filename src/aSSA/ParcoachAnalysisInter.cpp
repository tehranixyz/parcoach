#include "ParcoachAnalysisInter.h"
#include "../utils/Collectives.h"
#include "Options.h"
#include "Utils.h"

#include "llvm/ADT/SCCIterator.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
//#include <llvm/Analysis/LoopInfo.h>
#include "llvm/Pass.h"

using namespace llvm;
using namespace std;

int ParcoachAnalysisInter::id = 0;

int nbColNI = 0;

void ParcoachAnalysisInter::run() {
  // Parcoach analysis

  /* (1) BFS on each function of the Callgraph in reverse topological order
   *  -> set a function summary with sequence of collectives
   *  -> keep a set of collectives per BB and set the conditionals at NAVS if
   *     it can lead to a deadlock
   */
  errs() << " (1) BFS\n";
  scc_iterator<PTACallGraph *> cgSccIter = scc_begin(&PTACG);
  while (!cgSccIter.isAtEnd()) {
    const vector<PTACallGraphNode *> &nodeVec = *cgSccIter;
    for (PTACallGraphNode *node : nodeVec) {
      Function *F = node->getFunction();
      if (!F || F->isDeclaration() || !PTACG.isReachableFromEntry(F))
        continue;
      // DBG: errs() << "Function: " << F->getName() << "\n";

      if (optMpiTaint)
        MPI_BFS(F);
      else
        BFS(F);
    } // END FOR
    ++cgSccIter;
  }

  /* (2) Check collectives */
  errs() << " (2) CheckCollectives\n";
  cgSccIter = scc_begin(&PTACG);
  while (!cgSccIter.isAtEnd()) {
    const vector<PTACallGraphNode *> &nodeVec = *cgSccIter;
    for (PTACallGraphNode *node : nodeVec) {
      Function *F = node->getFunction();
      if (!F || F->isDeclaration() || !PTACG.isReachableFromEntry(F))
        continue;
      // DBG: //errs() << "Function: " << F->getName() << "\n";
      checkCollectives(F);
      // Get the number of MPI communicators
      if (optMpiTaint) {
        // errs() << " ... Found " << mpiCollperFuncMap[F].size() << " MPI
        // communicators in " << F->getName() << "\n";
        /*for(auto& pair : mpiCollperFuncMap[F]){
                errs() << pair.first << "{" << pair.second << "}\n";
                }*/
      }
    }
    ++cgSccIter;
  }
  /*
          if(nbWarnings !=0){
          cgSccIter = scc_begin(&PTACG);
          while(!cgSccIter.isAtEnd()) {
                  const vector<PTACallGraphNode*> &nodeVec = *cgSccIter;
                  for (PTACallGraphNode *node : nodeVec) {
                          Function *F = node->getFunction();
                          if (!F || F->isDeclaration() ||
     !PTACG.isReachableFromEntry(F))
                                  continue;
                          countCollectivesToInst(F);
                  }
                  ++cgSccIter;
          }
          }
  */
  // If you always want to instrument the code, uncomment the following line
  // if(nbWarnings !=0){
  if (nbWarnings != 0 && !disableInstru) {
    errs() << "\033[0;35m=> Static instrumentation of the code ...\033[0;0m\n";
    for (Function &F : M) {
      instrumentFunction(&F);
    }
  }
  //	errs() << "Number of collectives to instrument = " << nbColNI << "\n";
  errs() << " ... Parcoach analysis done\n";
}

/*
 * FUNCTIONS USED TO CHECK COLLECTIVES
 */

// Get the sequence of collectives in a BB
void ParcoachAnalysisInter::setCollSet(BasicBlock *BB) {
  for (auto i = BB->rbegin(), e = BB->rend(); i != e; ++i) {
    const Instruction *inst = &*i;
    if (const CallInst *CI = dyn_cast<CallInst>(inst)) {

      Function *callee = CI->getCalledFunction();

      //// Indirect calls
      if (callee == NULL) {
        for (const Function *mayCallee : PTACG.indirectCallMap[inst]) {
          if (isIntrinsicDbgFunction(mayCallee))
            continue;
          callee = const_cast<Function *>(mayCallee);
          // Is it a function containing collectives?
          if (!collperFuncMap[callee].empty()) // && collMap[BB]!="NAVS")
            collMap[BB] = collperFuncMap[callee] + " " + collMap[BB];
          // Is it a collective operation?
          if (isCollective(callee)) {
            string OP_name = callee->getName().str();
            if (collMap[BB].empty())
              collMap[BB] = OP_name;
            else
              collMap[BB] = OP_name + " " + collMap[BB];
          }
        }
        //// Direct calls
      } else {
        // Is it a function containing collectives?
        if (!collperFuncMap[callee].empty()) {
          if (collperFuncMap[callee] == "NAVS" || collMap[BB] == "NAVS")
            collMap[BB] = "NAVS";
          else
            collMap[BB] = collperFuncMap[callee] + " " + collMap[BB];
        }
        // Is it a collective operation?
        if (isCollective(callee)) {
          string OP_name = callee->getName().str();
          if (collMap[BB].empty())
            collMap[BB] = OP_name;
          else
            collMap[BB] = OP_name + " " + collMap[BB];
        }
      }
    }
  }
}

// Get the sequence of collectives in a BB, per MPI communicator
void ParcoachAnalysisInter::setMPICollSet(BasicBlock *BB) {

  for (auto i = BB->rbegin(), e = BB->rend(); i != e; ++i) {
    const Instruction *inst = &*i;
    if (const CallInst *CI = dyn_cast<CallInst>(inst)) {
      Function *callee = CI->getCalledFunction();

      //// Indirect calls
      if (callee == NULL) {
        for (const Function *mayCallee : PTACG.indirectCallMap[inst]) {
          if (isIntrinsicDbgFunction(mayCallee))
            continue;
          callee = const_cast<Function *>(mayCallee);
          // Is it a function containing collectives?
          if (!mpiCollperFuncMap[callee]
                   .empty()) { // && mpiCollMap[BB]!="NAVS"){
            auto &BBcollMap = mpiCollMap[BB];
            for (auto &pair : mpiCollperFuncMap[callee]) {
              if (!BBcollMap[pair.first].empty())
                BBcollMap[pair.first] =
                    pair.second + " " + BBcollMap[pair.first];
              else
                BBcollMap[pair.first] = pair.second;
            }
          }
          // Is it a collective operation?
          if (isCollective(callee)) {
            string OP_name = callee->getName().str();
            int OP_color = getCollectiveColor(callee);
            Value *OP_com = CI->getArgOperand(Com_arg_id(OP_color));
            if (!mpiCollMap[BB][OP_com].empty())
              mpiCollMap[BB][OP_com] = OP_name + " " + mpiCollMap[BB][OP_com];
            else
              mpiCollMap[BB][OP_com] = OP_name;
          }
        }
        //// Direct calls
      } else {
        // Is it a function containing collectives?
        if (!mpiCollperFuncMap[callee].empty()) { // && collMap[BB]!="NAVS"){
          auto &BBcollMap = mpiCollMap[BB];
          // errs() << "call to function " << callee->getName() << "\n";
          for (auto &pair : mpiCollperFuncMap[callee]) {
            // errs() << pair.first << "{" << pair.second << "}\n";
            // EMMAAAA
            /*if(pair.second == "NAVS" || BBcollMap[pair.first] == "NAVS"){
                    BBcollMap[pair.first] = "NAVS";
            }else{*/
            if (!BBcollMap[pair.first].empty()) {
              BBcollMap[pair.first] = pair.second + " " + BBcollMap[pair.first];
            } else {
              BBcollMap[pair.first] = pair.second;
            }
            //}
          }
        }
        // Is it a collective operation?
        if (isCollective(callee)) {
          string OP_name = callee->getName().str();
          int OP_color = getCollectiveColor(callee);
          Value *OP_com = CI->getArgOperand(Com_arg_id(OP_color));
          if (!mpiCollMap[BB][OP_com].empty()) {
            mpiCollMap[BB][OP_com] = OP_name + " " + mpiCollMap[BB][OP_com];
          } else {
            mpiCollMap[BB][OP_com] = OP_name;
          }
        }
      }
    }
  }
}

// Tag loop preheader
void ParcoachAnalysisInter::Tag_LoopPreheader(llvm::Loop *L) {
  BasicBlock *Lheader = L->getHeader();
  if (L->getNumBlocks() == 1)
    return; // header=preheader when the loop has only one node, no need for a
            // particular case
  pred_iterator PI = pred_begin(Lheader), E = pred_end(Lheader);
  for (; PI != E; ++PI) {
    BasicBlock *Pred = *PI;
    if (L->contains(Pred))
      bbPreheaderMap[Pred] = true;
  }
}

// Return true if the basic block is an exit node
bool ParcoachAnalysisInter::isExitNode(llvm::BasicBlock *BB) {
  if (isa<ReturnInst>(BB->getTerminator()))
    return true;
  for (auto &I : *BB) {
    Instruction *i = &I;
    CallInst *CI = dyn_cast<CallInst>(i);
    if (!CI)
      continue;
    Function *f = CI->getCalledFunction();
    if (!f)
      continue;
    // if(f->getName().equals("exit")||f->getName().equals("MPI_Abort") ||
    // f->getName().equals("abort")){
    if (f->getName().equals("MPI_Finalize") ||
        f->getName().equals("MPI_Abort") || f->getName().equals("abort")) {
      return true;
    }
  }
  return false;
}

// FOR MPI APPLIS: BFS on loops
void ParcoachAnalysisInter::MPI_BFS_Loop(llvm::Function *F) {

  std::vector<BasicBlock *> Unvisited;
  curLoop = &pass->getAnalysis<LoopInfoWrapperPass>(*const_cast<Function *>(F))
                 .getLoopInfo();

  for (Loop *L : *curLoop) {
    Tag_LoopPreheader(L);
    BasicBlock *Lheader = L->getHeader();
    Unvisited.push_back(Lheader);

    while (Unvisited.size() > 0) {
      BasicBlock *header = *Unvisited.begin();
      // DEBUG//errs() << "Header " << header->getName() << "\n";
      Unvisited.erase(Unvisited.begin());
      if (mustWait(header)) { // all successors have not been seen
        Unvisited.push_back(header);
        header = *Unvisited.begin(); // take the next BB in Unvisited
        // DEBUG//errs() << "New header " << header->getName() << "\n";
        Unvisited.erase(Unvisited.begin());
      }
      pred_iterator PI = pred_begin(header), E = pred_end(header);
      for (; PI != E; ++PI) {
        BasicBlock *Pred = *PI;
        if (!L->contains(Pred)) // ignore BB not in the loop
          continue;
        // BB NOT SEEN BEFORE
        if (bbVisitedMap[Pred] == white) {
          // DEBUG//errs() << F->getName() << " Pred: " << Pred->getName() <<
          // "\n";
          for (auto &pair : mpiCollMap[header])
            mpiCollMap[Pred][pair.first] = mpiCollMap[header][pair.first];
          setMPICollSet(Pred);
          bbVisitedMap[Pred] = grey;
          if (Pred != Lheader)
            Unvisited.push_back(Pred);
          // BB ALREADY SEEN
        } else {
          // DEBUG//errs() << F->getName() << " Pred: " << Pred->getName() << "
          // already seen\n";
          ComCollMap temp(mpiCollMap[Pred]);
          /* DEBUG//
                   errs() << "* Temp = \n";
                   for(auto& pair : temp){
                   errs() << pair.first << "{" << pair.second << "}\n";
                   }
                   errs() << "****\n";*/
          mpiCollMap[Pred].clear(); // reset
          for (auto &pair : mpiCollMap[header])
            mpiCollMap[Pred][pair.first] = mpiCollMap[header][pair.first];
          setMPICollSet(Pred);
          /* DEBUG//
                   errs() << "* New = \n";
                   for(auto& pair : mpiCollMap[Pred]){
                   errs() << pair.first << "{" << pair.second << "}\n";
                   }
                   errs() << "****\n";*/

          // Compare and update
          for (auto &pair : mpiCollMap[Pred]) {
            if (mpiCollMap[Pred][pair.first] != temp[pair.first]) {
              mpiCollMap[Pred][pair.first] = "NAVS";
            }
            temp.erase(pair.first);
          }
          for (auto &pair : temp) { // for remaining communicators
            mpiCollMap[Pred][pair.first] = "NAVS";
          }
          /* DEBUG//
                   errs() << "* Then = \n";
                   for(auto& pair : mpiCollMap[Pred]){
                   errs() << pair.first << "{" << pair.second << "}\n";
                   }
                   errs() << "****\n";*/

        } // END ELSE
        bbVisitedMap[header] = black;
      } // END FOR
    }   // END WHILE
  }     // END FOR
}

// FOR MPI APPLIS: BFS
void ParcoachAnalysisInter::MPI_BFS(llvm::Function *F) {
  std::vector<BasicBlock *> Unvisited;

  // DEBUG//errs() << "** Analyzing function " << F->getName() << "\n";

  // BFS ON EACH LOOP IN F
  // DEBUG//errs() << "-- BFS IN EACH LOOP --\n";
  MPI_BFS_Loop(F);

  // DEBUG//errs() << "-- BFS --\n";

  // GET ALL EXIT NODES
  for (BasicBlock &I : *F) {
    // Set all nodes to white
    bbVisitedMap[&I] = white;
    // Return inst / exit nodes
    /*	if(isExitNode(&I)){
                    Unvisited.push_back(&I);
                    //DEBUG//errs() << "Exit node: " << I.getName() << "\n";
                    setMPICollSet(&I);
                    bbVisitedMap[&I]=grey;
            }*/
    // PB: some functions do not have a return inst..
    if (isa<ReturnInst>(I.getTerminator())) {
      Unvisited.push_back(&I);
      // errs() << "Exit node: " << I.getName() << "\n";
      setMPICollSet(&I);
      bbVisitedMap[&I] = grey;
    }
  }

  // Keep the loop header in black
  /*curLoop = &pass->getAnalysis<LoopInfoWrapperPass>
    (*const_cast<Function *>(F)).getLoopInfo();

  for(Loop *L: *curLoop){
    BasicBlock *Lheader = L->getHeader();
                                  bbVisitedMap[Lheader]=black;
                          }*/

  while (Unvisited.size() > 0) {
    BasicBlock *header = *Unvisited.begin();
    Unvisited.erase(Unvisited.begin());
    // DEBUG//errs() << "Header " << header->getName() << "\n";
    if (mustWait(header) == true) { // all successors have not been seen
      Unvisited.push_back(header);
      header = *Unvisited.begin(); // take the next BB in Unvisited
      // DEBUG//errs() << "New header " << header->getName() << "\n";
      Unvisited.erase(Unvisited.begin());
    }
    pred_iterator PI = pred_begin(header), E = pred_end(header);
    for (; PI != E; ++PI) {
      BasicBlock *Pred = *PI;
      if (bbPreheaderMap[Pred] == true) { // ignore loops preheader
        // DEBUG//errs() << F->getName() << " - BB " << Pred->getName() << " is
        // preheader - ignore\n";
        continue;
      }
      // BB NOT SEEN BEFORE
      if (bbVisitedMap[Pred] == white) {
        // DEBUG//errs() << F->getName() << " Pred: " << Pred->getName() <<
        // "\n";

        // Loop header may have a mpiCollMap -> normalement ca n'arrive pas
        // comme on a mis les header a black
        if (mpiCollMap[Pred].empty()) {
          for (auto &pair : mpiCollMap[header])
            mpiCollMap[Pred][pair.first] = mpiCollMap[header][pair.first];
        } else {
          for (auto &pair : mpiCollMap[header]) {
            if (mpiCollMap[Pred][pair.first].empty()) // copy only when empty
                                                      // sequences (a collective
                                                      // in a loop = NAVS)
              mpiCollMap[Pred][pair.first] = mpiCollMap[header][pair.first];
          }
        }
        /* DEBUG//
                 for(auto& pair : mpiCollMap[Pred]){
                 errs() << pair.first << "{" << pair.second << "}\n";
                 }*/
        setMPICollSet(Pred);
        /* DEBUG//
                 errs() << Pred->getName() << ":\n";
                 for(auto& pair : mpiCollMap[Pred]){
                 errs() << pair.first << "{" << pair.second << "}\n";
                 }*/

        bbVisitedMap[Pred] = grey;
        // if(header != Pred) // to handle loops of size 1
        Unvisited.push_back(Pred);
        // BB ALREADY SEEN
      } else {
        // DEBUG//errs() << F->getName() << " Pred: " << Pred->getName() << "
        // already seen\n";
        // Loop header may have a mpiCollMap  --> Emma test
        /*			if(bbVisitedMap[Pred] == black){

                                        //errs() << F->getName() << " Pred: " <<
Pred->getName() << " is a loop header\n";
                                        errs() << " = \n";
for(auto& pair : mpiCollMap[Pred]){
errs() << pair.first << "{" << pair.second << "}\n";
}
errs() << "****\n";
                                        //

if(mpiCollMap[Pred].empty()){
for(auto& pair : mpiCollMap[header])
mpiCollMap[Pred][pair.first] = mpiCollMap[header][pair.first];
}else{
for(auto& pair : mpiCollMap[header]){
if(mpiCollMap[Pred][pair.first].empty()) // copy only when empty sequences (a
collective in a loop = NAVS)
mpiCollMap[Pred][pair.first] = mpiCollMap[header][pair.first];
}
                                        }
                                        //Unvisited.push_back(Pred); // we want
to get its predecessor
}
                                else{ */
        ComCollMap temp(mpiCollMap[Pred]);
        /* DEBUG
                 errs() << "* Temp = \n";
                 for(auto& pair : temp){
                 errs() << pair.first << "{" << pair.second << "}\n";
                 }
                 errs() << "****\n"; */
        mpiCollMap[Pred].clear(); // reset
        for (auto &pair : mpiCollMap[header])
          mpiCollMap[Pred][pair.first] = mpiCollMap[header][pair.first];
        setMPICollSet(Pred);
        /* DEBUG
                 errs() << "* New = \n";
                 for(auto& pair : mpiCollMap[Pred]){
                 errs() << pair.first << "{" << pair.second << "}\n";
                 }
                 errs() << "****\n"; */

        // Compare and update
        for (auto &pair : mpiCollMap[Pred]) {
          if (mpiCollMap[Pred][pair.first] != temp[pair.first]) {
            mpiCollMap[Pred][pair.first] = "NAVS";
          }
          temp.erase(pair.first);
        }
        for (auto &pair : temp) { // for remaining communicators
          mpiCollMap[Pred][pair.first] = "NAVS";
        }
        /* DEBUG
                 errs() << "* Then = \n";
                 for(auto& pair : mpiCollMap[Pred]){
                 errs() << pair.first << "{" << pair.second << "}\n";
                 }
                 errs() << "****\n"; */
        ////}

      } // END ELSE
      bbVisitedMap[header] = black;
    } // END FOR
  }   // END WHILE

  BasicBlock &entry = F->getEntryBlock();
  for (auto &pair : mpiCollMap[&entry]) {
    mpiCollperFuncMap[F][pair.first] = mpiCollMap[&entry][pair.first];
  }
  /*if(F->getName() == "main"){
          errs() << F->getName() << " summary = \n";
          for(auto& pair : mpiCollperFuncMap[F]){
                  errs() << pair.first << "{" << pair.second << "}\n";
          }
  }*/
}

// FOR BFS: Returns true if all successors are not black
bool ParcoachAnalysisInter::mustWait(llvm::BasicBlock *bb) {
  succ_iterator SI = succ_begin(bb), E = succ_end(bb);
  for (; SI != E; ++SI) {
    BasicBlock *Succ = *SI;
    if (bbVisitedMap[Succ] != black)
      return true;
  }
  return false;
}

// BFS ON EACH LOOP
void ParcoachAnalysisInter::BFS_Loop(llvm::Function *F) {
  std::vector<BasicBlock *> Unvisited;

  curLoop = &pass->getAnalysis<LoopInfoWrapperPass>(*const_cast<Function *>(F))
                 .getLoopInfo();

  for (Loop *L : *curLoop) {
    Tag_LoopPreheader(L);
    BasicBlock *Lheader = L->getHeader();
    Unvisited.push_back(Lheader);

    while (Unvisited.size() > 0) {
      BasicBlock *header = *Unvisited.begin();
      // DEBUG//errs() << "Header " << header->getName() << "\n";
      Unvisited.erase(Unvisited.begin());
      if (mustWait(header)) { // all successors have not been seen
        Unvisited.push_back(header);
        header = *Unvisited.begin(); // take the next BB in Unvisited
        // DEBUG//errs() << "New header " << header->getName() << "\n";
        Unvisited.erase(Unvisited.begin());
      }

      pred_iterator PI = pred_begin(header), E = pred_end(header);
      for (; PI != E; ++PI) {
        BasicBlock *Pred = *PI;
        if (!L->contains(Pred)) // ignore BB not in the loop
          continue;

        // BB NOT SEEN BEFORE
        if (bbVisitedMap[Pred] == white) {
          // DEBUG//errs() << F->getName() << " Pred " << Pred->getName() <<
          // "\n";
          collMap[Pred] = collMap[header];
          setCollSet(Pred);
          // DEBUG//errs() << "  -> has " << collMap[Pred] << "\n";
          bbVisitedMap[Pred] = grey;
          if (Pred != Lheader)
            Unvisited.push_back(Pred);
          // BB ALREADY SEEN
        } else {
          // DEBUG//errs() << F->getName() << " - BB " << Pred->getName() << "
          // already seen\n";
          string temp = collMap[Pred];
          collMap[Pred] = collMap[header];
          setCollSet(Pred);
          // DEBUG//errs() << collMap[Pred] << " = " << temp << "?\n";
          if (temp != collMap[Pred]) {
            collMap[Pred] = "NAVS";
            // DEBUG//errs() << "  -> BB has " << collMap[Pred] << "\n";
          }
        }
        bbVisitedMap[header] = black;
      } // END FOR
    }   // END WHILE
  }     // END FOR
}

static std::string getSimpleNodeLabel(const BasicBlock *Node) {
  if (!Node->getName().empty())
    return Node->getName().str();

  std::string Str;
  raw_string_ostream OS(Str);

  Node->printAsOperand(OS, false);
  return OS.str();
}

// BFS
void ParcoachAnalysisInter::BFS(llvm::Function *F) {
  std::vector<BasicBlock *> Unvisited;

  // BFS ON EACH LOOP IN F
  // DEBUG//errs() << "-- BFS IN EACH LOOP --\n";
  BFS_Loop(F);

  // DEBUG//errs() << "-- BFS --\n";
  // GET ALL EXIT NODES
  for (BasicBlock &I : *F) {
    bbVisitedMap[&I] = white;
    if (isa<ReturnInst>(I.getTerminator())) {
      Unvisited.push_back(&I);
      setCollSet(&I);
      bbVisitedMap[&I] = grey;
    }
  }

  // Keep the loop header in black
  curLoop = &pass->getAnalysis<LoopInfoWrapperPass>(*const_cast<Function *>(F))
                 .getLoopInfo();

  for (Loop *L : *curLoop) {
    BasicBlock *Lheader = L->getHeader();
    bbVisitedMap[Lheader] = black;
  }

  while (Unvisited.size() > 0) {
    BasicBlock *header = *Unvisited.begin();
    Unvisited.erase(Unvisited.begin());
    if (mustWait(header)) { // all successors have not been seen
      Unvisited.push_back(header);
      header = *Unvisited.begin(); // take the next BB in Unvisited
      Unvisited.erase(Unvisited.begin());
    }

    pred_iterator PI = pred_begin(header), E = pred_end(header);
    for (; PI != E; ++PI) {
      BasicBlock *Pred = *PI;
      if (bbPreheaderMap[Pred] == true) { // ignore loops preheader
        // DEBUG//errs() << F->getName() << " - BB " << Pred->getName() << " is
        // preheader\n";
        continue;
      }
      // BB NOT SEEN BEFORE
      if (bbVisitedMap[Pred] == white) {
        // DEBUG//errs() << F->getName() << " Pred " << Pred->getName() << "\n";

        // Loop header may have a collMap
        if (collMap[Pred].empty())
          collMap[Pred] = collMap[header];
        else { // if not empty, it should be a loop header with NAVS
          collMap[Pred] = "NAVS";
        }
        setCollSet(Pred);
        bbVisitedMap[Pred] = grey;
        if (header != Pred) // to handle loops of size 1
          Unvisited.push_back(Pred);
        // BB ALREADY SEEN
      } else {
        // DEBUG//errs() << F->getName() << " - BB " << Pred->getName() << "
        // already seen\n";
        if (bbVisitedMap[Pred] == black) {
          if (mpiCollMap[Pred].empty()) {
            for (auto &pair : mpiCollMap[header])
              mpiCollMap[Pred][pair.first] = mpiCollMap[header][pair.first];
          } else {
            for (auto &pair : mpiCollMap[header]) {
              if (mpiCollMap[Pred][pair.first].empty()) // copy only when empty
                                                        // sequences (a
                                                        // collective in a loop
                                                        // = NAVS)
                mpiCollMap[Pred][pair.first] = mpiCollMap[header][pair.first];
            }
          }
          // Unvisited.push_back(Pred);
        } else {
          string temp = collMap[Pred];
          collMap[Pred] = collMap[header];
          setCollSet(Pred);
          // DEBUG//errs() << collMap[Pred] << " = " << temp << "?\n";
          if (temp != collMap[Pred])
            collMap[Pred] = "NAVS";
          // DEBUG//errs() << "  -> BB has " << collMap[Pred] << "\n";
        }
      }
      bbVisitedMap[header] = black;
    }
  } // END WHILE
  BasicBlock &entry = F->getEntryBlock();
  collperFuncMap[F] = collMap[&entry];
  // if(F->getName() == "main")
  // DEBUG//errs() << F->getName() << " summary = " << collperFuncMap[F] <<
  // "\n";
}

// COUNT COLLECTIVES TO INST AND INSTRUMENT THEM
// Not correct..
void ParcoachAnalysisInter::countCollectivesToInst(llvm::Function *F) {
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    bool toinstrument = false;
    Instruction *i = &*I;
    CallInst *CI = dyn_cast<CallInst>(i);
    if (!CI)
      continue;
    Function *f = CI->getCalledFunction();
    if (!f)
      continue;
    string OP_name = f->getName().str();
    string Warning = getWarning(*i);
    DebugLoc locC = i->getDebugLoc();
    int OP_line = 0;
    string File = "";
    if (locC) {
      OP_line = locC.getLine();
      File = locC->getFilename();
    }
    // Is it a collective call?
    if (!isCollective(f)) {
      // check if MPI_Finalize or abort for instrumentation
      if (f->getName().equals("MPI_Finalize") ||
          f->getName().equals("MPI_Abort") || f->getName().equals("abort")) {
        errs() << "-> insert a check before " << OP_name << " line " << OP_line
               << "\n";
        insertCC(i, v_coll.size() + 1, OP_name, OP_line, Warning, File);
        // nbCC++;
      }
      continue;
    }
    // Get collective infos
    int OP_color = getCollectiveColor(f);
    Value *OP_com = CI->getArgOperand(Com_arg_id(OP_color));
    // Get conditionals from the callsite
    set<const BasicBlock *> callIPDF;
    DG->getCallInterIPDF(CI, callIPDF);

    for (const BasicBlock *BB : callIPDF) {
      // Is this node detected as potentially dangerous by parcoach?
      if (!optMpiTaint && collMap[BB] != "NAVS")
        continue;
      if (optMpiTaint && mpiCollMap[BB][OP_com] != "NAVS")
        continue;

      // Is this condition tainted?
      const Value *cond = getBasicBlockCond(BB);
      if (!cond || (!optNoDataFlow && !DG->isTaintedValue(cond))) {
        const Instruction *instE = BB->getTerminator();
        DebugLoc locE = instE->getDebugLoc();
        continue;
      }
      toinstrument = true;
    } // END FOR

    if (toinstrument) {
      nbColNI++; // collective to instrument
      // Instrument
      errs() << "-> insert a check before " << OP_name << " line " << OP_line
             << "\n";
      insertCountColl(i, OP_name, OP_line, File, 1);
      insertCC(i, OP_color, OP_name, OP_line, Warning, File);
      nbCC++;
      // If the coll is in a function f, all conds not in f has NAVS to
      // instrument them
      for (const BasicBlock *BB : callIPDF) {
        // if BB not in the same function, set it as NAVS
        const llvm::Function *fBB = BB->getParent();
        if (F != fBB)
          mpiCollMap[BB][OP_com] = "NAVS";
      }
    } else {
      // insert count_collectives(const char* OP_name, int OP_line, char
      // *FILE_name,int inst)
      insertCountColl(i, OP_name, OP_line, File, 0);
      // insertCC(i,OP_color, OP_name, OP_line, Warning, File);
    }

  } // END FOR
}

// CHECK COLLECTIVES FUNCTION
void ParcoachAnalysisInter::checkCollectives(llvm::Function *F) {
  StringRef FuncSummary;
  MDNode *mdNode;

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    Instruction *i = &*I;
    // Debug info (line in the source code, file)
    DebugLoc DLoc = i->getDebugLoc();
    StringRef File = "";
    unsigned OP_line = 0;
    if (DLoc) {
      OP_line = DLoc.getLine();
      File = DLoc->getFilename();
    }
    // Warning info
    string WarningMsg;
    const char *ProgName = "PARCOACH";
    SMDiagnostic Diag;
    std::string COND_lines;

    CallInst *CI = dyn_cast<CallInst>(i);
    if (!CI)
      continue;

    Function *f = CI->getCalledFunction();
    if (!f)
      continue;

    string OP_name = f->getName().str();

    // Is it a collective call?
    if (!isCollective(f)) {
      continue;
    }
    int OP_color = getCollectiveColor(f);
    Value *OP_com = nullptr;
    if (optMpiTaint)
      OP_com = CI->getArgOperand(Com_arg_id(OP_color)); // 0 for Barrier only

    // CI->getArgOperand(0)->dump();
    // errs() << "Found " << OP_name << " on " << OP_com << " line " << OP_line
    // << " in " << File << "\n";

    nbCollectivesFound++;
    bool isColWarning = false;
    bool isColWarningParcoach = false;

    // Get conditionals from the callsite
    set<const BasicBlock *> callIPDF;
    DG->getCallInterIPDF(CI, callIPDF);
    // For the summary-based approach, use the following instead of the previous
    // line
    // DG->getCallIntraIPDF(CI, callIPDF);

    if (!callIPDF.empty())
      nbCollectivesCondCalled++;

    for (const BasicBlock *BB : callIPDF) {

      // Is this node detected as potentially dangerous by parcoach?
      if (!optMpiTaint && collMap[BB] != "NAVS")
        continue;
      if (optMpiTaint && mpiCollMap[BB][OP_com] != "NAVS")
        continue;
      /*const Value *cond = getBasicBlockCond(BB);
      errs() << "Cond : " << cond->getName() << "\n";
            for(auto& pair : mpiCollMap[BB]){
errs() << pair.first << "{" << pair.second << "}\n";}*/

      isColWarningParcoach = true;
      nbCondsParcoachOnly++;
      conditionSetParcoachOnly.insert(BB);

      // Is this condition tainted?
      const Value *cond = getBasicBlockCond(BB);

      /*errs() << "Cond with NAVS: " << cond->getName() << "\n";

for(auto& pair : mpiCollMap[BB]){
errs() << pair.first << "{" << pair.second << "}\n";
}
      */

      if (!cond || (!optNoDataFlow && !DG->isTaintedValue(cond))) {
        const Instruction *instE = BB->getTerminator();
        DebugLoc locE = instE->getDebugLoc();
        // errs() << "-> not tainted\n";
        continue;
      }

      isColWarning = true;
      nbConds++;
      conditionSet.insert(BB);

      DebugLoc BDLoc = (BB->getTerminator())->getDebugLoc();
      const Instruction *inst = BB->getTerminator();
      DebugLoc loc = inst->getDebugLoc();
      if (loc) {
        COND_lines.append(" ").append(to_string(loc.getLine()));
        COND_lines.append(" (").append(loc->getFilename()).append(")");
      } else {
        COND_lines.append(" ").append("?");
        COND_lines.append(" (").append("?").append(")");
      }

      if (optDotTaintPaths) {
        string dotfilename("taintedpath-");
        string cfilename = loc->getFilename();
        size_t lastpos_slash = cfilename.find_last_of('/');
        if (lastpos_slash != cfilename.npos)
          cfilename = cfilename.substr(lastpos_slash + 1, cfilename.size());
        dotfilename.append(cfilename).append("-");
        dotfilename.append(to_string(loc.getLine())).append(".dot");
        DG->dotTaintPath(cond, dotfilename, i);
      }
    } // END FOR

    // Is there at least one node from the IPDF+ detected as potentially
    // dangerous by parcoach
    if (isColWarningParcoach) {
      nbWarningsParcoachOnly++;
      warningSetParcoachOnly.insert(CI);
    }

    // Is there at least one node from the IPDF+ tainted
    if (!isColWarning)
      continue;

    nbWarnings++;
    warningSet.insert(CI);
    WarningMsg =
        OP_name + " line " + to_string(OP_line) +
        " possibly not called by all processes because of conditional(s) "
        "line(s) " +
        COND_lines + " (full-inter)";
    mdNode = MDNode::get(i->getContext(),
                         MDString::get(i->getContext(), WarningMsg));
    i->setMetadata("inter.inst.warning" + to_string(id), mdNode);
    Diag = SMDiagnostic(File, SourceMgr::DK_Warning, WarningMsg);
    Diag.print(ProgName, errs(), 1, 1);
  }
}

/*
 * INSTRUMENTATION
 */

void ParcoachAnalysisInter::instrumentFunction(llvm::Function *F) {
  for (Function::iterator bb = F->begin(), e = F->end(); bb != e; ++bb) {
    for (BasicBlock::iterator i = bb->begin(), e = bb->end(); i != e; ++i) {
      Instruction *Inst = &*i;
      string Warning = getWarning(*Inst);
      // Debug info (line in the source code, file)
      DebugLoc DLoc = i->getDebugLoc();
      string File = "o";
      int OP_line = -1;
      if (DLoc) {
        OP_line = DLoc.getLine();
        File = DLoc->getFilename();
      }
      // call instruction
      if (CallInst *CI = dyn_cast<CallInst>(i)) {
        Function *callee = CI->getCalledFunction();
        if (callee == NULL)
          continue;
        string OP_name = callee->getName().str();
        int OP_color = getCollectiveColor(callee);

        // Before finalize or exit/abort
        if (callee->getName().equals("MPI_Finalize") ||
            callee->getName().equals("MPI_Abort") ||
            callee->getName().equals("abort")) {
          errs() << "-> insert check before " << OP_name << " line " << OP_line
                 << "\n";
          insertCC(Inst, v_coll.size() + 1, OP_name, OP_line, Warning, File);
          // nbCC++;
          continue;
        }
        // Before a collective
        if (OP_color >= 0) {
          errs() << "-> insert check before " << OP_name << " line " << OP_line
                 << "\n";
          insertCC(Inst, OP_color, OP_name, OP_line, Warning, File);
          // nbCC++;
        }
      } // END IF
    }   // END FOR
  }     // END FOR
}

void ParcoachAnalysisInter::insertCountColl(llvm::Instruction *I,
                                            std::string OP_name, int OP_line,
                                            llvm::StringRef File, int inst) {

  IRBuilder<> builder(I);
  // Arguments of the new function
  vector<const Type *> params = vector<const Type *>();
  params.push_back(PointerType::getInt8PtrTy(M.getContext())); // OP_name
  Value *strPtr_NAME = builder.CreateGlobalStringPtr(OP_name);
  params.push_back(Type::getInt32Ty(M.getContext()));          // OP_line
  params.push_back(PointerType::getInt8PtrTy(M.getContext())); // FILE_name
  const std::string Filename = File.str();
  Value *strPtr_FILENAME = builder.CreateGlobalStringPtr(Filename);
  params.push_back(Type::getInt32Ty(M.getContext())); // inst
  // Set new function name, type and arguments
  FunctionType *FTy = FunctionType::get(
      Type::getVoidTy(M.getContext()),
      ArrayRef<Type *>((Type **)params.data(), params.size()), false);
  Value *CallArgs[] = {
      strPtr_NAME, ConstantInt::get(Type::getInt32Ty(M.getContext()), OP_line),
      strPtr_FILENAME,
      ConstantInt::get(Type::getInt32Ty(M.getContext()), inst)};
  std::string FunctionName = "count_collectives";
  Value *CCFunction = M.getOrInsertFunction(FunctionName, FTy);
  // Create new function
  CallInst::Create(CCFunction, ArrayRef<Value *>(CallArgs), "", I);
}

// Check Collective function before a collective
// + Check Collective function before return statements
// --> check_collective_MPI(int OP_color, const char* OP_name, int OP_line,
// char* OP_warnings, char *FILE_name)
// --> void check_collective_UPC(int OP_color, const char* OP_name,
// int OP_line, char* warnings, char *FILE_name)
void ParcoachAnalysisInter::insertCC(llvm::Instruction *I, int OP_color,
                                     std::string OP_name, int OP_line,
                                     llvm::StringRef WarningMsg,
                                     llvm::StringRef File) {

  IRBuilder<> builder(I);
  // Arguments of the new function
  vector<const Type *> params = vector<const Type *>();
  params.push_back(Type::getInt32Ty(M.getContext()));          // OP_color
  params.push_back(PointerType::getInt8PtrTy(M.getContext())); // OP_name
  Value *strPtr_NAME = builder.CreateGlobalStringPtr(OP_name);
  params.push_back(Type::getInt32Ty(M.getContext()));          // OP_line
  params.push_back(PointerType::getInt8PtrTy(M.getContext())); // OP_warnings
  const std::string Warnings = WarningMsg.str();
  Value *strPtr_WARNINGS = builder.CreateGlobalStringPtr(Warnings);
  params.push_back(PointerType::getInt8PtrTy(M.getContext())); // FILE_name
  const std::string Filename = File.str();
  Value *strPtr_FILENAME = builder.CreateGlobalStringPtr(Filename);
  // Set new function name, type and arguments
  FunctionType *FTy = FunctionType::get(
      Type::getVoidTy(M.getContext()),
      ArrayRef<Type *>((Type **)params.data(), params.size()), false);
  Value *CallArgs[] = {
      ConstantInt::get(Type::getInt32Ty(M.getContext()), OP_color), strPtr_NAME,
      ConstantInt::get(Type::getInt32Ty(M.getContext()), OP_line),
      strPtr_WARNINGS, strPtr_FILENAME};
  std::string FunctionName;

  if (isMpiCollective(OP_color)) {
    FunctionName = "check_collective_MPI";
  } else if (isOmpCollective(OP_color)) {
    FunctionName = "check_collective_OMP";
  } else if (isUpcCollective(OP_color)) {
    FunctionName = "check_collective_UPC";
  } else {
    FunctionName = "check_collective_return";
  }

  Value *CCFunction = M.getOrInsertFunction(FunctionName, FTy);
  // Create new function
  CallInst::Create(CCFunction, ArrayRef<Value *>(CallArgs), "", I);
  errs() << "=> Insertion of " << FunctionName << " (" << OP_color << ", "
         << OP_name << ", " << OP_line << ", " << WarningMsg << ", " << File
         << ")\n";
}

std::string ParcoachAnalysisInter::getWarning(llvm::Instruction &inst) {
  string warning = " ";
  if (MDNode *node = inst.getMetadata("inter.inst.warning" + to_string(id))) {
    if (Metadata *value = node->getOperand(0)) {
      MDString *mdstring = cast<MDString>(value);
      warning = mdstring->getString();
    }
  } else {
    // errs() << "Did not find metadata\n";
  }
  return warning;
}
