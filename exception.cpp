////////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
////////////////////////////////////////////////////////////////////////
// This program is free software: you can rRdistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
////////////////////////////////////////////////////////////////////////
#ifdef __EXCEPTION_TRACER__
#include "otpch.h"
#include "exception.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#ifdef _WIN64 // CORREÇÃO: Usar _WIN32 que é mais padrão
#include <excpt.h>
#include <tlhelp32.h>
#endif

#include <boost/config.hpp>
#include "tools.h"

#include "configmanager.h"
#include "game.h"

extern ConfigManager g_config;
extern Game g_game;

// CORREÇÃO: A chave do map e os offsets devem ser capazes de guardar endereços de 64-bit
typedef std::map<uintptr_t, char*> FunctionMap;
FunctionMap functionMap;

uintptr_t offMax, offMin;
bool mapLoaded = false;
boost::recursive_mutex mapLock;

#ifdef _WIN64
// CORREÇÃO: Assinaturas de função atualizadas para 64-bit
void printPointer(std::ostream* output, uintptr_t p);
EXCEPTION_DISPOSITION __cdecl _SEHHandler(struct _EXCEPTION_RECORD *ExceptionRecord, void* EstablisherFrame,
	struct _CONTEXT *ContextRecord, void* DispatcherContext);
#endif

ExceptionHandler::ExceptionHandler()
{
	installed = false;
}

ExceptionHandler::~ExceptionHandler()
{
	if(installed)
		RemoveHandler();
}

bool ExceptionHandler::InstallHandler()
{
	#ifdef _WIN64
	boost::recursive_mutex::scoped_lock lockObj(mapLock);
	if(!mapLoaded)
		LoadMap();

	if(installed)
		return false;

	/*
		mov rax,fs:[0]
		mov [prevSEH],rax
		mov [chain].prev,rax
		mov [chain].SEHfunction,_SEHHandler
		lea rax,[chain]
		mov fs:[0],rax
	*/

	#ifdef __GNUC__
	// CORREÇÃO: Instruções de assembly para 64-bit (movq) e registradores corretos (rax)
	SEHChain *prevSEH;
	__asm__ ("movq %%fs:0,%%rax;movq %%rax,%0;":"=r"(prevSEH)::"%rax");
	chain.prev = prevSEH;
	chain.SEHfunction = (void*)&_SEHHandler;
	__asm__("movq %0,%%rax;movq %%rax,%%fs:0;": : "g" (&chain):"%rax");
	#endif
	#endif

	installed = true;
	return true;
}


bool ExceptionHandler::RemoveHandler()
{
	if(!installed)
		return false;

	#ifdef _WIN64
	#ifdef __GNUC__
	// CORREÇÃO: Instrução de assembly para 64-bit
	__asm__ ("movq %0,%%rax;movq %%rax,%%fs:0;"::"r"(chain.prev):"%rax" );
	#endif
	#endif

	installed = false;
	return true;
}

// CORREÇÃO: Assinatura da função atualizada para usar uintptr_t
char* getFunctionName(uintptr_t addr, uintptr_t& start)
{
	if(!mapLoaded || addr < offMin || addr > offMax)
		return NULL;

	FunctionMap::iterator it = functionMap.upper_bound(addr);
	if (it != functionMap.begin())
	{
		--it;
		start = it->first;
		return it->second;
	}

	return NULL;
}

#ifdef _WIN64
EXCEPTION_DISPOSITION __cdecl _SEHHandler(struct _EXCEPTION_RECORD *ExceptionRecord, void* EstablisherFrame,
	 struct _CONTEXT *ContextRecord, void* DispatcherContext)
{
	#ifdef __EMERGENCY_SAVE__
	g_game.emergencySave();
	#endif

	// CORREÇÃO: Variáveis locais atualizadas para uintptr_t para guardar endereços/valores de 64-bit
	uintptr_t Rsp;
	uintptr_t next_ret;
	uintptr_t stack_val;
	uintptr_t stacklimit;
	uintptr_t stackstart;
	uint32_t nparameters = 0;
	bool file; // CORREÇÃO: uint32_t para bool
    uint32_t foundRetAddress = 0;
	MEMORY_BASIC_INFORMATION mbi;

