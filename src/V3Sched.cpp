// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Code scheduling
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2022 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//
// V3Sched::schedule is the top level entry-point to the scheduling algorithm
// at a high level, the process is:
//
//  - Gather and classify all logic in the design based on what triggers its execution
//  - Schedule static, initial and final logic classes in source order
//  - Break combinational cycles by introducing hybrid logic
//  - Create 'settle' region that restores the combinational invariant
//  - Partition the clocked and combinational (including hybrid) logic into pre/act/nba.
//    All clocks (signals referenced in an AstSenTree) generated via a blocking assignment
//    (including combinationally generated signals) are computed within the act region.
//  - Replicate combinational logic
//  - Create input combinational logic loop
//  - Create the pre/act/nba triggers
//  - Create the 'act' region evaluation function
//  - Create the 'nba' region evaluation function
//  - Bolt it all together to create the '_eval' function
//
// Details of the algorithm are described in the internals documentation docs/internals.rst
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3EmitCBase.h"
#include "V3EmitV.h"
#include "V3Order.h"
#include "V3Sched.h"
#include "V3Stats.h"
#include "V3UniqueNames.h"

#include <unordered_map>
#include <unordered_set>

namespace V3Sched {

namespace {

//============================================================================
// Utility functions

AstCFunc* makeSubFunction(AstNetlist* netlistp, const string& name, bool slow) {
    AstScope* const scopeTopp = netlistp->topScopep()->scopep();
    AstCFunc* const funcp = new AstCFunc{netlistp->fileline(), name, scopeTopp, ""};
    funcp->dontCombine(true);
    funcp->isStatic(false);
    funcp->isLoose(true);
    funcp->slow(slow);
    funcp->isConst(false);
    funcp->declPrivate(true);
    scopeTopp->addActivep(funcp);
    return funcp;
}

AstCFunc* makeTopFunction(AstNetlist* netlistp, const string& name, bool slow) {
    AstCFunc* const funcp = makeSubFunction(netlistp, name, slow);
    funcp->entryPoint(true);
    return funcp;
}

std::vector<const AstSenTree*> getSenTreesUsedBy(std::vector<const LogicByScope*> lbsps) {
    const VNUser1InUse user1InUse;
    std::vector<const AstSenTree*> result;
    for (const LogicByScope* const lbsp : lbsps) {
        for (const auto& pair : *lbsp) {
            AstActive* const activep = pair.second;
            AstSenTree* const senTreep = activep->sensesp();
            if (senTreep->user1SetOnce()) continue;
            if (senTreep->hasClocked() || senTreep->hasHybrid()) result.push_back(senTreep);
        }
    }
    return result;
}

AstAssign* setVar(AstVarScope* vscp, uint32_t val) {
    FileLine* const flp = vscp->fileline();
    AstVarRef* const refp = new AstVarRef{flp, vscp, VAccess::WRITE};
    AstConst* const zerop = new AstConst{flp, AstConst::DtypedValue{}, vscp->dtypep(), val};
    return new AstAssign{flp, refp, zerop};
};

void remapSensitivities(LogicByScope& lbs,
                        std::unordered_map<const AstSenTree*, AstSenTree*> senTreeMap) {
    for (const auto& pair : lbs) {
        AstActive* const activep = pair.second;
        AstSenTree* const senTreep = activep->sensesp();
        if (senTreep->hasCombo()) continue;
        activep->sensesp(senTreeMap.at(senTreep));
    }
}

void invertAndMergeSenTreeMap(std::unordered_map<const AstSenItem*, const AstSenTree*>& result,
                              std::unordered_map<const AstSenTree*, AstSenTree*> senTreeMap) {
    for (const auto& pair : senTreeMap) {
        UASSERT_OBJ(!pair.second->sensesp()->nextp(), pair.second, "Should be single AstSenIem");
        result.emplace(pair.second->sensesp(), pair.first);
    }
}

//============================================================================
// Split large function according to --output-split-cfuncs

void splitCheck(AstCFunc* ofuncp) {
    if (!v3Global.opt.outputSplitCFuncs() || !ofuncp->stmtsp()) return;
    if (ofuncp->nodeCount() < v3Global.opt.outputSplitCFuncs()) return;

    int funcnum = 0;
    int func_stmts = 0;
    AstCFunc* funcp = nullptr;

    // Unlink all statements, then add item by item to new sub-functions
    AstBegin* const tempp = new AstBegin{ofuncp->fileline(), "[EditWrapper]",
                                         ofuncp->stmtsp()->unlinkFrBackWithNext()};
    // Currently we do not use finalsp in V3Sched, if we do, it needs to be handled here
    UASSERT_OBJ(!ofuncp->finalsp(), ofuncp, "Should not have any finalps");
    while (tempp->stmtsp()) {
        AstNode* const itemp = tempp->stmtsp()->unlinkFrBack();
        const int stmts = itemp->nodeCount();
        if (!funcp || (func_stmts + stmts) > v3Global.opt.outputSplitCFuncs()) {
            // Make a new function
            funcp = new AstCFunc{ofuncp->fileline(), ofuncp->name() + "__" + cvtToStr(funcnum++),
                                 ofuncp->scopep()};
            funcp->dontCombine(true);
            funcp->isStatic(false);
            funcp->isLoose(true);
            funcp->slow(ofuncp->slow());
            ofuncp->scopep()->addActivep(funcp);
            //
            AstCCall* const callp = new AstCCall{funcp->fileline(), funcp};
            ofuncp->addStmtsp(callp);
            func_stmts = 0;
        }
        funcp->addStmtsp(itemp);
        func_stmts += stmts;
    }
    VL_DO_DANGLING(tempp->deleteTree(), tempp);
}

//============================================================================
// Collect and classify all logic in the design

LogicClasses gatherLogicClasses(AstNetlist* netlistp) {
    LogicClasses result;

    netlistp->foreach<AstScope>([&](AstScope* scopep) {
        std::vector<AstActive*> empty;

        scopep->foreach<AstActive>([&](AstActive* activep) {
            AstSenTree* const senTreep = activep->sensesp();
            if (!activep->stmtsp()) {
                // Some AstActives might be empty due to previous optimizations
                empty.push_back(activep);
            } else if (senTreep->hasStatic()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "static initializer with additional sensitivities");
                result.m_static.emplace_back(scopep, activep);
            } else if (senTreep->hasInitial()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "'initial' logic with additional sensitivities");
                result.m_initial.emplace_back(scopep, activep);
            } else if (senTreep->hasFinal()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "'final' logic with additional sensitivities");
                result.m_final.emplace_back(scopep, activep);
            } else if (senTreep->hasCombo()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "combinational logic with additional sensitivities");
                result.m_comb.emplace_back(scopep, activep);
            } else {
                UASSERT_OBJ(senTreep->hasClocked(), activep, "What else could it be?");
                result.m_clocked.emplace_back(scopep, activep);
            }
        });

