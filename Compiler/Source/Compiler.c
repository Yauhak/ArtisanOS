// 该部分代码由DeepSeek优化而来
// 原始的编译器代码太极吧丑了
// 现在看起来清爽多了
// 我就再完善完善注释部分好了 :-)
#include "Compiler.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------- 全局编译环境 ----------
static unsigned char codePool[8192] = {0};
static unsigned char *ptr = codePool + 4;	// 首4字节存放main入口
static unsigned char *funcHead = NULL;	// 用于存储任意函数程序头部的位置
static int entryPoint = 0;	// main入口地址
static char needMemInit = 0;	// 指示符，表明是否需要为函数程序进行内存初始化
static char inMemSection = 0;	// 指示符，标明当前解析代码是否在mem...end_mem结构内
static char currentFuncName[32] = {0};	// 存储当前解析的函数程序名

static Symbol symbols[MAX_SYMBOLS];	// 符号（变量）表
static int symbolCount = 0;	// 符号（变量）数量
static int totalVarLen = 0;	// 当前函数程序所需的总运行内存

static CompilerState state = {0};	// 全局编译器状态
static int levelCount = 0;	// 作用域层级计数器，每解析到一个新的函数程序时加一，解析完该函数程序时减一

static char result[MAX_TOKEN_LEN] = {0};     // 当前token缓冲区
static int gCurrentOpcode = 0;               // 传递给算术/比较指令的opcode

// 全局行指针
static char *gLinePtr = NULL;

// ---------- 指令名称表 ----------
static const char *instrNames[] = {
	"mov", "set_array", "read_array", "init_array",
	"push", "pushp",
	"add", "sub", "mul", "div",
	"eq", "lt", "gt", "le", "ge", "ne",
	"jmp", "jmp_t", "call", "ret",
	"bit_aox", "bit_mov",
	"abi_invoke", "reg_write", "reg_read",
	"val", "to_int", "to_float",
	"ipc_send", "ipc_recv",
	"hlt"
};

// ---------- 辅助函数 ----------
static char *skipBlank(char *s) {
	while (*s == ' ' || *s == '\t') s++;
	return s;
}

static char *readToken(char *s) {
	memset(result, 0, MAX_TOKEN_LEN);
	char *p = result, *q = s;
	if (*q != '"') {
		while (*q && *q != ' ' && *q != '\t') *p++ = *q++;
	} else {
		*p++ = *q++;
		while (*q && *q != '"') *p++ = *q++;
		if (*q == '"') *p++ = *q++;
	}
	return q;
}

static int getVarOffset(const char *name) {
	for (int i = 0; i < symbolCount; i++) {
		if (strcmp(name, symbols[i].name) == 0)
			return symbols[i].addr;
	}
	fprintf(stderr, "Error: undefined variable '%s'\n", name);
	exit(1);
}

static int getOpcode(const char *name) {
	for (int i = 0; i <= HLT; i++) {
		if (strcmp(name, instrNames[i]) == 0)
			return i;
	}
	return -1;
}

// ---------- 字节码生成函数 ----------
static void emitInstr(int opcode, int paramType, const int *params, int count) {
	unsigned char byte = (opcode << 3) | (paramType & 0x07);
	*ptr++ = byte;
	for (int i = 0; i < count; i++) {
		*(int32_t *)ptr = params[i];
		ptr += 4;
	}
}

static void emit0(int opcode) {
	emitInstr(opcode, 0, NULL, 0);
}

static void emit1(int opcode, int paramType, int p1) {
	int params[1] = {p1};
	emitInstr(opcode, paramType, params, 1);
}

static void emit2(int opcode, int paramType, int p1, int p2) {
	int params[2] = {p1, p2};
	emitInstr(opcode, paramType, params, 2);
}

static void emit3(int opcode, int paramType, int p1, int p2, int p3) {
	int params[3] = {p1, p2, p3};
	emitInstr(opcode, paramType, params, 3);
}

