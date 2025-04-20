#include "INTERPRETER.h"
#define EXP EXE_SC_POS[taskId]<SCREEN_BUFFSIZE?EXE_SC_POS[taskId]++:SCREEN_BUFFSIZE-1
#define CASE if(EXE_SC_POS[taskId]>=SCREEN_BUFFSIZE)\
				ARS_memmove(EXE_SCBUFF[taskId],&EXE_SCBUFF[taskId][1],SCREEN_BUFFSIZE-1);

volatile uint8_t *CurCmd[OS_MAX_TASK];//每个程序当前执行的命令的位置，0表示未启用
//为每个程序都分配的窗口缓冲区
extern uint8_t EXE_SCBUFF[OS_MAX_TASK][SCREEN_BUFFSIZE];
extern uint16_t EXE_SC_POS[OS_MAX_TASK];
extern volatile uint32_t ID;
//置顶窗口的ID
uint8_t TopWindowId;
ParamStack Stack[OS_MAX_PARAM];//子程序参数栈
uint8_t IndexOfSPS = 0; //子程序参数栈压入了几个参数
//上述两个变量共同用于子程序调用完毕后跳转返回的内存和命令
//起到保存上下文的作用
volatile uint8_t *FreeHead = 0;
//上面的变量表示空闲内存链表的表头
//寻找空闲内存从头找
volatile Magic *FreeTail = 0;
volatile uint8_t *MemHead[OS_MAX_TASK];//每个程序占用的物理内存的首地址
volatile uint8_t *MemTail[OS_MAX_TASK];//每个程序最近启动的子程序占用的物理内存的首地址
int16_t MemLevel[OS_MAX_TASK];//每个程序变量作用域最大层级，-2表示未启用，-1在未来打算表示公有变量
volatile uint8_t *LastMEM = (volatile uint8_t *)OS_PHY_MEM_START; //对于物理内存而言，目前占用的总计长度
int32_t CalcResu[OS_MAX_TASK] = {0}; //一些运算的运行结果寄存
volatile uint8_t *CurPhyMem[OS_MAX_TASK]; //“安全”的起始物理内存
//通过"SPLT"字样来分割内存
//"FREE"表示此处空闲
//在我的设想中
//每个主程序或子程序所占用的内存大小在大部分情况下是固定的（可变长度数组暂时不考虑）
//在“编译”成ARS字节码时，该段程序占用的内存信息会被提取出来
//然后在调用时基于此进行内存分配

//16位类型高8位
int8_t HigherInt16(int16_t x) {
	return (int8_t)(x >> 8);
}

//16低8位
int8_t LowerInt16(int16_t x) {
	return (int8_t)((x << 8) >> 8);
}

//32高16位
int16_t HigherInt32(int x) {
	return (int16_t)(x >> 16);
}

//32低16位
int16_t LowerInt32(int x) {
	return (int16_t)((x << 16) >> 16);
}

//合并 双字节
int16_t Tran8To16(int8_t higher, int8_t lower) {
	return ((int16_t)higher << 8) | lower;
}

//合并 四字节
int Tran16To32(int16_t higher, int16_t lower) {
	return ((int)higher << 16) | lower;
}

//修复意外越界的内存
//如果意外越界连魔术字头都修改了，那我们认为是意外
//但如果魔术字没变，其他字段却变动了
//那我们只好怀疑是“黑客”捣的鬼
//不得不直接返回错误了哦~
int8_t ResumeMem(Magic *M, uint8_t id) {
	if (M->Check == CHECK) {
		M->MagicHead[0] = 'S';
		M->MagicHead[1] = 'P';
		M->MagicHead[2] = 'L';
		M->MagicHead[3] = 'T';
		M->id = id;
	} else {
		//最后的守卫字符都修改了
		//救不了了
		return HEAD_ERR;
	}
}

//内存模型：[魔术字块][数据块][末尾三个字节的预留缓冲块]

/*
	空闲块链表并不是按物理地址顺序排序的，而是按释放的时间先后顺序排序，
	每次重利用释放的内存是从头开始查找空闲块的（先释放的先使用），
	在这个时候才会更新FreeHead
	所有合并的内存块来源于链表此前记录的内存块
*/