        for (AstActive* const activep : empty) activep->unlinkFrBack()->deleteTree();
    });

    return result;
}

//============================================================================
// Simple ordering in source order

void orderSequentially(AstCFunc* funcp, const LogicByScope& lbs) {
    const VNUser1InUse user1InUse;  // AstScope -> AstCFunc: the sub-function for the scope
    for (const auto& pair : lbs) {
        AstScope* const scopep = pair.first;
        AstActive* const activep = pair.second;
        if (!scopep->user1p()) {
            // Create a sub-function per scope so we can V3Combine them later
            const string subName{funcp->name() + "__" + scopep->nameDotless()};
            AstCFunc* const subFuncp = new AstCFunc{scopep->fileline(), subName, scopep};
            subFuncp->isLoose(true);
            subFuncp->isConst(false);
            subFuncp->declPrivate(true);
            subFuncp->slow(funcp->slow());
            scopep->addActivep(subFuncp);
            scopep->user1p(subFuncp);
            // Call it from the top function
            funcp->addStmtsp(new AstCCall{scopep->fileline(), subFuncp});
        }
        AstCFunc* const subFuncp = VN_AS(scopep->user1p(), CFunc);
        // Add statements to sub-function
        for (AstNode *logicp = activep->stmtsp(), *nextp; logicp; logicp = nextp) {
            nextp = logicp->nextp();
            if (AstNodeProcedure* const procp = VN_CAST(logicp, NodeProcedure)) {
                if (AstNode* const bodyp = procp->bodysp()) {
                    bodyp->unlinkFrBackWithNext();
                    subFuncp->addStmtsp(bodyp);
                }
            } else {
                logicp->unlinkFrBack();
                subFuncp->addStmtsp(logicp);
            }
        }
        if (activep->backp()) activep->unlinkFrBack();
        VL_DO_DANGLING(activep->deleteTree(), activep);
    }
}

//============================================================================
// Create simply ordered functions

void createStatic(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval_static", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_static);
    splitCheck(funcp);
}

AstCFunc* createInitial(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval_initial", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_initial);
    return funcp;  // Not splitting yet as it is not final
}

void createFinal(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval_final", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_final);
    splitCheck(funcp);
}

//============================================================================
// SenExprBuilder constructs the expressions used to compute if an
// AstSenTree have triggered

class SenExprBuilder final {
    // STATE
    AstCFunc* const m_initp;  // The initialization function
    AstScope* const m_scopeTopp;  // Top level scope

    std::vector<AstNodeStmt*> m_updates;  // Update assignments

    std::unordered_map<VNRef<AstNode>, AstVarScope*> m_prev;  // The 'previous value' signals
    std::unordered_set<VNRef<AstNode>> m_hasUpdate;  // Whether the given sen expression already
                                                     // has an update statement in m_updates

    V3UniqueNames m_uniqueNames{"__Vtrigprev__expression"};  // For generating unique signal names

    // METHODS
    AstVarScope* getPrev(AstNode* currp) {
        FileLine* const flp = currp->fileline();
        const auto rdCurr = [=]() { return currp->cloneTree(false); };

        // Create the 'previous value' variable
        auto it = m_prev.find(*currp);
        if (it == m_prev.end()) {
            // For readability, use the scoped signal name if the trigger is a simple AstVarRef
            string name;
            if (AstVarRef* const refp = VN_CAST(currp, VarRef)) {
                AstVarScope* vscp = refp->varScopep();
                name = "__Vtrigrprev__" + vscp->scopep()->nameDotless() + "__"
                       + vscp->varp()->name();
            } else {
                name = m_uniqueNames.get(currp);
            }

            AstVarScope* const prevp = m_scopeTopp->createTemp(name, currp->dtypep());
            it = m_prev.emplace(*currp, prevp).first;

            // Add the initializer init
            AstNode* const initp = rdCurr();
            m_initp->addStmtsp(
                new AstAssign{flp, new AstVarRef{flp, prevp, VAccess::WRITE}, initp});
        }

        AstVarScope* const prevp = it->second;

        // Add update if it does not exist yet in this round
        if (m_hasUpdate.emplace(*currp).second) {
            m_updates.push_back(
                new AstAssign{flp, new AstVarRef{flp, prevp, VAccess::WRITE}, rdCurr()});
        }

        return prevp;
    }

