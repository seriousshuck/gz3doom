
#include "jit.h"
#include "jitintern.h"

static asmjit::JitRuntime *jit;
static int jitRefCount = 0;

asmjit::JitRuntime *JitGetRuntime()
{
	if (!jit)
		jit = new asmjit::JitRuntime;
	jitRefCount++;
	return jit;
}

void JitCleanUp(VMScriptFunction *func)
{
	jitRefCount--;
	if (jitRefCount == 0)
	{
		delete jit;
		jit = nullptr;
	}
}

static void OutputJitLog(const asmjit::StringLogger &logger)
{
	// Write line by line since I_FatalError seems to cut off long strings
	const char *pos = logger.getString();
	const char *end = pos;
	while (*end)
	{
		if (*end == '\n')
		{
			FString substr(pos, (int)(ptrdiff_t)(end - pos));
			Printf("%s\n", substr.GetChars());
			pos = end + 1;
		}
		end++;
	}
	if (pos != end)
		Printf("%s\n", pos);
}

//#define DEBUG_JIT

JitFuncPtr JitCompile(VMScriptFunction *sfunc)
{
#if defined(DEBUG_JIT)
	if (strcmp(sfunc->PrintableName.GetChars(), "StatusScreen.drawNum") != 0)
		return nullptr;
#endif

	//Printf("Jitting function: %s\n", sfunc->PrintableName.GetChars());

	using namespace asmjit;
	StringLogger logger;
	try
	{
		auto *jit = JitGetRuntime();

		ThrowingErrorHandler errorHandler;
		CodeHolder code;
		code.init(jit->getCodeInfo());
		code.setErrorHandler(&errorHandler);
		code.setLogger(&logger);

		JitCompiler compiler(&code, sfunc);
		compiler.Codegen();

		JitFuncPtr fn = nullptr;
		Error err = jit->add(&fn, &code);
		if (err)
			I_FatalError("JitRuntime::add failed: %d", err);

#if defined(DEBUG_JIT)
		OutputJitLog(logger);
#endif

		return fn;
	}
	catch (const std::exception &e)
	{
		OutputJitLog(logger);
		I_FatalError("Unexpected JIT error: %s\n", e.what());
		return nullptr;
	}
}

/////////////////////////////////////////////////////////////////////////////

static const char *OpNames[NUM_OPS] =
{
#define xx(op, name, mode, alt, kreg, ktype)	#op,
#include "vmops.h"
#undef xx
};

void JitCompiler::Codegen()
{
	Setup();

	pc = sfunc->Code;
	auto end = pc + sfunc->CodeSize;
	while (pc != end)
	{
		int i = (int)(ptrdiff_t)(pc - sfunc->Code);
		op = pc->op;

		cc.bind(labels[i]);

		ResetTemp();

		FString lineinfo;
		lineinfo.Format("; %s(line %d): %02x%02x%02x%02x %s", sfunc->PrintableName.GetChars(), sfunc->PCToLine(pc), pc->op, pc->a, pc->b, pc->c, OpNames[op]);
		cc.comment(lineinfo.GetChars(), lineinfo.Len());

		EmitOpcode();

		pc++;
	}

	cc.endFunc();
	cc.finalize();
}

void JitCompiler::EmitOpcode()
{
	switch (op)
	{
		#define xx(op, name, mode, alt, kreg, ktype)	case OP_##op: Emit##op(); break;
		#include "vmops.h"
		#undef xx

	default:
		I_FatalError("JIT error: Unknown VM opcode %d\n", op);
		break;
	}
}

