#ifndef MEMORY
	#include "Memory.h"
#endif

volatile uint8_t *CurCmd[OS_MAX_TASK];//每个程序当前执行的命令的位置，0表示未启用
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
volatile uint8_t *CurPhyMem[OS_MAX_TASK]; //“安全”的起始物理内存
//通过"SPLT"字样来分割内存
//"FREE"表示此处空闲
//在我的设想中
//每个主程序或子程序所占用的内存大小在大部分情况下是固定的（可变长度数组暂时不考虑）
//在“编译”成ARS字节码时，该段程序占用的内存信息会被提取出来
//然后在调用时基于此进行内存分配

void init_mem_info() {
	for (int i = 0; i < OS_MAX_TASK; i++) {
		MemHead[i] = 0;
		MemTail[i] = 0;
		MemLevel[i] = -2;
		CurCmd[i] = 0;
		CurPhyMem[i] = (volatile uint8_t *)OS_PHY_MEM_START;
	}
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
		return 0;
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
	uint8_t findPrev = 0;
	uint8_t findNext = 0;
	Magic *ptr = (Magic *)FreeHead, *spot1, *spot2, *img1 = 0;
	if (!ptr) {
		return NO_FREE_MEM;
	}
	//从FreeHead头开始就出错了
	//空闲表视为完全没救了
	if (ARS_strcmp(ptr->MagicHead, FREE, 4) && ptr->Check != CHECK) {
		ptr = 0; //从头注销所有空闲内存表
		return BAD_FREE_BLOCK | NO_FREE_MEM;
	}
	Magic *img = 0;
	//从头开始搜索空闲内存
	//img相当于ptr上一步的地址的快照
	//因为要考虑前后合并的情况
	while (ptr) {
		//中途出错
		//终止继续合并
		//并且缩小空闲内存范围
		//防止污染
		//哪怕魔术字出错，只要守卫字没错，照样回收内存
		if (ARS_strcmp(ptr->MagicHead, FREE, 4)) {
			if (ptr->Check != CHECK) {
				FreeTail = ptr;
				return BAD_FREE_BLOCK;
			}
		}
		//空闲内存链表记录的内存块之一恰好在block前，向前合并
		if (((uint8_t *)ptr) + ptr->len + sizeof(Magic) == (uint8_t *)block) {
			findPrev = 1;
			spot1 = ptr;
			status++;
		}
		if (((uint8_t *)block) + block->len + sizeof(Magic) == (uint8_t *)ptr) {
			findNext = 1;
			spot2 = ptr;
			if (img)img1 = img; //上一块的快照
			status++;
		}
		//等于二表示前后内存都扫描完了
		//不需要一直访问到链表末尾
		//虽然是惰性内存回收
		//但似乎。。理论上来说，这几乎可以完全规避外部碎片？
		if (status == 2) break;
		img = ptr;
		if (ptr->next_block)ptr = (Magic *)ptr->next_block;
		else break;
	}
	//如果可以前后合并
	if (findPrev && findNext) {
		spot1->len += 2 * sizeof(Magic) + block->len + spot2->len;
		img1->next_block = spot2->next_block;//将前块指向的下一块地址更改为后块
		//其实就是删除spot2
		uint8_t *x = (uint8_t *)block;
		//销毁魔术字头，将其归为内存总体一部分
		for (int i = 0; i < sizeof(Magic); i++)
			*x++ = 0;
		x = (uint8_t *)spot2;
		for (int i = 0; i < sizeof(Magic); i++)
			*x++ = 0;
		if (spot2 == FreeTail) {
			FreeTail = spot1;
		}
		return 0;
	} else if (findPrev) {
		//只更新内存块数据区长度
		//把block包括魔术字头的部分全部吞入
		spot1->len += sizeof(Magic) + block->len;
		uint8_t *x = (uint8_t *)block;
		//销毁魔术字头，将其归为内存总体一部分
		for (int i = 0; i < sizeof(Magic); i++) {
			*x++ = 0;
		}
		return 0;
	}
	//空闲内存链表记录的内存块在后，向后合并
	else if (findNext) {
		//首先抄一下空闲内存链表记录的信息
		//因为包含了下文
		ARS_memmove((uint8_t *)block, (uint8_t *)spot2, sizeof(Magic));
		block->len += sizeof(Magic) + spot2->len;
		uint8_t *x = (uint8_t *)spot2;
		for (int i = 0; i < sizeof(Magic); i++) {
			*x++ = 0;
		}
		//如果存在上一块空闲内存
		if (img1) {
			//那么上一块内存指向的下一块内存地址需要修改
			//改成block的
			img1->next_block = (uint8_t *)block;
		}
		// 在向后合并后检查是否为FreeTail
		if (spot2 == FreeTail) {
			FreeTail = block; // 更新FreeTail为新合并的块
		}
		if (spot2 == (Magic *)FreeHead) {
			FreeHead = (uint8_t *)block; // 更新FreeHead为新合并的块
		}
		return 0;
	}
	//如果找不到可以合并的，提示将该块内存追加到FreeTail
	if (!status)return NEED_APPEND_TO_TAIL;
}

