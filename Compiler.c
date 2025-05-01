#include "Compiler.h"
#define isnum(x) ((x>='0' && x<='9')||x=='.'||x=='-')
#define ischr(x) (x=='\'')

Symbol imm_buff[3];

int to_int(double x) {
	if (x - (int)x) {
		float y = x;
		return tranFloatToInt(y);
	} else return (int) x;
}

uint8_t is_float(double x) {
	if (x - (int)x) return 1;
	return 0;
}

char *skip_space(const char *s) {
	char *p = (char *)s;
	while (*p == ' ' || *p == '\t') {
		*p++;
	}
	return p;
}

void extract_token(const char *ptr, char *buff) {
	char *y = buff;
	while (*ptr != ' ' && *ptr != '\t' && *ptr) {
		*y++ = *ptr++;
	}
}

void del_imm_buf() {
	for (int i = 0; i < 3; i++) {
		imm_buff[i].addr_or_val = 0;
	}
}

// 自定义字符串比较
int str_match(const char *a, const char *b, int len) {
	while (len-- > 0) {
		if (*a++ != *b++) return 0;
	}
	return 1;
}

// 初始化编译器
void compile_init(CompilerState *state) {
	ARS_memset(state, 0, sizeof(CompilerState));
	state->current_scope = -1;
}

// 查找符号
Symbol* find_symbol(CompilerState *state, const char *name) {
	if (isnum(*name) || ischr(*name)) {
		for (int i = 0; i < 3; i++) {
			if (imm_buff[i].addr_or_val == 0) {
				imm_buff[i].addr_or_val = str_to_double((char *)name);
				if (is_float(imm_buff[i].addr_or_val)) {
					imm_buff[i].type = VAR_FLOAT;
				} else imm_buff[i].type = VAR_INT;
				if (ischr(*name))imm_buff[i].addr_or_val = *(name + 1);
				imm_buff[i].is_addr = 0;
				return &imm_buff[i];
			}
		}
	}
	for (int i = state->symbol_count - 1; i >= 0; i--) {
		if (str_match(state->symbols[i].name, name, ARS_strlen(name))) {
			state->symbols[i].is_addr = 1;
			return &state->symbols[i];
		}
	}
	return (void *)0;
}

// 处理变量声明
void handle_var_declare(CompilerState *state, char *type_str, char *name) {
	Symbol sym;
	ARS_memset(&sym, 0, sizeof(Symbol));
	// 确定类型
	if (str_match(type_str, "int", 3)) sym.type = VAR_INT;
	else if (str_match(type_str, "float", 5)) sym.type = VAR_FLOAT;
	else if (str_match(type_str, "byte", 4)) sym.type = VAR_BYTE;
	else sym.type = VAR_STRING;
	// 分配地址（线性分配）
	sym.addr_or_val = state->symbol_count;
	ARS_memmove(sym.name, name, sizeof(sym.name) - 1);
	sym.scope_level = state->current_scope;
	state->symbols[state->symbol_count += (sym.type == VAR_BYTE) ? 1 : 4] = sym;
}

// 生成指令
void emit_code(CompilerState *state, uint8_t op, double *args, uint8_t len) {
	state->code[state->code_pos++] = op;
	if (len >= 1) {
		*(int32_t*)(state->code + state->code_pos) = to_int(args[0]);
		state->code_pos += 4;
	}
	if (len >= 2) {
		*(int32_t*)(state->code + state->code_pos) = to_int(args[1]);
		state->code_pos += 4;
	}
	if (len >= 3) {
		*(int32_t*)(state->code + state->code_pos) = to_int(args[2]);
		state->code_pos += 4;
	}
}

