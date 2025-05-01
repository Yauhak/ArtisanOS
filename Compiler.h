#ifndef IO_INCLUDE
	#include "IO_INCLUDE.h"
#endif
#ifndef COMPILER
#define COMPILER

#define MAX_SYMBOLS   256
#define MAX_LABELS    128

// 变量类型定义
typedef enum {
	VAR_INT,
	VAR_FLOAT,
	VAR_BYTE,
	VAR_STRING
} VarType;

// 符号表条目
typedef struct {
	char name[256];
	VarType type;
	double addr_or_val;
	uint8_t is_addr;
	uint8_t is_param;
	int scope_level;
} Symbol;

// 标签信息
typedef struct {
	char name[256];
	uint32_t addr;
} Label;

// 编译器全局状态
typedef struct {
	Symbol symbols[MAX_SYMBOLS];
	Label labels[MAX_LABELS];
	uint8_t *code;
	int symbol_count;
	int label_count;
	int code_pos;
	int current_scope;
} CompilerState;

// 编译器接口
void compile_init(CompilerState *state);
void compile_line(CompilerState *state, const char *line);
void finalize_compilation(CompilerState *state);

#endif
