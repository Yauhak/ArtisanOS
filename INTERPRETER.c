#include "INTERPRETER.h"
#include "DiskAndFAT.h"
#include "Memory.h"
#include "TUI.h"

//extern from Memory.h
extern volatile uint8_t *CurPhyMem[OS_MAX_TASK];
extern volatile uint8_t *CurCmd[OS_MAX_TASK];
extern volatile uint8_t *MemTail[OS_MAX_TASK];
extern int16_t MemLevel[OS_MAX_TASK];
extern ParamStack Stack[OS_MAX_PARAM];
extern uint8_t IndexOfSPS;
extern uint16_t cursor_pos;

int32_t CalcResu[OS_MAX_TASK] = {0}; //一些运算的运行结果寄存
menu menu_obj;

#define INVALID_INPUT  -10
#define OVERFLOW_ERR   -11
#define INVALID_TYPE   -12

//字节码-函数映射表
typedef int8_t (*OpHandler)(uint8_t, int32_t*, uint16_t);
static OpHandler opcode_table[HLT + 1] = {
	[PCHR] = pchr,
	[PSTR] = pstr,
	[PVAL] = pval,
	[KEYINPUT] = key_input,
	[VALINPUT] = val_input,
	[READFILE] = _read_file,
	[WRITEFILE] = _write_file,
	[DEL_FILE] = _del_file,
	[MOV] = mov,
	[IVKARRAY] = invoke_array,
	[PUSH] = push,
	[PUSHP] = pushp,
	[EQ] = conds,
	[LT] = conds,
	[GT] = conds,
	[LE] = conds,
	[GE] = conds,
	[NE] = conds,
	[JMP] = jmp,
	[JMP_T] = jmp_t,
	[CALL] = call,
	[RET] = ret,
	[ADD] = calc,
	[SUB] = calc,
	[MUL] = calc,
	[DIV] = calc,
	[MENU_HANG] = menu_hang,
	[MENU_REG] = menu_reg,
	[MENU_SHOW] = menu_show,
	[HLT] = hlt
};

static OpHandler ex_opcode[EX_END + 1] = {
	[BIT_AOX] = bit_and_or_xor,
	[BIT_MOV] = bit_move
};

//输出一个字符
//PCHR [地址或立即数]，无返回值
int8_t pchr(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	if (ParamType == 0) {
		ARS_pc((char)params[0], taskId);
	} else {
		ARS_pc(findByteWithAddr(taskId), taskId);
	}
}

//输出字符串
//PSTR [地址]，无返回值
int8_t pstr(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	FindPhyMemOffByID(taskId, params[0]);
	uint8_t c = 1;
	while (c = findByteWithAddr(taskId)) {
		ARS_pc(c, taskId);
	}
}

//输出数值（INT，FLOAT）
//PVAL [地址或立即数]，无返回值
int8_t pval(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	// 参数解析
	uint8_t data_type = params[0];  //0=INT, 1=FLOAT
	int32_t value;
	float f_value;
	// 根据 ParamType 获取数值
	if (ParamType == 0) {  // 立即数
		value = params[1];
		if (data_type == 2) {  // FLOAT 需要特殊处理
			ARS_memmove(&f_value, &value, sizeof(float));
		}
	} else {  // 地址
		FindPhyMemOffByID(taskId, params[1]);
		switch (data_type) {
			case 0:  // INT
				value = findIntWithAddr(taskId);
				break;
			case 1:  // FLOAT
				f_value = findFloatWithAddr(taskId);
				break;
		}
	}
	// 转换为字符串并输出
	char buffer[32];
	switch (data_type) {
		case 0:  // INT (32-bit)
			_int_to_str(value, buffer);
			break;
		case 1:  // FLOAT
			_float_to_str(f_value, buffer, 4);  // 默认保留4位小数
			break;
	}
	// 逐字符输出
	for (int i = 0; buffer[i] != '\0'; i++)
		ARS_pc(buffer[i], taskId);
}

//从键盘接收一个字符
//KEY_INPUT [地址]
int8_t key_input(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	FindPhyMemOffByID(taskId, params[0]);
	setByte(ARS_gc(taskId, 1), taskId);
}