    std::pair<AstNode*, bool> createTerm(AstSenItem* senItemp) {
        FileLine* const flp = senItemp->fileline();
        AstNode* const senp = senItemp->sensp();

        const auto currp = [=]() { return senp->cloneTree(false); };
        const auto prevp = [=]() { return new AstVarRef{flp, getPrev(senp), VAccess::READ}; };
        const auto lsb = [=](AstNodeMath* opp) { return new AstSel{flp, opp, 0, 1}; };

        // All event signals should be 1-bit at this point
        switch (senItemp->edgeType()) {
        case VEdgeType::ET_ILLEGAL:
            return {nullptr, false};  // We already warn for this in V3LinkResolve
        case VEdgeType::ET_CHANGED:
        case VEdgeType::ET_HYBRID:  //
            return {new AstNeq(flp, currp(), prevp()), true};
        case VEdgeType::ET_BOTHEDGE:  //
            return {lsb(new AstXor{flp, currp(), prevp()}), false};
        case VEdgeType::ET_POSEDGE:  //
            return {lsb(new AstAnd{flp, currp(), new AstNot{flp, prevp()}}), false};
        case VEdgeType::ET_NEGEDGE:  //
            return {lsb(new AstAnd{flp, new AstNot{flp, currp()}, prevp()}), false};
        case VEdgeType::ET_EVENT: {
            UASSERT_OBJ(v3Global.hasEvents(), senItemp, "Inconsistent");
            {
                // If the event is fired, set up the clearing process
                AstCMethodHard* const callp = new AstCMethodHard{flp, currp(), "isFired"};
                callp->dtypeSetBit();
                AstIf* const ifp = new AstIf{flp, callp};
                m_updates.push_back(ifp);

                // Clear 'fired' state when done
                AstCMethodHard* const clearp = new AstCMethodHard{flp, currp(), "clearFired"};
                ifp->addIfsp(clearp);
                clearp->dtypeSetVoid();
                clearp->statement(true);

                // Enqueue for clearing 'triggered' state on next eval
                AstTextBlock* const blockp = new AstTextBlock{flp};
                ifp->addIfsp(blockp);
                const auto add = [&](const string& text) { blockp->addText(flp, text, true); };
                add("vlSymsp->enqueueTriggeredEventForClearing(");
                blockp->addNodep(currp());
                add(");\n");
            }

            // Get 'fired' state
            AstCMethodHard* const callp = new AstCMethodHard{flp, currp(), "isFired"};
            callp->dtypeSetBit();
            return {callp, false};
        }
        default:  // LCOV_EXCL_START
            senItemp->v3fatalSrc("Unknown edge type");
            return {nullptr, false};
        }  // LCOV_EXCL_STOP
    }

public:
    // Returns the expression computing the trigger, and a bool indicating that
    // this trigger should be fired on the first evaluation (at initialization)
    std::pair<AstNode*, bool> build(const AstSenTree* senTreep) {
        FileLine* const flp = senTreep->fileline();
        AstNode* resultp = nullptr;
        bool firedAtInitialization = false;
        for (AstSenItem* senItemp = senTreep->sensesp(); senItemp;
             senItemp = VN_AS(senItemp->nextp(), SenItem)) {
            const auto& pair = createTerm(senItemp);
            if (AstNode* const termp = pair.first) {
                resultp = resultp ? new AstOr{flp, resultp, termp} : termp;
                firedAtInitialization |= pair.second;
            }
        }
        return {resultp, firedAtInitialization};
    }

    std::vector<AstNodeStmt*> getAndClearUpdates() {
        m_hasUpdate.clear();
        return std::move(m_updates);
    }

    // CONSTRUCTOR
    SenExprBuilder(AstNetlist* netlistp, AstCFunc* initp)
        : m_initp{initp}
        , m_scopeTopp{netlistp->topScopep()->scopep()} {}
};

//============================================================================
// A TriggerKit holds all the components related to a TRIGGERVEC variable

struct TriggerKit {
    // The TRIGGERVEC AstVarScope representing these trigger flags
    AstVarScope* const m_vscp;
    // The AstCFunc that computes the current active triggers
    AstCFunc* const m_funcp;
    // The AstCFunc that dumps the current active triggers
    AstCFunc* const m_dumpp;
    // The map from input sensitivity list to trigger sensitivity list
    const std::unordered_map<const AstSenTree*, AstSenTree*> m_map;

    VL_UNCOPYABLE(TriggerKit);

    // Create an AstSenTree that is sensitive to the given trigger index. Must not exist yet!
    AstSenTree* createTriggerSenTree(AstNetlist* netlistp, uint32_t index) const {
        AstTopScope* const topScopep = netlistp->topScopep();
        FileLine* const flp = topScopep->fileline();
        AstVarRef* const vrefp = new AstVarRef{flp, m_vscp, VAccess::READ};
        AstCMethodHard* const callp
            = new AstCMethodHard{flp, vrefp, "at", new AstConst{flp, index}};
        callp->dtypeSetBit();
        callp->pure(true);
        AstSenItem* const senItemp = new AstSenItem{flp, VEdgeType::ET_TRUE, callp};
        AstSenTree* const resultp = new AstSenTree{flp, senItemp};
        topScopep->addSenTreep(resultp);
        return resultp;
    }

    // Utility that assigns the given index trigger to fire when the given variable is zero
    void addFirstIterationTriggerAssignment(AstVarScope* counterp, uint32_t index) const {
        FileLine* const flp = counterp->fileline();
        AstVarRef* const vrefp = new AstVarRef{flp, m_vscp, VAccess::WRITE};
        AstCMethodHard* const callp = new AstCMethodHard{flp, vrefp, "at", new AstConst{flp, 0}};
        callp->dtypeSetBit();
        callp->pure(true);
        m_funcp->stmtsp()->addHereThisAsNext(new AstAssign{
            flp, callp,
            new AstEq{flp, new AstVarRef{flp, counterp, VAccess::READ}, new AstConst{flp, 0}}});
    }

