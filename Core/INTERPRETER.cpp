#include "INTERPRETER.h"
#include "Memory.h"
#include "Glue.h"
//extern from Memory.cpp
extern volatile uars_i8 *CurPhyMem[OS_MAX_TASK];
extern volatile uars_i8 *CurCmd[OS_MAX_TASK];
extern volatile uars_i8 *MemTail[OS_MAX_TASK];
extern ars_i16 MemLevel[OS_MAX_TASK];
extern uars_i8 Stack[OS_MAX_TASK][OS_MAX_PARAM] __attribute__((aligned(4)));
extern uars_i8 IndexOfSPS[OS_MAX_TASK];
#define INVALID_INPUT -10
#define OVERFLOW_ERR -11
#define INVALID_TYPE -12
ars_i32 CalcResu[OS_MAX_TASK] = { 0 };  //一些运算的运行结果寄存
int needJump[OS_MAX_TASK] = { 0 };
Mail Msgs[OS_MAX_TASK] = { 0 };  //IPC邮箱
//字节码-函数映射表
typedef ars_i8 (*OpHandler)(uars_i8, ars_i32 *, uars_i16);
static OpHandler opcode_table[HLT + 1] = {
	[MOV] = mov,
	[SETARRAY] = set_read_array,
	[READARRAY] = set_read_array,
	[INITARRAY] = init_array,
	[PUSH] = push,
	[PUSHP] = pushp,
	[ADD] = calc,
	[SUB] = calc,
	[MUL] = calc,
	[DIV] = calc,
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
	[BIT_AOX] = bit_and_or_xor,
	[BIT_MOV] = bit_move,
	[ABI_INVOKE] = abi_invoke,
	[REG_WRITE] = reg_write,
	[REG_READ] = reg_read,
	[VAL] = val,
	[TO_INT] = to_int,
	[TO_FLOAT] = to_float,
	[IPC_SEND] = ipc_send,
	[IPC_RECV] = ipc_recv,
	[HLT] = hlt,
};

