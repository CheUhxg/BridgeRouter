/*
 * Copyright (C) 2019 Yueqi (Lewis) Chen
 *
 * For licensing details see LICENSE
 */

#ifndef LC_H_
#define LC_H_

#include "GlobalCtx.h"
#include <set>


class StructCheckerPass : public IterativeModulePass {
private:

    typedef llvm::DenseMap<llvm::Value*, unsigned> SliceMap;
    void runOnFunction(llvm::Function*);
    void backwardSlice(llvm::Value*, unsigned, SliceMap&);
    void backwardSliceCrossFunc(llvm::Function*, unsigned, SliceMap&);
    void forwardSlice(llvm::Value*, unsigned, SliceMap&);
    void analyzeSlice(SliceMap&, StructInfo*, Instruction*);

    void interpretSlice2Checks(SliceMap&, StructInfo::CheckMap&, StructInfo*);

    void interpretICmp(llvm::ICmpInst*);
    void loadHardCodedKeyStruct();

    SmallPtrSet<Value*, 16> getAliasSet(Value* V, Function* F);

    typedef std::vector<llvm::Value*> SrcSlice;
    void findICMPSrc(llvm::Value* V, SrcSlice& srcSlice, SrcSlice& tracedV, unsigned loadDep);
    void analyzeChecker(llvm::ICmpInst* I, SrcSlice& s1, SrcSlice& s2);
    Value* findGEPPointerSrc(Value* V);

    typedef std::pair<llvm::StructType*, unsigned> SrcInfo;
    typedef std::vector<SrcInfo> SrcInfoV;

    unsigned isReachable(
        llvm::BasicBlock* from, 
        llvm::BasicBlock* to); 

    void collectChecks(
        StructInfo::SiteInfo& siteInfo, 
        Instruction* I, 
        Instruction* copySite, 
        Value* V,
        string offset, 
        unsigned loadDep,
        SrcSlice& srcSlice);

    void collectCmpSrc(
        llvm::Value* srcOp, 
        StructInfo::CmpSrc& cmpSrc, 
        SrcSlice& tracedV, 
        unsigned loadDep);

public:
    StructCheckerPass(GlobalContext *Ctx_)
        : IterativeModulePass(Ctx_, "KeyStructChecker") {}

    virtual bool doInitialization(llvm::Module* );
    virtual bool doFinalization(llvm::Module* );
    virtual bool doModulePass(llvm::Module* );

    // debug
    void dumpChecks();
};

#endif
