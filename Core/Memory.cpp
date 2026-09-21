#ifndef MEMORY
#include "Memory.h"
#endif
uars_i8 runMem[OS_MAX_MEM] __attribute__((aligned(4)));
uars_i8 exeMem[OS_MAX_TASK][OS_MAX_SGL_PG] __attribute__((aligned(4)));
#define OS_PHY_MEM_START runMem
#define OS_EXE_LOAD_START(i) exeMem[i]
volatile uars_i8 *CurCmd[OS_MAX_TASK];     //每个程序当前执行的命令的位置，0表示未启用
uars_i8 Stack[OS_MAX_TASK][OS_MAX_PARAM] __attribute__((aligned(4)));  //子程序参数栈
uars_i8 IndexOfSPS[OS_MAX_TASK] = { 0 };   //子程序参数栈参数总长度
//上述两个变量共同用于子程序调用完毕后跳转返回的内存和命令
//起到保存上下文的作用
volatile uars_i8 *FreeHead = 0;
//上面的变量表示空闲内存链表的表头
//寻找空闲内存从头找
volatile Magic *FreeTail = 0;
volatile uars_i8 *MemHead[OS_MAX_TASK];                            //每个程序占用的物理内存的首地址
volatile uars_i8 *MemTail[OS_MAX_TASK];                            //每个程序最近启动的子程序占用的物理内存的首地址
ars_i16 MemLevel[OS_MAX_TASK];                                     //每个程序变量作用域最大层级，-1表示未启用
volatile uars_i8 *LastMEM = (volatile uars_i8 *)OS_PHY_MEM_START;  //对于物理内存而言，目前占用的总计长度
volatile uars_i8 *CurPhyMem[OS_MAX_TASK];                          //“安全”的起始物理内存
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
		MemLevel[i] = -1;
		CurCmd[i] = 0;
		CurPhyMem[i] = (volatile uars_i8 *)OS_PHY_MEM_START;
	}
}