//合并空闲块
int8_t SuperFree(Magic *block) {
	uint8_t status = 0;
	Magic *ptr = (Magic *)FreeHead;
	if (!ptr) {
		return NO_FREE_MEM;
	}
	Magic *img = 0;
	//从头开始搜索空闲内存
	//img相当于ptr上一步的地址的快照
	//因为要考虑前后合并的情况
	while (ptr->next_block) {
		//空闲内存链表记录的内存块之一恰好在block前，向前合并
		//RESERVED_BLOCKSIZE表示预留缓冲区大小,三字节
		if (!ARS_strcmp(ptr->MagicHead, FREE, 4) && ptr + ptr->len + sizeof(Magic) + RESERVED_BLOCKSIZE == block) {
			//只更新内存块数据区长度
			//把block包括魔术字头的部分全部吞入
			ptr->len += sizeof(Magic) + block->len;
			uint8_t *x = (uint8_t *)block;
			//销毁魔术字头，将其归为内存总体一部分
			for (int i = 0; i < sizeof(Magic); i++) {
				*x++ = 0;
			}
			//status=1表示该块内存与原先记录内存合并
			//不需要追加到FreeTail
			status = 1;
		}
		//空闲内存链表记录的内存块在后，向后合并
		if (!ARS_strcmp(ptr->MagicHead, FREE, 4) && block + block->len + sizeof(Magic) + RESERVED_BLOCKSIZE == ptr) {
			//首先抄一下空闲内存链表记录的信息
			//因为包含了下文
			ARS_memmove((uint8_t *)block, (uint8_t *)ptr, sizeof(Magic));
			block->len += sizeof(Magic) + ptr->len;
			uint8_t *x = (uint8_t *)ptr;
			for (int i = 0; i < sizeof(Magic); i++) {
				*x++ = 0;
			}
			//如果存在上一块空闲内存
			if (img) {
				//那么上一块内存指向的下一块内存地址需要修改
				//改成block的
				img->next_block = (uint8_t *)block;
			}
			//等于二表示前后内存都扫描完了
			//该尝试的合并已经完成
			//可以直接返回结果
			//不需要一直访问到链表末尾
			status = 2;
		}
		if (status == 2) {
			return 0;
		}
		img = ptr;
		if (ptr->next_block)ptr = (Magic *)ptr->next_block;
	}
	//如果找不到可以合并的，提示将该块内存追加到FreeTail
	if (!status)return NEED_APPEND_TO_TAIL;
}

//该函数用在“应用程序”结束后启动
//用来销毁该应用所有的内存
//当整个程序退出时发挥作用
//isForce表示强制销毁内存
//此时内存块数据可能受到严重损坏
int8_t ReArrangeMemAndTask(uint8_t id, uint8_t isForce) {
	//从头开始
	Magic *M = (Magic *)MemHead[id];
	while (M < (Magic *)(OS_PHY_MEM_START + OS_MAX_MEM)) {
		//发现魔术字
		if (!ARS_strcmp((const char*) M->MagicHead, SPLIT, 4) || isForce) {
			//id对应上了
			if (M->id == id || isForce) {
				//魔术字声明：该块内存被释放
				M->MagicHead[0] = 'F';
				M->MagicHead[1] = 'R';
				M->MagicHead[2] = 'E';
				M->MagicHead[3] = 'E';
				//去除魔术字后该块内存的长度
				//加上缓冲字节
				int size = M->len + RESERVED_BLOCKSIZE;
				//如果在强制销毁模式下发现魔术字被破坏的内存
				//并且连守卫文字也被损坏
				//为保护其他内存
				//终止内存的继续销毁
				if (ARS_strcmp((const char*) M->MagicHead, SPLIT, 4) && isForce) {
					if (M->Check != CHECK) {
						//提示进程结束
						//此时仍有部分内存无法释放
						CurCmd[id] = 0;
						return MEM_CLEAN_PARTLY;
					}
				}
				volatile uint8_t *next = M->next_block;
				//跳过Magic头注销内存
				volatile uint8_t *PhyMem = (uint8_t *)(M + sizeof(Magic));
				for (int i = 0; i < size ; i++) {
					*PhyMem++ = 0;
				}
				//记录空闲内存链表信息
				if (FreeHead == 0) {
					FreeHead = (volatile uint8_t *)M;
					FreeTail = (Magic *)FreeHead;
					FreeTail->next_block = 0;
				} else {
					FreeTail->next_block = (volatile uint8_t *)M;
					Magic *Now = (Magic *)M;
					if (SuperFree(Now) == NEED_APPEND_TO_TAIL) {
						FreeTail = M;
						FreeTail->next_block = 0;
					} else {
						FreeTail->next_block = 0;
					}
				}
				//程序没有下一块分配的内存了（注销内存完毕）
				if (next == 0) {
					CurCmd[id] = 0;
					MemLevel[id] = -2;
					return 0;
				}
				//链表跳转至下一块内存
				M = (Magic *)next;
				continue;
			}
			//该段内存ID匹配出错
			return ID_ERR;
		}
		//在非强制模式下该段内存魔术字匹配出错
		else {
			int8_t Repair = ResumeMem(M, id);
			if (Repair == HEAD_ERR) {
				CurCmd[id] = 0;
				return MEM_CLEAN_PARTLY;
			}
		}
	}
	//访问超出最大访问内存的边界
	return OUT_BOUND;
	//对，你没看错
	//就这么简单
}