// ---------- 指令处理函数（无参数，使用全局 gLinePtr） ----------
static void compileMov(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int type;
	if (strcmp(result, "B") == 0) type = 0;
	else if (strcmp(result, "I") == 0) type = 1;
	else if (strcmp(result, "F") == 0) type = 2;
	else {
		fprintf(stderr, "Error: mov missing type\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int dest = getVarOffset(result);

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int srcIsAddr = (result[0] == '$');
	int srcVal;
	if (srcIsAddr) {
		srcVal = getVarOffset(result);
	} else {
		if (type != 2) srcVal = atoi(result);
		else {
			float f = atof(result);
			memcpy(&srcVal, &f, sizeof(float));
		}
	}
	int paramType = (type << 1) | (srcIsAddr ? 1 : 0);
	emit2(MOV, paramType, dest, srcVal);
}

static void compileSetArray(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int elemType;
	if (strcmp(result, "B") == 0) elemType = 0;
	else if (strcmp(result, "I") == 0) elemType = 1;
	else if (strcmp(result, "F") == 0) elemType = 2;
	else {
		fprintf(stderr, "Error: set_array missing type\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int arrayAddr = getVarOffset(result);

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int idxIsAddr = (result[0] == '$');
	int idxVal;
	if (idxIsAddr) idxVal = getVarOffset(result);
	else idxVal = atoi(result);

	int paramType = (elemType << 1) | (idxIsAddr ? 1 : 0);
	emit2(SETARRAY, paramType, arrayAddr, idxVal);
}

static void compileReadArray(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int elemType;
	if (strcmp(result, "B") == 0) elemType = 0;
	else if (strcmp(result, "I") == 0) elemType = 1;
	else if (strcmp(result, "F") == 0) elemType = 2;
	else {
		fprintf(stderr, "Error: read_array missing type\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int arrayAddr = getVarOffset(result);

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int idxIsAddr = (result[0] == '$');
	int idxVal;
	if (idxIsAddr) idxVal = getVarOffset(result);
	else idxVal = atoi(result);

	int paramType = (elemType << 1) | (idxIsAddr ? 1 : 0);
	emit2(READARRAY, paramType, arrayAddr, idxVal);
}

static void compileInitArray(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int elemType;
	if (strcmp(result, "B") == 0) elemType = 0;
	else if (strcmp(result, "I") == 0) elemType = 1;
	else if (strcmp(result, "F") == 0) elemType = 2;
	else {
		fprintf(stderr, "Error: init_array missing type\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int arrayAddr = getVarOffset(result);

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int countIsAddr = (result[0] == '$');
	int countVal;
	if (countIsAddr) countVal = getVarOffset(result);
	else countVal = atoi(result);

	int paramType = (elemType << 1) | (countIsAddr ? 1 : 0);
	emit2(INITARRAY, paramType, arrayAddr, countVal);

	while (1) {
		gLinePtr = skipBlank(gLinePtr);
		if (*gLinePtr == '\0' || *gLinePtr == '\n' || *gLinePtr == ';') break;
		gLinePtr = readToken(gLinePtr);
		int isAddr = (result[0] == '$');
		int val;
		if (isAddr) val = getVarOffset(result);
		*ptr++ = isAddr ? 1 : 0;

		if (elemType == 0) {
			if (isAddr) {
				*(int32_t *)ptr = val;
				ptr += 4;
			} else {
				*ptr++ = (unsigned char)atoi(result);
			}
		} else if (elemType == 1) {
			if (isAddr) *(int32_t *)ptr = val;
			else *(int32_t *)ptr = atoi(result);
			ptr += 4;
		} else {
			if (isAddr) *(int32_t *)ptr = val;
			else {
				float f = atof(result);
				memcpy(ptr, &f, sizeof(float));
			}
			ptr += 4;
		}
	}
}

static void compilePush(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int type;
	if (strcmp(result, "B") == 0) type = 0;
	else if (strcmp(result, "I") == 0) type = 1;
	else if (strcmp(result, "F") == 0) type = 2;
	else {
		fprintf(stderr, "Error: push missing type\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int dest = getVarOffset(result);
	emit1(PUSH, type, dest);
}

static void compilePushp(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int type;
	if (strcmp(result, "B") == 0) type = 0;
	else if (strcmp(result, "I") == 0) type = 1;
	else if (strcmp(result, "F") == 0) type = 2;
	else {
		fprintf(stderr, "Error: pushp missing type\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int isAddr = (result[0] == '$');
	int val;
	if (isAddr) val = getVarOffset(result);
	else {
		if (type != 2) val = atoi(result);
		else {
			float f = atof(result);
			memcpy(&val, &f, sizeof(float));
		}
	}
	int paramType = (type << 1) | (isAddr ? 1 : 0);
	emit1(PUSHP, paramType, val);
}

// 算术/比较指令的统一处理函数
static void compileCalcCmp(void) {
	int opcode = gCurrentOpcode;

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int isFloat = (strcmp(result, "F") == 0) ? 1 : 0;
	if (strcmp(result, "I") != 0 && !isFloat) {
		fprintf(stderr, "Error: missing type (I/F)\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int p1IsAddr = (result[0] == '$');
	int p1Val;
	if (p1IsAddr) p1Val = getVarOffset(result);
	else {
		if (!isFloat) p1Val = atoi(result);
		else {
			float f = atof(result);
			memcpy(&p1Val, &f, sizeof(float));
		}
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int p2IsAddr = (result[0] == '$');
	int p2Val;
	if (p2IsAddr) p2Val = getVarOffset(result);
	else {
		if (!isFloat) p2Val = atoi(result);
		else {
			float f = atof(result);
			memcpy(&p2Val, &f, sizeof(float));
		}
	}

	int paramType;
	if (!p1IsAddr && !p2IsAddr) paramType = 0;
	else if (p1IsAddr && !p2IsAddr) paramType = 1;
	else if (!p1IsAddr && p2IsAddr) paramType = 2;
	else paramType = 3;
	if (isFloat) paramType |= (1 << 2);

	emit2(opcode, paramType, p1Val, p2Val);
}

static void compileJmp(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	strcpy(state.unlables[state.unlabelCount].name, result);
	state.unlables[state.unlabelCount].codeAddr = (uint32_t)(ptr - codePool);
	state.unlables[state.unlabelCount].isTaken = 0;
	state.unlabelCount++;
	int placeholder = 0;
	emit1(JMP, 0, placeholder);
}

static void compileJmpT(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	strcpy(state.unlables[state.unlabelCount].name, result);
	state.unlables[state.unlabelCount].codeAddr = (uint32_t)(ptr - codePool);
	state.unlables[state.unlabelCount].isTaken = 0;
	state.unlabelCount++;
	int placeholder = 0;
	emit1(JMP_T, 0, placeholder);
}

static void compileCall(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	uint32_t funcAddr = 0;
	for (int i = 0; i < state.funcCount; i++) {
		if (strcmp(result, state.functions[i].name) == 0) {
			funcAddr = state.functions[i].addr;
			break;
		}
	}
	if (!funcAddr) {
		fprintf(stderr, "Error: undefined function '%s'\n", result);
		exit(1);
	}
	emit1(CALL, 0, funcAddr);
}

static void compileRet(void) {
	emit0(RET);
}

static void compileBitAox(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int opType;
	if (strcmp(result, "A") == 0) opType = 1;
	else if (strcmp(result, "O") == 0) opType = 2;
	else if (strcmp(result, "X") == 0) opType = 3;
	else {
		fprintf(stderr, "Error: bit_aox requires A/O/X\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int p1IsAddr = (result[0] == '$');
	int p1Val;
	if (p1IsAddr) p1Val = getVarOffset(result);
	else p1Val = atoi(result);

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int p2IsAddr = (result[0] == '$');
	int p2Val;
	if (p2IsAddr) p2Val = getVarOffset(result);
	else p2Val = atoi(result);

	int paramType;
	if (!p1IsAddr && !p2IsAddr) paramType = 0;
	else if (p1IsAddr && !p2IsAddr) paramType = 1;
	else if (p1IsAddr && p2IsAddr) paramType = 2;
	else {
		fprintf(stderr, "Error: first operand cannot be immediate when second is address\n");
		exit(1);
	}
	emit3(BIT_AOX, paramType, opType, p1Val, p2Val);
}

static void compileBitMov(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int shiftDir;
	if (strcmp(result, "L") == 0) shiftDir = 0;
	else if (strcmp(result, "R") == 0) shiftDir = 1;
	else {
		fprintf(stderr, "Error: bit_move requires L/R\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int p1IsAddr = (result[0] == '$');
	int p1Val;
	if (p1IsAddr) p1Val = getVarOffset(result);
	else p1Val = atoi(result);

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int p2IsAddr = (result[0] == '$');
	int p2Val;
	if (p2IsAddr) p2Val = getVarOffset(result);
	else p2Val = atoi(result);

	int paramType;
	if (!p1IsAddr && !p2IsAddr) paramType = 0;
	else if (p1IsAddr && !p2IsAddr) paramType = 1;
	else if (p1IsAddr && p2IsAddr) paramType = 2;
	else {
		fprintf(stderr, "Error: first operand cannot be immediate when second is address\n");
		exit(1);
	}
	paramType |= (shiftDir << 2);
	emit2(BIT_MOV, paramType, p1Val, p2Val);
}

static void compileAbiInvoke(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int isAddr = (result[0] == '$');
	int abiNum;
	if (isAddr) abiNum = getVarOffset(result);
	else abiNum = atoi(result);
	emit1(ABI_INVOKE, isAddr ? 1 : 0, abiNum);
}

static void compileRegWrite(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int p1IsAddr = (result[0] == '$');
	int p1Val;
	if (p1IsAddr) p1Val = getVarOffset(result);
	else p1Val = atoi(result);

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int p2IsAddr = (result[0] == '$');
	int p2Val;
	if (p2IsAddr) p2Val = getVarOffset(result);
	else p2Val = atoi(result);

	int paramType;
	if (!p1IsAddr && !p2IsAddr) paramType = 0;
	else if (p1IsAddr && !p2IsAddr) paramType = 1;
	else if (!p1IsAddr && p2IsAddr) paramType = 2;
	else paramType = 3;
	emit2(REG_WRITE, paramType, p1Val, p2Val);
}

static void compileRegRead(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int dest = getVarOffset(result);

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int srcIsAddr = (result[0] == '$');
	int srcVal;
	if (srcIsAddr) srcVal = getVarOffset(result);
	else srcVal = atoi(result);

	int paramType = srcIsAddr ? 1 : 0;
	emit2(REG_READ, paramType, dest, srcVal);
}

static void compileVal(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int type;
	if (strcmp(result, "B") == 0) type = 0;
	else if (strcmp(result, "I") == 0) type = 1;
	else if (strcmp(result, "F") == 0) type = 2;
	else {
		fprintf(stderr, "Error: val missing type\n");
		exit(1);
	}

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int isAddr = (result[0] == '$');
	int val;
	if (isAddr) val = getVarOffset(result);
	else {
		if (type != 2) val = atoi(result);
		else {
			float f = atof(result);
			memcpy(&val, &f, sizeof(float));
		}
	}
	int paramType = (type << 1) | (isAddr ? 1 : 0);
	emit1(VAL, paramType, val);
}

static void compileToInt(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	if (result[0] != '$') {
		fprintf(stderr, "Error: to_int requires variable address (with $)\n");
		exit(1);
	}
	int addr = getVarOffset(result);
	emit1(TO_INT, 0, addr);
}

static void compileToFloat(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	if (result[0] != '$') {
		fprintf(stderr, "Error: to_float requires variable address (with $)\n");
		exit(1);
	}
	int addr = getVarOffset(result);
	emit1(TO_FLOAT, 0, addr);
}

static void compileIpcSend(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int idIsAddr = (result[0] == '$');
	int idVal;
	if (idIsAddr) idVal = getVarOffset(result);
	else idVal = atoi(result);

	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int msgIsAddr = (result[0] == '$');
	int msgVal;
	if (msgIsAddr) msgVal = getVarOffset(result);
	else msgVal = atoi(result);

	int paramType;
	if (!idIsAddr && !msgIsAddr) paramType = 0;
	else if (idIsAddr && !msgIsAddr) paramType = 1;
	else if (!idIsAddr && msgIsAddr) paramType = 2;
	else paramType = 3;
	emit2(IPC_SEND, paramType, idVal, msgVal);
}

static void compileIpcRecv(void) {
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);
	int dest = getVarOffset(result);
	emit1(IPC_RECV, 0, dest);
}

static void compileHlt(void) {
	emit0(HLT);
	symbolCount = 0;
	totalVarLen = 0;
	levelCount--;
}

// ---------- 指令分发表（无参数） ----------
typedef void (*InstrHandler)(void);
static InstrHandler handlers[HLT + 1] = {
	[MOV]        = compileMov,
	[SETARRAY]   = compileSetArray,
	[READARRAY]  = compileReadArray,
	[INITARRAY]  = compileInitArray,
	[PUSH]       = compilePush,
	[PUSHP]      = compilePushp,
	[ADD]        = compileCalcCmp,
	[SUB]        = compileCalcCmp,
	[MUL]        = compileCalcCmp,
	[DIV]        = compileCalcCmp,
	[EQ]         = compileCalcCmp,
	[LT]         = compileCalcCmp,
	[GT]         = compileCalcCmp,
	[LE]         = compileCalcCmp,
	[GE]         = compileCalcCmp,
	[NE]         = compileCalcCmp,
	[JMP]        = compileJmp,
	[JMP_T]      = compileJmpT,
	[CALL]       = compileCall,
	[RET]        = compileRet,
	[BIT_AOX]    = compileBitAox,
	[BIT_MOV]    = compileBitMov,
	[ABI_INVOKE] = compileAbiInvoke,
	[REG_WRITE]  = compileRegWrite,
	[REG_READ]   = compileRegRead,
	[VAL]        = compileVal,
	[TO_INT]     = compileToInt,
	[TO_FLOAT]   = compileToFloat,
	[IPC_SEND]   = compileIpcSend,
	[IPC_RECV]   = compileIpcRecv,
	[HLT]        = compileHlt
};

// ---------- 编译一行 ----------
static void compileOneLine(char *line) {
	char *s = line;
	s = skipBlank(s);
	if (*s == '\0' || *s == ';') return;

	// 设置全局行指针
	gLinePtr = s;
	gLinePtr = readToken(gLinePtr);
	gLinePtr = skipBlank(gLinePtr);

	// 处理 MEM 段
	if (needMemInit) {
		if (strcmp(result, "mem") == 0) {
			inMemSection = 1;
			return;
		}
		if (strcmp(result, "end_mem") == 0) {
			inMemSection = 0;
			needMemInit = 0;
			//把"变量区大小"写到函数头部预留的 4 字节里
			//必须写 funcHead 而不是当前的 ptr：
			//main 的变量声明集中在程序末尾，若写 ptr 就等于把大小放在程序尾部，
			//而 call() 是按"函数头 + 4 = 第一条指令"来定位的，会导致入口跑飞
			//（funcHead 在 main/fn 声明时已被设为头部地址，两种情形都正确）
			memcpy(funcHead, &totalVarLen, 4);
			return;
		}
		if (inMemSection) {
			char varName[32];
			strcpy(varName, result);
			gLinePtr = readToken(gLinePtr);
			gLinePtr = skipBlank(gLinePtr);
			int elemSize = 0;
			if (strcmp(result, "B") == 0) elemSize = 1;
			else if (strcmp(result, "I") == 0 || strcmp(result, "F") == 0) elemSize = 4;
			else {
				fprintf(stderr, "Error: unknown type %s\n", result);
				exit(1);
			}

			gLinePtr = readToken(gLinePtr);
			gLinePtr = skipBlank(gLinePtr);
			int arrayLen = atoi(result);
			strcpy(symbols[symbolCount].name, varName);
			symbols[symbolCount].addr = totalVarLen;
			symbolCount++;
			totalVarLen += elemSize * (arrayLen + 1);
			return;
		}
	}

	// main / fn
	if (strcmp(result, "main") == 0 || strcmp(result, "fn") == 0) {
		int isMain = (strcmp(result, "main") == 0);
		if (isMain) {
			//main 也按"函数"来布局：先留出 4 字节的变量区大小字段
			//然后才是 main 的第一条指令
			//这样 call() 的 "头部 + 4 = 第一条指令" 规则对 main 同样成立
			entryPoint = (int)(ptr - codePool);
			memcpy(codePool, &entryPoint, 4);
			strcpy(currentFuncName, "main");
			printf("Addr of main head: %d, head+4: %d\n", entryPoint, entryPoint + 4);
		} else {
			gLinePtr = readToken(gLinePtr);
			gLinePtr = skipBlank(gLinePtr);
			strcpy(state.functions[state.funcCount].name, result);
			state.functions[state.funcCount].addr = (uint32_t)(ptr - codePool);
			state.funcCount++;
			strcpy(currentFuncName, result);
			printf("Addr of function %s: %d\n", result, (int)(ptr - codePool));
		}
		needMemInit = 1;
		levelCount++;
		funcHead = ptr;
		ptr += 4;   // 预留总变量长度
		return;
	}

	// 标签
	if (strcmp(result, "lb") == 0) {
		gLinePtr = readToken(gLinePtr);
		gLinePtr = skipBlank(gLinePtr);
		strcpy(state.labels[state.labelCount].name, result);
		state.labels[state.labelCount].addr = (uint32_t)(ptr - codePool);
		state.labelCount++;
		printf("Label %s: %d\n", result, (int)(ptr - codePool));
		return;
	}

	// endfn / endmain
	if (strcmp(result, "endfn") == 0 || strcmp(result, "endmain") == 0) {
		symbolCount = 0;
		totalVarLen = 0;
		levelCount--;
		return;
	}

	// 普通指令
	int opcode = getOpcode(result);
	if (opcode < 0) {
		fprintf(stderr, "Warning: unknown instruction '%s' ignored\n", result);
		return;
	}
	gCurrentOpcode = opcode;
	handlers[opcode]();   // 无参数调用
}

// ---------- 回填标签 ----------
static void resolveLabels() {
	for (int i = 0; i < state.unlabelCount; i++) {
		uint32_t target = 0;
		for (int j = 0; j < state.labelCount; j++) {
			if (strcmp(state.unlables[i].name, state.labels[j].name) == 0) {
				target = state.labels[j].addr;
				break;
			}
		}
		if (!target) {
			fprintf(stderr, "Error: undefined label '%s'\n", state.unlables[i].name);
			exit(1);
		}
		uint32_t *addr = (uint32_t *)(codePool + state.unlables[i].codeAddr + 1);
		*addr = target;
	}
}

// ---------- 主函数 ----------
int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("Usage: %s <source file>\n", argv[0]);
		return 1;
	}

	FILE *fp = fopen(argv[1], "r");
	if (!fp) {
		perror("Failed to open source file");
		return 1;
	}

	char line[1024];
	while (fgets(line, sizeof(line), fp)) {
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
		compileOneLine(line);
	}
	fclose(fp);

	resolveLabels();

	char outFile[512];
	strcpy(outFile, argv[1]);
	char *dot = strrchr(outFile, '.');
	if (dot) strcpy(dot, ".ars_bin");
	else strcat(outFile, ".ars_bin");

	FILE *out = fopen(outFile, "wb");
	if (!out) {
		perror("Failed to create output file");
		return 1;
	}
	size_t codeSize = ptr - codePool;
	fwrite(codePool, 1, codeSize, out);
	fclose(out);

	printf("\nCompiled successfully to %s (%zu bytes)\n", outFile, codeSize);
	printf("Bytecode dump:\n");
	for (size_t i = 0; i < codeSize; i++) {
		printf("0x%02X,", codePool[i]);
		if ((i + 1) % 16 == 0) printf("\n");
	}
	printf("\n");
	return 0;
}
