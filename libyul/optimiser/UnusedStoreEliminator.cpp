/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Optimiser component that removes stores to memory and storage slots that are not used
 * or overwritten later on.
 */

#include <libyul/optimiser/UnusedStoreEliminator.h>

#include <libyul/optimiser/Semantics.h>
#include <libyul/optimiser/OptimizerUtilities.h>
#include <libyul/optimiser/Semantics.h>
#include <libyul/optimiser/SSAValueTracker.h>
#include <libyul/optimiser/DataFlowAnalyzer.h>
#include <libyul/optimiser/KnowledgeBase.h>
#include <libyul/ControlFlowSideEffectsCollector.h>
#include <libyul/AST.h>

#include <libyul/backends/evm/EVMDialect.h>

#include <libsolutil/CommonData.h>

#include <libevmasm/Instruction.h>
#include <libevmasm/SemanticInformation.h>

#include <range/v3/algorithm/all_of.hpp>

using namespace std;
using namespace solidity;
using namespace solidity::yul;

/// Variable names for special constants that can never appear in actual Yul code.
static string const zero{"@ 0"};
static string const one{"@ 1"};
static string const thirtyTwo{"@ 32"};


void UnusedStoreEliminator::run(OptimiserStepContext& _context, Block& _ast)
{
	map<YulString, SideEffects> functionSideEffects = SideEffectsPropagator::sideEffects(
		_context.dialect,
		CallGraphGenerator::callGraph(_ast)
	);

	SSAValueTracker ssaValues;
	ssaValues(_ast);
	map<YulString, AssignedValue> values;
	for (auto const& [name, expression]: ssaValues.values())
		values[name] = AssignedValue{expression, {}};
	Expression const zeroLiteral{Literal{{}, LiteralKind::Number, YulString{"0"}, {}}};
	Expression const oneLiteral{Literal{{}, LiteralKind::Number, YulString{"1"}, {}}};
	Expression const thirtyTwoLiteral{Literal{{}, LiteralKind::Number, YulString{"32"}, {}}};
	values[YulString{zero}] = AssignedValue{&zeroLiteral, {}};
	values[YulString{one}] = AssignedValue{&oneLiteral, {}};
	values[YulString{thirtyTwo}] = AssignedValue{&thirtyTwoLiteral, {}};

	bool const ignoreMemory = MSizeFinder::containsMSize(_context.dialect, _ast);
	UnusedStoreEliminator rse{
		_context.dialect,
		functionSideEffects,
		ControlFlowSideEffectsCollector{_context.dialect, _ast}.functionSideEffectsNamed(),
		values,
		ignoreMemory
	};
	rse(_ast);

	auto evmDialect = dynamic_cast<EVMDialect const*>(&_context.dialect);
	if (evmDialect && evmDialect->providesObjectAccess())
		rse.clearActive(Location::Memory);
	else
		rse.markActiveAsUsed(Location::Memory);
	rse.markActiveAsUsed(Location::Storage);

	StatementRemover remover{rse.m_allStores - rse.m_usedStores};
	remover(_ast);
}

void UnusedStoreEliminator::operator()(FunctionCall const& _functionCall)
{
	UnusedStoreBase::operator()(_functionCall);

	for (Operation const& op: operationsFromFunctionCall(_functionCall))
		applyOperation(op);

	ControlFlowSideEffects sideEffects;
	if (auto builtin = m_dialect.builtin(_functionCall.functionName.name))
		sideEffects = builtin->controlFlowSideEffects;
	else
		sideEffects = m_controlFlowSideEffects.at(_functionCall.functionName.name);

	if (sideEffects.canTerminate)
		markActiveAsUsed(Location::Storage);
	if (!sideEffects.canContinue)
	{
		clearActive(Location::Memory);
		if (!sideEffects.canTerminate)
			clearActive(Location::Storage);
	}
}

void UnusedStoreEliminator::operator()(FunctionDefinition const& _functionDefinition)
{
	ScopedSaveAndRestore storeOperations(m_storeOperations, {});
	UnusedStoreBase::operator()(_functionDefinition);
}