//销毁某个应用程序最末一个子程序占用的内存
int8_t DelLastFuncMem(uint8_t id) {
	//从尾部开始
	Magic *P = (Magic *)MemTail[id];
	//获取调用该子程序的代码块此前运行到的内存地址和命令地址
	//起到上下文的作用
	volatile uint8_t * LastFuncMem = 0;
	volatile uint8_t * LastFuncCmd = 0;
	Magic *M = (Magic *)MemTail[id];
	//如果当前内存不是主程序占用内存
	//则将最末一个子程序的占用内存与其前面的内存断联
	if (M->next_block && M->last_block && MemLevel[id] > 0) {
		((Magic *)M->last_block)->next_block = 0;
		LastFuncMem = (volatile uint8_t *)(M + sizeof(Magic));
		LastFuncCmd = (volatile uint8_t *)(M + sizeof(Magic) + sizeof(int32_t));
	}
	//不可越界
	while (M < (Magic *)(OS_PHY_MEM_START + OS_MAX_MEM)) {
		//魔术字匹配
		if (!ARS_strcmp((const char*) M->MagicHead, SPLIT, 4)) {
			//ID匹配
			if (M->id == id) {
				//不是主程序
				if (M->level > 0) {
					//声明内存释放
					uint8_t *ptr = (uint8_t *)M;
					*ptr++ = 'F';
					*ptr++ = 'R';
					*ptr++ = 'E';
					*ptr++ = 'E';
					//销毁内存
					M->len += 3;
					//必须加上缓冲字节长度
					ptr += sizeof(Magic) - sizeof(M->MagicHead);
					for (int i = 0; i < M->len; i++) {
						*ptr++ = 0;
					}
					if (FreeHead == 0) {
						FreeHead = (volatile uint8_t *)M;
						FreeTail = (Magic *)FreeHead;
						FreeTail->next_block = 0;
					} else {
						FreeTail->next_block = (volatile uint8_t *)M;
						Magic *Now = (Magic *)M;
						if (SuperFree(Now) == NEED_APPEND_TO_TAIL) {
							FreeTail = M;
							FreeTail->next_block = 0;
						} else {
							FreeTail->next_block = 0;
						}
					}
					if (M->next_block)M = (Magic *)M->next_block;
					else {//内存已全部销毁完毕
						//作用域等级降低
						MemLevel[id]--;
						//跳转回上文
						if (LastFuncMem && LastFuncCmd) {
							CurPhyMem[id] = (volatile uint8_t*)LastFuncMem;
							CurCmd[id] = (volatile uint8_t*)LastFuncCmd;
						}
					}
				} else {
					//此时便是主程序
					ReArrangeMemAndTask(id, 0);
					return 0;
				}
			}
			return ID_ERR;
		} else {
			int8_t Repair = ResumeMem(M, id);
			if (Repair == HEAD_ERR)return HEAD_ERR;
		}
	}
	return OUT_BOUND;
}