    // Utility to set then clear the dpiExportTrigger trigger
    void addDpiExportTriggerAssignment(AstVarScope* dpiExportTriggerVscp, uint32_t index) const {
        FileLine* const flp = dpiExportTriggerVscp->fileline();
        AstVarRef* const vrefp = new AstVarRef{flp, m_vscp, VAccess::WRITE};
        AstCMethodHard* const callp
            = new AstCMethodHard{flp, vrefp, "at", new AstConst{flp, index}};
        callp->dtypeSetBit();
        callp->pure(true);
        AstNode* stmtp
            = new AstAssign{flp, callp, new AstVarRef{flp, dpiExportTriggerVscp, VAccess::READ}};
        stmtp->addNext(new AstAssign{flp, new AstVarRef{flp, dpiExportTriggerVscp, VAccess::WRITE},
                                     new AstConst{flp, AstConst::BitFalse{}}});
        m_funcp->stmtsp()->addHereThisAsNext(stmtp);
    }
};

//============================================================================
// Create a TRIGGERVEC and the related TriggerKit for the given AstSenTree vector

const TriggerKit createTriggers(AstNetlist* netlistp, SenExprBuilder& senExprBuilder,
                                std::vector<const AstSenTree*> senTreeps, const string& name,
                                unsigned extra, bool slow = false) {
    AstTopScope* const topScopep = netlistp->topScopep();
    AstScope* const scopeTopp = topScopep->scopep();
    FileLine* const flp = scopeTopp->fileline();

    std::unordered_map<const AstSenTree*, AstSenTree*> map;

    const uint32_t nTriggers = senTreeps.size() + extra;

    // Create the TRIGGERVEC variable
    AstBasicDType* const tDtypep = new AstBasicDType(flp, VBasicDTypeKwd::TRIGGERVEC,
                                                     VSigning::UNSIGNED, nTriggers, nTriggers);
    netlistp->typeTablep()->addTypesp(tDtypep);
    AstVarScope* const vscp = scopeTopp->createTemp("__V" + name + "Triggered", tDtypep);

    // Create the trigger computation function
    AstCFunc* const funcp = makeSubFunction(netlistp, "_eval_triggers__" + name, slow);

    // Create the trigger dump function (for debugging)
    AstCFunc* const dumpp = makeSubFunction(netlistp, "_dump_triggers__" + name, slow);
    dumpp->ifdef("VL_DEBUG");

    // Add a print to the dumping function if there are no triggers pending
    {
        AstCMethodHard* const callp
            = new AstCMethodHard{flp, new AstVarRef{flp, vscp, VAccess::READ}, "any"};
        callp->dtypeSetBit();
        AstIf* const ifp = new AstIf{flp, callp};
        dumpp->addStmtsp(ifp);
        ifp->addElsesp(
            new AstText{flp, "VL_DBG_MSGF(\"         No triggers active\\n\");\n", true});
    }

    // Create a reference to a trigger flag
    const auto getTrigRef = [&](uint32_t index, VAccess access) {
        AstVarRef* const vrefp = new AstVarRef{flp, vscp, access};
        AstConst* const idxp = new AstConst{flp, index};
        AstCMethodHard* callp = new AstCMethodHard{flp, vrefp, "at", idxp};
        callp->dtypeSetBit();
        callp->pure(true);
        return callp;
    };

    // Add a debug dumping statement for this trigger
    const auto addDebug = [&](uint32_t index, const string& text = "") {
        std::stringstream ss;
        ss << "VL_DBG_MSGF(\"         '" << name << "' region trigger index " << cvtToStr(index)
           << " is active";
        if (!text.empty()) ss << ": " << text;
        ss << "\\n\");\n";
        const string message{ss.str()};

        AstIf* const ifp = new AstIf{flp, getTrigRef(index, VAccess::READ)};
        dumpp->addStmtsp(ifp);
        ifp->addIfsp(new AstText{flp, message, true});
    };

    // Add a print for each of the extra triggers
    for (unsigned i = 0; i < extra; ++i) addDebug(i);

    // Add trigger computation
    uint32_t triggerNumber = extra;
    AstNode* initialTrigsp = nullptr;
    for (const AstSenTree* const senTreep : senTreeps) {
        UASSERT_OBJ(senTreep->hasClocked() || senTreep->hasHybrid(), senTreep,
                    "Cannot create trigger expression for non-clocked sensitivity");

        // Create the trigger AstSenTrees and associate it with the original AstSenTree
        AstCMethodHard* const senp = getTrigRef(triggerNumber, VAccess::READ);
        AstSenItem* const senItemp = new AstSenItem{flp, VEdgeType::ET_TRUE, senp};
        AstSenTree* const trigpSenp = new AstSenTree{flp, senItemp};
        topScopep->addSenTreep(trigpSenp);
        map[senTreep] = trigpSenp;

        // Add the trigger computation
        const auto& pair = senExprBuilder.build(senTreep);
        funcp->addStmtsp(
            new AstAssign{flp, getTrigRef(triggerNumber, VAccess::WRITE), pair.first});

        // Add initialization time trigger
        if (pair.second || v3Global.opt.xInitialEdge()) {
            AstNode* const assignp = new AstAssign{flp, getTrigRef(triggerNumber, VAccess::WRITE),
                                                   new AstConst{flp, 1}};
            initialTrigsp = AstNode::addNext(initialTrigsp, assignp);
        }

        // Add a debug statement for this trigger
        std::stringstream ss;
        V3EmitV::verilogForTree(senTreep, ss);
        addDebug(triggerNumber, ss.str());

        //
        ++triggerNumber;
    }
    // Add the update statements
    for (AstNodeStmt* const nodep : senExprBuilder.getAndClearUpdates()) funcp->addStmtsp(nodep);

    // Add the initialization statements
    if (initialTrigsp) {
        AstVarScope* const vscp = scopeTopp->createTemp("__V" + name + "DidInit", 1);
        AstVarRef* const condp = new AstVarRef{flp, vscp, VAccess::READ};
        AstIf* const ifp = new AstIf{flp, new AstNot{flp, condp}};
        funcp->addStmtsp(ifp);
        ifp->branchPred(VBranchPred::BP_UNLIKELY);
        ifp->addIfsp(setVar(vscp, 1));
        ifp->addIfsp(initialTrigsp);
    }

    // Add a call to the dumping function if debug is enabled
    {
        AstTextBlock* const blockp = new AstTextBlock{flp};
        funcp->addStmtsp(blockp);
        const auto add = [&](const string& text) { blockp->addText(flp, text, true); };
        add("#ifdef VL_DEBUG\n");
        add("if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {\n");
        blockp->addNodep(new AstCCall(flp, dumpp));
        add("}\n");
        add("#endif\n");
    }

    return {vscp, funcp, dumpp, map};
}