void UnusedStoreEliminator::operator()(Leave const&)
{
	markActiveAsUsed();
}

void UnusedStoreEliminator::visit(Statement const& _statement)
{
	using evmasm::Instruction;

	UnusedStoreBase::visit(_statement);

	auto const* exprStatement = get_if<ExpressionStatement>(&_statement);
	if (!exprStatement)
		return;

	FunctionCall const* funCall = get_if<FunctionCall>(&exprStatement->expression);
	yulAssert(funCall);
	optional<Instruction> instruction = toEVMInstruction(m_dialect, funCall->functionName.name);
	if (!instruction)
		return;

	if (!ranges::all_of(funCall->arguments, [](Expression const& _expr) -> bool {
		return get_if<Identifier>(&_expr) || get_if<Literal>(&_expr);
	}))
		return;

	// We determine if this is a store instruction without additional side-effects
	// both by querying a combination of semantic information and by listing the instructions.
	// This way the assert below should be triggered on any change.
	using evmasm::SemanticInformation;
	bool isStorageWrite = (*instruction == Instruction::SSTORE);
	bool isMemoryWrite =
		*instruction == Instruction::EXTCODECOPY ||
		*instruction == Instruction::CODECOPY ||
		*instruction == Instruction::CALLDATACOPY ||
		*instruction == Instruction::RETURNDATACOPY ||
		*instruction == Instruction::MSTORE ||
		*instruction == Instruction::MSTORE8;
	bool isCandidateForRemoval =
		SemanticInformation::otherState(*instruction) != SemanticInformation::Write && (
			SemanticInformation::storage(*instruction) == SemanticInformation::Write ||
			(!m_ignoreMemory && SemanticInformation::memory(*instruction) == SemanticInformation::Write)
		);
	yulAssert(isCandidateForRemoval == (isStorageWrite || (!m_ignoreMemory && isMemoryWrite)));
	if (isCandidateForRemoval)
	{
		if (*instruction == Instruction::RETURNDATACOPY)
		{
			// Out-of-bounds access to the returndata buffer results in a revert,
			// so we are careful not to remove a potentially reverting call to a builtin.
			// The only way the Solidity compiler uses `returndatacopy` is
			// `returndatacopy(X, 0, returndatasize())`, so we only allow to remove this pattern
			// (which is guaranteed to never cause an out-of-bounds revert).
			bool allowReturndatacopyToBeRemoved = false;
			auto startOffset = identifierNameIfSSA(funCall->arguments.at(1));
			auto length = identifierNameIfSSA(funCall->arguments.at(2));
			KnowledgeBase knowledge(m_dialect, [this](YulString _var) { return util::valueOrNullptr(m_ssaValues, _var); });
			if (length && startOffset)
			{
				FunctionCall const* lengthCall = get_if<FunctionCall>(m_ssaValues.at(*length).value);
				if (
					knowledge.knownToBeZero(*startOffset) &&
					lengthCall &&
					toEVMInstruction(m_dialect, lengthCall->functionName.name) == Instruction::RETURNDATASIZE
				)
					allowReturndatacopyToBeRemoved = true;
			}
			if (!allowReturndatacopyToBeRemoved)
				return;
		}
		m_allStores.insert(&_statement);
		vector<Operation> operations = operationsFromFunctionCall(*funCall);
		yulAssert(operations.size() == 1, "");
		if (operations.front().location == Location::Storage)
			activeStorageStores().insert(&_statement);
		else
			activeMemoryStores().insert(&_statement);
		m_storeOperations[&_statement] = std::move(operations.front());
	}
}