//查找空闲的内存空间
//为新的主/子程序分配内存
int findFreeMemById(uint8_t id, int allocLen, int level) {
	//总长度TTL=子程序运行所需内存+4字节调用时 运行内存指针 指向地址+4字节调用时 命令内存指针 指向地址
	//以便RET后跳回调用该子程序的代码块和内存
	int TTL = allocLen + 8;
	//如果当前命令指针指向不为0（即该进程已启用）则指向当前程序的末尾内存地址
	Magic *M;
	if (CurCmd[id])M = (Magic *)MemTail[id];
	//从头查找空闲内存
	uint8_t *M2 = (uint8_t *)FreeHead;
	uint8_t isalloc = 0;
	//下一块空闲块
	volatile uint8_t *next_free;
	while (M2 < (uint8_t *)LastMEM) {
		//发现空闲的魔术字
		if (!ARS_strcmp((const char*) M2, FREE, 4)) {
			//该块内存至少大于Magic头+末尾三个预留字节
			//因为SetDByte和SetInt函数是连续写入内存的
			//但分配给程序的内存在空间上并不是完全连续的
			//可能导致其他内存的意外覆盖
			//而预留缓冲字节是我想到的最简单的处理办法
			if (((Magic*)M2)->len > sizeof(Magic) + RESERVED_BLOCKSIZE) {
				//魔术字覆盖
				*M2 = 'S';
				*(M2 + 1) = 'P';
				*(M2 + 2) = 'L';
				*(M2 + 3) = 'T';
				//ID覆盖
				((Magic*)M2)->id = id;
				//设置在该块空闲内存的占用长度
				//为了方便内存释放后的管理
				//只好统一设定每个FREE内存块重利用时预留后三个字节为缓冲字节
				//释放后要吞并这三个字节
				((Magic*)M2)->len = (TTL > ((Magic*)M2)->len - RESERVED_BLOCKSIZE) ?
				                    ((Magic*)M2)->len - RESERVED_BLOCKSIZE : TTL;
				// 分配后，剩余空间处理：
				// 如果剩余空间大小超过（魔术字头+3字节缓冲）才重新划分
				if (((Magic*)M2)->len > TTL + sizeof(Magic) + RESERVED_BLOCKSIZE) {
					Magic *remaining = (Magic *)((uint8_t *)M2 + TTL + sizeof(Magic));
					remaining->MagicHead[0] = 'F';
					remaining->MagicHead[1] = 'R';
					remaining->MagicHead[2] = 'E';
					remaining->MagicHead[3] = 'E';
					remaining->len = ((Magic*)M2)->len - TTL;
					if (FreeHead == 0) {
						FreeHead = (volatile uint8_t *)remaining;
						FreeTail = (Magic *)FreeHead;
						FreeTail->next_block = 0;
					} else {
						FreeTail->next_block = (volatile uint8_t *)remaining;
						Magic *Now = (Magic *)remaining;
						if (SuperFree(Now) == NEED_APPEND_TO_TAIL) {
							FreeTail = remaining;
							FreeTail->next_block = 0;
						} else {
							FreeTail->next_block = 0;
						}
					}
				}
				((Magic *)M2)->Check = CHECK;
				//链表指向上一块内存
				if (CurCmd[id]) {
					((Magic *)M2)->last_block = (volatile uint8_t *)M;
				} else {
					//若该进程此次才启用
					MemHead[id] = M2;
					MemTail[id] = M2;
					M = (Magic *)M2;
					//此时没有上一块内存
					((Magic *)M2)->last_block = 0;
				}
				next_free = ((Magic *)M2)->next_block;
				//暂无需下一块内存
				((Magic *)M2)->next_block = 0;
				((Magic *)M2)->level = level;
				//需求内存的长度自减
				TTL = TTL - ((Magic*)M2)->len;
				//存在上下块内存的指向关系
				if (CurCmd[id]) {
					//上一块内存指向下一块内存
					M->next_block = M2;
					//跳转至最新分配的内存
					M = (Magic *)M->next_block;
				} else {
					//跳转到“应用程序”的加载地址
					CurCmd[id] = (volatile uint8_t *)OS_EXE_LOAD_START;
				}
				//如果存在下一个空闲块
				if (next_free) {
					//跳转至下一块空闲块
					M2 = (uint8_t *)next_free;
					//空闲块头更新
					FreeHead = (uint8_t *)next_free;
				}
				//内存已分配完毕
				if (TTL <= 0) {
					isalloc = 1;
					break;
				}
			} else {
				int8_t Repair = ResumeMem(M, id);
				if (Repair == HEAD_ERR)return HEAD_ERR;
			}
		}
	}
	//还是没有分配完毕
	if (!isalloc) {
		//视为此前释放的空闲块已经用光，清零
		FreeHead = 0;
		//从目前占用的物理内存的末尾开始启用新内存
		Magic *M3 = (Magic *)LastMEM;
		//越界！
		if (M3 + sizeof(Magic) + TTL >= (Magic *)OS_PHY_MEM_START + OS_MAX_MEM) {
			return OUT_BOUND;
		}
		M3->MagicHead[0] = 'S';
		M3->MagicHead[1] = 'P';
		M3->MagicHead[2] = 'L';
		M3->MagicHead[3] = 'T';
		M3->id = id;
		M3->len = TTL;
		M3->Check = CHECK;
		//双向链表建立
		M3->last_block = (volatile uint8_t *)M;
		M3->next_block = 0;
		M3->level = level;
		M->next_block = (volatile uint8_t *)M3;
		M = (Magic *)M->next_block;
		LastMEM += sizeof(Magic) + TTL;
	}
	//回溯至子程序物理内存的首地址
	while (M >= (Magic *)(OS_PHY_MEM_START)) {
		M = (Magic *)M->last_block;
		if (M->level != level || M->last_block == 0) {
			if (M->level != level)MemTail[id] = M->next_block;
			if (M->last_block == 0)MemTail[id] = (volatile uint8_t *)M;
			//告知虚拟机为主、子程序分配的物理内存首地址
			CurPhyMem[id] = (volatile uint8_t *)MemTail[id];
			return 0;
		}
	}
}

