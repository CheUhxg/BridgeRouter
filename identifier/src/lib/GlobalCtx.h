#ifndef _GLOBAL_H
#define _GLOBAL_H

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Analysis/AliasAnalysis.h>

#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#include "Common.h"
#include "StructAnalyzer.h"
#include "llvm/IR/DerivedTypes.h"

using namespace llvm;
using namespace std;

typedef std::vector< std::pair<llvm::Module*, llvm::StringRef> > ModuleList;
typedef std::unordered_map<llvm::Module*, llvm::StringRef> ModuleMap;
typedef std::unordered_map<std::string, llvm::Function*> FuncMap;
typedef std::unordered_map<std::string, llvm::GlobalVariable*> GObjMap;

/****************** Call Graph **************/
typedef unordered_map<string, llvm::Function*> NameFuncMap;
typedef llvm::SmallPtrSet<llvm::CallInst*, 8> CallInstSet;
typedef llvm::SmallPtrSet<llvm::Function*, 32> FuncSet;
typedef std::unordered_map<std::string, FuncSet> FuncPtrMap;
typedef llvm::DenseMap<llvm::Function*, CallInstSet> CallerMap;
typedef llvm::DenseMap<llvm::CallInst*, FuncSet> CalleeMap;
typedef unordered_map<std::string, FuncSet> HookMap;
/****************** end Call Graph **************/

/****************** Alias **************/
typedef DenseMap<Value *, SmallPtrSet<Value *, 16>> PointerAnalysisMap;
typedef unordered_map<Function *, PointerAnalysisMap> FuncPointerAnalysisMap;
typedef unordered_map<Function *, AAResults *> FuncAAResultsMap;
/****************** end Alias **************/

/****************** Key Object Identification **************/


typedef std::unordered_map<std::string, StructInfo*> KeyStructMap;

typedef llvm::SmallPtrSet<llvm::Instruction*, 32> InstSet;
typedef std::unordered_map<std::string, InstSet> InstMap;
typedef std::unordered_map<std::string, FuncSet> SyscallMap;

typedef llvm::SmallPtrSet<llvm::Module*, 32> ModuleSet;
typedef std::unordered_map<std::string, ModuleSet> StructModuleMap; 

typedef llvm::SmallPtrSet<llvm::StructType*, 32> StructTypeSet;
typedef llvm::DenseMap<llvm::Module*, StructTypeSet> ModuleStructMap;

typedef std::unordered_map<std::string, InstSet> KeyStructList;

typedef std::unordered_map<unsigned, InstSet> StoreMap;

/**************** End Key Object Identification ************/

/****************** Key Object Evaluation **************/

/**************** End Key Object Evaluation ************/

class GlobalContext {
private:
  // pass specific data
  std::map<std::string, void*> PassData;

public:
  bool add(std::string name, void* data) {
    if (PassData.find(name) != PassData.end())
      return false;

    PassData[name] = data;
    return true;
  }

  void* get(std::string name) {
    std::map<std::string, void*>::iterator itr;

    itr = PassData.find(name);
    if (itr != PassData.end())
      return itr->second;
    else
      return nullptr;
  }

  // StructAnalyzer
  StructAnalyzer structAnalyzer;

  // Map global object name to object definition
  GObjMap Gobjs;

  // Map global function name to function defination
  FuncMap Funcs;

  // Map function pointers (IDs) to possible assignments
  FuncPtrMap FuncPtrs;

  // functions whose addresses are taken
  FuncSet AddressTakenFuncs;

  // Map a callsite to all potential callee functions.
  CalleeMap Callees;

  unordered_map<std::string, FuncSet> Name2Func;

  // Map a function to all potential caller instructions.
  CallerMap Callers;

  // Map a function name to a set of hook functions.
  HookMap HookCallees;

  // Indirect call instructions
  std::vector<CallInst *>IndirectCallInsts;

  // Map function signature to functions
  DenseMap<size_t, FuncSet>sigFuncsMap;

  // Map global function name to function.
  NameFuncMap GlobalFuncs;

  // Unified functions -- no redundant inline functions
  DenseMap<size_t, Function *>UnifiedFuncMap;
  set<Function *>UnifiedFuncSet;

  /****** Alias Analysis *******/
  FuncPointerAnalysisMap FuncPAResults;
  FuncAAResultsMap FuncAAResults;

  /****** Key struct **********/
  KeyStructMap keyStructMap;

  /****************** Key Object Identification **************/

  // map structure to allocation site
  InstMap allocInstMap;

  // map structure to copy site
  InstMap copyInstMap;

  // map structure to syscall entry reaching allocation site
  SyscallMap allocSyscallMap;
  
  // map structure to syscall entry reaching copy site
  SyscallMap copySyscallMap;

  // map module to set of used key structure
  ModuleStructMap moduleStructMap;

  // map key structure to module
  StructModuleMap structModuleMap;

  KeyStructList keyStructList;

  // device permission allow function list and deny function list
  FuncSet devAllowList;
  FuncSet devDenyList;

  /**************** End Key Object Identification ************/
  
  /****************** Key Object Evaluation **************/
  // TODO: delete if no evaluation needed
  /**************** End Key Object Evaluation ************/

  // A factory object that knows how to manage AndersNodes
  // AndersNodeFactory nodeFactory;

  ModuleList Modules;

  ModuleMap ModuleMaps;
};

class IterativeModulePass {
protected:
  GlobalContext *Ctx;
  const char *ID;
public:
  IterativeModulePass(GlobalContext *Ctx_, const char *ID_)
    : Ctx(Ctx_), ID(ID_) { }

  // run on each module before iterative pass
  virtual bool doInitialization(llvm::Module *M)
    { return true; }

  // run on each module after iterative pass
  virtual bool doFinalization(llvm::Module *M)
    { return true; }

  // iterative pass
  virtual bool doModulePass(llvm::Module *M)
    { return false; }

  virtual void run(ModuleList &modules);
};

#endif
