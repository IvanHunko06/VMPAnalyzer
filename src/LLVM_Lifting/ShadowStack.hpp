#pragma once
#include <llvm/IR/IRBuilder.h>
#include "../VmHandlerMatcher.hpp"
#include <deque>
#include <optional>

template <typename TMetadata>
struct ShadowStackSlot {
	llvm::Value* value{ nullptr };
	int64_t bitDepth{ BitDepth_64 };
	TMetadata metadata;
};

template <typename TMetadata>
struct ShadowStackPop {
	llvm::Value* value{ nullptr };
	TMetadata metadata;
};

template <typename TMetadata>
class ShadowStack {
private:
	using SlotType = ShadowStackSlot<TMetadata>;
	using StackType = std::deque<SlotType>;
	using RealPopFunctionType = std::function<llvm::Value* (HandlerBitDepth)>;

	StackType shadowStack_;
	llvm::IRBuilder<>* builder_;
	RealPopFunctionType realPop_;
public:
	using PopType = ShadowStackPop<TMetadata>;
	using OptionalPopType = std::optional<PopType>;

	ShadowStack(llvm::IRBuilder<>* builder, RealPopFunctionType realPop) {
		this->builder_ = builder;
		this->realPop_ = realPop;
	}

	inline StackType::iterator begin() {
		return shadowStack_.begin();
	}
	inline StackType::iterator end() {
		return shadowStack_.end();
	}
	inline StackType::const_iterator cbegin() const{
		return shadowStack_.begin();
	}
	inline StackType::const_iterator cend() const{
		return shadowStack_.end();
	}

	PopType Pop(HandlerBitDepth requestedDepth) {
		if (shadowStack_.empty()) {
			auto* value = realPop_(requestedDepth);
			PopType result;
			result.value = value;
			return result;
		}

		auto& slot = shadowStack_.back();

		if (slot.bitDepth == requestedDepth) {
			shadowStack_.pop_back();
			PopType result;
			result.metadata = std::move(slot.metadata);
			result.value = slot.value;
			return result;
		}

		if (slot.value->getType()->isPointerTy()) {
			slot.value = builder_->CreatePtrToInt(slot.value,
				builder_->getIntNTy(slot.bitDepth),
				"shadow_pop_ptr_to_int"
			);
			slot.metadata = TMetadata{};
		}

		if (slot.bitDepth < requestedDepth) {
			auto currentBits = static_cast<HandlerBitDepth>(slot.bitDepth);
			auto lowPart = Pop(currentBits);
			HandlerBitDepth neededBits = (HandlerBitDepth)(requestedDepth - currentBits);

			auto highPart = Pop(neededBits);

			llvm::Type* targetType = builder_->getIntNTy(requestedDepth);
			llvm::Value* lowExt = builder_->CreateZExt(lowPart.value, targetType, "merge_low_ext");
			llvm::Value* highExt = builder_->CreateZExt(highPart.value, targetType, "merge_high_ext");

			llvm::Value* shiftAmt = builder_->getIntN(targetType->getIntegerBitWidth(), currentBits);
			llvm::Value* highShifted = builder_->CreateShl(highExt, shiftAmt, "merge_high_shifted");

			llvm::Value* result = builder_->CreateOr(lowExt, highShifted, "merged_res");
			PopType finalResult;
			finalResult.value = result;
			return finalResult;
		}

		auto* fullVal = slot.value;
		unsigned fullWidth = fullVal->getType()->getIntegerBitWidth();

		auto* result = builder_->CreateTrunc(
			fullVal,
			builder_->getIntNTy(requestedDepth),
			"pop_slice"
		);
		// 2. —двигаем, чтобы получить остаток
		auto* shiftAmt = builder_->getIntN(fullWidth, requestedDepth);
		auto* remainingValHigh = builder_->CreateLShr(fullVal, shiftAmt, "stack_rem_shifted");

		unsigned newBitDepth = fullWidth - requestedDepth;

		// ¬ажно: GetTypeByDepth может вернуть стандартные типы, 
		// но нам нужен точный IntegerType произвольной ширины дл€ промежуточного хранени€
		auto* remainingValTrunc = builder_->CreateTrunc(
			remainingValHigh,
			builder_->getIntNTy(newBitDepth),
			"stack_rem_trunc"
		);

		slot.bitDepth = newBitDepth; // ќбновл€ем размер
		slot.value = remainingValTrunc; // “еперь типы совпадают (i48 в слоте с depth 48)
		slot.metadata = TMetadata{};

		if (newBitDepth == 0) {
			shadowStack_.pop_back();
		}
		PopType finalResult;
		finalResult.value = result;

		return finalResult;
	}
	void Push(HandlerBitDepth bitDepth, llvm::Value* value, TMetadata& metadata) {
		SlotType slot;
		slot.bitDepth = bitDepth;
		slot.metadata = std::move(metadata);
		slot.value = value;

		shadowStack_.push_back(slot);
	}
	inline void Clear() {
		shadowStack_.clear();
	}
	inline size_t Size() {
		return shadowStack_.size();
	}
	inline uint64_t GetFullShadowStackSize() {
		uint64_t fullSize = 0;
		for (auto& slot : shadowStack_) {
			fullSize += (slot.bitDepth / 8);
		}
		return fullSize;
	}
	inline uint64_t GetShadowStackSizeFor(SlotType* target) {
		uint64_t fullSize = 0;
		for (auto& slot : shadowStack_) {
			if (&slot == target) break;
			fullSize += (slot.bitDepth / 8);
		}
		return fullSize;
	}
};