//根据程序id查找其目前访问的“虚拟地址”对应的“物理地址”
uint8_t FindPhyMemOffByID(uint8_t id, uint32_t offset) {
	int VAddr = 0;
	//除了公有变量，活跃的变量一定是最近启动的子程序的变量
	Magic *M = (Magic *)MemTail[id];
	while (M < (Magic *)(OS_PHY_MEM_START + OS_MAX_MEM)) {
		//发现魔术字
		if (!ARS_strcmp((const char*) M->MagicHead, SPLIT, 4)) {
			//id对应上了
			if (M->id == id) {
				//魔术字头记录的内存块长度不包括魔术字头本身
				int size = M->len;
				if (size < 0) {
					return INVALID_LEN;
				}
				if (VAddr + size >= offset) {
					CurPhyMem[id] = ((volatile uint8_t *)M) + sizeof(Magic) + offset - VAddr; //成功查找到
					return 0;
				}
				if (M->next_block == 0 && VAddr + size < offset) {
					return BAD_MEM_TRACE;
				}
				M = (Magic *)M->next_block;
				VAddr += size;
				continue;
			}
			return ID_ERR;
		} else {
			int8_t Repair = ResumeMem(M, id);
			if (Repair == HEAD_ERR)return HEAD_ERR;
		}
	}
	return OUT_BOUND;
}

void ReadByteMem(uint8_t *Recv) {
	*Recv = *CurPhyMem[ID];
}

//访问 单字节
//访问完后自动跳过该段内存
//下方的访问双字节和四字节也是如此
int8_t findByteWithAddr() {
	int8_t Byte_Buff;
	ReadByteMem(&Byte_Buff);
	CurPhyMem[ID]++;
	return Byte_Buff;
}

//访问 双字节
int16_t findDByteWithWithAddr() {
	int16_t value;
	value = *((int16_t *)CurPhyMem[ID]);
	CurPhyMem[ID] += 2;
	return value;
}

//访问 四字节
int32_t findIntWithAddr() {
	int32_t value;
	value = *((int32_t *)CurPhyMem[ID]);
	CurPhyMem[ID] += 4;
	return value;
}

//设置 单字节
//对于多字节类型则拆分成单个字节依次存放
void setByte(int8_t byteText) {
	*CurPhyMem[ID]++ = byteText;
}

void setDByte(int16_t DbyteText) {
	int8_t H = HigherInt16(DbyteText);
	int8_t L = LowerInt16(DbyteText);
	*CurPhyMem[ID]++ = H;
	*CurPhyMem[ID]++ = L;
}

void setInt(int32_t intText) {
	int16_t H = HigherInt32(intText);
	int16_t L = LowerInt32(intText);
	setDByte(H);
	setDByte(L);
}