//============================================================================
// Helpers to construct an evaluation loop.

AstNode* buildLoop(AstNetlist* netlistp, const string& name,
                   std::function<void(AstVarScope*, AstWhile*)> build)  //
{
    AstTopScope* const topScopep = netlistp->topScopep();
    AstScope* const scopeTopp = topScopep->scopep();
    FileLine* const flp = scopeTopp->fileline();
    // Create the loop condition variable
    AstVarScope* const condp = scopeTopp->createTemp("__V" + name + "Continue", 1);
    // Initialize the loop condition variable to true
    AstNode* const resp = setVar(condp, 1);
    // Add the loop
    AstWhile* const loopp = new AstWhile{flp, new AstVarRef{flp, condp, VAccess::READ}};
    resp->addNext(loopp);
    // Clear the loop condition variable in the loop
    loopp->addBodysp(setVar(condp, 0));
    // Build the body
    build(condp, loopp);
    // Done
    return resp;
};

std::pair<AstVarScope*, AstNode*> makeEvalLoop(AstNetlist* netlistp, const string& tag,
                                               const string& name, AstVarScope* trigVscp,
                                               AstCFunc* trigDumpp,
                                               std::function<AstNode*()> computeTriggers,
                                               std::function<AstNode*()> makeBody) {
    UASSERT_OBJ(trigVscp->dtypep()->basicp()->isTriggerVec(), trigVscp, "Not TRIGGERVEC");
    AstTopScope* const topScopep = netlistp->topScopep();
    AstScope* const scopeTopp = topScopep->scopep();
    FileLine* const flp = scopeTopp->fileline();

    AstVarScope* const counterp = scopeTopp->createTemp("__V" + tag + "IterCount", 32);

    AstNode* nodep = setVar(counterp, 0);
    nodep->addNext(buildLoop(netlistp, tag, [&](AstVarScope* continuep, AstWhile* loopp) {
        // Compute triggers
        loopp->addBodysp(computeTriggers());
        // Invoke body if triggered
        {
            AstVarRef* const refp = new AstVarRef{flp, trigVscp, VAccess::READ};
            AstCMethodHard* const callp = new AstCMethodHard{flp, refp, "any"};
            callp->dtypeSetBit();
            AstIf* const ifp = new AstIf{flp, callp};
            loopp->addBodysp(ifp);
            ifp->addIfsp(setVar(continuep, 1));

            // If we exceeded the iteration limit, die
            {
                const uint32_t limit = v3Global.opt.convergeLimit();
                AstVarRef* const refp = new AstVarRef{flp, counterp, VAccess::READ};
                AstConst* const constp
                    = new AstConst{flp, AstConst::DtypedValue{}, counterp->dtypep(), limit};
                AstNodeMath* const condp = new AstGt{flp, refp, constp};
                AstIf* const failp = new AstIf{flp, condp};
                ifp->addIfsp(failp);
                AstTextBlock* const blockp = new AstTextBlock{flp};
                failp->addIfsp(blockp);
                FileLine* const locp = netlistp->topModulep()->fileline();
                const string& file = EmitCBaseVisitor::protect(locp->filename());
                const string& line = cvtToStr(locp->lineno());
                const auto add = [&](const string& text) { blockp->addText(flp, text, true); };
                add("#ifdef VL_DEBUG\n");
                blockp->addNodep(new AstCCall{flp, trigDumpp});
                add("#endif\n");
                add("VL_FATAL_MT(\"" + file + "\", " + line + ", \"\", ");
                add("\"" + name + " region did not converge.\");\n");
            }

            // Increment iteration count
            {
                AstVarRef* const wrefp = new AstVarRef{flp, counterp, VAccess::WRITE};
                AstVarRef* const rrefp = new AstVarRef{flp, counterp, VAccess::READ};
                AstConst* const onep
                    = new AstConst{flp, AstConst::DtypedValue{}, counterp->dtypep(), 1};
                ifp->addIfsp(new AstAssign{flp, wrefp, new AstAdd{flp, rrefp, onep}});
            }

            // Add body
            ifp->addIfsp(makeBody());
        }
    }));

    return {counterp, nodep};
}

//============================================================================
// Order the combinational logic to create the settle loop