//从键盘接收数值输入
//VAL_INPUT [地址]
int8_t val_input(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	// 定位目标内存地址
	int8_t ret = FindPhyMemOffByID(taskId, params[0]);
	if (ret != 0) return ret; // 地址无效直接返回
	char inputBuf[128] = {0}; // 输入缓冲区初始化
	uint8_t current_char = 0;
	uint8_t buf_index = 0;
	uint8_t is_negative = 0;
	uint8_t has_decimal = 0;
	float decimal_factor = 0.1f;
	double val = 0.0;
	// 读取输入直到换行或缓冲区满
	while (current_char != '\n' && buf_index < sizeof(inputBuf) - 1) {
		current_char = ARS_gc(taskId, 1);
		// 退格处理
		if (current_char == 8) {
			if (buf_index > 0) {
				buf_index--;
				inputBuf[buf_index] = '\0';
			}
			continue;
		}
		// 回车结束输入
		if (current_char == '\n') {
			inputBuf[buf_index] = '\0';
			break;
		}
		// 过滤非法字符（仅允许数字、负号、小数点）
		if ((current_char < '0' || current_char > '9') &&
		    current_char != '-' &&
		    current_char != '.') {
			continue; // 忽略非法字符
		}
		// 负号只能在首位
		if (current_char == '-' && buf_index != 0) {
			continue; // 忽略中间负号
		}
		// 小数点不能重复
		if (current_char == '.' && has_decimal) {
			continue; // 忽略重复小数点
		}
		// 存储字符并回显
		inputBuf[buf_index++] = current_char;
		inputBuf[buf_index] = '\0';
		if (TopWindowId == taskId) {
			ARS_pc(current_char, taskId);
		}
		// 标记负号或小数点
		if (current_char == '-') is_negative = 1;
		if (current_char == '.') has_decimal = 1;
	}
	// 解析数值
	val = str_to_float(inputBuf);
	// 类型转换与存储
	switch (ParamType) {
		case 0: { // INT (int32_t)
			if (val > 2147483647 || val < -2147483648) {
				return OVERFLOW_ERR;
			}
			setInt((int32_t)val, taskId);
			break;
		}
		case 1: { // FLOAT
			setFloat((float)val, taskId);
			break;
		}
		default:
			return INVALID_TYPE;
	}
}

//读文件
//READFILE [读取到的内存地址][文件名][大小（簇数）]
int8_t _read_file(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	char nameBuff[256];
	int i = 0;
	FindPhyMemOffByID(taskId, params[1]);
	while (*CurPhyMem) {
		nameBuff[i++] = findByteWithAddr(taskId);
	}
	nameBuff[i] = 0; //文件名
	int sect_count;
	if (ParamType == 0) {
		sect_count = params[2];
	} else {
		FindPhyMemOffByID(taskId, params[2]);
		sect_count = findIntWithAddr(taskId);
	}
	uint16_t file_handle = find_file(nameBuff);
	FindPhyMemOffByID(taskId, params[0]);
	read_file(file_handle, (uint8_t *)CurPhyMem[taskId], sect_count);
}

//写文件
//WRITEFILE [写入文件的字符串内存地址][文件名][大小（字节）]
int8_t _write_file(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	char nameBuff[256];
	int i = 0;
	FindPhyMemOffByID(taskId, params[0]);
	while (*CurPhyMem) {
		nameBuff[i++] = findByteWithAddr(taskId);
	}
	nameBuff[i] = 0; //文件名
	int file_size;
	if (ParamType == 0) {
		file_size = params[2];
	} else {
		FindPhyMemOffByID(taskId, params[2]);
		file_size = findIntWithAddr(taskId);
	}
	FindPhyMemOffByID(taskId, params[1]);
	write_file(nameBuff, (uint8_t *)CurPhyMem[taskId], file_size);
}

//删除文件
//DEL_FILE [文件名字符串地址]
int8_t _del_file(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	char nameBuff[256];
	int i = 0;
	FindPhyMemOffByID(taskId, params[0]);
	while (*CurPhyMem) {
		nameBuff[i++] = findByteWithAddr(taskId);
	}
	nameBuff[i] = 0; //文件名
	del_file(nameBuff);
}