//修复意外越界的内存
//如果意外越界连魔术字头都修改了，那我们认为是意外
//但如果魔术字没变，其他字段却变动了
//那我们只好怀疑是“黑客”捣的鬼
//不得不直接返回错误了哦~
ars_i8 ResumeMem(Magic *M, uars_i8 id) {
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

//内存模型：[魔术字块][数据块]

/*
	空闲块链表并不是按物理地址顺序排序的，而是按释放的时间先后顺序排序，
	每次重利用释放的内存是从头开始查找空闲块的（先释放的先使用），
	在这个时候才会更新FreeHead
	所有合并的内存块来源于链表此前记录的内存块
*/

//合并空闲块
ars_i8 SuperFree(Magic *block) {
	uars_i8 status = 0;
	uars_i8 findPrev = 0;
	uars_i8 findNext = 0;
	Magic *ptr = (Magic *)FreeHead, *spot1, *spot2;
	if (!ptr) {
		return NO_FREE_MEM;
	}
	//从FreeHead头开始就出错了
	//空闲表视为完全没救了
	if (ARS_strcmp(ptr->MagicHead, FREE, 4) && ptr->Check != CHECK) {
		ptr = 0;  //从头注销所有空闲内存表
		return BAD_FREE_BLOCK | NO_FREE_MEM;
	}
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
		if (((uars_i8 *)ptr) + ptr->len + sizeof(Magic) == (uars_i8 *)block) {
			findPrev = 1;
			spot1 = ptr;
			status++;
		}
		if (((uars_i8 *)block) + block->len + sizeof(Magic) == (uars_i8 *)ptr) {
			findNext = 1;
			spot2 = ptr;
			status++;
		}
		//等于二表示前后内存都扫描完了
		//不需要一直访问到链表末尾
		//虽然是惰性内存回收
		//但似乎。。理论上来说，这几乎可以完全规避外部碎片？
		if (status == 2) break;
		if (ptr->next_block) ptr = (Magic *)ptr->next_block;
		else break;
	}
	//如果可以前后合并
	//则将spot2从空闲链表中剔除，与block合并后插入spot1
	//基于规定，三者的内存排布一定遵循以下格式：[spot1][block][spot2]
	if (findPrev && findNext) {
		if (spot2->next_block && spot2->last_block) {
			((Magic *)spot2->last_block)->next_block = (volatile uars_i8 *)((Magic *)spot2->next_block);
			((Magic *)spot2->next_block)->last_block = (volatile uars_i8 *)((Magic *)spot2->last_block);
			//只有向下的内存链表（spot2为空闲链表链首）
		} else if (spot2->next_block) {
			((Magic *)spot2->next_block)->last_block = 0;
			FreeHead = (volatile uars_i8 *)((Magic *)spot2->next_block);
			//只有向上的内存链表（spot2为空闲链表链尾）
		} else if (spot2->last_block) {
			((Magic *)spot2->last_block)->next_block = 0;
			FreeTail = ((Magic *)spot2->last_block);
		}
		spot1->len += 2 * sizeof(Magic) + block->len + spot2->len;
		uars_i8 *x = (uars_i8 *)block;
		//销毁魔术字头，将其归为内存总体一部分
		for (int i = 0; i < sizeof(Magic); i++)
			*x++ = 0;
		x = (uars_i8 *)spot2;
		for (int i = 0; i < sizeof(Magic); i++)
			*x++ = 0;
		return 0;
	} else if (findPrev) {
		//只更新内存块数据区长度
		//把block包括魔术字头的部分全部吞入
		spot1->len += sizeof(Magic) + block->len;
		uars_i8 *x = (uars_i8 *)block;
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
		spot2->len += sizeof(Magic) + block->len;
		ARS_memset(block, spot2, sizeof(Magic));
		//如果存在上一块空闲内存
		if (block->last_block) {
			//那么上一块内存指向的下一块内存地址需要修改
			//改成block的
			((Magic *)block->last_block)->next_block = (uars_i8 *)block;
		}
		//如果存在下一块空闲内存
		if (block->next_block) {
			//那么下一块内存指向的上一块内存地址需要修改
			((Magic *)block->next_block)->last_block = (uars_i8 *)block;
		}
		// 在向后合并后检查是否为FreeTail
		if (spot2 == FreeTail) {
			FreeTail = block;  // 更新FreeTail为新合并的块
		}
		if (spot2 == (Magic *)FreeHead) {
			FreeHead = (uars_i8 *)block;  // 更新FreeHead为新合并的块
		}
		uars_i8 *x = (uars_i8 *)spot2;
		for (int i = 0; i < sizeof(Magic); i++) {
			*x++ = 0;
		}
		return 0;
	}
	//如果找不到可以合并的，提示将该块内存追加到FreeTail
	if (!status) return NEED_APPEND_TO_TAIL;
}