void createSettle(AstNetlist* netlistp, SenExprBuilder& senExprBulider,
                  LogicClasses& logicClasses) {
    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval_settle", true);

    // Clone, because ordering is destructive, but we still need them for "_eval"
    LogicByScope comb = logicClasses.m_comb.clone();
    LogicByScope hybrid = logicClasses.m_hybrid.clone();

    // Nothing to do if there is no logic.
    // While this is rare in real designs, it reduces noise in small tests.
    if (comb.empty() && hybrid.empty()) return;

    // We have an extra trigger denoting this is the first iteration of the settle loop
    constexpr unsigned firstIterationTrigger = 0;
    constexpr unsigned extraTriggers = firstIterationTrigger + 1;

    // Gather the relevant sensitivity expressions and create the trigger kit
    const auto& senTreeps = getSenTreesUsedBy({&comb, &hybrid});
    const TriggerKit& trig
        = createTriggers(netlistp, senExprBulider, senTreeps, "stl", extraTriggers, true);

    // Remap sensitivities (comb has none, so only do the hybrid)
    remapSensitivities(hybrid, trig.m_map);

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    std::unordered_map<const AstSenItem*, const AstSenTree*> trigToSen;
    invertAndMergeSenTreeMap(trigToSen, trig.m_map);

    // First trigger is for pure combinational triggers (first iteration)
    AstSenTree* const inputChanged = trig.createTriggerSenTree(netlistp, firstIterationTrigger);

    // Create and the body function
    AstCFunc* const stlFuncp = V3Order::order(
        netlistp, {&comb, &hybrid}, trigToSen, "stl", false, true,
        [=](const AstVarScope*, std::vector<AstSenTree*>& out) { out.push_back(inputChanged); });
    splitCheck(stlFuncp);

    // Create the eval loop
    const auto& pair = makeEvalLoop(
        netlistp, "stl", "Settle", trig.m_vscp, trig.m_dumpp,
        [&]() {  // Trigger
            return new AstCCall{stlFuncp->fileline(), trig.m_funcp};
        },
        [&]() {  // Body
            return new AstCCall{stlFuncp->fileline(), stlFuncp};
        });

    // Add the first iteration trigger to the trigger computation function
    trig.addFirstIterationTriggerAssignment(pair.first, firstIterationTrigger);

    // Add the eval loop to the top function
    funcp->addStmtsp(pair.second);
}

//============================================================================
// Order the replicated combinational logic to create the 'ico' region

AstNode* createInputCombLoop(AstNetlist* netlistp, SenExprBuilder& senExprBuilder,
                             LogicByScope& logic) {
    // Nothing to do if no combinational logic is sensitive to top level inputs
    if (logic.empty()) return nullptr;

    // SystemC only: Any top level inputs feeding a combinational logic must be marked,
    // so we can make them sc_sensitive
    if (v3Global.opt.systemC()) {
        logic.foreachLogic([](AstNode* logicp) {
            logicp->foreach<AstVarRef>([](AstVarRef* refp) {
                if (refp->access().isWriteOnly()) return;
                AstVarScope* const vscp = refp->varScopep();
                if (vscp->scopep()->isTop() && vscp->varp()->isNonOutput()) {
                    vscp->varp()->scSensitive(true);
                }
            });
        });
    }

    // We have some extra trigger denoting external conditions
    AstVarScope* const dpiExportTriggerVscp = netlistp->dpiExportTriggerp();

    unsigned extraTriggers = 0;
    const unsigned firstIterationTrigger = extraTriggers++;
    const unsigned dpiExportTriggerIndex
        = dpiExportTriggerVscp ? extraTriggers++ : std::numeric_limits<unsigned>::max();

    // Gather the relevant sensitivity expressions and create the trigger kit
    const auto& senTreeps = getSenTreesUsedBy({&logic});
    const TriggerKit& trig
        = createTriggers(netlistp, senExprBuilder, senTreeps, "ico", extraTriggers);

    if (dpiExportTriggerVscp) {
        trig.addDpiExportTriggerAssignment(dpiExportTriggerVscp, dpiExportTriggerIndex);
    }

    // Remap sensitivities
    remapSensitivities(logic, trig.m_map);

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    std::unordered_map<const AstSenItem*, const AstSenTree*> trigToSen;
    invertAndMergeSenTreeMap(trigToSen, trig.m_map);

    // The trigger top level inputs (first iteration)
    AstSenTree* const inputChanged = trig.createTriggerSenTree(netlistp, firstIterationTrigger);

    // The DPI Export trigger
    AstSenTree* const dpiExportTriggered
        = trig.createTriggerSenTree(netlistp, dpiExportTriggerIndex);

    // Create and Order the body function
    AstCFunc* const icoFuncp
        = V3Order::order(netlistp, {&logic}, trigToSen, "ico", false, false,
                         [=](const AstVarScope* vscp, std::vector<AstSenTree*>& out) {
                             if (vscp->scopep()->isTop() && vscp->varp()->isNonOutput()) {
                                 out.push_back(inputChanged);
                             }
                             if (vscp->varp()->isWrittenByDpi()) out.push_back(dpiExportTriggered);
                         });
    splitCheck(icoFuncp);

    // Create the eval loop
    const auto& pair = makeEvalLoop(
        netlistp, "ico", "Input combinational", trig.m_vscp, trig.m_dumpp,
        [&]() {  // Trigger
            return new AstCCall{icoFuncp->fileline(), trig.m_funcp};
        },
        [&]() {  // Body
            return new AstCCall{icoFuncp->fileline(), icoFuncp};
        });

    // Add the first iteration trigger to the trigger computation function
    trig.addFirstIterationTriggerAssignment(pair.first, firstIterationTrigger);

    // Return the eval loop itself
    return pair.second;
}

//============================================================================
// Bold together parts to create the top level _eval function