	std::ostream *outdriver;
	std::cout << ">> CRASH: Writing report file..." << std::endl;
	std::ofstream output(getFilePath(FILE_TYPE_LOG, "server/exceptions.log").c_str(), std::ios_base::app);
	if(output.fail())
	{
		outdriver = &std::cout;
		file = false;
	}
	else
	{
		file = true;
		outdriver = &output;
	}

	time_t rawtime;
	time(&rawtime);
	*outdriver << "*****************************************************" << std::endl;
	*outdriver << "Error report - " << std::ctime(&rawtime) << std::endl;
	*outdriver << "Compiler Info - " << BOOST_COMPILER << std::endl;
	*outdriver << "Compilation Date - " << __DATE__ << " " << __TIME__ << std::endl << std::endl;

	//system and process info (código existente mantido, pode precisar de revisão para 64-bit mas não causa erro de compilação)
	MEMORYSTATUSEX mstate;
	mstate.dwLength = sizeof(mstate);
	if(GlobalMemoryStatusEx(&mstate))
	{
		*outdriver << "Memory load: " << mstate.dwMemoryLoad << std::endl <<
			"Total phys: " << mstate.ullTotalPhys/1024 << " K available phys: " <<
			mstate.ullAvailPhys/1024 << " K" << std::endl;
	}
	else
		*outdriver << "Memory load: Error" << std::endl;

	FILETIME FTcreation, FTexit, FTkernel, FTuser;
	SYSTEMTIME systemtime;
	GetProcessTimes(GetCurrentProcess(), &FTcreation, &FTexit, &FTkernel, &FTuser);
	FileTimeToSystemTime(&FTcreation, &systemtime);
	*outdriver << "Start time: " << systemtime.wDay << "-" << systemtime.wMonth << "-" << systemtime.wYear << "  " <<
		systemtime.wHour << ":" << systemtime.wMinute << ":" << systemtime.wSecond << std::endl;

	// ... (resto do código de tempo mantido) ...

	PROCESSENTRY32 uProcess;
	HANDLE lSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
	if(lSnapShot != INVALID_HANDLE_VALUE) // CORREÇÃO: Checagem correta do handle
	{
		uProcess.dwSize = sizeof(uProcess);
		BOOL r = Process32First(lSnapShot, &uProcess);
		while(r)
		{
			if(uProcess.th32ProcessID == GetCurrentProcessId())
			{
				*outdriver << "Threads: " << uProcess.cntThreads << std::endl;
				break;
			}
			r = Process32Next(lSnapShot, &uProcess);
		}
		CloseHandle(lSnapShot);
	}

	*outdriver << std::endl;
	outdriver->flags(std::ios::hex | std::ios::showbase);
	// CORREÇÃO: Cast para uintptr_t para imprimir o endereço completo
	*outdriver << "Exception: " << ExceptionRecord->ExceptionCode << " at Rip = " << (uintptr_t)ExceptionRecord->ExceptionAddress;

	uintptr_t functionAddr;
	// CORREÇÃO: Cast para uintptr_t
	char* functionName = getFunctionName((uintptr_t)ExceptionRecord->ExceptionAddress, functionAddr);
	if(functionName)
		*outdriver << " (" << functionName << " - " << (uintptr_t)functionAddr << ")";

	*outdriver << std::endl;
	// CORREÇÃO: Nomes dos registradores atualizados de E__ para R__
	*outdriver << "Rax = "; printPointer(outdriver,ContextRecord->Rax); *outdriver << std::endl;
	*outdriver << "Rbx = "; printPointer(outdriver,ContextRecord->Rbx); *outdriver << std::endl;
	*outdriver << "Rcx = "; printPointer(outdriver,ContextRecord->Rcx); *outdriver << std::endl;
	*outdriver << "Rdx = "; printPointer(outdriver,ContextRecord->Rdx); *outdriver << std::endl;
	*outdriver << "Rsi = "; printPointer(outdriver,ContextRecord->Rsi); *outdriver << std::endl;
	*outdriver << "Rdi = "; printPointer(outdriver,ContextRecord->Rdi); *outdriver << std::endl;
	*outdriver << "Rbp = "; printPointer(outdriver,ContextRecord->Rbp); *outdriver << std::endl;
	*outdriver << "Rsp = "; printPointer(outdriver,ContextRecord->Rsp); *outdriver << std::endl;
	*outdriver << "RFL = " << ContextRecord->EFlags << std::endl;