void init_mem_info() {
	for (int i = 0; i < OS_MAX_TASK; i++) {
		MemHead[i] = 0;
		MemTail[i] = 0;
		MemLevel[i] = -2;
		CurCmd[i] = 0;
		CurPhyMem[i] = (volatile uint8_t *)OS_PHY_MEM_START;
	}
}

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
				CASE
				EXE_SCBUFF[taskId][EXP] = (char)params[0];
				if (TopWindowId == taskId)ARS_pc((char)params[0], taskId);
			} else {
				CASE
				FindPhyMemOffByID(taskId, params[0]);
				EXE_SCBUFF[taskId][EXP] = *CurPhyMem[taskId];
				if (TopWindowId == taskId)ARS_pc(*CurPhyMem[taskId], taskId);
			}
			break;
		//输出字符串
		case PSTR:
			//该函数只能以地址作为参数
			FindPhyMemOffByID(taskId, params[0]);
			while (*CurPhyMem[taskId]++) {
				CASE
				EXE_SCBUFF[taskId][EXP] = *CurPhyMem[taskId];
				if (TopWindowId == taskId)ARS_pc(*CurPhyMem[taskId], taskId);
			}
			break;
		//接收一个字符的输入
		//该函数只能以地址作为参数
		case KEYINPUT:
			FindPhyMemOffByID(taskId, params[0]);
			*CurPhyMem[taskId] = ARS_gc(taskId);
			break;
		// 将参数2（立即数或地址，大小为一个字节）存入参数1表示的地址内存
		case MOV_BYTE:
			if (ParamType == 0) {
				FindPhyMemOffByID(taskId, params[0]);
				setByte((int8_t)params[1]);
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				int8_t x = *(volatile int8_t*)CurPhyMem[taskId];
				FindPhyMemOffByID(taskId, params[0]);
				setByte(x);
			}
			break;
		// 将参数2（立即数或地址，大小为二个字节）存入参数1表示的地址内存
		case MOV_DWRD:
			if (ParamType == 0) {
				FindPhyMemOffByID(taskId, params[0]);
				setDByte((int16_t)params[1]);
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				int16_t x = *(volatile int16_t*)CurPhyMem[taskId];
				FindPhyMemOffByID(taskId, params[0]);
				setDByte(x);
			}
			break;
		// 将参数2（立即数或地址，大小为四个字节）存入参数1表示的地址内存
		case MOV_INT:
			if (ParamType == 0) {
				FindPhyMemOffByID(taskId, params[0]);
				setInt((int32_t)params[1]);
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				int32_t x = *(volatile int32_t*)CurPhyMem[taskId];
				FindPhyMemOffByID(taskId, params[0]);
				setInt(x);
			}
			break;
		//将CalcResu的值保存于参数表示的地址中
		case PUSH:
			FindPhyMemOffByID(taskId, params[0]);
			//保存为BYTE
			if (ParamType == 0) {
				setByte((int8_t)CalcResu[taskId]);
				//保存为DWORD
			} else if (ParamType == 1) {
				setDByte((int16_t)CalcResu[taskId]);
				//保存为INT？我不知道四字节的变量怎么称呼
			} else {
				setInt((int32_t)CalcResu[taskId]);
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
				} else {
					Stack[IndexOfSPS].DATA.INT = (int32_t)params[1];
					Stack[IndexOfSPS].Type = 4;
				}
			} else {
				FindPhyMemOffByID(taskId, params[1]);
				if (ParamType == 0) {
					Stack[IndexOfSPS].DATA.BYTE = findByteWithAddr();
					Stack[IndexOfSPS].Type = 1;
				} else if (ParamType == 1) {
					Stack[IndexOfSPS].DATA.DWORD = findDByteWithAddr();
					Stack[IndexOfSPS].Type = 2;
				} else {
					Stack[IndexOfSPS].DATA.INT = findIntWithAddr();
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
						setByte(Stack[i].DATA.BYTE);
					} else if (Stack[i].Type == 2) {
						setDByte(Stack[i].DATA.DWORD);
					} else {
						setInt(Stack[i].DATA.INT);
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
			uint8_t dataSize = params[0]; // 新增参数：1=BYTE, 2=DWORD, 4=INT
			if (dataSize != 1 && dataSize != 2 && dataSize != 4) {
				return -1; // 非法数据大小
			}
			int32_t val1, val2;
			// 读取参数1的值（根据ParamType和dataSize）
			if (ParamType == 0) {       // 两个立即数
				val1 = params[1];
				val2 = params[2];
				// 对立即数进行符号扩展（若需要）
				if (dataSize == 1) val1 = (int8_t)(val1 & 0xFF);
				else if (dataSize == 2) val1 = (int16_t)(val1 & 0xFFFF);
			} else if (ParamType == 1) { // 参数1为地址，参数2为立即数
				// 从内存读取参数1的值
				FindPhyMemOffByID(taskId, params[1]);
				val1 = findIntWithAddr() << (4 - dataSize) >> (4 - dataSize);
				// 立即数参数2处理
				val2 = params[2];
			} else if (ParamType == 2) { // 两个参数均为地址
				// 读取参数1的地址
				FindPhyMemOffByID(taskId, params[1]);
				val1 = findIntWithAddr() << (4 - dataSize) >> (4 - dataSize);
				// 读取参数2的地址
				FindPhyMemOffByID(taskId, params[2]);
				val2 = findIntWithAddr() << (4 - dataSize) >> (4 - dataSize);
			} else {
				return -1; // 非法参数类型
			}
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
			CurCmd[taskId] = OS_EXE_LOAD_START + (volatile uint8_t *)params[0];
			break;
		case JMP_T:
			if (CalcResu)CurCmd[taskId] = OS_EXE_LOAD_START + (volatile uint8_t *)params[0];
			break;
		//运算时全部视为INT类型
		case ADD:
			//全为立即数
			if (ParamType == 0) {
				CalcResu[taskId] = params[0] + params[1];
				//一个为立即数一个为地址
			} else if (ParamType == 1) {
				FindPhyMemOffByID(taskId, params[0]);
				int32_t x = *(volatile int32_t *)CurPhyMem[taskId];
				CalcResu[taskId] = x + params[1];
				//全为地址
			} else if (ParamType == 2) {
				FindPhyMemOffByID(taskId, params[0]);
				int32_t x = *(volatile int32_t *)CurPhyMem[taskId];
				FindPhyMemOffByID(taskId, params[1]);
				int32_t y = *(volatile int32_t *)CurPhyMem[taskId];
				CalcResu[taskId] = x + y;
			}
			break;
		case SUB:
			if (ParamType == 0) {
				CalcResu[taskId] = params[0] - params[1];
			} else if (ParamType == 1) {
				FindPhyMemOffByID(taskId, params[0]);
				int32_t x = *(volatile int32_t *)CurPhyMem[taskId];
				CalcResu[taskId] = x - params[1];
				//全为地址
			} else if (ParamType == 2) {
				FindPhyMemOffByID(taskId, params[0]);
				int32_t x = *(volatile int32_t *)CurPhyMem[taskId];
				FindPhyMemOffByID(taskId, params[1]);
				int32_t y = *(volatile int32_t *)CurPhyMem[taskId];
				CalcResu[taskId] = x - y;
			}
			break;
		case MUL:
			if (ParamType == 0) {
				CalcResu[taskId] = params[0] * params[1];
			} else if (ParamType == 1) {
				FindPhyMemOffByID(taskId, params[0]);
				int32_t x = *(volatile int32_t *)CurPhyMem[taskId];
				CalcResu[taskId] = x * params[1];
				//全为地址
			} else if (ParamType == 2) {
				FindPhyMemOffByID(taskId, params[0]);
				int32_t x = *(volatile int32_t *)CurPhyMem[taskId];
				FindPhyMemOffByID(taskId, params[1]);
				int32_t y = *(volatile int32_t *)CurPhyMem[taskId];
				CalcResu[taskId] = x * y;
			}
			break;
		case DIV:
			if (ParamType == 0) {
				if (params[1] == 0)return DIV_BY_0;
				CalcResu[taskId] = params[0] / params[1];
			} else if (ParamType == 1) {
				if (params[1] == 0)return DIV_BY_0;
				FindPhyMemOffByID(taskId, params[0]);
				int32_t x = *(volatile int32_t *)CurPhyMem[taskId];
				CalcResu[taskId] = x / params[1];
			} else if (ParamType == 2) {
				FindPhyMemOffByID(taskId, params[0]);
				int32_t x = *(volatile int32_t *)CurPhyMem[taskId];
				FindPhyMemOffByID(taskId, params[1]);
				int32_t y = *(volatile int32_t *)CurPhyMem[taskId];
				if (y == 0)return DIV_BY_0;
				CalcResu[taskId] = x / y;
			}
			break;
		case HLT:
			return 1;
			break;
		//未完待续
		default:
			return -1;
			break;
	}
}