//该函数用在“应用程序”结束后启动
//用来销毁该应用所有的内存
//当整个程序退出时发挥作用
ars_i8 ReArrangeMemAndTask(uars_i8 id) {
	//从头开始
	Magic *M = (Magic *)MemHead[id];
	if (!M) {
		return NO_MEM_HEAD;
	}
	while (M && M < (Magic *)(OS_PHY_MEM_START + OS_MAX_MEM)) {
		//发现魔术字
		if (!ARS_strcmp((const char *)M->MagicHead, SPLIT, 4)) {
			//id对应上了
			if (M->id == id) {
				//魔术字声明：该块内存被释放
				M->MagicHead[0] = 'F';
				M->MagicHead[1] = 'R';
				M->MagicHead[2] = 'E';
				M->MagicHead[3] = 'E';
				//去除魔术字后该块内存的长度
				int size = M->len;
				volatile uars_i8 *next = M->next_block;
				//跳过Magic头注销内存
				volatile uars_i8 *PhyMem = ((uars_i8 *)M + sizeof(Magic));
				for (int i = 0; i < size; i++) {
					*PhyMem++ = 0;
				}
				//记录空闲内存链表信息
				if (FreeHead == 0) {
					FreeHead = (volatile uars_i8 *)M;
					FreeTail = (Magic *)FreeHead;
					FreeTail->last_block = 0;
					FreeTail->next_block = 0;
				} else {
					if (SuperFree(M) == NEED_APPEND_TO_TAIL) {
						FreeTail->next_block = (volatile uars_i8 *)M;
						Magic *next = (Magic *)FreeTail->next_block;
						next->next_block = 0;
						next->last_block = (volatile uars_i8 *)FreeTail;
						FreeTail = next;
					}
				}
				//程序没有下一块分配的内存了（注销内存完毕）
				//将该任务彻底还原为初始状态
				if (next == 0) {
					CurCmd[id] = 0;
					MemHead[id] = 0;
					MemTail[id] = 0;
					MemLevel[id] = -1;
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
			ars_i8 Repair = ResumeMem(M, id);
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
ars_i8 DelLastFuncMem(uars_i8 id) {
	Magic *M = (Magic *)MemTail[id];
	if (!M) {
		return NO_MEM_TAIL;
	}
	//如果当前内存不是主程序占用内存
	//则将最末一个子程序的占用内存与其前面的内存断联
	//同时修改MemTail
	if (M->last_block && MemLevel[id] > 0) {
		((Magic *)M->last_block)->next_block = 0;
		MemTail[id] = (volatile uars_i8 *)(M->last_block);
	}
	//不可越界
	if (((uars_i8 *)M) + M->len < (uars_i8 *)(OS_PHY_MEM_START + OS_MAX_MEM)) {
		//魔术字匹配
		if (!ARS_strcmp((const char *)M->MagicHead, SPLIT, 4)) {
			//ID匹配
			if (M->id == id) {
				//不是主程序
				if (MemLevel[id] > 0 /*M->level > 0*/) {
					//声明内存释放
					uars_i8 *ptr = (uars_i8 *)M;
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
						FreeHead = (uars_i8 *)M;
						FreeTail = (Magic *)FreeHead;
						FreeTail->last_block = 0;
						FreeTail->next_block = 0;
					} else {
						if (SuperFree(M) == NEED_APPEND_TO_TAIL) {
							FreeTail->next_block = (volatile uars_i8 *)M;
							Magic *next = (Magic *)FreeTail->next_block;
							next->next_block = 0;
							next->last_block = (volatile uars_i8 *)FreeTail;
							FreeTail = next;
						}
					}
					//内存已全部销毁完毕
					//作用域等级降低
					MemLevel[id]--;
					return 1;
				} else {
					//此时便是主程序
					ReArrangeMemAndTask(id);
					return 0;
				}
			} else return ID_ERR;
		} else {
			ars_i8 Repair = ResumeMem(M, id);
			if (Repair == HEAD_ERR) return MEM_CLEAN_PARTLY;
		}
	} else return OUT_BOUND;
}

//查找空闲的内存空间
//为新的主/子程序分配内存
//简化内存分配与管理
int findFreeMemById(uars_i8 id, int allocLen, int level) {
	////Serial.println("Yes @findFreeMemById\n");
	//总长度TTL=子程序运行所需内存+4字节调用时 命令内存指针 指向地址
	//以便RET后跳回调用该子程序的代码块和内存
	int TTL = allocLen + 4;
	//如果当前命令指针指向不为0（即该进程已启用）则指向当前程序的末尾内存地址
	Magic *M = 0;
	if (CurCmd[id]) {
		////Serial.println("not main @findFreeMemById\n");
		M = (Magic *)MemTail[id];
		if (!M) {
			return NO_MEM_TAIL;
		}
	}
	//从头查找空闲内存
	uars_i8 *M2 = (uars_i8 *)FreeHead;
	uars_i8 isalloc = 0;
	if (M2) {
		while (M2 < (uars_i8 *)LastMEM && M2) {
			//发现空闲的魔术字
			if (!ARS_strcmp((const char *)M2, FREE, 4)) {
				//若该块空闲内存大小不小于需求大小
				//注意：此处必须是 >= 而不是 >
				//释放的子程序块长度恰好等于 TTL 时（这在“固定大小子程序反复调用”的
				//场景中是最常见的情况），用 > 会跳过该空闲块，导致内存分配不断向后
				//“抬升”，最终耗尽 OS_MAX_MEM 并使程序报 OUT_BOUND 而停摆
				if (((Magic *)M2)->len >= TTL) {
					//如果该空闲块内存比总需求大不足50B，则将该内存块全部分给请求
					if (((Magic *)M2)->len - TTL < 50) {
						//判断该块空闲内存在链表中的位置情况
						if (((Magic *)M2)->last_block && ((Magic *)M2)->next_block) {
							//中间位置
							((Magic *)(((Magic *)M2)->last_block))->next_block = ((Magic *)M2)->next_block;
							((Magic *)(((Magic *)M2)->next_block))->last_block = ((Magic *)M2)->last_block;
						} else if (M2 == FreeHead && M2 != (uars_i8 *)FreeTail) {
							//链头就是
							((Magic *)(((Magic *)M2)->next_block))->last_block = 0;
							FreeHead = ((Magic *)M2)->next_block;
						} else if (M2 != FreeHead && M2 == (uars_i8 *)FreeTail) {
							//链尾
							((Magic *)(((Magic *)M2)->last_block))->next_block = 0;
							FreeTail = ((Magic *)(((Magic *)M2)->last_block));
						} else if (M2 == FreeHead && M2 == (uars_i8 *)FreeTail) {
							FreeHead = 0;
							FreeTail = 0;
						} else break;
						//此处有个非常特殊的情况：空闲块全部合并
					} else {
						//将一大块空闲内存块分割
						//spare表示分割后的空闲内存的地址
						volatile uars_i8 *spare = M2 + sizeof(Magic) + TTL;
						//这里看似“多此一举”的动作实际上是为了防止spare和M2有内存重叠导致互相覆盖
						Magic M2_now;
						ARS_memset(&M2_now, M2, sizeof(Magic));
						ARS_memset(spare, &M2_now, sizeof(Magic));
						((Magic *)M2)->len = TTL;
						((Magic *)spare)->len = ((Magic *)spare)->len - TTL - sizeof(Magic);
						if (((Magic *)M2)->last_block && ((Magic *)M2)->next_block) {
							//中间位置
							((Magic *)(((Magic *)M2)->last_block))->next_block = spare;
							((Magic *)(((Magic *)M2)->next_block))->last_block = spare;
						} else if (M2 == FreeHead && M2 != (uars_i8 *)FreeTail) {
							//链头就是
							((Magic *)(((Magic *)M2)->next_block))->last_block = spare;
							FreeHead = spare;
						} else if (M2 != FreeHead && M2 == (uars_i8 *)FreeTail) {
							//链尾
							((Magic *)(((Magic *)M2)->last_block))->next_block = spare;
							FreeTail = (Magic *)spare;
						} else if (M2 == FreeHead && M2 == (uars_i8 *)FreeTail) {
							FreeHead = spare;
							FreeTail = (Magic *)spare;
						} else break;
					}
					//魔术字覆盖
					*M2 = 'S';
					*(M2 + 1) = 'P';
					*(M2 + 2) = 'L';
					*(M2 + 3) = 'T';
					//ID覆盖
					((Magic *)M2)->id = id;
					((Magic *)M2)->Check = CHECK;
					//链表指向上一块内存
					if (CurCmd[id]) {
						((Magic *)M2)->last_block = (volatile uars_i8 *)M;
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
					//((Magic *)M2)->level = level;
					//存在上下块内存的指向关系
					if (CurCmd[id]) {
						//上一块内存指向下一块内存
						M->next_block = M2;
						//跳转至最新分配的内存
					} else {
						//跳转到“应用程序”的入口地址
						//CurCmd[id] = (volatile uars_i8 *)(OS_EXE_LOAD_START + * (int *)exe_mem);
					}
					isalloc = 1;
					break;
				}
			} else {
				ars_i8 Repair = ResumeMem((Magic *)M2, id);
				if (Repair == HEAD_ERR) return HEAD_ERR;
			}
			M2 = (uars_i8 *)((Magic *)M2)->next_block;
		}
	}
	//没有合适的块
	if (!isalloc) {
		//从目前占用的物理内存的末尾开始启用新内存
		Magic *M3 = (Magic *)LastMEM;
		//越界！
		if ((uars_i8 *)M3 + sizeof(Magic) + TTL >= (uars_i8 *)OS_PHY_MEM_START + OS_MAX_MEM) {
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
		M3->last_block = (volatile uars_i8 *)M;
		M3->next_block = 0;
		//M3->level = level;
		if (CurCmd[id]) {
			//Serial.println("not main @findFreeMemById");
			M->next_block = (volatile uars_i8 *)M3;
			MemTail[id] = (uars_i8 *)M3;
		} else {
			//若该进程此次才启用
			//Serial.println("is main @findFreeMemById");
			MemHead[id] = (uars_i8 *)M3;
			MemTail[id] = (uars_i8 *)M3;
			//此时没有上一块内存
			M3->last_block = 0;
			M3->next_block = 0;
		}
		LastMEM += sizeof(Magic) + TTL;
		MemTail[id] = (uars_i8 *)M3;
	}
	CurPhyMem[id] = MemTail[id];
}

//根据程序id查找其目前访问的“虚拟地址”对应的“物理地址”
uars_i8 FindPhyMemOffByID(uars_i8 id, uars_i32 offset) {
	////Serial.println("Yes @FindPhyMemOffByID\n");
	//除了公有变量，活跃的变量一定是最近启动的子程序的变量
	Magic *M = (Magic *)MemTail[id];
	if (!M) {
		return NO_MEM_TAIL;
	}
	//发现魔术字
Execute:
	if (!ARS_strcmp((const char *)M->MagicHead, SPLIT, 4)) {
		//id对应上了
		if (M->id == id) {
			//魔术字头记录的内存块长度不包括魔术字头本身
			int size = M->len;
			if (size < 0) {
				return INVALID_LEN;
			}
			if (size >= offset) {
				CurPhyMem[id] = ((volatile uars_i8 *)M) + sizeof(Magic) + 4 + offset;  //成功查找到
				return 0;
			} else return OUT_BOUND;
		} else return ID_ERR;
	} else {
		ars_i8 Repair = ResumeMem(M, id);
		if (Repair == HEAD_ERR) return HEAD_ERR;
		goto Execute;
	}
}

void ReadByteMem(uars_i8 *Recv, uars_i8 id) {
	*Recv = *CurPhyMem[id];
}

//访问 单字节
//访问完后自动跳过该段内存
//下方的访问双字节和四字节也是如此
ars_i8 findByteWithAddr(uars_i8 id) {
	uars_i8 Byte_Buff;
	ReadByteMem(&Byte_Buff, id);
	CurPhyMem[id]++;
	return Byte_Buff;
}

//访问 四字节
ars_i32 findIntWithAddr(uars_i8 id) {
	ars_i32 value;
	ARS_memset(&value, (void *)CurPhyMem[id], 4);
	CurPhyMem[id] += 4;
	return value;
}

//访问 Float
float findFloatWithAddr(uars_i8 id) {
	float value;
	ARS_memset(&value, (void *)CurPhyMem[id], 4);
	CurPhyMem[id] += 4;
	return value;
}

//设置 单字节
//对于多字节类型则拆分成单个字节依次存放
void setByte(ars_i8 byteText, uars_i8 id) {
	*CurPhyMem[id]++ = byteText;
}

void setInt(ars_i32 intText, uars_i8 id) {
	ARS_memmove(CurPhyMem[id], &intText, sizeof(ars_i32));
	CurPhyMem[id] += sizeof(ars_i32);
}

void setFloat(float fText, uars_i8 id) {
	ARS_memmove(CurPhyMem[id], &fText, sizeof(float));
	CurPhyMem[id] += sizeof(float);
}
