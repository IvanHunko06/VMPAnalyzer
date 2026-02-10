#include "CFGRepatcher.hpp"
using namespace llvm;

void CFGRepatcher::Patch(LLVMTraceLifter& lifter) {
	for (auto& [vip, bb] : lifter.virtualBasicBlockMap) {
		TryPatchBlock(lifter, bb);
	}

	//ReplaceWithDirectJump(lifter, lifter.entryBasicBlock, nullptr, lifter.protectedCodeEntryBlock);
}

void CFGRepatcher::TryPatchBlock(LLVMTraceLifter& lifter, llvm::BasicBlock* bb) {
	auto vipStore = FindVipStore(lifter, bb);
	if (!vipStore) return;

	Value* storedVal = vipStore->getValueOperand();
	if (auto* constInt = dyn_cast<ConstantInt>(storedVal)) {
		auto targetVip = constInt->getZExtValue();
		if (targetVip == 0) {
			ReplaceWithDirectJump(lifter, bb, vipStore, lifter.exitBlock);
			return;
		}
		if (!lifter.virtualBasicBlockMap.contains(targetVip)) return;
		ReplaceWithDirectJump(lifter, bb, vipStore, lifter.virtualBasicBlockMap[targetVip]);
		RemoveCaseFromSwitch(lifter.dispatchSwitch, targetVip);
		return;
	}

	uint64_t targetVip;
	if (MatchImageBaseAdd(storedVal, targetVip)) {
		if (!lifter.virtualBasicBlockMap.contains(targetVip)) return;
		ReplaceWithDirectJump(lifter, bb, vipStore, lifter.virtualBasicBlockMap[targetVip]);
		RemoveCaseFromSwitch(lifter.dispatchSwitch, targetVip);
		return;
	}
}

llvm::StoreInst* CFGRepatcher::FindVipStore(LLVMTraceLifter& lifter, llvm::BasicBlock* bb)
{
	// Идем с конца блока вверх
	for (auto it = bb->rbegin(); it != bb->rend(); ++it) {
		if (auto* store = dyn_cast<StoreInst>(&*it)) {
			if (store->getPointerOperand() == lifter.targetVip) {
				return store;
			}
		}
	}
	return nullptr;
}

void CFGRepatcher::ReplaceWithDirectJump(LLVMTraceLifter& lifter, BasicBlock* bb, StoreInst* storeToRemove, BasicBlock* targetBB) {
	// Удаляем старый прыжок в диспетчер (он должен быть после store)
	Instruction* oldTerm = bb->getTerminator();
	if (!oldTerm) return; // Если терминатора нет, значит нет и прыжка, ничего не делаем

	auto* branchInstr = llvm::dyn_cast<BranchInst>(oldTerm);
	auto* oldTarget = branchInstr->getSuccessor(0);
	for (auto it = oldTarget->begin(); it != oldTarget->end(); ) {
		Instruction& I = *it++;

		auto* phiNode = dyn_cast<PHINode>(&I);
		if (!phiNode) continue;

		int idx = phiNode->getBasicBlockIndex(bb);
		if (idx != -1) {
			phiNode->removeIncomingValue(idx);
		}
	}

	if (oldTerm && oldTerm != storeToRemove) oldTerm->eraseFromParent();

	
	// Создаем ветку
	BranchInst::Create(targetBB, bb);

	// Удаляем сам store, он больше не нужен
	if(storeToRemove) storeToRemove->eraseFromParent();
}

bool CFGRepatcher::MatchImageBaseAdd(Value* val, uint64_t& outVip) {

	auto* addOperator = dyn_cast<AddOperator>(val);
	if (!addOperator) return false;

	auto* op1 = addOperator->getOperand(0);
	auto* op2 = addOperator->getOperand(1);

	auto* constOp1 = dyn_cast<ConstantInt>(op1);
	auto* constOp2 = dyn_cast<ConstantInt>(op2);
	if (constOp1 && constOp2) {
		// Оба операнда константы, не подходит
		return false;
	}

	if (!constOp1 && !constOp2) {
		// Нет констант, не подходит
		return false;
	}

	ConstantInt* constOp = constOp1 ? constOp1 : constOp2;
	outVip = constOp->getZExtValue();
	return true;
}

void CFGRepatcher::RemoveCaseFromSwitch(llvm::SwitchInst* switchInst, uint64_t targetVip) {
	const auto* caseVal = ConstantInt::get(switchInst->getCondition()->getType(), targetVip);
	auto* constVal = llvm::dyn_cast<ConstantInt>(caseVal); // Убедимся, что это константа нужного типа
	auto it = switchInst->findCaseValue(constVal);
	if (it != switchInst->case_end()) {
		BasicBlock* destBB = it->getCaseSuccessor();
		BasicBlock* switchBB = switchInst->getParent();

		// 3. Удаляем сам case из свича
		// ВАЖНО: Мы делаем это ДО обновления PHI, чтобы проверка uniqueSuccessor сработала корректно
		switchInst->removeCase(it);

		// 4. Обновляем PHI узлы в целевом блоке (destBB)
		// Нам нужно сказать destBB: "Ты больше не получаешь управление от блока с диспетчером"

		// НЮАНС: В SwitchInst может быть несколько case, ведущих в ОДИН И ТОТ ЖЕ блок.
		// Мы должны удалять запись в PHI только если это была ПОСЛЕДНЯЯ связь между SwitchBB и DestBB.

		// Проверяем: остался ли DestBB наследником SwitchBB?
		bool stillHasEdge = false;
		for (unsigned i = 0; i < switchInst->getNumSuccessors(); ++i) {
			if (switchInst->getSuccessor(i) == destBB) {
				stillHasEdge = true;
				break;
			}
		}

		// Если связей больше нет — чистим PHI
		if (!stillHasEdge) {
			for (auto& inst : *destBB) {
				if (auto* phi = dyn_cast<PHINode>(&inst)) {
					// Удаляем входящее значение от блока диспетчера
					phi->removeIncomingValue(switchBB, false);
				}
				else {
					// PHI всегда идут первыми, можно выходить
					break;
				}
			}
		}
	}

}