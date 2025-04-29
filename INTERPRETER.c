#include "INTERPRETER.h"
#include "DiskAndFAT.h"
#include "Memory.h"

//extern from Memory.h
extern volatile uint8_t *CurPhyMem[OS_MAX_TASK];
extern volatile uint8_t *CurCmd[OS_MAX_TASK];
extern volatile uint8_t *MemTail[OS_MAX_TASK];
extern int16_t MemLevel[OS_MAX_TASK];
extern ParamStack Stack[OS_MAX_PARAM];
extern uint8_t IndexOfSPS;

int32_t CalcResu[OS_MAX_TASK] = {0}; //一些运算的运行结果寄存

#define INVALID_INPUT  -10
#define OVERFLOW_ERR   -11
#define INVALID_TYPE   -12

//注意！！
//params并不代表它一定表示的是int类型
//可能由float类型变换而来
int8_t interprete(uint8_t cmdAndPmTp, int32_t *params, uint16_t taskId) {
	// 前五个字节代表命令
	uint8_t cmd = cmdAndPmTp >> 3;
	// 后三个字节共同代表参数的一些性质
	uint8_t ParamType = cmdAndPmTp & 0x07;
	switch (cmd) {
		//输出一个字符
		case PCHR:
			//若为立即数
			if (ParamType == 0) {
				ARS_pc((char)params[0], taskId);
			} else {
				ARS_pc(findByteWithAddr(taskId), taskId);
			}
			break;
		//输出字符串
		case PSTR:{
			//该函数只能以地址作为参数
			FindPhyMemOffByID(taskId, params[0]);
			uint8_t c=1;
			while (c=findByteWithAddr(taskId)) {
				ARS_pc(c, taskId);
			}
			break;
		}
		case PVAL: {
			// 参数解析
			uint8_t data_type = params[0];  // 0=DWORD, 1=INT, 2=FLOAT
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
					case 0:  // DWORD
						value = (int32_t)findDByteWithAddr(taskId);
						break;
					case 1:  // INT
						value = findIntWithAddr(taskId);
						break;
					case 2:  // FLOAT
						f_value = findFloatWithAddr(taskId);
						break;
				}
			}
			// 转换为字符串并输出
			char buffer[32];
			switch (data_type) {
				case 0:  // DWORD (16-bit)
					_int_to_str((int16_t)value, buffer);
					break;
				case 1:  // INT (32-bit)
					_int_to_str(value, buffer);
					break;
				case 2:  // FLOAT
					_float_to_str(f_value, buffer, 4);  // 默认保留4位小数
					break;
			}
			// 逐字符输出
			for (int i = 0; buffer[i] != '\0'; i++)
				ARS_pc(buffer[i], taskId);
			break;
		}
		//接收一个字符的输入
		//该函数只能以地址作为参数
		case KEYINPUT:
			FindPhyMemOffByID(taskId, params[0]);
			setByte(ARS_gc(taskId),taskId); 
			break;
		//输入数值
		//DWORD，INT或FLOAT
		case VALINPUT: {
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
				current_char = ARS_gc(taskId);
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
			char *ptr = inputBuf;
			if (*ptr == '-') {
				is_negative = 1;
				ptr++;
			}
			while (*ptr != '\0') {
				if (*ptr == '.') {
					has_decimal = 1;
					ptr++;
					continue;
				}
				if (*ptr >= '0' && *ptr <= '9') {
					if (!has_decimal) {
						val = val * 10 + (*ptr - '0');
					} else {
						val += (*ptr - '0') * decimal_factor;
						decimal_factor *= 0.1f;
					}
				} else { // 遇到非数字字符终止解析
					break;
				}
				ptr++;
			}
			// 应用负号
			if (is_negative) val = -val;
			// 类型转换与存储
			switch (ParamType) {
				case 0: { // DWORD (int16_t)
					if (val > 32767 || val < -32768) {
						return OVERFLOW_ERR;
					}
					setDByte((int16_t)val, taskId);
					break;
				}
				case 1: { // INT (int32_t)
					if (val > 2147483647 || val < -2147483648) {
						return OVERFLOW_ERR;
					}
					setInt((int32_t)val, taskId);
					break;
				}
				case 2: { // FLOAT
					setFloat((float)val, taskId);
					break;
				}
				default:
					return INVALID_TYPE;
			}
			break;
		}
		case READFILE: {
			char nameBuff[256], i = 0;
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
		break;
		case WRITEFILE: {
			char nameBuff[256], i = 0;
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
		break;
		case DEL_FILE: {
			char nameBuff[256], i = 0;
			FindPhyMemOffByID(taskId, params[0]);
			while (*CurPhyMem) {
				nameBuff[i++] = findByteWithAddr(taskId);
			}
			nameBuff[i] = 0; //文件名
			del_file(nameBuff);
		}
		break;
		// 将参数2（立即数或地址）存入参数1表示的地址内存
		case MOV: {
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
			case 1://DWORD
				if (ParamType == 0) {
					FindPhyMemOffByID(taskId, params[0]);
					setDByte((int16_t)params[1], taskId);
				} else {
					FindPhyMemOffByID(taskId, params[1]);
					int16_t x = findDByteWithAddr(taskId);
					FindPhyMemOffByID(taskId, params[0]);
					setDByte(x, taskId);
				}
				break;
			case 2://INT
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
			case 3://FLOAT
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
		break;
		//将CalcResu的值保存于参数表示的地址中
		case PUSH:
			FindPhyMemOffByID(taskId, params[0]);
			//保存为BYTE
			if (ParamType == 0) {
				setByte((int8_t)CalcResu[taskId], taskId);
				//保存为DWORD
			} else if (ParamType == 1) {
				setDByte((int16_t)CalcResu[taskId], taskId);
				//保存为INT？我不知道四字节的变量怎么称呼
			} else if (ParamType == 2) {
				setInt((int32_t)CalcResu[taskId], taskId);
			} else {
				setFloat((float)CalcResu[taskId], taskId);
			}
			break;
		//子程序传参
		case PUSHP:
			if (params[0] == 0) {
				if (ParamType == 0) {
					Stack[IndexOfSPS].DATA.BYTE = (int8_t)params[1];
					Stack[IndexOfSPS].Type = 1;
				} else if (ParamType == 1) {
					Stack[IndexOfSPS].DATA.DWORD = (int16_t)params[1];
					Stack[IndexOfSPS].Type = 2;
				} else if (ParamType == 2) {
					Stack[IndexOfSPS].DATA.INT = (int32_t)params[1];
					Stack[IndexOfSPS].Type = 3;
				} else {
					Stack[IndexOfSPS].DATA.FLOAT = tranIntToFloat(params[1]);
					Stack[IndexOfSPS].Type = 4;
				}
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				if (ParamType == 0) {
					Stack[IndexOfSPS].DATA.BYTE = findByteWithAddr(taskId);
					Stack[IndexOfSPS].Type = 1;
				} else if (ParamType == 1) {
					Stack[IndexOfSPS].DATA.DWORD = findDByteWithAddr(taskId);
					Stack[IndexOfSPS].Type = 2;
				} else if (ParamType == 2) {
					Stack[IndexOfSPS].DATA.INT = findIntWithAddr(taskId);
					Stack[IndexOfSPS].Type = 3;
				} else {
					Stack[IndexOfSPS].DATA.FLOAT = tranIntToFloat(params[1]);
					Stack[IndexOfSPS].Type = 4;
				}
			}
			IndexOfSPS++;
			if (IndexOfSPS >= OS_MAX_PARAM) {
				return OUT_PARAM_BOUND;
			}
			break;
		//子程序调用
		case CALL:
			//保存上下文数据
			//这里没办法，必须强制将地址和数值相互转化，因为需要将相关信息写入内存
			int32_t CurAddrOfMemPtr = (volatile int32_t)(void *)CurPhyMem[taskId];
			int32_t CurAddrOfCmd = (volatile int32_t)(void *)CurCmd[taskId];
			//程序命令指针指向参数所表示的地址
			CurCmd[taskId] = (volatile uint8_t *)params[0];
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
						setDByte(Stack[i].DATA.DWORD, taskId);
					} else if (Stack[i].Type == 3) {
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
			break;
		//销毁是容易的
		//但建立是困难的
		case RET:
			//提取上下文信息
			int32_t Mem = *(volatile int32_t *)MemTail[taskId];
			int32_t Cmd = *(volatile int32_t *)(MemTail[taskId] + sizeof(int32_t));
			//销毁变量（内存层级在该函数中自减）
			DelLastFuncMem(taskId);
			//跳回上文
			CurPhyMem[taskId] = (volatile uint8_t *)Mem;
			CurCmd[taskId] = (volatile uint8_t *)Cmd;
			break;
		//数值比较判断
		case EQ:
		case LT:
		case GT:
		case LE:
		case GE:
		case NE: {
			// 解析参数类型和数据大小
			uint8_t dataSize = params[0]; // 新增参数：1=BYTE, 2=DWORD, 4=INT/FLOAT
			if (dataSize != 1 && dataSize != 2 && dataSize != 4) {
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
			break;
		}
		//命令指针跳转到相应地址
		case JMP:
			CurCmd[taskId] = (volatile uint8_t *)(OS_EXE_LOAD_START + params[0]);
			break;
		case JMP_T:
			if (CalcResu[taskId])CurCmd[taskId] = (volatile uint8_t *)(OS_EXE_LOAD_START + params[0]);
			break;
		// 在 INTERPRETER.c 的 interprete 函数中修改以下部分
		case ADD:
		case SUB:
		case MUL:
		case DIV: {
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
			}
			break;
		}
		case HLT:
			return 1;
			break;
		//未完待续
		default:
			return -1;
			break;
	}
}
