/*
 * Copyright (C) 2019 Yueqi (Lewis) Chen
 *
 * For licensing details see LICENSE
 */

#ifndef AS_H_
#define AS_H_

#include "GlobalCtx.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include <set>

class AllocAnalyzerPass : public IterativeModulePass {

private:

    void runOnFunction(llvm::Function*);
    bool isCall2Alloc(std::string calleeName);
    llvm::Instruction* forwardUseAnalysis(llvm::Value *V);
    void forwardAnalysis(llvm::Value *V, std::set<llvm::StoreInst *> &StoreInstSet, std::set<llvm::Value *> &TrackSet);
    llvm::Value* getOffset(llvm::GetElementPtrInst *GEP);
    llvm::Value* removeUnaryOp(llvm::Value *I);

    bool expandBinaryOp(llvm::Value *V, std::set<llvm::Value *> &OpendSet);
    void handleGetElement(llvm::Value *V, StoreMap &SM);
    void analyzeAlloc(llvm::CallInst* callInst);
    bool getAllocSite(std::vector<Value *> &argSet, std::set<CallInst *> &retSet);

    bool isPriviledged(llvm::Function *F);

    FuncSet getSyscalls(Function *F);
    FuncSet reachableSyscall(llvm::Function*);

    // hard-coded SLAB/SLUB allocation API
    std::vector<std::string> allocAPIVec = {
    "__kmalloc", "__kmalloc_node", "kmalloc", "kvzalloc",
    "kmalloc_node", "kmalloc_array", "kzalloc", "kmalloc_array_node", "kzalloc_node",
    "kcalloc_node", "kcalloc", "sock_kmalloc",
    };

    // hard-coded privileged syscalls
    std::vector<std::string> rootSyscall = {
    // CAP_SYS_BOOT
    "sys_reboot", "sys_kexec_load",
    // CAP_SYS_ADMIN
    "sys_swapon", "sys_swapoff", "sys_umount", "sys_oldumount", "sys_quotactl", 
    "sys_mount", "sys_pivot_root", "sys_lookup_dcookie", "sys_bdflush", "sys_seccomp",
    // CAP_SYS_MODULE
    "sys_finit_module", "sys_init_module", "sys_delete_module",
    // CAP_DAC_READ_SEARCH
    "sys_open_by_handle_at",
    // CAP_CHOWN
    "sys_fchown", "sys_fchown16", "sys_fchownat", "sys_lchown", "sys_lchown16",
    "sys_chown", "sys_chown16", "sys_fchmodat", "sys_chmod", "sys_fchmod",
    // CAP_SYS_PACCT
    "sys_acct",
    // CAP_SYS_TIME
    "sys_settimeofday", "sys_stime", "sys_adjtimex",
    // CAP_SYS_CHROOT
    "sys_chroot",
    // CAP_SYSLOG
    "sys_syslog"
    };

    typedef llvm::DenseMap<llvm::Function*, FuncSet> ReachableSyscallCache;
    ReachableSyscallCache reachableSyscallCache;

public:

    AllocAnalyzerPass(GlobalContext *Ctx_)
        : IterativeModulePass(Ctx_, "AllocAnalyzer") {}
    virtual bool doInitialization(llvm::Module* );
    virtual bool doFinalization(llvm::Module* );
    virtual bool doModulePass(llvm::Module* );
};

#endif