// 核心编译函数
void compile_line(CompilerState *state, const char *line) {
	char buf[256];
	const char *p = line;
	// 分割第一个token
	int i = 0;
	while (*p && *p != ' ' && *p != '\t' && i < 255) {
		buf[i++] = *p++;
	}
	buf[i] = 0;
	// 主程序/子程序声明
	if (str_match(buf, "MAIN", 4) || (buf[0] == ':' && buf[1] == ':')) {
		state->current_scope++;
		return;
	}
	// 变量声明
	if (str_match(buf, "VAR", 3)) {
		char type[16], name[256];
		p = skip_space(p);
		extract_token(p, type);
		p = skip_space(p + ARS_strlen(type));
		extract_token(p, name);
		handle_var_declare(state, type, name);
		return;
	}
	// 算术运算处理
	if (str_match(buf, "ADD", 3) || str_match(buf, "SUB", 3) ||
	    str_match(buf, "MUL", 3) || str_match(buf, "DIV", 3)) {
		uint8_t op = ADD;
		if (buf[0] == 'S') op = SUB;
		else if (buf[0] == 'M') op = MUL;
		else if (buf[0] == 'D') op = DIV;
		uint8_t type;
		if (str_match(buf, "_I", 2)) {
			type = 0;
		} else type = 1;
		char dest[256], src1[256], src2[256];
		p = skip_space(p);
		extract_token(p, dest);
		p = skip_space(p + ARS_strlen(dest));
		extract_token(p, src1);
		Symbol *s1 = find_symbol(state, src1);
		Symbol *dst = find_symbol(state, dest);
		int x, t;
		double y, z;
		if (dst->is_addr && s1->is_addr) {
			x = 2;
			y = dst->addr_or_val;
			t = (dst->type == VAR_FLOAT) ? 1 : 0;
			z = s1->addr_or_val;
		} else if (!dst->is_addr && s1->is_addr) {
			x = 1;
			y = dst->addr_or_val;
			t = (dst->type == VAR_FLOAT) ? 1 : 0;
			z = s1->addr_or_val;
		} else if (dst->is_addr && !s1->is_addr) {
			x = 1;
			y = s1->addr_or_val;
			t = (s1->type == VAR_FLOAT) ? 1 : 0;
			z = dst->addr_or_val;
		} else if (!dst->is_addr && !s1->is_addr) {
			x = 0;
			y = dst->addr_or_val;
			t = (dst->type == VAR_FLOAT) ? 1 : 0;
			z = s1->addr_or_val;
		}
		emit_code(state, op << 3 | t << 2 | x, (double []) {
			y, z
		}, 2);
		return;
	}
	if (str_match(buf, "EQ", 2) || str_match(buf, "LT", 2) ||
	    str_match(buf, "GT", 2) || str_match(buf, "LE", 2) ||
	    str_match(buf, "GE", 2) || str_match(buf, "NE", 2)) {
		uint8_t op = EQ;
		if (buf[0] == 'N') op = NE;
		else if (buf[0] == 'L') {
			if (buf[1] == 'T') {
				op = LT;
			} else op = LE;
		} else if (buf[0] == 'G') {
			if (buf[1] == 'T') {
				op = GT;
			} else op = GE;
		}
		char dest[256], src1[256], src2[256];
		p = skip_space(p);
		extract_token(p, dest);
		p = skip_space(p + ARS_strlen(dest));
		extract_token(p, src1);
		Symbol *s1 = find_symbol(state, src1);
		Symbol *s2 = find_symbol(state, src2);
		Symbol *dst = find_symbol(state, dest);
		emit_code(state, op, (double []) {
			dst->addr_or_val, s1->addr_or_val, s2->addr_or_val
		}, 3);
		return;
	}
	// 输入输出处理
	if (str_match(buf, "PSTR", 4)) {
		p = skip_space(p);
		char var_name[256];
		extract_token(p, var_name);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, PSTR << 3, (double []) {
			sym->addr_or_val
		}, 1);
	}
	if (str_match(buf, "PCHR", 4)) {
		p = skip_space(p);
		char var_name[256];
		extract_token(p, var_name);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, (PCHR << 3) | sym->is_addr ? 1 : 0, (double []) {
			sym->addr_or_val
		}, 1);
	}
	if (str_match(buf, "PVAL", 4)) {
		p = skip_space(p);
		char type[256], var_name[256];
		extract_token(p, type);
		p = skip_space(p);
		extract_token(p, var_name);
		Symbol *_type = find_symbol(state, type);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, (PCHR << 3) | sym->is_addr ? 1 : 0, (double []) {
			_type->addr_or_val, sym->addr_or_val
		}, 2);
	}
	if (str_match(buf, "KEYINPUT", 8)) {
		p = skip_space(p);
		char var_name[256];
		extract_token(p, var_name);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, KEYINPUT << 3, (double []) {
			sym->addr_or_val
		}, 1);
	}
	if (str_match(buf, "VALINPUT", 8)) {
		p = skip_space(p);
		char var_name[256];
		extract_token(p, var_name);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, VALINPUT << 3 | str_match(buf, "_I", 2) ? 0 : 1, (double []) {
			sym->addr_or_val
		}, 1);
	}
	if (str_match(buf, "READFILE", 8)) {
		p = skip_space(p);
		char var_name[256], file_name[256], size[256];
		extract_token(p, var_name);
		p = skip_space(p);
		extract_token(p, file_name);
		p = skip_space(p);
		extract_token(p, size);
		Symbol *sym = find_symbol(state, var_name);
		Symbol *sym1 = find_symbol(state, file_name);
		Symbol *sym2 = find_symbol(state, size);
		emit_code(state, READFILE << 3 | sym2->is_addr ? 1 : 0, (double []) {
			sym->addr_or_val, sym1->addr_or_val, sym2->addr_or_val
		}, 3);
	}
	if (str_match(buf, "WRITEFILE", 8)) {
		p = skip_space(p);
		char var_name[256], file_name[256], size[256];
		extract_token(p, var_name);
		p = skip_space(p);
		extract_token(p, file_name);
		p = skip_space(p);
		extract_token(p, size);
		Symbol *sym = find_symbol(state, var_name);
		Symbol *sym1 = find_symbol(state, file_name);
		Symbol *sym2 = find_symbol(state, size);
		emit_code(state, WRITEFILE << 3 | sym2->is_addr ? 1 : 0, (double []) {
			sym->addr_or_val, sym1->addr_or_val, sym2->addr_or_val
		}, 3);
	}
	if (str_match(buf, "DEL_FILE", 8)) {
		p = skip_space(p);
		char var_name[256];
		extract_token(p, var_name);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, DEL_FILE << 3 | sym->is_addr ? 1 : 0, (double []) {
			sym->addr_or_val
		}, 1);
	}
	if (str_match(buf, "MOV", 3)) {
		int x;
		if (str_match(buf, "_B", 2)) {
			x = 0;
		} else if (str_match(buf, "_I", 2)) {
			x = 1;
		} else {
			x = 2;
		}
		p = skip_space(p);
		char var_name[256], var2[256];
		extract_token(p, var_name);
		p = skip_space(p);
		extract_token(p, var2);
		Symbol *sym = find_symbol(state, var_name);
		Symbol *sym1 = find_symbol(state, var2);
		uint8_t op = (MOV << 3) | x << 1;
		emit_code(state, op | sym1->is_addr ? 1 : 0, (double []) {
			sym->addr_or_val, sym1->addr_or_val
		}, 2);
	}
	if (str_match(buf, "IVKARRAY", 8)) {
		int x;
		if (str_match(buf, "_B", 2)) {
			x = 0;
		} else if (str_match(buf, "_I", 2)) {
			x = 1;
		} else {
			x = 2;
		}
		p = skip_space(p);
		char var_name[256], var1[256], var2[256];
		extract_token(p, var_name);
		p = skip_space(p);
		extract_token(p, var1);
		p = skip_space(p);
		extract_token(p, var2);
		Symbol *sym = find_symbol(state, var_name);
		Symbol *sym1 = find_symbol(state, var1);
		Symbol *sym2 = find_symbol(state, var2);
		uint8_t op = (IVKARRAY << 3) | x << 1;
		emit_code(state, op | sym2->is_addr ? 1 : 0, (double []) {
			sym->addr_or_val, sym1->addr_or_val, sym2->addr_or_val
		}, 3);
	}
	if (str_match(buf, "PUSH", 4)) {
		int x;
		if (str_match(buf, "_B", 2)) {
			x = 0;
		} else if (str_match(buf, "_I", 2)) {
			x = 1;
		} else {
			x = 2;
		}
		p = skip_space(p);
		char var_name[256];
		extract_token(p, var_name);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, (PUSH << 3) | x, (double []) {
			sym->addr_or_val
		}, 1);
	}
	if (str_match(buf, "PUSHP", 5)) {
		int x;
		if (str_match(buf, "_B", 2)) {
			x = 0;
		} else if (str_match(buf, "_I", 2)) {
			x = 1;
		} else {
			x = 2;
		}
		p = skip_space(p);
		char var_name[256];
		extract_token(p, var_name);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, (PUSHP << 3 ) | x, (double []) {
			sym->is_addr ? 1 : 0, sym->addr_or_val
		}, 2);
	}
	if (str_match(buf, "CALL", 4)) {
		p = skip_space(p);
		char var_name[256];
		extract_token(p, var_name);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, CALL << 3, (double []) {
			sym->addr_or_val
		}, 1);
	}
	if (str_match(buf, "RET", 3)) {
		emit_code(state, CALL << 3, (double []) {
			0
		}, 1);
	}
	if (str_match(buf, "MENU_HANG", 9)) {
		p = skip_space(p);
		char var_name[256];
		extract_token(p, var_name);
		Symbol *sym = find_symbol(state, var_name);
		emit_code(state, (MENU_HANG << 3) | sym->is_addr ? 1 : 0, (double []) {
			sym->addr_or_val
		}, 1);
	}
	if (str_match(buf, "MENU_REG", 8)) {
		p = skip_space(p);
		char var_name[256], action[256];
		extract_token(p, var_name);
		p = skip_space(p);
		extract_token(p, action);
		Symbol *sym = find_symbol(state, var_name);
		Symbol *sym2 = find_symbol(state, action);
		emit_code(state, (MENU_REG << 3), (double []) {
			sym->addr_or_val, sym2->addr_or_val
		}, 2);
	}
	if (str_match(buf, "MENU_SHOW", 9)) {
		emit_code(state, MENU_SHOW << 3, (double []) {
			0
		}, 1);
	}
	if (str_match(buf, "HLT", 3)) {
		emit_code(state, HLT << 3, (double []) {
			0
		}, 1);
	}
	if (str_match(buf, "BIT", 3)) {
		emit_code(state, EXCODE, (double []) {
			0
		}, 1);
	}
	// 其他指令类似处理...
}

// 最终处理标签
void finalize_compilation(CompilerState *state) {
	// 这里处理标签回填（示例）
	for (int i = 0; i < state->label_count; i++) {
		Label *l = &state->labels[i];
		for (int j = 0; j < state->code_pos; j++) {
			if (state->code[j] == JMP || state->code[j] == JMP_T) {
				int32_t *addr = (int32_t*)(state->code + j + 1);
				if (*addr == 0xFFFFFFFF) { // 需要回填
					*addr = l->addr;
				}
			}
		}
	}
}