//可以叫赋值？
//MOV [地址][立即数或地址]
/*关于mov的一些小技巧
	bit_mov L $var_int 24
	mov B $var_byte $var_int
	;int->byte，将int的最低位字节移到最前方，mov B表示只会读取一个字节的数据（在该例子中即移至最高位的最低位字节）
	mov I $var_int $var_byte
	bit_mov R $var_int 24
	;byte->int，将byte连带后续三个字节一起读取至$var_int并通过位移消去后三个垃圾字节
	;单独的mov指令只能实现同类型变量的赋值
	;直接处理类型长度不一致的变量时会截断或越界
	;但配合bit_mov就可以实现不同长度变量间的赋值了
*/
ars_i8 mov(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	uars_i8 type = (ParamType & 0x06) >> 1;
	ParamType &= 0x01;
	switch (type) {
		case 0:  //BYTE
			if (ParamType == 0) {
				FindPhyMemOffByID(taskId, params[0]);
				setByte((ars_i8)params[1], taskId);
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				ars_i8 x = findByteWithAddr(taskId);
				FindPhyMemOffByID(taskId, params[0]);
				setByte(x, taskId);
			}
			break;
		case 1:  //INT
			if (ParamType == 0) {
				FindPhyMemOffByID(taskId, params[0]);
				setInt((ars_i32)params[1], taskId);
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				ars_i32 x = findIntWithAddr(taskId);
				FindPhyMemOffByID(taskId, params[0]);
				setInt(x, taskId);
			}
			break;
		case 2:  //FLOAT
			if (ParamType == 0) {
				FindPhyMemOffByID(taskId, params[0]);
				float x = copyIntToFloat(params[1]);
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

//INIT_ARRAY [array] [count] [[1-byte tag(addr/imm)][param]...]
/*
	CurCmd 的推进（与调度器约定一致，务必保持）：
		进入本函数时，调度器只越过并消费了操作码字节（+1）
		由于本函数会把 CurCmd 直接指向"变长初始化表"，
		调度器看到 needJump 后【不会】再补加参数长度
		所以这里必须自己越过 2 个固定参数（数组偏移、元素个数）
		随后循环逐项解析变长表，把 CurCmd 带到本条指令之后
	若漏掉这个 +2*sizeof(ars_i32)，CurCmd 会停在固定参数中间，
	变长表被当成指令解析，其后的整个指令流随之错位，程序必然跑飞
*/
ars_i8 init_array(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	needJump[taskId] = 1;
	CurCmd[taskId] += 2 * sizeof(ars_i32);
	uars_i8 ivk_type = (ParamType & 0x06) >> 1;
	ParamType &= 0x01;
	ars_i32 count;
	if (ParamType == 0) {
		count = params[1];
	} else {
		FindPhyMemOffByID(taskId, params[1]);
		count = findIntWithAddr(taskId);
	}
	FindPhyMemOffByID(taskId, params[0]);
	ars_i32 i32, addr;
	float f32;
	for (int i = 0; i < count; i++) {
		uars_i8 tag = *CurCmd[taskId]++;
		switch (ivk_type) {
			case 0:
				{
					if (!tag) {
						setByte(*CurCmd[taskId]++, taskId);
					} else {
						ARS_memmove(&addr, CurCmd[taskId], sizeof(ars_i32));
						CurCmd[taskId] += sizeof(ars_i32);
						FindPhyMemOffByID(taskId, addr);
						setByte(findByteWithAddr(taskId), taskId);
					}
					break;
				}
			case 1:
				{
					if (!tag) {
						ARS_memmove(&i32, CurCmd[taskId], sizeof(ars_i32));
						setInt(i32, taskId);
						CurCmd[taskId] += sizeof(ars_i32);
					} else {
						ARS_memmove(&addr, CurCmd[taskId], sizeof(ars_i32));
						CurCmd[taskId] += sizeof(ars_i32);
						FindPhyMemOffByID(taskId, addr);
						setInt(findIntWithAddr(taskId), taskId);
					}
					break;
				}
			case 2:
				{
					if (!tag) {
						ARS_memmove(&f32, CurCmd[taskId], sizeof(float));
						setFloat(f32, taskId);
						CurCmd[taskId] += sizeof(float);
					} else {
						ARS_memmove(&addr, CurCmd[taskId], sizeof(ars_i32));
						CurCmd[taskId] += sizeof(ars_i32);
						FindPhyMemOffByID(taskId, addr);
						setFloat(findFloatWithAddr(taskId), taskId);
					}
					break;
				}
		}
	}
}

//SET_ARRAY/READ_ARRAY [array] [index]
ars_i8 set_read_array(uars_i8 cmdAndPmTp, ars_i32 *params, uars_i16 taskId) {
	uars_i8 cmd = cmdAndPmTp >> 3;
	uars_i8 ivk_type = (cmdAndPmTp & 0x06) >> 1;
	cmdAndPmTp &= 0x01;
	ars_i32 index;
	if (cmdAndPmTp == 0) {
		index = params[1];
	} else {
		FindPhyMemOffByID(taskId, params[1]);
		index = findIntWithAddr(taskId);
	}
	FindPhyMemOffByID(taskId, params[0]);
	CurPhyMem[taskId] += (!ivk_type ? 1 : 4) * index;
	switch (ivk_type) {
		case 0:
			{
				if (cmd == READARRAY)
					CalcResu[taskId] = findByteWithAddr(taskId);
				else
					setByte(CalcResu[taskId], taskId);
				break;
			}
		case 1:
			{
				if (cmd == READARRAY)
					CalcResu[taskId] = findIntWithAddr(taskId);
				else
					setInt(CalcResu[taskId], taskId);
				break;
			}
		case 2:
			{
				if (cmd == READARRAY)
					CalcResu[taskId] = copyFloatToInt(findFloatWithAddr(taskId));
				else {
					float x = copyIntToFloat(CalcResu[taskId]);
					setFloat(x, taskId);
				}
				break;
			}
	}
}

//VAL [立即数或地址]
//根据类型和地址标志，将值存入 CalcResu[taskId]
ars_i8 val(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	uars_i8 type = (ParamType & 0x06) >> 1;  //类型：0=B,1=I,2=F
	uars_i8 is_addr = ParamType & 0x01;      //0=立即数，1=地址
	if (is_addr) {
		FindPhyMemOffByID(taskId, params[0]);
		if (type == 0) {
			CalcResu[taskId] = findByteWithAddr(taskId);  //字节符号扩展
		} else if (type == 1) {
			CalcResu[taskId] = findIntWithAddr(taskId);
		} else {  //float
			float f = findFloatWithAddr(taskId);
			CalcResu[taskId] = copyFloatToInt(f);
		}
	} else {
		//立即数
		if (type == 0) {
			CalcResu[taskId] = (ars_i8)params[0];
		} else if (type == 1) {
			CalcResu[taskId] = params[0];
		} else {  //float
			float f = copyIntToFloat(params[0]);
			CalcResu[taskId] = copyFloatToInt(f);
		}
	}
	return 0;
}

//TO_INT [地址]：将地址处（float）转换为 int 存入 CalcResu
ars_i8 to_int(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	//数只有一个地址，忽略 ParamType（无类型指定）
	FindPhyMemOffByID(taskId, params[0]);
	float f = findFloatWithAddr(taskId);
	CalcResu[taskId] = (ars_i32)f;  //浮点转整数
	return 0;
}

//TO_FLOAT [地址]：将地址处（int）转换为 float 存入 CalcResu
ars_i8 to_float(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	FindPhyMemOffByID(taskId, params[0]);
	ars_i32 i = findIntWithAddr(taskId);
	float f = (float)i;
	//将浮点数按 IEEE754 位模式存入 CalcResu（CalcResu 是 ars_i32，需保持位模式）
	ARS_memmove(&CalcResu[taskId], &f, sizeof(float));
	return 0;
}

//将CalcResu存入内存
//PUSH [内存地址]
ars_i8 push(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	FindPhyMemOffByID(taskId, params[0]);
	//保存为BYTE
	if (ParamType == 0) {
		setByte((ars_i8)CalcResu[taskId], taskId);
		//保存为INT？我不知道四字节的变量怎么称呼
	} else if (ParamType == 1) {
		setInt((ars_i32)CalcResu[taskId], taskId);
	} else {
		setFloat((float)CalcResu[taskId], taskId);
	}
}

//通过ABIs胶水层访问ABI
//在调用ABI前可以通过pushp指令进行参数传递
/*示例：
	mov I $gpio_write 0
	pushp I 10
	pushp I 1
	abi_invoke $gpio_write
*/
ars_i8 abi_invoke(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	if (ParamType == 0) {
		ABIs[params[0]](Stack[taskId], taskId);
	} else {
		FindPhyMemOffByID(taskId, params[0]);
		uars_i32 index = (uars_i32)findIntWithAddr(taskId);
		ABIs[index](Stack[taskId], taskId);
	}
	for (int j = 0; j < IndexOfSPS[taskId]; j++) {
		Stack[taskId][j] = 0;
	}
	IndexOfSPS[taskId] = 0;
}

//向指定地址的寄存器写入一/四字节指定值
//其中指定地址一定是四字节
//此处的ParamLen表示指定值的长度（0：一字节；4：四字节）
//至于四字节参数到底是整型还是浮点型...其实可以不怎么关心
//因为在我的设计中，所有内存操作函数几乎都是基于位操作的
//除了在最终写入时需要强制指定传入参数类型
//对于浮点参数而言可以使用以下技巧：
/*
	mov F $var_int $val_float
	;通过mov将float值位拷贝进int类型变量中
	reg_write $addr $var_int
	;reg_write在该情况下会强制转换为ars_i32
	;但由于基于位拷贝，int类型变量的内存内容完全与float值内存内容一样
	;就规避了类型不支持的问题
*/
ars_i8 reg_write(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	int addr, val, ParamLen = ParamType & 0x04;
	ParamType &= 0x03;
	if (ParamType == 0) {  //两个立即数
		addr = params[0];
		ARS_memset(&val, &params[1], ParamLen ? 4 : 1);
	} else if (ParamType == 1) {  //参数1为地址，参数2为立即数
		FindPhyMemOffByID(taskId, params[0]);
		addr = findIntWithAddr(taskId);
		ARS_memset(&val, &params[1], ParamLen ? 4 : 1);
	} else if (ParamType == 2) {  //参数2为地址，参数1为立即数
		FindPhyMemOffByID(taskId, params[1]);
		val = ParamLen ? findIntWithAddr(taskId) : findByteWithAddr(taskId);
		addr = params[0];
	} else if (ParamType == 3) {  //两个参数均为地址
		FindPhyMemOffByID(taskId, params[0]);
		addr = findIntWithAddr(taskId);
		FindPhyMemOffByID(taskId, params[1]);
		val = ParamLen ? findIntWithAddr(taskId) : findByteWithAddr(taskId);
	}
	if (!ParamLen)
		*((volatile ars_i8 *)addr) = (ars_i8)val;
	else
		*((volatile ars_i32 *)addr) = (ars_i32)val;
}

//在指定地址的寄存器读取一或四字节指定值
ars_i8 reg_read(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	int ParamLen = ParamType & 0x02;
	ParamType &= 0x01;
	ars_i32 readContent;
	if (ParamType == 0) {
		readContent = ParamLen ? *((ars_i32 *)params[1]) : *((ars_i8 *)params[1]);
	} else {
		FindPhyMemOffByID(taskId, params[1]);
		uars_i32 trueAddr = (uars_i32)findIntWithAddr(taskId);
		readContent = ParamLen ? *((ars_i32 *)trueAddr) : *((ars_i8 *)trueAddr);
	}
	FindPhyMemOffByID(taskId, params[0]);
	if (!ParamLen)
		setByte(readContent, taskId);
	else setInt(readContent, taskId);
}

//向指定任务通过IPC发送信息
//其中信息一定是四字节
//id一定是一字节
//ipc收发的代码格式跟上述reg_write/reg_read很像
ars_i8 ipc_send(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	ParamType &= 0x07;
	int id, val;
	if (ParamType == 0) {  //两个立即数
		id = params[0];
		val = params[1];
	} else if (ParamType == 1) {  //参数1为地址，参数2为立即数
		FindPhyMemOffByID(taskId, params[0]);
		id = findByteWithAddr(taskId);
		val = params[1];
	} else if (ParamType == 2) {  //参数2为地址，参数1为立即数
		FindPhyMemOffByID(taskId, params[1]);
		val = findIntWithAddr(taskId);
		id = params[0];
	} else if (ParamType == 3) {  //两个参数均为地址
		FindPhyMemOffByID(taskId, params[0]);
		val = findIntWithAddr(taskId);
		FindPhyMemOffByID(taskId, params[1]);
		id = findByteWithAddr(taskId);
	}
	//与寄存器操作指令不一样的是，ipc_send有返回值
	//如果消息发送成功则通过calcResu返回1
	//如果flag非0（通道被占用）则返回0
	if (!Msgs[id].flag) {
		Msgs[id].content = val;
		Msgs[id].flag = 1;
		CalcResu[taskId] = 1;
	} else {
		CalcResu[taskId] = 0;
	}
}

//读取（自己的）IPC信息
ars_i8 ipc_recv(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	//必须先置初值：没有消息时 recv 原本保持未初始化状态
	//会把栈上的随机值写进程序内存，属于难以复现的内存污染
	ars_i32 recv = 0;
	if (Msgs[taskId].flag) {
		recv = Msgs[taskId].content;
		Msgs[taskId].flag = 0;
	}
	FindPhyMemOffByID(taskId, params[0]);
	setInt(recv, taskId);
}

//子程序参数栈压入参数
//PUSHP [子程序参数的内存地址或立即数]
//此处的子程序参数缓存栈可以与ABI序列化参数栈复用
//算是我之前无意间的缓存栈设计带来的好处吧（笑
//pushp进去的形参会以立即数的形式在缓存栈内紧密相连（ByVal传参）
//参数的排序信息则存储在“应用程序”中
ars_i8 pushp(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	uars_i8 p = ParamType & 0x01;
	ParamType = (ParamType & 0x06) >> 1;
	//越界防护必须在写入之前进行
	//原先的检查放在写入之后，一旦 IndexOfSPS 到达上限，写入已经越过了 Stack，
	//会直接踩坏相邻任务（OS_MAX_TASK 个任务的 Stack 是连续排列的）的参数栈
	//这里按"本次还要写入几字节"预留空间，写不下就整体拒绝
	uars_i8 need = (ParamType == 0) ? 1 : 4;
	if ((uars_i16)IndexOfSPS[taskId] + need > OS_MAX_PARAM) {
		return OUT_PARAM_BOUND;
	}
	//指令参数为立即数
	if (p == 0) {
		//0:Byte;1:Int;2:Float
		if (ParamType == 0) {
			ars_i8 tmp = (ars_i8)params[0];
			ARS_memset(&Stack[taskId][IndexOfSPS[taskId]], &tmp, 1);
			IndexOfSPS[taskId]++;
		} else if (ParamType == 1) {
			ARS_memset(&Stack[taskId][IndexOfSPS[taskId]], &params[0], 4);
			IndexOfSPS[taskId] += 4;
		} else {
			float f = copyIntToFloat(params[0]);
			ARS_memset(&Stack[taskId][IndexOfSPS[taskId]], &f, 4);
			IndexOfSPS[taskId] += 4;
		}
		//指令参数为地址
	} else {
		FindPhyMemOffByID(taskId, params[0]);
		if (ParamType == 0) {
			ars_i8 b = findByteWithAddr(taskId);
			ARS_memset(&Stack[taskId][IndexOfSPS[taskId]], &b, 1);
			IndexOfSPS[taskId]++;
		} else if (ParamType == 1) {
			ars_i32 i = findIntWithAddr(taskId);
			ARS_memset(&Stack[taskId][IndexOfSPS[taskId]], &i, 4);
			IndexOfSPS[taskId] += 4;
		} else {
			float f = findFloatWithAddr(taskId);
			ARS_memset(&Stack[taskId][IndexOfSPS[taskId]], &f, 4);
			IndexOfSPS[taskId] += 4;
		}
	}
	return 0;
}

//调用子程序
//CALL [子程序编号，在编译过程中确定]
/*
	返回地址的保存位置必须与调度器的推进保持一致：
	CALL 指令在字节码中占 1 + 4 = 5 字节（1字节操作码 + 4字节程序入口偏移）
	进入本函数时调度器已经消费掉操作码，CurCmd 正指向那 4 字节入口偏移，
	因此"下一条指令"的偏移是 CurCmd - base + 4
	若这里少加了这 4 字节，保存下来的返回地址会落在 CALL 参数的中间，
	RET 之后就会从一条指令的中间继续执行，整个指令流随之错位
	（这正是"循环中反复调用子程序"的流水灯/闪烁灯程序会跑飞的原因）
*/
ars_i8 call(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	needJump[taskId] = 1;
	//内存层级+1
	MemLevel[taskId]++;
	//保存上下文数据：跳过 CALL 自己的 4 字节入口偏移，指向下一条指令
	//在rp2040这种32位环境下指针大小为4，与int大小一致
	uars_i32 CurAddrOfCmd = CurCmd[taskId] - OS_EXE_LOAD_START(taskId) + sizeof(ars_i32);
	//程序命令指针指向参数所表示的地址
	uars_i32 tmpCurCmd = (uars_i32)params[0];
	//前四个字节代表运行所需内存总大小（包括形参）
	//这个值在编译过程中确定
	uars_i32 ReqMemSize;
	ARS_memset(&ReqMemSize, (void *)(OS_EXE_LOAD_START(taskId) + tmpCurCmd), 4);
	//跳过四字节进入程序主体
	tmpCurCmd += sizeof(uars_i32);
	//分配内存
	//分配完后CurPhyMem[taskId]跳转至分配的内存的首地址
	findFreeMemById(taskId, ReqMemSize, MemLevel[taskId]);
	CurCmd[taskId] = (volatile uars_i8 *)(OS_EXE_LOAD_START(taskId) + tmpCurCmd);
	//跳过魔术字头
	CurPhyMem[taskId] += sizeof(Magic);
	//压入上下文数据
	ARS_memset((void *)CurPhyMem[taskId], &CurAddrOfCmd, 4);
	CurPhyMem[taskId] += sizeof(ars_i32);
	//压入参数（注意这里是 CurPhyMem[taskId]，即已经越过魔术字头和上下文的首地址）
	ARS_memset(CurPhyMem[taskId], Stack[taskId], IndexOfSPS[taskId]);
	//销毁参数栈的形参
	for (int j = 0; j < IndexOfSPS[taskId]; j++) {
		Stack[taskId][j] = 0;
	}
	IndexOfSPS[taskId] = 0;
	return 0;
}

//子程序返回上文
//无参数
ars_i8 ret(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	needJump[taskId] = 1;
	//提取上下文信息
	uars_i32 Cmd;
	ARS_memset(&Cmd, (void *)(MemTail[taskId] + sizeof(Magic)), 4);
	//销毁变量（内存层级在该函数中自减）
	DelLastFuncMem(taskId);
	//跳回上文
	CurCmd[taskId] = (volatile uars_i8 *)(Cmd + OS_EXE_LOAD_START(taskId));
}

//条件判断
ars_i8 conds(uars_i8 cmdAndPmTp, ars_i32 *params, uars_i16 taskId) {
	// 前五个字节代表命令
	uars_i8 cmd = cmdAndPmTp >> 3;
	// 后三个字节共同代表参数的一些性质
	uars_i8 ParamType = cmdAndPmTp & 0x07;
	// 解析参数类型和数据大小
	//用double来覆盖所有类型最大可表示的值
	//简化操作
	double val1, val2;
	//x用来判断参数是不是float类型（ParamType第三位）
	uars_i8 x = (ParamType & 0x04) >> 2;
	//无论如何去掉第三位，否则可能参数误判
	ParamType &= 0x03;
	// 读取参数1的值（根据ParamType）
	if (ParamType == 0) {  // 两个立即数
		val1 = x ? copyIntToFloat(params[0]) : params[0];
		val2 = x ? copyIntToFloat(params[1]) : params[1];
	} else if (ParamType == 1) {  // 参数1为地址，参数2为立即数
		// 从内存读取参数1的值
		FindPhyMemOffByID(taskId, params[0]);
		val1 = x ? findFloatWithAddr(taskId) : findIntWithAddr(taskId);
		// 立即数参数2处理
		val2 = x ? copyIntToFloat(params[1]) : params[1];
	} else if (ParamType == 2) {  // 参数2为地址，参数1为立即数
		// 从内存读取参数2的值
		FindPhyMemOffByID(taskId, params[1]);
		val2 = x ? findFloatWithAddr(taskId) : findIntWithAddr(taskId);
		// 立即数参数1处理
		val1 = x ? copyIntToFloat(params[0]) : params[0];
	} else if (ParamType == 3) {  // 两个参数均为地址
		// 读取参数1的地址
		FindPhyMemOffByID(taskId, params[0]);
		val1 = x ? findFloatWithAddr(taskId) : findIntWithAddr(taskId);
		// 读取参数2的地址
		FindPhyMemOffByID(taskId, params[1]);
		val2 = x ? findFloatWithAddr(taskId) : findIntWithAddr(taskId);
	}
	// 根据指令进行比较
	switch (cmd) {
		case EQ:
			CalcResu[taskId] = (val1 == val2);
			break;
		case LT:
			CalcResu[taskId] = (val1 < val2);
			break;
		case GT:
			CalcResu[taskId] = (val1 > val2);
			break;
		case LE:
			{
				CalcResu[taskId] = (val1 <= val2);
			}
			break;
		case GE:
			CalcResu[taskId] = (val1 >= val2);
			break;
		case NE:
			CalcResu[taskId] = (val1 != val2);
			break;
		default:
			return -1;  //非法指令
	}
}

//无条件跳转
ars_i8 jmp(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	needJump[taskId] = 1;
	CurCmd[taskId] = (volatile uars_i8 *)(OS_EXE_LOAD_START(taskId) + params[0]);
}

//情况成立（CalcResu不为0）跳转
ars_i8 jmp_t(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	needJump[taskId] = 1;
	if (CalcResu[taskId]) CurCmd[taskId] = (volatile uars_i8 *)(OS_EXE_LOAD_START(taskId) + params[0]);
}

//加减乘除
ars_i8 calc(uars_i8 cmdAndPmTp, ars_i32 *params, uars_i16 taskId) {
	uars_i8 cmd = cmdAndPmTp >> 3;
	//后三个字节共同代表参数的一些性质
	uars_i8 ParamType = cmdAndPmTp & 0x07;
	//新增参数类型标识：ParamType 的第三位表示是否为浮点运算 (1=float)
	uars_i8 is_float = (ParamType & 0x04) >> 2;  //取第三位
	ParamType &= 0x03;                           //保留原始参数类型
	float val1_f, val2_f, result_f;
	ars_i32 val1_i, val2_i, result_i;
	//根据参数类型读取操作数（支持立即数、地址、混合类型）
	if (is_float) {
		// 处理浮点运算
		if (ParamType == 0) {  //两个立即数（需将 ars_i32 转换为 float）
			val1_f = copyIntToFloat(params[0]);
			val2_f = copyIntToFloat(params[1]);
		} else if (ParamType == 1) {  //参数1为地址，参数2为立即数
			FindPhyMemOffByID(taskId, params[0]);
			val1_f = findFloatWithAddr(taskId);
			val2_f = copyIntToFloat(params[1]);
		} else if (ParamType == 2) {  //参数2为地址，参数1为立即数
			FindPhyMemOffByID(taskId, params[1]);
			val1_f = findFloatWithAddr(taskId);
			val2_f = copyIntToFloat(params[0]);
		} else if (ParamType == 3) {  //两个参数均为地址
			FindPhyMemOffByID(taskId, params[0]);
			val1_f = findFloatWithAddr(taskId);
			FindPhyMemOffByID(taskId, params[1]);
			val2_f = findFloatWithAddr(taskId);
		}
		//执行浮点运算
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
		//将结果转换为 ars_i32 存入 CalcResu（需确保内存对齐）
		ARS_memmove(&CalcResu[taskId], &result_f, sizeof(float));
	} else {
		//原有整数运算逻辑（略作调整）
		if (ParamType == 0) {
			val1_i = params[0];
			val2_i = params[1];
		} else if (ParamType == 1) {
			FindPhyMemOffByID(taskId, params[0]);
			val1_i = findIntWithAddr(taskId);
			val2_i = params[1];
		} else if (ParamType == 2) {
			FindPhyMemOffByID(taskId, params[1]);
			val2_i = findIntWithAddr(taskId);
			val1_i = params[0];
		} else if (ParamType == 3) {
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
}

ars_i8 hlt(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	ReArrangeMemAndTask(taskId);
}

ars_i8 bit_and_or_xor(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	ars_i32 val1_i, val2_i;
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
	CalcResu[taskId] = val1_i;
}

ars_i8 bit_move(uars_i8 ParamType, ars_i32 *params, uars_i16 taskId) {
	ars_i32 val1_i, val2_i;
	uars_i8 operate = (ParamType & 0x04) >> 2;
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
	CalcResu[taskId] = val1_i;
}

//注意！！
//params并不代表它一定表示的是int类型
//可能是与float类型共用相同的四字节内存
//
//关于 CurCmd 的推进约定（非常重要）：
//	操作码本身只占 1 字节，紧跟在它后面的是若干个 4 字节参数
//	调度器（main.ino 的 loop）已经越过操作码字节后才把 params 交给本函数
//	所以本函数只负责"再越过参数"，即各处理函数自行跳转时按 1 + 4*参数个数 推进
//	一旦这里多算或少算 1 字节，后续所有指令都会整体错位
ars_i8 interprete(uars_i8 cmdAndPmTp, ars_i32 *params, uars_i16 taskId) {
	//前五个字节代表命令
	uars_i8 cmd = cmdAndPmTp >> 3;
	//后三个字节共同代表参数的一些性质
	uars_i8 ParamType = cmdAndPmTp & 0x07;
	//非法操作码防护：防止越界访问 opcode_table
	//（损坏的字节码、被踩踏的程序内存都可能产生 >= HLT+1 的操作码）
	if (cmd > HLT) {
		return INVALID_INPUT;
	}
	if (!((cmd >= ADD && cmd <= NE) || cmd == SETARRAY || cmd == READARRAY)) {
		return opcode_table[cmd](ParamType, params, taskId);
	} else {
		return opcode_table[cmd](cmdAndPmTp, params, taskId);
	}
}