vector<UnusedStoreEliminator::Operation> UnusedStoreEliminator::operationsFromFunctionCall(
	FunctionCall const& _functionCall
) const
{
	using evmasm::Instruction;

	YulString functionName = _functionCall.functionName.name;
	SideEffects sideEffects;
	if (BuiltinFunction const* f = m_dialect.builtin(functionName))
		sideEffects = f->sideEffects;
	else
		sideEffects = m_functionSideEffects.at(functionName);

	optional<Instruction> instruction = toEVMInstruction(m_dialect, functionName);
	if (!instruction)
	{
		vector<Operation> result;
		// Unknown read is worse than unknown write.
		if (sideEffects.memory != SideEffects::Effect::None)
			result.emplace_back(Operation{Location::Memory, Effect::Read, {}, {}});
		if (sideEffects.storage != SideEffects::Effect::None)
			result.emplace_back(Operation{Location::Storage, Effect::Read, {}, {}});
		return result;
	}

	using evmasm::SemanticInformation;

	return util::applyMap(
		SemanticInformation::readWriteOperations(*instruction),
		[&](SemanticInformation::Operation const& _op) -> Operation
		{
			yulAssert(!(_op.lengthParameter && _op.lengthConstant));
			yulAssert(_op.effect != Effect::None);
			Operation ourOp{_op.location, _op.effect, {}, {}};
			if (_op.startParameter)
				ourOp.start = identifierNameIfSSA(_functionCall.arguments.at(*_op.startParameter));
			if (_op.lengthParameter)
				ourOp.length = identifierNameIfSSA(_functionCall.arguments.at(*_op.lengthParameter));
			if (_op.lengthConstant)
				switch (*_op.lengthConstant)
				{
				case 1: ourOp.length = YulString(one); break;
				case 32: ourOp.length = YulString(thirtyTwo); break;
				default: yulAssert(false);
				}
			return ourOp;
		}
	);
}

void UnusedStoreEliminator::applyOperation(UnusedStoreEliminator::Operation const& _operation)
{
	set<Statement const*> toRemove;
	set<Statement const*>& active =
		_operation.location == Location::Storage ?
		activeStorageStores() :
		activeMemoryStores();

	// TODO this loop could be done more efficiently - removing while iterating.
	for (Statement const* statement: active)
	{
		Operation const& storeOperation = m_storeOperations.at(statement);
		if (_operation.effect == Effect::Read && !knownUnrelated(storeOperation, _operation))
		{
			// This store is read from, mark it as used and remove it from the active set.
			m_usedStores.insert(statement);
			toRemove.insert(statement);
		}
		else if (_operation.effect == Effect::Write && knownCovered(storeOperation, _operation))
			// This store is overwritten before being read, remove it from the active set.
			toRemove.insert(statement);
	}
	active -= toRemove;
}

bool UnusedStoreEliminator::knownUnrelated(
	UnusedStoreEliminator::Operation const& _op1,
	UnusedStoreEliminator::Operation const& _op2
) const
{
	KnowledgeBase knowledge(m_dialect, [this](YulString _var) { return util::valueOrNullptr(m_ssaValues, _var); });

	if (_op1.location != _op2.location)
		return true;
	if (_op1.location == Location::Storage)
	{
		if (_op1.start && _op2.start)
		{
			yulAssert(
				_op1.length &&
				_op2.length &&
				knowledge.valueIfKnownConstant(*_op1.length) == 1 &&
				knowledge.valueIfKnownConstant(*_op2.length) == 1
			);
			return knowledge.knownToBeDifferent(*_op1.start, *_op2.start);
		}
	}
	else
	{
		yulAssert(_op1.location == Location::Memory, "");
		if (
			(_op1.length && knowledge.knownToBeZero(*_op1.length)) ||
			(_op2.length && knowledge.knownToBeZero(*_op2.length))
		)
			return true;

		if (_op1.start && _op1.length && _op2.start)
		{
			optional<u256> length1 = knowledge.valueIfKnownConstant(*_op1.length);
			optional<u256> start1 = knowledge.valueIfKnownConstant(*_op1.start);
			optional<u256> start2 = knowledge.valueIfKnownConstant(*_op2.start);
			if (
				(length1 && start1 && start2) &&
				*start1 + *length1 >= *start1 && // no overflow
				*start1 + *length1 <= *start2
			)
				return true;
		}
		if (_op2.start && _op2.length && _op1.start)
		{
			optional<u256> length2 = knowledge.valueIfKnownConstant(*_op2.length);
			optional<u256> start2 = knowledge.valueIfKnownConstant(*_op2.start);
			optional<u256> start1 = knowledge.valueIfKnownConstant(*_op1.start);
			if (
				(length2 && start2 && start1) &&
				*start2 + *length2 >= *start2 && // no overflow
				*start2 + *length2 <= *start1
			)
				return true;
		}

		if (_op1.start && _op1.length && _op2.start && _op2.length)
		{
			optional<u256> length1 = knowledge.valueIfKnownConstant(*_op1.length);
			optional<u256> length2 = knowledge.valueIfKnownConstant(*_op2.length);
			if (
				(length1 && *length1 <= 32) &&
				(length2 && *length2 <= 32) &&
				knowledge.knownToBeDifferentByAtLeast32(*_op1.start, *_op2.start)
			)
				return true;
		}
	}

	return false;
}

