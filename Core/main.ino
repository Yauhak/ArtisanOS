#include "INTERPRETER.h"
#include "Memory.h"
#include "ByteCode.h"

extern ars_i32 CalcResu[OS_MAX_TASK];
extern int needJump[OS_MAX_TASK];
extern volatile uars_i8 *CurCmd[OS_MAX_TASK];

/*
	MOV/SETARRAY/READARRAY/INITARRAY/ADD/SUB/MUL/DIV/EQ/LT/GT/LE/GE/NE
		具有两个参数（立即数或地址）
	PUSH      只有一个参数（目标地址）
	PUSHP     只有一个参数（立即数或地址）
	JMP/JMP_T/CALL  只有一个参数（目标偏移）
	RET/HLT   没有参数
	BIT_AOX   三个参数（运算种类 + 两个操作数）
	BIT_MOV   两个参数
	ABI_INVOKE/VAL/TO_INT/TO_FLOAT  一个参数
	REG_WRITE/REG_READ/IPC_SEND/IPC_RECV  两个参数

	特别注意 INITARRAY：它的参数是变长的（末尾跟着"标签+数据"的数组初始化表），
	所以这里只记录两个固定参数，变长部分由 init_array() 自行跨越
	（init_array 内部会置位 needJump，调度器据此跳过自动步进）
*/
const char paramQ[] = { 2, 2, 2, 2, 1,
                        1, 2, 2, 2, 2,
                        2, 2, 2, 2, 2,
                        2, 1, 1, 1, 0,
                        3, 2, 1, 2, 2,
                        1, 1, 1, 2, 2,
                        0 };

//	[0..3]          main 函数头偏移 H（编译器写入，解释器并不直接读取）
//	...             各子程序（4 字节变量区大小 + 子程序主体指令）
//	[H]             main 的变量区大小
//	[H+4 ...]       main 的第一条指令
static void loadTask(uars_i8 taskId) {
  switch (taskId) {
    case 0:
      ARS_memset((void *)OS_EXE_LOAD_START(taskId), LED_Flash, LED_Flash_Len);
      break;
    case 1:
      ARS_memset((void *)OS_EXE_LOAD_START(taskId), LED_Stream, LED_Stream_Len);
      break;
    default:
      return;
  }
  call(0, (ars_i32 *)OS_EXE_LOAD_START(taskId), taskId);
  needJump[taskId] = 0;
}

void setup() {
  //Serial.begin(115200);
  delay(1000);
  //Serial.println("RP2040 ARS VM Starting...");
  init_mem_info();
  loadTask(0);  //任务0：光敏电阻（13号引脚DO）控制6号引脚的LED
  loadTask(1);  //任务1：流水灯
  //Serial.println("Load Programs successed!");
}

void loop() {
  static uars_i8 tid = 0;
  if (CurCmd[tid]) {
    //取指令（操作码只占 1 字节）
    uars_i8 ins = *CurCmd[tid]++;
    uars_i8 op = ins >> 3;
    ars_i32 params[3] = { 0, 0, 0 };
    uars_i8 argc = (uars_i8)paramQ[op];
    if (argc) {
      ARS_memset(params, (const void *)CurCmd[tid], sizeof(ars_i32) * argc);
    }
    interprete(ins, params, tid);
    //若处理函数没有自行跳转，则越过本条指令的全部参数
    if (!needJump[tid] || (op == JMP_T && !CalcResu[tid])) {
      CurCmd[tid] += sizeof(ars_i32) * argc;
    }
    needJump[tid] = 0;
  }
  tid = (tid + 1) % OS_MAX_TASK;
  //让出 CPU 以便其他 Arduino 任务（如串口处理）运行
  //实际生产中可去掉该delay
  delay(1);
}