//可以叫赋值？
//MOV [地址][立即数或地址]
int8_t mov(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	uint8_t type = (ParamType & 0x06) >> 1;
	ParamType &= 0x01;
	switch (type) {
		case 0://BYTE
			if (ParamType == 0) {
				FindPhyMemOffByID(taskId, params[0]);
				setByte((int8_t)params[1], taskId);
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				int8_t x = findByteWithAddr(taskId);
				FindPhyMemOffByID(taskId, params[0]);
				setByte(x, taskId);
			}
			break;
		case 1://INT
			if (ParamType == 0) {
				FindPhyMemOffByID(taskId, params[0]);
				setInt((int32_t)params[1], taskId);
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				int32_t x = findIntWithAddr(taskId);
				FindPhyMemOffByID(taskId, params[0]);
				setInt(x, taskId);
			}
			break;
		case 2://FLOAT
			if (ParamType == 0) {
				FindPhyMemOffByID(taskId, params[0]);
				float x = tranIntToFloat(params[1]);
				setFloat(x, taskId);
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				float x = findFloatWithAddr(taskId);
				FindPhyMemOffByID(taskId, params[0]);
				setFloat(x, taskId);
			}
			break;
	}
}

int8_t invoke_array(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	uint8_t ivk_type = (ParamType & 0x06) >> 1;
	ParamType &= 0x01;
	int32_t index;
	if (ParamType == 0) {
		index = params[2];
	} else {
		FindPhyMemOffByID(taskId, params[2]);
		index = findIntWithAddr(taskId);
	}
	FindPhyMemOffByID(taskId, params[1]);
	CurPhyMem[taskId] += index;
	switch (ivk_type) {
		case 0:
			CalcResu[taskId] = findByteWithAddr(taskId);
			FindPhyMemOffByID(taskId, params[0]);
			setByte(CalcResu[taskId], taskId);
			break;
		case 1:
			CalcResu[taskId] = findIntWithAddr(taskId);
			FindPhyMemOffByID(taskId, params[0]);
			setInt(CalcResu[taskId], taskId);
			break;
		case 2: {
			float x = findFloatWithAddr(taskId);
			CalcResu[taskId] = tranFloatToInt(x);
			FindPhyMemOffByID(taskId, params[0]);
			setFloat(x, taskId);
			break;
		}
	}
}

//将CalcResu存入内存
//PUSH [内存地址]
int8_t push(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	FindPhyMemOffByID(taskId, params[0]);
	//保存为BYTE
	if (ParamType == 0) {
		setByte((int8_t)CalcResu[taskId], taskId);
		//保存为INT？我不知道四字节的变量怎么称呼
	} else if (ParamType == 1) {
		setInt((int32_t)CalcResu[taskId], taskId);
	} else {
		setFloat((float)CalcResu[taskId], taskId);
	}
}

//子程序参数栈压入参数
//PUSHP [子程序参数的内存地址或立即数]
int8_t pushp(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	if (params[0] == 0) {
		if (ParamType == 0) {
			Stack[IndexOfSPS].DATA.BYTE = (int8_t)params[1];
			Stack[IndexOfSPS].Type = 1;
		} else if (ParamType == 1) {
			Stack[IndexOfSPS].DATA.INT = (int32_t)params[1];
			Stack[IndexOfSPS].Type = 2;
		} else {
			Stack[IndexOfSPS].DATA.FLOAT = tranIntToFloat(params[1]);
			Stack[IndexOfSPS].Type = 3;
		}
	} else {
		FindPhyMemOffByID(taskId, params[1]);
		if (ParamType == 0) {
			Stack[IndexOfSPS].DATA.BYTE = findByteWithAddr(taskId);
			Stack[IndexOfSPS].Type = 1;
		} else if (ParamType == 1) {
			Stack[IndexOfSPS].DATA.INT = findIntWithAddr(taskId);
			Stack[IndexOfSPS].Type = 2;
		} else {
			Stack[IndexOfSPS].DATA.FLOAT = tranIntToFloat(params[1]);
			Stack[IndexOfSPS].Type = 3;
		}
	}
	IndexOfSPS++;
	if (IndexOfSPS >= OS_MAX_PARAM) {
		return OUT_PARAM_BOUND;
	}
}

