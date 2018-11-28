/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include "FunctionUpgrader.h"

using namespace llvm;

void FunctionUpgrader::SetFunctionToUpgrade(llvm::Function* pFunction)
{
	m_pFunction = pFunction;
}

void FunctionUpgrader::Clean()
{
	m_pFunction = nullptr;
	m_pNewArguments.clear();
}

llvm::Value* FunctionUpgrader::AddArgument(llvm::StringRef argName, llvm::Type* argType)
{
	// Get from first basic block first instruction
	auto firstInstInFuncOld = m_pFunction->begin()->begin();
	// Create place holder alloc for the arg purposes
	auto pPlaceHolderArgAlloc = new llvm::AllocaInst(argType, 0, "", &*firstInstInFuncOld);
	// Create place holder load, which will simulate the argument for now
	auto PlaceHolderArg = new llvm::LoadInst(pPlaceHolderArgAlloc, argName, pPlaceHolderArgAlloc->getNextNode());

	m_pNewArguments[PlaceHolderArg] = nullptr;

	return PlaceHolderArg;
}

bool FunctionUpgrader::IsUsedPlacedHolder(llvm::Value* PlaceHolderToCheck)
{
	return m_pNewArguments.find((LoadInst*)PlaceHolderToCheck) != m_pNewArguments.end();
}

Argument* FunctionUpgrader::GetArgumentFromRebuild(llvm::Value* pPlaceHolderArg)
{
	return GetArgumentFromRebuild((LoadInst*)pPlaceHolderArg);
}

Argument* FunctionUpgrader::GetArgumentFromRebuild(llvm::LoadInst* pPlaceHolderArg)
{
	if (IsUsedPlacedHolder(pPlaceHolderArg))
	{
		if (m_pNewArguments[pPlaceHolderArg] != nullptr)
		{
			return m_pNewArguments[pPlaceHolderArg];
		}
		else
		{
			assert(false && "There is no created new argument, did you call RebuildFunction?");
			return nullptr;
		}
	}
	else
	{
		assert(false && "Didn't found new argument!");
		return nullptr;
	}
}

Function* FunctionUpgrader::RebuildFunction()
{
	Function* pFunctionRebuild = UpgradeFunctionWithNewArgs();

	auto i_arg_old = m_pFunction->arg_begin();
	auto i_arg_new = pFunctionRebuild->arg_begin();

	// Replace all usage in new method from old arguments to new arguments
	while(i_arg_old != m_pFunction->arg_end())
	{
#if LLVM_VERSION_MAJOR == 4
		i_arg_old->replaceAllUsesWith(&*i_arg_new);
#elif LLVM_VERSION_MAJOR == 7
		i_arg_old->replaceAllUsesWith(i_arg_new);
#endif
		++i_arg_old;
		++i_arg_new;
	}

	// Replace all usage in new method from place holder to real argument
	for (auto it = m_pNewArguments.begin(); it != m_pNewArguments.end(); ++it)
	{
		it->first->replaceAllUsesWith(it->second);
	}

	CleanPlaceHoldersArgs();

	pFunctionRebuild->copyAttributesFrom(m_pFunction);
	pFunctionRebuild->setSubprogram(m_pFunction->getSubprogram());
	m_pFunction->setSubprogram(nullptr);
	pFunctionRebuild->takeName(m_pFunction);

	return pFunctionRebuild;
}

FunctionType* FunctionUpgrader::UpgradeFunctionTypeWithNewArgs()
{
	std::vector<Type*> args;

	// Get from old function all arguments type
	for (auto i = m_pFunction->arg_begin(); i != m_pFunction->arg_end(); ++i)
	{
		args.push_back(i->getType());
	}

	// Append new list arguments for new function
	for (auto it = m_pNewArguments.begin(); it != m_pNewArguments.end(); ++it)
	{
		args.push_back(it->first->getType());
	}

	return FunctionType::get(m_pFunction->getReturnType(), args, m_pFunction->isVarArg());
}

Function* FunctionUpgrader::UpgradeFunctionWithNewArgs()
{
	auto fType = UpgradeFunctionTypeWithNewArgs();
	auto fModule = m_pFunction->getParent();
	auto fName = m_pFunction->getName();
	auto fLinkage = m_pFunction->getLinkage();

	// Create a new function 
	Function* pNewFunc =
		Function::Create(
			fType,
			fLinkage,
			fName);

	fModule->getFunctionList().insert(m_pFunction->getIterator(), pNewFunc);

	// rename all args
	auto i_arg_old = m_pFunction->arg_begin();
	auto i_arg_new = pNewFunc->arg_begin();

	// Map old -> new args
	// and setname for new func args from old func args
	for (; i_arg_old != m_pFunction->arg_end(); ++i_arg_old, ++i_arg_new)
	{
#if LLVM_VERSION_MAJOR == 4
		auto arg_it = &*i_arg_old;
#elif LLVM_VERSION_MAJOR == 7
		auto arg_it = i_arg_old;
#endif
		i_arg_new->takeName(arg_it);
	}
	// setname for new func args which was added as new one
	for (auto it = m_pNewArguments.begin(); it != m_pNewArguments.end(); ++it)
	{
#if LLVM_VERSION_MAJOR == 4
		auto arg_it = &*i_arg_new;
#elif LLVM_VERSION_MAJOR == 7
		auto arg_it = i_arg_new;
#endif
		//add to map pointer of the new argument
		m_pNewArguments[it->first] = arg_it;
		
		m_pNewArguments[it->first]->takeName(it->first);

		++i_arg_new;
	}

	pNewFunc->getBasicBlockList().splice(pNewFunc->begin(), m_pFunction->getBasicBlockList());

	return pNewFunc;
}

void FunctionUpgrader::CleanPlaceHoldersArgs()
{
	for (auto it = m_pNewArguments.begin(); it != m_pNewArguments.end(); ++it)
	{
		// Remove all place holder stuff from new func
		LoadInst* pPlaceHolderArg = it->first;
		AllocaInst* pPlaceHolderArgAlloc = cast<AllocaInst>(pPlaceHolderArg->getPointerOperand());
		
		pPlaceHolderArg->eraseFromParent();
		pPlaceHolderArgAlloc->eraseFromParent();
	}
}

int FunctionUpgrader::SizeArgFromRebuild()
{
	return m_pNewArguments.size();
}

bool FunctionUpgrader::NeedToRebuild()
{
	return m_pNewArguments.size() > 0;
}