//该函数用在“应用程序”结束后启动
//用来销毁该应用所有的内存
//当整个程序退出时发挥作用
int8_t ReArrangeMemAndTask(uint8_t id) {
	//从头开始
	Magic *M = (Magic *)MemHead[id];
	if (!M) {
		return NO_MEM_HEAD;
	}
	while (M < (Magic *)(OS_PHY_MEM_START + OS_MAX_MEM)) {
		//发现魔术字
		if (!ARS_strcmp((const char*) M->MagicHead, SPLIT, 4)) {
			//id对应上了
			if (M->id == id) {
				//魔术字声明：该块内存被释放
				M->MagicHead[0] = 'F';
				M->MagicHead[1] = 'R';
				M->MagicHead[2] = 'E';
				M->MagicHead[3] = 'E';
				//去除魔术字后该块内存的长度
				int size = M->len;
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
			} else return ID_ERR;
			//该段内存ID匹配出错
		}
		//内存魔术字匹配出错
		//并且无法修复魔术字头
		//此时关于内存上下文的链表可能被破坏
		//为了不破坏其他程序的内存
		//只好终止内存释放
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
	//获取调用该子程序的代码块此前运行到的内存地址和命令地址
	//起到上下文的作用
	volatile uint8_t * LastFuncMem = 0;
	volatile uint8_t * LastFuncCmd = 0;
	Magic *M = (Magic *)MemTail[id];
	if (!M) {
		return NO_MEM_TAIL;
	}
	//如果当前内存不是主程序占用内存
	//则将最末一个子程序的占用内存与其前面的内存断联
	if (M->next_block && M->last_block && MemLevel[id] > 0) {
		((Magic *)M->last_block)->next_block = 0;
		LastFuncMem = (volatile uint8_t *)(M + sizeof(Magic));
		LastFuncCmd = (volatile uint8_t *)(M + sizeof(Magic) + sizeof(int32_t));
	}
	//不可越界
	if (((uint8_t *)M) + M->len < (uint8_t *)(OS_PHY_MEM_START + OS_MAX_MEM)) {
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
					ReArrangeMemAndTask(id);
					return 0;
				}
			} else return ID_ERR;
		} else {
			int8_t Repair = ResumeMem(M, id);
			if (Repair == HEAD_ERR)return MEM_CLEAN_PARTLY;
		}
	} else return OUT_BOUND;
}