//调用子程序
//CALL [子程序编号，在编译过程中确定]
int8_t call(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	//保存上下文数据
	//这里没办法，必须强制将地址和数值相互转化，因为需要将相关信息写入内存
	int32_t CurAddrOfMemPtr = (volatile int32_t)(int8_t *)CurPhyMem[taskId];
	int32_t CurAddrOfCmd = (volatile int32_t)(int8_t *)CurCmd[taskId];
	//程序命令指针指向参数所表示的地址
	CurCmd[taskId] = (volatile uint8_t *)(uint32_t)params[0];
	//前四个字节代表运行所需内存总大小（包括行参）
	//这个值在编译过程中确定
	volatile uint32_t ReqMemSize = *(volatile uint32_t *)CurCmd[taskId];
	//跳过四字节进入程序主体
	CurCmd[taskId] += sizeof(uint32_t);
	//分配内存
	//分配完后CurPhyMem[taskId]跳转至分配的内存的首地址
	findFreeMemById(taskId, ReqMemSize, MemLevel[taskId]);
	//暂存当前内存指针指向地址
	volatile uint8_t *tmp_ptr = CurPhyMem[taskId];
	//跳过魔术字头
	CurPhyMem[taskId] += sizeof(Magic);
	//压入上下文数据
	*(volatile int32_t *)CurPhyMem[taskId] = CurAddrOfMemPtr;
	*(volatile int32_t *)(CurPhyMem[taskId] + sizeof(int32_t)) = CurAddrOfCmd;
	CurPhyMem[taskId] += 2 * sizeof(int32_t);
	int i;
	//压入参数
	for (i = 0; i < OS_MAX_PARAM; i++) {
		if (Stack[i].Type) {
			if (Stack[i].Type == 1) {
				setByte(Stack[i].DATA.BYTE, taskId);
			} else if (Stack[i].Type == 2) {
				setInt(Stack[i].DATA.INT, taskId);
			} else {
				setFloat(Stack[i].DATA.FLOAT, taskId);
			}
			//Type=0表示从这儿开始往后的参数栈都未启用
			//参数栈的数据紧密相连，不存在跳跃
		} else {
			break;
		}
	}
	//销毁参数栈的形参
	for (int j = 0; j < i; j++) {
		Stack[j].Type = 0;
	}
	IndexOfSPS = 0;
	//返回内存头
	CurPhyMem[taskId] = tmp_ptr;
	//内存层级+1
	MemLevel[taskId]++;
}

//子程序返回上文
//无参数
int8_t ret(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	//提取上下文信息
	int32_t Mem = *(volatile int32_t *)MemTail[taskId];
	int32_t Cmd = *(volatile int32_t *)(MemTail[taskId] + sizeof(int32_t));
	//销毁变量（内存层级在该函数中自减）
	DelLastFuncMem(taskId);
	//跳回上文
	CurPhyMem[taskId] = (volatile uint8_t *)Mem;
	CurCmd[taskId] = (volatile uint8_t *)Cmd;
}

//条件判断
int8_t conds(uint8_t cmdAndPmTp, int32_t *params, uint16_t taskId) {
	// 前五个字节代表命令
	uint8_t cmd = cmdAndPmTp >> 3;
	// 后三个字节共同代表参数的一些性质
	uint8_t ParamType = cmdAndPmTp & 0x07;
	// 解析参数类型和数据大小
	uint8_t dataSize = params[0]; // 新增参数：1=BYTE, 4=INT/FLOAT
	if (dataSize != 1 && dataSize != 4) {
		return -1; // 非法数据大小
	}
	//用double来覆盖所有类型最大可表示的值
	//简化操作
	double val1, val2;
	//x用来判断参数是不是float类型（ParamType第三位）
	uint8_t x = (ParamType & 0x04) >> 2;
	//无论如何去掉第三位，否则可能参数误判
	ParamType &= 0x03;
	// 读取参数1的值（根据ParamType和dataSize）
	if (ParamType == 0) {       // 两个立即数
		val1 = x ? tranIntToFloat(params[1]) : params[1];
		val2 = x ? tranIntToFloat(params[2]) : params[2];
	} else if (ParamType == 1) { // 参数1为地址，参数2为立即数
		// 从内存读取参数1的值
		FindPhyMemOffByID(taskId, params[1]);
		val1 = x ? findFloatWithAddr(taskId) : findIntWithAddr(taskId) << (4 - dataSize) >> (4 - dataSize);
		// 立即数参数2处理
		val2 = x ? tranIntToFloat(params[2]) : params[2];
	} else if (ParamType == 2) { // 两个参数均为地址
		// 读取参数1的地址
		FindPhyMemOffByID(taskId, params[1]);
		val1 = x ? findFloatWithAddr(taskId) : findIntWithAddr(taskId) << (4 - dataSize) >> (4 - dataSize);
		// 读取参数2的地址
		FindPhyMemOffByID(taskId, params[2]);
		val2 = x ? findFloatWithAddr(taskId) : findIntWithAddr(taskId) << (4 - dataSize) >> (4 - dataSize);
	} else return -1; // 非法参数类型
	// 根据指令进行比较
	switch (cmd) {
		case EQ:
			CalcResu[taskId] = (val1 == val2);
			break;
		case LT:
			CalcResu[taskId] = (val1 <  val2);
			break;
		case GT:
			CalcResu[taskId] = (val1 >  val2);
			break;
		case LE:
			CalcResu[taskId] = (val1 <= val2);
			break;
		case GE:
			CalcResu[taskId] = (val1 >= val2);
			break;
		case NE:
			CalcResu[taskId] = (val1 != val2);
			break;
		default:
			return -1; // 非法指令
	}
}