bool UnusedStoreEliminator::knownCovered(
	UnusedStoreEliminator::Operation const& _covered,
	UnusedStoreEliminator::Operation const& _covering
) const
{
	if (_covered.location != _covering.location)
		return false;
	if (
		(_covered.start && _covered.start == _covering.start) &&
		(_covered.length && _covered.length == _covering.length)
	)
		return true;
	if (_covered.location == Location::Memory)
	{
		KnowledgeBase knowledge(m_dialect, [this](YulString _var) { return util::valueOrNullptr(m_ssaValues, _var); });

		if (_covered.length && knowledge.knownToBeZero(*_covered.length))
			return true;

		// Condition (i = cover_i_ng, e = cover_e_d):
		// i.start <= e.start && e.start + e.length <= i.start + i.length
		if (!_covered.start || !_covering.start || !_covered.length || !_covering.length)
			return false;
		optional<u256> coveredLength = knowledge.valueIfKnownConstant(*_covered.length);
		optional<u256> coveringLength = knowledge.valueIfKnownConstant(*_covering.length);
		if (knowledge.knownToBeEqual(*_covered.start, *_covering.start))
			if (coveredLength && coveringLength && *coveredLength <= *coveringLength)
				return true;
		optional<u256> coveredStart = knowledge.valueIfKnownConstant(*_covered.start);
		optional<u256> coveringStart = knowledge.valueIfKnownConstant(*_covering.start);
		if (coveredStart && coveringStart && coveredLength && coveringLength)
			if (
				*coveringStart <= *coveredStart &&
				*coveringStart + *coveringLength >= *coveringStart && // no overflow
				*coveredStart + *coveredLength >= *coveredStart && // no overflow
				*coveredStart + *coveredLength <= *coveringStart + *coveringLength
			)
				return true;

		// TODO for this we probably need a non-overflow assumption as above.
		// Condition (i = cover_i_ng, e = cover_e_d):
		// i.start <= e.start && e.start + e.length <= i.start + i.length
	}
	return false;
}

void UnusedStoreEliminator::markActiveAsUsed(
	optional<UnusedStoreEliminator::Location> _onlyLocation
)
{
	if (_onlyLocation == nullopt || _onlyLocation == Location::Memory)
		for (Statement const* statement: activeMemoryStores())
			m_usedStores.insert(statement);
	if (_onlyLocation == nullopt || _onlyLocation == Location::Storage)
		for (Statement const* statement: activeStorageStores())
			m_usedStores.insert(statement);
	clearActive(_onlyLocation);
}

void UnusedStoreEliminator::clearActive(
	optional<UnusedStoreEliminator::Location> _onlyLocation
)
{
	if (_onlyLocation == nullopt || _onlyLocation == Location::Memory)
		activeMemoryStores() = {};
	if (_onlyLocation == nullopt || _onlyLocation == Location::Storage)
		activeStorageStores() = {};
}

optional<YulString> UnusedStoreEliminator::identifierNameIfSSA(Expression const& _expression) const
{
	if (Identifier const* identifier = get_if<Identifier>(&_expression))
		if (m_ssaValues.count(identifier->name))
			return {identifier->name};
	return nullopt;
}