void createEval(AstNetlist* netlistp,  //
                AstNode* icoLoop,  //
                const TriggerKit& actTrig,  //
                AstVarScope* preTrigsp,  //
                AstVarScope* nbaTrigsp,  //
                AstCFunc* actFuncp,  //
                AstCFunc* nbaFuncp  //
) {
    FileLine* const flp = netlistp->fileline();

    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval", false);
    netlistp->evalp(funcp);

    // Start with the ico loop, if any
    if (icoLoop) funcp->addStmtsp(icoLoop);

    // Create the NBA trigger dumping function, which is the same as act trigger
    // dumping function, but referencing the nba trigger vector.
    AstCFunc* const nbaDumpp = actTrig.m_dumpp->cloneTree(false);
    actTrig.m_dumpp->addNextHere(nbaDumpp);
    nbaDumpp->name("_dump_triggers__nba");
    nbaDumpp->foreach<AstVarRef>([&](AstVarRef* refp) {
        UASSERT_OBJ(refp->access().isReadOnly(), refp, "Should only read state");
        if (refp->varScopep() == actTrig.m_vscp) {
            refp->replaceWith(new AstVarRef{refp->fileline(), nbaTrigsp, VAccess::READ});
        }
    });
    nbaDumpp->foreach<AstText>([&](AstText* textp) {  //
        textp->text(VString::replaceWord(textp->text(), "act", "nba"));
    });

    // Create the active eval loop
    AstNode* const activeEvalLoopp
        = makeEvalLoop(
              netlistp, "act", "Active", actTrig.m_vscp, actTrig.m_dumpp,
              [&]() {  // Trigger
                  return new AstCCall{flp, actTrig.m_funcp};
              },
              [&]() {  // Body
                  AstNode* resultp = nullptr;

                  // Compute the pre triggers
                  {
                      AstVarRef* const lhsp = new AstVarRef{flp, preTrigsp, VAccess::WRITE};
                      AstVarRef* const opap = new AstVarRef{flp, actTrig.m_vscp, VAccess::READ};
                      AstVarRef* const opbp = new AstVarRef{flp, nbaTrigsp, VAccess::READ};
                      opap->addNext(opbp);
                      AstCMethodHard* const callp = new AstCMethodHard{flp, lhsp, "andNot", opap};
                      callp->statement(true);
                      callp->dtypeSetVoid();
                      resultp = AstNode::addNext(resultp, callp);
                  }

                  // Latch the active trigger flags under the NBA trigger flags
                  {
                      AstVarRef* const lhsp = new AstVarRef{flp, nbaTrigsp, VAccess::WRITE};
                      AstVarRef* const argp = new AstVarRef{flp, actTrig.m_vscp, VAccess::READ};
                      AstCMethodHard* const callp = new AstCMethodHard{flp, lhsp, "set", argp};
                      callp->statement(true);
                      callp->dtypeSetVoid();
                      resultp = AstNode::addNext(resultp, callp);
                  }

                  // Invoke body function
                  return AstNode::addNext(resultp, new AstCCall{flp, actFuncp});
              })
              .second;

    // Create the NBA eval loop. This uses the Active eval loop in the trigger section.
    AstNode* const nbaEvalLoopp
        = makeEvalLoop(
              netlistp, "nba", "NBA", nbaTrigsp, nbaDumpp,
              [&]() {  // Trigger
                  AstNode* resultp = nullptr;

                  // Reset NBA triggers
                  {
                      AstVarRef* const refp = new AstVarRef{flp, nbaTrigsp, VAccess::WRITE};
                      AstCMethodHard* const callp = new AstCMethodHard{flp, refp, "clear"};
                      callp->statement(true);
                      callp->dtypeSetVoid();
                      resultp = AstNode::addNext(resultp, callp);
                  }

                  // Run the Active eval loop
                  return AstNode::addNext(resultp, activeEvalLoopp);
              },
              [&]() {  // Body
                  return new AstCCall{flp, nbaFuncp};
              })
              .second;

    // Add the NBA eval loop
    funcp->addStmtsp(nbaEvalLoopp);
}

}  // namespace

//============================================================================
// Top level entry-point to scheduling