//无条件跳转
int8_t jmp(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	CurCmd[taskId] = (volatile uint8_t *)(OS_EXE_LOAD_START + params[0]);
}

//情况成立（CalcResu不为0）跳转
int8_t jmp_t(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	if (CalcResu[taskId])CurCmd[taskId] = (volatile uint8_t *)(OS_EXE_LOAD_START + params[0]);
}

//加减乘除
int8_t calc(uint8_t cmdAndPmTp, int32_t *params, uint16_t taskId) {
	uint8_t cmd = cmdAndPmTp >> 3;
	// 后三个字节共同代表参数的一些性质
	uint8_t ParamType = cmdAndPmTp & 0x07;
	// 新增参数类型标识：ParamType 的第三位表示是否为浮点运算 (1=float)
	uint8_t is_float = (ParamType & 0x04) >> 2;  // 取第三位
	ParamType &= 0x03;  // 保留原始参数类型
	float val1_f, val2_f, result_f;
	int32_t val1_i, val2_i, result_i;
	// 根据参数类型读取操作数（支持立即数、地址、混合类型）
	if (is_float) {
		// 处理浮点运算
		if (ParamType == 0) {       // 两个立即数（需将 int32_t 转换为 float）
			val1_f = *(float*)&params[0];
			val2_f = *(float*)&params[1];
		} else if (ParamType == 1) { // 参数1为地址，参数2为立即数
			FindPhyMemOffByID(taskId, params[0]);
			val1_f = findFloatWithAddr(taskId);
			val2_f = *(float*)&params[1];
		} else if (ParamType == 2) { // 两个参数均为地址
			FindPhyMemOffByID(taskId, params[0]);
			val1_f = findFloatWithAddr(taskId);
			FindPhyMemOffByID(taskId, params[1]);
			val2_f = findFloatWithAddr(taskId);
		}
		// 执行浮点运算
		switch (cmd) {
			case ADD:
				result_f = val1_f + val2_f;
				break;
			case SUB:
				result_f = val1_f - val2_f;
				break;
			case MUL:
				result_f = val1_f * val2_f;
				break;
			case DIV:
				if (val2_f == 0.0f) return DIV_BY_0;
				result_f = val1_f / val2_f;
				break;
		}
		// 将结果转换为 int32_t 存入 CalcResu（需确保内存对齐）
		ARS_memmove(&CalcResu[taskId], &result_f, sizeof(float));
		if (ParamType) {
			FindPhyMemOffByID(taskId, params[0]);
			setFloat(result_f, taskId);
		}
	} else {
		// 原有整数运算逻辑（略作调整）
		if (ParamType == 0) {
			val1_i = params[0];
			val2_i = params[1];
		} else if (ParamType == 1) {
			FindPhyMemOffByID(taskId, params[0]);
			val1_i = findIntWithAddr(taskId);
			val2_i = params[1];
		} else if (ParamType == 2) {
			FindPhyMemOffByID(taskId, params[0]);
			val1_i = findIntWithAddr(taskId);
			FindPhyMemOffByID(taskId, params[1]);
			val2_i = findIntWithAddr(taskId);
		}
		switch (cmd) {
			case ADD:
				result_i = val1_i + val2_i;
				break;
			case SUB:
				result_i = val1_i - val2_i;
				break;
			case MUL:
				result_i = val1_i * val2_i;
				break;
			case DIV:
				if (val2_i == 0) return DIV_BY_0;
				result_i = val1_i / val2_i;
				break;
		}
		CalcResu[taskId] = result_i;
		if (ParamType) {
			FindPhyMemOffByID(taskId, params[0]);
			setInt(result_i, taskId);
		}
	}
}

