/*========================== begin_copyright_notice ============================

Copyright (c) 2020-2021 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

============================= end_copyright_notice ===========================*/

#include "VISADebugDecoder.hpp"

template <typename T>
static void PrintItems(llvm::raw_ostream& OS, const T& Items,
                       const char* Separator = " ") {
    bool First = true;
    std::for_each(Items.begin(), Items.end(),
        [&First, &OS, &Separator](const auto& Item) {

        if (!First)
            OS << Separator;
        else
            First = false;

        OS << "(";
        Item.print(OS);
        OS << ")";
    });
}
void IGC::DbgDecoder::Mapping::Register::print(llvm::raw_ostream& OS) const {
    OS << "RegMap<R#: " << regNum << ", Sub#:" << subRegNum << ">";
}
void IGC::DbgDecoder::Mapping::Memory::print(llvm::raw_ostream& OS) const {
    OS << "MemMap<" << ((isBaseOffBEFP == 1) ? "AbsBase(" : "BE_FP(") <<
        memoryOffset << ")>";
}
void IGC::DbgDecoder::VarAlloc::print(llvm::raw_ostream& OS) const {
    switch(virtualType) {
    case VarAlloc::VirTypeAddress: OS << "v:A->"; break;
    case VarAlloc::VirTypeFlag:    OS << "v:F->"; break;
    case VarAlloc::VirTypeGRF:     OS << "v:G->"; break;
    };
    switch(physicalType) {
    case VarAlloc::PhyTypeAddress: OS << "p:A !GRF"; break;
    case VarAlloc::PhyTypeFlag:    OS << "p:F !GRF"; break;
    case VarAlloc::PhyTypeGRF:     OS << "p:G ";
      mapping.r.print(OS);
    break;
    case VarAlloc::PhyTypeMemory:  OS << "p:M !GRF"; break;
    };
}
void IGC::DbgDecoder::LiveIntervalsVISA::print(llvm::raw_ostream& OS) const {
    OS << "LInt-V[" << start << ";" << end << "]";
    var.print(OS);
}
void IGC::DbgDecoder::VarInfo::print(llvm::raw_ostream& OS) const {
    OS << "{ " << name << " - ";
    PrintItems(OS, lrs, ", ");
    OS << " }";
}
void IGC::DbgDecoder::LiveIntervalGenISA::print(llvm::raw_ostream& OS) const {
    OS << "LInt-G[" << start << ";" << end << "] ";
    var.print(OS);
}
void IGC::DbgDecoder::SubroutineInfo::print(llvm::raw_ostream& OS) const {
    OS << "Name=" << name << " [" << startVISAIndex << ";" << endVISAIndex <<
        "), retvals: ";
    PrintItems(OS, retval, ", ");
}
void IGC::DbgDecoder::RegInfoMapping::print(llvm::raw_ostream& OS) const {
    OS << "srcRegOff: " << srcRegOff << ", " << numBytes << " bytes; ";
    if (dstInReg)
        dst.r.print(OS);
    else
        dst.m.print(OS);
}
void IGC::DbgDecoder::PhyRegSaveInfoPerIP::print(llvm::raw_ostream& OS) const {
    OS << "PhyR_SaveInfo: " << "IPOffset " << genIPOffset << ", numEntries " <<
      numEntries << "\n";
    OS << "   >RegInfoMapping: [";

    PrintItems(OS, data, ", ");
    OS << "   ]";
}
void IGC::DbgDecoder::CallFrameInfo::print(llvm::raw_ostream& OS) const {
  OS << "    frameSize: " << frameSize << "\n";
  OS << "    befpValid: " << befpValid << "\n";
  OS << "    callerbefpValid: " << callerbefpValid << "\n";
  OS << "    retAddrValid: " << retAddrValid << "\n";

  OS << "    befp list: [\n";
  PrintItems(OS, befp, "\n        ");
  OS << "    ]\n";

  OS << "    callerbefp list: [\n";
  PrintItems(OS, callerbefp, "\n        ");
  OS << "    ]\n";

  OS << "    retaddr list: [\n";
  PrintItems(OS, retAddr, "\n        ");
  OS << "    ]\n";

  OS << "    callee save entry list: [\n";
  PrintItems(OS, calleeSaveEntry, "\n        ");
  OS << "    ]\n";

  OS << "    caller save entry list: [\n";
  PrintItems(OS, callerSaveEntry, "\n        ");
  OS << "    ]\n";
}
void IGC::DbgDecoder::DbgInfoFormat::print(llvm::raw_ostream& OS) const {
    OS << "<VISADebugInfo>\n";
    OS << "Kernel: " << kernelName << "\n";
    OS << "RelocOffset: " << relocOffset << "\n";
    OS << "NumSubroutines: " << numSubRoutines << "\n";

    IGC_ASSERT(numSubRoutines == subs.size());
    OS << "Subroutines:\n    ";
    PrintItems(OS, subs, "\n    ");
    OS << "CFI: {\n";
    cfi.print(OS);
    OS << "  }\n";

    OS << "Vars:\n  ";
    PrintItems(OS, Vars, "\n  ");
    OS << "\nCisaIndex:\n";
    std::for_each(CISAIndexMap.begin(), CISAIndexMap.end(), [&OS](const auto& V) {
            auto VisaIndex = V.first;
            auto GenOff = V.second;
            OS << "  GI: " << GenOff << " -> VI: " << VisaIndex << "\n";
        });
    OS << "</VISADebugInfo>";
}