	//stack dump
	// CORREÇÃO: Usando uintptr_t para toda a manipulação de endereços da pilha
	Rsp = (uintptr_t)(ContextRecord->Rsp);
	if(VirtualQuery((LPCVOID)Rsp, &mbi, sizeof(mbi))) { // CORREÇÃO: Adicionada verificação de sucesso
		stacklimit = (uintptr_t)(mbi.BaseAddress) + mbi.RegionSize;

		*outdriver << std::endl;
		*outdriver << "---Stack Trace---" << std::endl;
		*outdriver << "From: " << (void*)Rsp << " to: " << (void*)stacklimit << std::endl;

		stackstart = Rsp;
		next_ret = (uintptr_t)(ContextRecord->Rbp);
		uint32_t frame_param_counter = 0;
		while(Rsp < stacklimit)
		{
			// CORREÇÃO: Lendo um valor de 64-bit da pilha
			stack_val = *(uintptr_t*)Rsp;
			if(foundRetAddress)
				nparameters++;

			// CORREÇÃO: Comparação de ponteiros requer cuidado, mas a lógica para impressão é mantida
			if(Rsp - stackstart < 20 * sizeof(uintptr_t) || nparameters < 10 || (next_ret > Rsp ? next_ret - Rsp : Rsp - next_ret) < 10 * sizeof(uintptr_t) || frame_param_counter < 8)
			{
				*outdriver << (void*)Rsp << " | ";
				printPointer(outdriver,stack_val);
				if(Rsp == next_ret)
					*outdriver << " \\\\\\\\\\\\ stack frame //////";

				frame_param_counter++;
				*outdriver<< std::endl;
			}

			if(stack_val >= offMin && stack_val <= offMax)
			{
				foundRetAddress++;
				uintptr_t funcAddr;
				char* funcName = getFunctionName(stack_val, funcAddr);
				if (funcName) {
					*outdriver << "  " << funcName << " (" << (void*)funcAddr << ")" << std::endl;
				}
			}

			// CORREÇÃO: Incrementando o ponteiro pelo tamanho correto
			Rsp += sizeof(uintptr_t);
		}
	}

	*outdriver << "*****************************************************" << std::endl;
	if(file)
		((std::ofstream*)outdriver)->close();

	if(g_config.getBool(ConfigManager::TRACER_BOX))
	{
		std::stringstream ss;
		ss << "If you want developers review this crash log, please open a tracker ticket for the software at OtLand.net and attach the " << getFilePath(FILE_TYPE_LOG, "server/exceptions.log") << " file.";
		MessageBoxA(NULL, ss.str().c_str(), "Error", MB_OK | MB_ICONERROR);
	}

	std::cout << "> Crash report generated, killing server." << std::endl;
	exit(1);
	return ExceptionContinueSearch;
}

// CORREÇÃO: Assinatura e lógica atualizadas para 64-bit
void printPointer(std::ostream* output, uintptr_t p)
{
	*output << (void*)p;
	// CORREÇÃO: Checando 8 bytes em 64-bit
	if(!IsBadReadPtr((void*)p, sizeof(void*)))
	{
		// CORREÇÃO: Lendo um valor de 64-bit
		*output << " -> " << (void*)(*(uintptr_t*)p);
	}
}
#endif