void JitCompiler::Setup()
{
	using namespace asmjit;

	ResetTemp();

	FString funcname;
	funcname.Format("Function: %s", sfunc->PrintableName.GetChars());
	cc.comment(funcname.GetChars(), funcname.Len());

	args = cc.newIntPtr("args"); // VMValue *params
	numargs = cc.newInt32("numargs"); // int numargs
	ret = cc.newIntPtr("ret"); // VMReturn *ret
	numret = cc.newInt32("numret"); // int numret
	exceptInfo = cc.newIntPtr("exceptinfo"); // JitExceptionInfo *exceptInfo

	cc.addFunc(FuncSignature5<int, void *, int, void *, int, void *>());
	cc.setArg(0, args);
	cc.setArg(1, numargs);
	cc.setArg(2, ret);
	cc.setArg(3, numret);
	cc.setArg(4, exceptInfo);

	auto stackalloc = cc.newStack(sizeof(VMReturn) * MAX_RETURNS, alignof(VMReturn));
	callReturns = cc.newIntPtr("callReturns");
	cc.lea(callReturns, stackalloc);

	konstd = sfunc->KonstD;
	konstf = sfunc->KonstF;
	konsts = sfunc->KonstS;
	konsta = sfunc->KonstA;

	regD.Resize(sfunc->NumRegD);
	regF.Resize(sfunc->NumRegF);
	regA.Resize(sfunc->NumRegA);
	regS.Resize(sfunc->NumRegS);

	frameD = cc.newIntPtr();
	frameF = cc.newIntPtr();
	frameS = cc.newIntPtr();
	frameA = cc.newIntPtr();
	params = cc.newIntPtr();

	// the VM version reads this from the stack, but it is constant data
	int offsetParams = ((int)sizeof(VMFrame) + 15) & ~15;
	int offsetF = offsetParams + (int)(sfunc->MaxParam * sizeof(VMValue));
	int offsetS = offsetF + (int)(sfunc->NumRegF * sizeof(double));
	int offsetA = offsetS + (int)(sfunc->NumRegS * sizeof(FString));
	int offsetD = offsetA + (int)(sfunc->NumRegA * sizeof(void*));
	offsetExtra = (offsetD + (int)(sfunc->NumRegD * sizeof(int32_t)) + 15) & ~15;

	stack = cc.newIntPtr("stack");
	auto allocFrame = CreateCall<VMFrameStack *, VMScriptFunction *, VMValue *, int, JitExceptionInfo *>([](VMScriptFunction *func, VMValue *args, int numargs, JitExceptionInfo *exceptinfo) -> VMFrameStack* {
		try
		{
			VMFrameStack *stack = &GlobalVMStack;
			VMFrame *newf = stack->AllocFrame(func);
			VMFillParams(args, newf, numargs);
			return stack;
		}
		catch (...)
		{
			exceptinfo->reason = X_OTHER;
			exceptinfo->cppException = std::current_exception();
			return nullptr;
		}
	});
	allocFrame->setRet(0, stack);
	allocFrame->setArg(0, imm_ptr(sfunc));
	allocFrame->setArg(1, args);
	allocFrame->setArg(2, numargs);
	allocFrame->setArg(3, exceptInfo);
	EmitCheckForException();

	vmframe = cc.newIntPtr();
	cc.mov(vmframe, x86::ptr(stack)); // stack->Blocks
	cc.mov(vmframe, x86::ptr(vmframe, VMFrameStack::OffsetLastFrame())); // Blocks->LastFrame

	cc.lea(params, x86::ptr(vmframe, offsetParams));
	cc.lea(frameF, x86::ptr(vmframe, offsetF));
	cc.lea(frameS, x86::ptr(vmframe, offsetS));
	cc.lea(frameA, x86::ptr(vmframe, offsetA));
	cc.lea(frameD, x86::ptr(vmframe, offsetD));

	for (int i = 0; i < sfunc->NumRegD; i++)
	{
		FString regname;
		regname.Format("regD%d", i);
		regD[i] = cc.newInt32(regname.GetChars());
		cc.mov(regD[i], x86::dword_ptr(frameD, i * sizeof(int32_t)));
	}

	for (int i = 0; i < sfunc->NumRegF; i++)
	{
		FString regname;
		regname.Format("regF%d", i);
		regF[i] = cc.newXmmSd(regname.GetChars());
		cc.movsd(regF[i], x86::qword_ptr(frameF, i * sizeof(double)));
	}

	for (int i = 0; i < sfunc->NumRegS; i++)
	{
		FString regname;
		regname.Format("regS%d", i);
		regS[i] = cc.newIntPtr(regname.GetChars());
		cc.lea(regS[i], x86::ptr(frameS, i * sizeof(FString)));
	}

	for (int i = 0; i < sfunc->NumRegA; i++)
	{
		FString regname;
		regname.Format("regA%d", i);
		regA[i] = cc.newIntPtr(regname.GetChars());
		cc.mov(regA[i], x86::ptr(frameA, i * sizeof(void*)));
	}

	int size = sfunc->CodeSize;
	labels.Resize(size);
	for (int i = 0; i < size; i++) labels[i] = cc.newLabel();
}