//查找空闲的内存空间
//为新的主/子程序分配内存
//首次适应分配，若该块空闲内存大小为需求大小的1至1.1倍间则分配
//简化内存分配与管理
int findFreeMemById(uint8_t id, int allocLen, int level) {
	//总长度TTL=子程序运行所需内存+4字节调用时 运行内存指针 指向地址+4字节调用时 命令内存指针 指向地址
	//以便RET后跳回调用该子程序的代码块和内存
	int TTL = allocLen + 8;
	//如果当前命令指针指向不为0（即该进程已启用）则指向当前程序的末尾内存地址
	Magic *M;
	if (CurCmd[id]) {
		M = (Magic *)MemTail[id];
		if (!M) {
			return NO_MEM_TAIL;
		}
	}
	//从头查找空闲内存
	uint8_t *M2 = (uint8_t *)FreeHead;
	uint8_t isalloc = 0;
	//上一块空闲块
	volatile uint8_t *prev_free = 0;
	if (M2) {
		while (M2 < (uint8_t *)LastMEM && M2) {
			//发现空闲的魔术字
			if (!ARS_strcmp((const char*) M2, FREE, 4)) {
				//若该块空闲内存大小为需求大小的1至1.1倍间
				if (((Magic*)M2)->len > TTL && ((Magic*)M2)->len < TTL * 1.1) {
					//判断该块空闲内存在链表中的位置情况
					if (prev_free && M2 != (uint8_t *)FreeTail) {
						//中间位置
						((Magic*)prev_free)->next_block = ((Magic*)M2)->next_block;
					} else if (!prev_free && M2 == FreeHead) {
						//链头就是
						FreeHead = ((Magic*)M2)->next_block;
					} else if (M2 == (uint8_t *)FreeTail) {
						//链尾
						FreeTail = ((Magic*)prev_free);
					} else break;
					//魔术字覆盖
					*M2 = 'S';
					*(M2 + 1) = 'P';
					*(M2 + 2) = 'L';
					*(M2 + 3) = 'T';
					//ID覆盖
					((Magic*)M2)->id = id;
					//((Magic*)M2)->len = TTL;
					//现修改为将该块可分配内存全部分配给请求内存
					//若反复执行((Magic*)M2)->len = TTL会加剧内存碎片
					((Magic *)M2)->Check = CHECK;
					//链表指向上一块内存
					if (CurCmd[id]) {
						((Magic *)M2)->last_block = (volatile uint8_t *)M;
						MemTail[id] = M2;
					} else {
						//若该进程此次才启用
						MemHead[id] = M2;
						MemTail[id] = M2;
						//此时没有上一块内存
						((Magic *)M2)->last_block = 0;
					}
					//暂无需下一块内存
					((Magic *)M2)->next_block = 0;
					((Magic *)M2)->level = level;
					//存在上下块内存的指向关系
					if (CurCmd[id]) {
						//上一块内存指向下一块内存
						M->next_block = M2;
						//跳转至最新分配的内存
					} else {
						//跳转到“应用程序”的加载地址
						CurCmd[id] = (volatile uint8_t *)OS_EXE_LOAD_START;
					}
					isalloc = 1;
					break;
				}
			} else {
				int8_t Repair = ResumeMem((Magic *)M2, id);
				if (Repair == HEAD_ERR)return HEAD_ERR;
			}
			prev_free = M2;
			M2 = (uint8_t *)((Magic *)M2)->next_block;
		}
	}
	//没有合适的块
	if (!isalloc) {
		//从目前占用的物理内存的末尾开始启用新内存
		Magic *M3 = (Magic *)LastMEM;
		//越界！
		if ((uint8_t *)M3 + sizeof(Magic) + TTL >= (uint8_t *)OS_PHY_MEM_START + OS_MAX_MEM) {
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
		if (CurCmd[id]) {
			M->next_block = (volatile uint8_t *)M3;
			MemTail[id] = (uint8_t *)M3;
		} else {
			//若该进程此次才启用
			MemHead[id] = (uint8_t *)M3;
			MemTail[id] = (uint8_t *)M3;
			//此时没有上一块内存
			M3->last_block = 0;
			M3->next_block = 0;
			CurCmd[id] = (volatile uint8_t *)OS_EXE_LOAD_START;
		}
		LastMEM += sizeof(Magic) + TTL;
		MemTail[id] = (uint8_t *)M3;
	}
	CurPhyMem[id] = MemTail[id];
}

//根据程序id查找其目前访问的“虚拟地址”对应的“物理地址”
uint8_t FindPhyMemOffByID(uint8_t id, uint32_t offset) {
	//除了公有变量，活跃的变量一定是最近启动的子程序的变量
	Magic *M = (Magic *)MemTail[id];
	if (!M) {
		return NO_MEM_TAIL;
	}
	//发现魔术字
Execute:
	if (!ARS_strcmp((const char*) M->MagicHead, SPLIT, 4)) {
		//id对应上了
		if (M->id == id) {
			//魔术字头记录的内存块长度不包括魔术字头本身
			int size = M->len;
			if (size < 0) {
				return INVALID_LEN;
			}
			if (size >= offset) {
				CurPhyMem[id] = ((volatile uint8_t *)M) + sizeof(Magic) + offset; //成功查找到
				return 0;
			} else return OUT_BOUND;
		} else return ID_ERR;
	} else {
		int8_t Repair = ResumeMem(M, id);
		if (Repair == HEAD_ERR)return HEAD_ERR;
		goto Execute;
	}
}

void ReadByteMem(uint8_t *Recv, uint8_t id) {
	*Recv = *CurPhyMem[id];
}

//访问 单字节
//访问完后自动跳过该段内存
//下方的访问双字节和四字节也是如此
int8_t findByteWithAddr(uint8_t id) {
	int8_t Byte_Buff;
	ReadByteMem(&Byte_Buff, id);
	CurPhyMem[id]++;
	return Byte_Buff;
}

//访问 四字节
int32_t findIntWithAddr(uint8_t id) {
	int32_t value;
	value = *((int32_t *)CurPhyMem[id]);
	CurPhyMem[id] += 4;
	return value;
}

//访问 Float
float findFloatWithAddr(uint8_t id) {
	float value;
	value = *((float *)CurPhyMem[id]);
	CurPhyMem[id] += 4;
	return value;
}

//设置 单字节
//对于多字节类型则拆分成单个字节依次存放
void setByte(int8_t byteText, uint8_t id) {
	*CurPhyMem[id]++ = byteText;
}

void setInt(int32_t intText, uint8_t id) {
	ARS_memmove(CurPhyMem[id], &intText, sizeof(int32_t));
	CurPhyMem[id] += sizeof(int32_t);
}

void setFloat(float fText, uint8_t id) {
	ARS_memmove(CurPhyMem[id], &fText, sizeof(float));
	CurPhyMem[id] += sizeof(float);
}

//这两个函数虽然名字看似是类型转换
//其实应该是将相同的内存块进行原封不动的抄写，并以不同形式（INT,FLOAT）读出来
//而内存块的内容并没有改变
//像float x=1.14;int y=x;就会改变因为类型转换而造成内存内容的不同
float tranIntToFloat(int x) {
	float result;
	ARS_memmove(&result, &x, sizeof(int));  // 安全转换
	return result;
}

int tranFloatToInt(float x) {
	int result;
	ARS_memmove(&result, &x, sizeof(float));  // 安全转换
	return result;
}