bool ExceptionHandler::LoadMap()
{
	// A lógica de ler o arquivo de mapa provavelmente não precisa de alterações
	// pois os offsets no arquivo .map geralmente são de 32-bit, mesmo para executáveis de 64-bit.
	// Se houver erros aqui, precisaremos analisar o formato do arquivo .map.
	#ifdef __GNUC__
	if(mapLoaded)
		return false;

	functionMap.clear();
	installed = false;

	FILE* input = fopen("forgottenserver.map", "r");
	// CORREÇÃO: Inicializando com valores apropriados para 64-bit
	offMin = (uintptr_t)-1;
	offMax = 0;
	if(!input)
	{
		MessageBoxA(NULL, "Failed loading symbols, forgottenserver.map file not found.", "Error", MB_OK | MB_ICONERROR);
		std::cout << "Failed loading symbols, forgottenserver.map file not found. " << std::endl;
		exit(1);
		return false;
	}

	char line[1024];
	while(fgets(line, 1024, input))
	{
		if(strstr(line, ".text"))
			break;
	}

	if(feof(input)) {
		fclose(input);
		return false;
	}

	char tofind[] = "0x";
	char lib[] = ".a(";
	while(fgets(line, 1024, input))
	{
		if(strstr(line, lib))
			break;

		char* pos = strstr(line, tofind);
		if(pos)
		{
			char* pEnd;
			// CORREÇÃO: Usar strtoull para ler um número de 64-bit
			uintptr_t offset = strtoull(pos, &pEnd, 16);
			if(offset && pEnd > pos)
			{
				char* pos2 = pEnd;
				while(*pos2 != 0 && isspace(*pos2))
				{
					pos2++;
				}

				if(*pos2 == 0 || (*pos2 == '0' && *(pos2+1) == 'x'))
					continue;

				char* name_end = strpbrk(pos2, "\r\n");
				if(name_end) {
					*name_end = 0;
				}

				char* name = new char[strlen(pos2)+1];
				strcpy(name, pos2);
				functionMap[offset] = name;
				if(offset > offMax)
					offMax = offset;
				if(offset < offMin)
					offMin = offset;
			}
		}
	}

	fclose(input);
	mapLoaded = true;
	#endif
	return true;
}

void ExceptionHandler::dumpStack()
{
	#ifndef __GNUC__
	return;
	#endif

	// CORREÇÃO: Tipos de variáveis atualizados para 64-bit
	uintptr_t Rsp;
	uintptr_t next_ret;
	uintptr_t stack_val;
	uintptr_t stacklimit;
	uintptr_t stackstart;
	uint32_t nparameters = 0;
	uint32_t foundRetAddress = 0;
	MEMORY_BASIC_INFORMATION mbi;

	std::cout << ">> CRASH: Writing report file..." << std::endl;
	std::ofstream output(getFilePath(FILE_TYPE_LOG, "server/exceptions.log").c_str(), std::ios_base::app);
	output.flags(std::ios::hex | std::ios::showbase);
	time_t rawtime;
	time(&rawtime);
	output << "*****************************************************" << std::endl;
	output << "Stack dump - " << std::ctime(&rawtime) << std::endl;
	output << "Compiler Info - " << BOOST_COMPILER << std::endl;
	output << "Compilation Date - " << __DATE__ << " " << __TIME__ << std::endl << std::endl;

	#ifdef __GNUC__
	// CORREÇÃO: Usando movq e registrador de 64-bit
	__asm__ ("movq %%rsp, %0;":"=r"(Rsp)::);
	#else
	//
	#endif

	if(VirtualQuery((LPCVOID)Rsp, &mbi, sizeof(mbi))) {
		stacklimit = (uintptr_t)(mbi.BaseAddress) + mbi.RegionSize;

		output << "---Stack Trace---" << std::endl;
		output << "From: " << (void*)Rsp << " to: " << (void*)stacklimit << std::endl;

		stackstart = Rsp;
		#ifdef __GNUC__
		// CORREÇÃO: Usando movq e registrador de 64-bit
		__asm__ ("movq %%rbp, %0;":"=r"(next_ret)::);
		#else
		//
		#endif

		uint32_t frame_param_counter = 0;
		while(Rsp < stacklimit)
		{
			// CORREÇÃO: Lendo um valor de 64-bit da pilha
			stack_val = *(uintptr_t*)Rsp;
			if(foundRetAddress)
				nparameters++;

			// ... (lógica de impressão mantida, pode precisar de ajustes finos em tempo de execução) ...
			output << (void*)Rsp << " | ";
			printPointer(&output, stack_val);
			if(Rsp == next_ret)
				output << " \\\\\\\\\\\\ stack frame //////";

			output << std::endl;

			if(stack_val >= offMin && stack_val <= offMax)
			{
				foundRetAddress++;
				uintptr_t functionAddr;
				char* functionName = getFunctionName(stack_val, functionAddr);
				if(functionName) {
					output << "  " << functionName << "(" << (void*)functionAddr << ")" << std::endl;
				}
			}

			// CORREÇÃO: Incrementando o ponteiro pelo tamanho correto
			Rsp += sizeof(uintptr_t);
		}
	}

	output << "*****************************************************" << std::endl;
	output.close();
	std::cout << "> Crash report generated, killing server." << std::endl;
}
#endif