void schedule(AstNetlist* netlistp) {
    const auto addSizeStat = [](const string name, const LogicByScope& lbs) {
        uint64_t size = 0;
        lbs.foreachLogic([&](AstNode* nodep) { size += nodep->nodeCount(); });
        V3Stats::addStat("Scheduling, " + name, size);
    };

    // Step 1. Gather and classify all logic in the design
    LogicClasses logicClasses = gatherLogicClasses(netlistp);

    if (v3Global.opt.stats()) {
        V3Stats::statsStage("sched-gather");
        addSizeStat("size of class: static", logicClasses.m_static);
        addSizeStat("size of class: initial", logicClasses.m_initial);
        addSizeStat("size of class: final", logicClasses.m_final);
    }

    // Step 2. Schedule static, initial and final logic classes in source order
    createStatic(netlistp, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-static");

    AstCFunc* const initp = createInitial(netlistp, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-initial");

    createFinal(netlistp, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-final");

    // Step 3: Break combinational cycles by introducing hybrid logic
    // Note: breakCycles also removes corresponding logic from logicClasses.m_comb;
    logicClasses.m_hybrid = breakCycles(netlistp, logicClasses.m_comb);
    if (v3Global.opt.stats()) {
        addSizeStat("size of class: clocked", logicClasses.m_clocked);
        addSizeStat("size of class: combinational", logicClasses.m_comb);
        addSizeStat("size of class: hybrid", logicClasses.m_hybrid);
        V3Stats::statsStage("sched-break-cycles");
    }

    // We pass around a single SenExprBuilder instance, as we only need one set of 'prev' variables
    // for edge/change detection in sensitivity expressions, which this keeps track of.
    SenExprBuilder senExprBuilder{netlistp, initp};

    // Step 4: Create 'settle' region that restores the combinational invariant
    createSettle(netlistp, senExprBuilder, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-settle");

    // Step 5: Partition the clocked and combinational (including hybrid) logic into pre/act/nba.
    // All clocks (signals referenced in an AstSenTree) generated via a blocking assignment
    // (including combinationally generated signals) are computed within the act region.
    LogicRegions logicRegions
        = partition(logicClasses.m_clocked, logicClasses.m_comb, logicClasses.m_hybrid);
    if (v3Global.opt.stats()) {
        addSizeStat("size of region: Active Pre", logicRegions.m_pre);
        addSizeStat("size of region: Active", logicRegions.m_act);
        addSizeStat("size of region: NBA", logicRegions.m_nba);
        V3Stats::statsStage("sched-partition");
    }

    // Step 6: Replicate combinational logic
    LogicReplicas logicReplicas = replicateLogic(logicRegions);
    if (v3Global.opt.stats()) {
        addSizeStat("size of replicated logic: Input", logicReplicas.m_ico);
        addSizeStat("size of replicated logic: Active", logicReplicas.m_act);
        addSizeStat("size of replicated logic: NBA", logicReplicas.m_nba);
        V3Stats::statsStage("sched-replicate");
    }

    // Step 7: Create input combinational logic loop
    AstNode* const icoLoopp = createInputCombLoop(netlistp, senExprBuilder, logicReplicas.m_ico);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-ico");

    // Step 8: Create the pre/act/nba triggers
    AstVarScope* const dpiExportTriggerVscp = netlistp->dpiExportTriggerp();

    unsigned extraTriggers = 0;
    // We may have an extra trigger for variable updated in DPI exports
    const unsigned dpiExportTriggerIndex
        = dpiExportTriggerVscp ? extraTriggers++ : std::numeric_limits<unsigned>::max();

    const auto& senTreeps = getSenTreesUsedBy({&logicRegions.m_pre,  //
                                               &logicRegions.m_act,  //
                                               &logicRegions.m_nba});
    const TriggerKit& actTrig
        = createTriggers(netlistp, senExprBuilder, senTreeps, "act", extraTriggers);

    if (dpiExportTriggerVscp) {
        actTrig.addDpiExportTriggerAssignment(dpiExportTriggerVscp, dpiExportTriggerIndex);
    }

    AstTopScope* const topScopep = netlistp->topScopep();
    AstScope* const scopeTopp = topScopep->scopep();

    AstVarScope* const actTrigVscp = actTrig.m_vscp;
    AstVarScope* const preTrigVscp = scopeTopp->createTempLike("__VpreTriggered", actTrigVscp);
    AstVarScope* const nbaTrigVscp = scopeTopp->createTempLike("__VnbaTriggered", actTrigVscp);

    const auto cloneMapWithNewTriggerReferences
        = [=](std::unordered_map<const AstSenTree*, AstSenTree*> map, AstVarScope* vscp) {
              // Copy map
              auto newMap{map};
              VNDeleter deleter;
              // Replace references in each mapped value with a reference to the given vscp
              for (auto& pair : newMap) {
                  pair.second = pair.second->cloneTree(false);
                  pair.second->foreach<AstVarRef>([&](AstVarRef* refp) {
                      UASSERT_OBJ(refp->varScopep() == actTrigVscp, refp, "Unexpected reference");
                      UASSERT_OBJ(refp->access() == VAccess::READ, refp, "Should be read ref");
                      refp->replaceWith(new AstVarRef{refp->fileline(), vscp, VAccess::READ});
                      deleter.pushDeletep(refp);
                  });
                  topScopep->addSenTreep(pair.second);
              }
              return newMap;
          };

    const auto& actTrigMap = actTrig.m_map;
    const auto preTrigMap = cloneMapWithNewTriggerReferences(actTrigMap, preTrigVscp);
    const auto nbaTrigMap = cloneMapWithNewTriggerReferences(actTrigMap, nbaTrigVscp);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-triggers");

    // Note: Experiments so far show that running the Act (or Ico) regions on
    // multiple threads is always a net loss, so only use multi-threading for
    // NBA for now. This can be revised if evidence is available that it would
    // be beneficial

    // Step 9: Create the 'act' region evaluation function

    // Remap sensitivities of the input logic to the triggers
    remapSensitivities(logicRegions.m_pre, preTrigMap);
    remapSensitivities(logicRegions.m_act, actTrigMap);
    remapSensitivities(logicReplicas.m_act, actTrigMap);

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    std::unordered_map<const AstSenItem*, const AstSenTree*> trigToSenAct;
    invertAndMergeSenTreeMap(trigToSenAct, preTrigMap);
    invertAndMergeSenTreeMap(trigToSenAct, actTrigMap);

    // The DPI Export trigger AstSenTree
    AstSenTree* const dpiExportTriggered
        = actTrig.createTriggerSenTree(netlistp, dpiExportTriggerIndex);

    AstCFunc* const actFuncp = V3Order::order(
        netlistp, {&logicRegions.m_pre, &logicRegions.m_act, &logicReplicas.m_act}, trigToSenAct,
        "act", false, false, [=](const AstVarScope* vscp, std::vector<AstSenTree*>& out) {
            if (vscp->varp()->isWrittenByDpi()) out.push_back(dpiExportTriggered);
        });
    splitCheck(actFuncp);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-act");

    // Step 10: Create the 'nba' region evaluation function

    // Remap sensitivities of the input logic to the triggers
    remapSensitivities(logicRegions.m_nba, nbaTrigMap);
    remapSensitivities(logicReplicas.m_nba, nbaTrigMap);

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    std::unordered_map<const AstSenItem*, const AstSenTree*> trigToSenNba;
    invertAndMergeSenTreeMap(trigToSenNba, nbaTrigMap);

    AstCFunc* const nbaFuncp = V3Order::order(
        netlistp, {&logicRegions.m_nba, &logicReplicas.m_nba}, trigToSenNba, "nba",
        v3Global.opt.mtasks(), false, [=](const AstVarScope* vscp, std::vector<AstSenTree*>& out) {
            if (vscp->varp()->isWrittenByDpi()) out.push_back(dpiExportTriggered);
        });
    splitCheck(nbaFuncp);
    netlistp->evalNbap(nbaFuncp);  // Remember for V3LifePost
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-nba");

    // Step 11: Bolt it all together to create the '_eval' function
    createEval(netlistp, icoLoopp, actTrig, preTrigVscp, nbaTrigVscp, actFuncp, nbaFuncp);

    splitCheck(initp);

    netlistp->dpiExportTriggerp(nullptr);

    V3Global::dumpCheckGlobalTree("sched", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}

}  // namespace V3Sched
