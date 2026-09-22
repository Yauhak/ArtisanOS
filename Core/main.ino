#include "INTERPRETER.h"
#include "Memory.h"
#include "ByteCode.h"

extern ars_i32 CalcResu[OS_MAX_TASK];
extern int needJump[OS_MAX_TASK];
extern volatile uars_i8 *CurCmd[OS_MAX_TASK];

/* ==========================================================================
 * 两种运行方式，由 USE_FILE_AND_UART 切换（定义在 IO_INCLUDE.h）：
 *
 *   0 固件内置模式：字节码随固件烧录在 ByteCode.h 里，直接装载运行。
 *   1 文件驱动模式：从文件系统读取 FCB[0]（SCHEDULE），按行装载多个文件轮转执行，
 *                   并启用串口命令 update/get/del/ls/occ 与字节码的文件 ABI。
 * ========================================================================== */

//每条指令所携带的参数个数
const char paramQ[] = { 2, 2, 2, 2, 1,
                        1, 2, 2, 2, 2,
                        2, 2, 2, 2, 2,
                        2, 1, 1, 1, 0,
                        3, 2, 1, 2, 2,
                        1, 1, 1, 2, 2,
                        0 };

//把程序装载进指定任务的代码区并启动
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

static void execOne(void) {
  static uars_i8 tid = 0;
  if (CurCmd[tid]) {
    uars_i8 ins = *CurCmd[tid]++;
    uars_i8 op = ins >> 3;
    ars_i32 params[3] = { 0, 0, 0 };
    uars_i8 argc = (uars_i8)paramQ[op];
    if (argc) ARS_memset(params, (const void *)CurCmd[tid], sizeof(ars_i32) * argc);
    interprete(ins, params, tid);
    if (!needJump[tid] || (op == JMP_T && !CalcResu[tid]))
      CurCmd[tid] += sizeof(ars_i32) * argc;
    needJump[tid] = 0;
  }
  tid = (tid + 1) % OS_MAX_TASK;
}

#if USE_FILE_AND_UART
/* 把出厂示例铺进文件系统（首次启动、以及 format 之后调用）。
 * 有内容就认为已经铺好了；但内容全是 FF 说明那些页早先被擦掉了，
 * 这种情况下重新铺一遍，免得开机读不出调度表、一个任务都跑不起来。 */
void ARS_provision(void) {
  static const uars_i8 sched[] = "LEDFLASH\nLEDSTREAM\n";
  uars_i8 probe[NAME_LEN];
  long n = readFile("SCHEDULE", probe, sizeof(probe));
  if (n > 0) {
    int blank = 1;
    for (int i = 0; i < n; i++)
      if (probe[i] != 0xFF) blank = 0;
    if (!blank) return;
  }

  createFileAt("SCHEDULE", 0);  //调度表固定占用 0 号目录项
  createFile("LEDFLASH");
  writeFile("LEDFLASH", LED_Flash, LED_Flash_Len);
  createFile("LEDSTREAM");
  writeFile("LEDSTREAM", LED_Stream, LED_Stream_Len);
  writeFile("SCHEDULE", sched, sizeof(sched) - 1);
}

/* 文件系统与调度器的启动推迟到 USB 枚举完成之后。
 * 擦写 FLASH 期间必须关中断（此时不能在 FLASH 上取指）。 */
static void deferredBoot(void) {
  static uars_i8 done = 0;
  if (done) return;
  done = 1;
  delay(1500);       //等主机把描述符取完
  init_mem_info();   //内存管理器必须先初始化，文件驱动模式同样需要
  arsfs_init();      //挂载文件系统（未格式化则自动格式化）
  ARS_provision();   //文件系统为空时写入出厂示例
  arssched_load();   //按 SCHEDULE 文件装载任务
}
#endif

void setup() {
#if USE_FILE_AND_UART
  arsuart_begin(115200);
#else
  //Serial.begin(115200);
  delay(1000);
  init_mem_info();
  loadTask(0);  //任务0：光敏电阻（13号引脚DO）控制6号引脚的LED
  loadTask(1);  //任务1：流水灯
#endif
}

void loop() {
#if USE_FILE_AND_UART
  deferredBoot();
  arssched_loop();  //轮转执行一条指令
  arsuart_poll();   //处理串口命令（非阻塞）
#else
  execOne();
#endif
  delay(1);  //让出 CPU 给底层栈
}