//菜单挂载
int8_t menu_hang(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	menu_obj.id = taskId;
	menu_obj.is_registered = 1;
	if (ParamType == 0) {
		menu_obj.handle = params[0];
	} else {
		FindPhyMemOffByID(taskId, params[0]);
		menu_obj.handle = (uint8_t)findByteWithAddr(taskId);
	}
}

//选项、对应动作注册
int8_t menu_reg(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	FindPhyMemOffByID(taskId, params[0]);
	menu_obj.menu_prompt[menu_obj.menu_len++] = (uint8_t *)CurPhyMem[taskId];
	menu_obj.action[menu_obj.menu_len] = params[1];
}

//显示菜单并循环直到选择了菜单的某个选项
int8_t menu_show(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	for (int i = 0; i < menu_obj.menu_len; i++) {
		uint8_t *x = menu_obj.menu_prompt[i];
		while (*x) {
			ARS_pc(*x++, taskId);
		}
		ARS_pc('\n', taskId);
	}
	char get_press;
	uint8_t choice = menu_obj.menu_len - 1;
	while ((get_press = ARS_gc(taskId, 0)) != '\n') {
		mark_line(cursor_pos / VGA_WIDTH);
		if (get_press == KEY_UP) {
			choice = (choice > 0) ? choice - 1 : 0;
		} else if (get_press == KEY_DOWN) {
			choice = (choice <= menu_obj.menu_len - 1) ? choice + 1 : menu_obj.menu_len - 1;
		}
	}
	int32_t act = menu_obj.action[choice];
	call(0, &act, taskId);
}

int8_t hlt(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	return 1;
}

int8_t bit_and_or_xor(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	int32_t val1_i, val2_i;
	if (ParamType == 0) {
		val1_i = params[1];
		val2_i = params[2];
	} else if (ParamType == 1) {
		FindPhyMemOffByID(taskId, params[1]);
		val1_i = findIntWithAddr(taskId);
		val2_i = params[2];
	} else if (ParamType == 2) {
		FindPhyMemOffByID(taskId, params[1]);
		val1_i = findIntWithAddr(taskId);
		FindPhyMemOffByID(taskId, params[2]);
		val2_i = findIntWithAddr(taskId);
	}
	switch (params[0]) {
		case 1:
			val1_i &= val2_i;
			break;
		case 2:
			val1_i |= val2_i;
			break;
		case 3:
			val1_i ^= val2_i;
			break;
	}
	if (ParamType) {
		FindPhyMemOffByID(taskId, params[1]);
		setInt(val1_i, taskId);
	}
}

int8_t bit_move(uint8_t ParamType, int32_t *params, uint16_t taskId) {
	int32_t val1_i, val2_i;
	uint8_t operate = (ParamType & 0x04) >> 2;
	ParamType &= 0x03;
	if (ParamType == 0) {
		val1_i = params[0];
		val2_i = params[1];
	} else if (ParamType == 1) {
		FindPhyMemOffByID(taskId, params[0]);
		val1_i = findIntWithAddr(taskId);
		val2_i = params[1];
	} else if (ParamType == 2) {
		FindPhyMemOffByID(taskId, params[0]);
		val1_i = findIntWithAddr(taskId);
		FindPhyMemOffByID(taskId, params[1]);
		val2_i = findIntWithAddr(taskId);
	}
	switch (operate) {
		case 0:
			val1_i <<= val2_i;
			break;
		case 1:
			val1_i >>= val2_i;
			break;
	}
	if (ParamType) {
		FindPhyMemOffByID(taskId, params[0]);
		setInt(val1_i, taskId);
	}
}

//注意！！
//params并不代表它一定表示的是int类型
//可能是与float类型共用相同的四字节内存
int8_t interprete(uint8_t cmdAndPmTp, int32_t *params, uint16_t taskId, uint8_t is_ex_op) {
	// 前五个字节代表命令
	uint8_t cmd = cmdAndPmTp >> 3;
	// 后三个字节共同代表参数的一些性质
	uint8_t ParamType = cmdAndPmTp & 0x07;
	if (is_ex_op) {
		if (!((cmd >= EQ && cmd <= NE) && (cmd >= ADD && cmd <= DIV))) {
			opcode_table[cmd](ParamType, params, taskId);
		} else {
			opcode_table[cmd](cmdAndPmTp, params, taskId);
		}
	} else {
		ex_opcode[cmd](ParamType, params, taskId);
	}
}
