#pragma once
#include <llvm/IR/PassManager.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>

class VmpDevirtualizationPass : public llvm::PassInfoMixin<VmpDevirtualizationPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function& F, llvm::FunctionAnalysisManager& AM) {
		using namespace llvm;
		using namespace llvm::PatternMatch;

        bool changed = false;
        std::vector<Instruction*> toRemove;

        for (auto& BB : F) {
            for (auto& I : BB) {
                // --- 2) ”даление LLVM_JIT_LogJmpIndirect с константами ---
                if (auto* Call = dyn_cast<CallInst>(&I)) {
                    if (Call->getCalledFunction() &&
                        Call->getCalledFunction()->getName() == "LLVM_JIT_LogJmpIndirect") {
                        // ≈сли аргумент - это жестка€ константа
                        if (isa<ConstantInt>(Call->getArgOperand(0))) {
                            toRemove.push_back(Call);
                            changed = true;
                            continue;
                        }
                    }
                }

                // --- 1) —вертка MBA математики в Select ---
                Value* X;
                ConstantInt* FalseVip, * TrueVip;

                // m_c_Add ищет (A + B) или (B + A)
                // m_Value() внутри m_Add игнорирует конкретную константу (теперь нас не волнует 8589934591 или -1)
                auto pattern = m_c_Add(
                    m_And(m_Add(m_Value(X), m_Value()), m_ConstantInt(FalseVip)),
                    m_And(m_Neg(m_Deferred(X)), m_ConstantInt(TrueVip))
                );

                if (match(&I, pattern)) {
                    IRBuilder<> builder(&I);

                    // X у нас равен либо 0, либо 1. 
                    // ѕревращаем его в чистый i1 (bool) дл€ оператора select: (X != 0)
                    Value* BoolCond = builder.CreateICmpNE(
                        X,
                        ConstantInt::getNullValue(X->getType()),
                        "clean_vip_cond"
                    );

                    // —оздаем тернарный оператор: Cond ? TrueVip : FalseVip
                    Value* CleanSelect = builder.CreateSelect(BoolCond, TrueVip, FalseVip, "clean_vip_select");

                    I.replaceAllUsesWith(CleanSelect);
                    toRemove.push_back(&I);
                    changed = true;
                }
            }
        }

        // ”дал€ем собранный мусор
        for (auto* I : toRemove) {
            if (I->use_empty()) { // «ащита: удал€ем только если результат нигде не используетс€
                I->eraseFromParent();
            }
        }

        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};