void JitCompiler::EmitPopFrame()
{
	auto popFrame = CreateCall<void, VMFrameStack *, JitExceptionInfo *>([](VMFrameStack *stack, JitExceptionInfo *exceptinfo) {
		try
		{
			stack->PopFrame();
		}
		catch (...)
		{
			exceptinfo->reason = X_OTHER;
			exceptinfo->cppException = std::current_exception();
		}
	});
	popFrame->setArg(0, stack);
	popFrame->setArg(1, exceptInfo);
}

void JitCompiler::EmitNullPointerThrow(int index, EVMAbortException reason)
{
	auto label = cc.newLabel();
	cc.test(regA[index], regA[index]);
	cc.jne(label);
	EmitThrowException(reason);
	cc.bind(label);
}

void JitCompiler::EmitThrowException(EVMAbortException reason)
{
	using namespace asmjit;

	// Update JitExceptionInfo struct
	cc.mov(x86::dword_ptr(exceptInfo, 0 * 4), (int32_t)reason);
	if (cc.is64Bit())
		cc.mov(x86::qword_ptr(exceptInfo, 4 * 4), imm_ptr(pc));
	else
		cc.mov(x86::dword_ptr(exceptInfo, 4 * 4), imm_ptr(pc));

	// Return from function
	EmitPopFrame();
	X86Gp vReg = newTempInt32();
	cc.mov(vReg, 0);
	cc.ret(vReg);
}

void JitCompiler::EmitThrowException(EVMAbortException reason, asmjit::X86Gp arg1)
{
	using namespace asmjit;

	// Update JitExceptionInfo struct
	cc.mov(x86::dword_ptr(exceptInfo, 0 * 4), (int32_t)reason);
	cc.mov(x86::dword_ptr(exceptInfo, 1 * 4), arg1);
	if (cc.is64Bit())
		cc.mov(x86::qword_ptr(exceptInfo, 4 * 4), imm_ptr(pc));
	else
		cc.mov(x86::dword_ptr(exceptInfo, 4 * 4), imm_ptr(pc));

	// Return from function
	EmitPopFrame();
	X86Gp vReg = newTempInt32();
	cc.mov(vReg, 0);
	cc.ret(vReg);
}

void JitCompiler::EmitCheckForException()
{
	auto noexception = cc.newLabel();
	auto exceptResult = newTempInt32();
	cc.mov(exceptResult, asmjit::x86::dword_ptr(exceptInfo, 0 * 4));
	cc.cmp(exceptResult, (int)-1);
	cc.je(noexception);
	EmitPopFrame();
	asmjit::X86Gp vReg = newTempInt32();
	cc.mov(vReg, 0);
	cc.ret(vReg);
	cc.bind(noexception);
}

asmjit::X86Gp JitCompiler::CheckRegD(int r0, int r1)
{
	if (r0 != r1)
	{
		return regD[r0];
	}
	else
	{
		auto copy = newTempInt32();
		cc.mov(copy, regD[r0]);
		return copy;
	}
}

asmjit::X86Xmm JitCompiler::CheckRegF(int r0, int r1)
{
	if (r0 != r1)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempXmmSd();
		cc.movsd(copy, regF[r0]);
		return copy;
	}
}

asmjit::X86Xmm JitCompiler::CheckRegF(int r0, int r1, int r2)
{
	if (r0 != r1 && r0 != r2)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempXmmSd();
		cc.movsd(copy, regF[r0]);
		return copy;
	}
}

asmjit::X86Xmm JitCompiler::CheckRegF(int r0, int r1, int r2, int r3)
{
	if (r0 != r1 && r0 != r2 && r0 != r3)
	{
		return regF[r0];
	}
	else
	{
		auto copy = newTempXmmSd();
		cc.movsd(copy, regF[r0]);
		return copy;
	}
}

asmjit::X86Gp JitCompiler::CheckRegS(int r0, int r1)
{
	if (r0 != r1)
	{
		return regS[r0];
	}
	else
	{
		auto copy = newTempIntPtr();
		cc.mov(copy, regS[r0]);
		return copy;
	}
}

asmjit::X86Gp JitCompiler::CheckRegA(int r0, int r1)
{
	if (r0 != r1)
	{
		return regA[r0];
	}
	else
	{
		auto copy = newTempIntPtr();
		cc.mov(copy, regA[r0]);
		return copy;
	}
}

void JitCompiler::EmitNOP()
{
	cc.nop();
}