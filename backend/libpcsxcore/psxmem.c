/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

/*
* PSX memory functions.
*/

#include "psxmem.h"
#include "r3000a.h"
#include "psxhw.h"
#include <sys/mman.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

s8 *psxM = NULL; // Kernel & User Memory (2 Meg)
s8 *psxP = NULL; // Parallel Port (64K)
s8 *psxR = NULL; // BIOS ROM (512K)
s8 *psxH = NULL; // Scratch Pad (1K) & Hardware Registers (8K)

u8 **psxMemWLUT = NULL;
u8 **psxMemRLUT = NULL;

/*  Playstation Memory Map (from Playstation doc by Joshua Walker)
0x0000_0000-0x0000_ffff		Kernel (64K)
0x0001_0000-0x001f_ffff		User Memory (1.9 Meg)

0x1f00_0000-0x1f00_ffff		Parallel Port (64K)

0x1f80_0000-0x1f80_03ff		Scratch Pad (1024 bytes)

0x1f80_1000-0x1f80_2fff		Hardware Registers (8K)

0x1fc0_0000-0x1fc7_ffff		BIOS (512K)

0x8000_0000-0x801f_ffff		Kernel and User Memory Mirror (2 Meg) Cached
0x9fc0_0000-0x9fc7_ffff		BIOS Mirror (512K) Cached

0xa000_0000-0xa01f_ffff		Kernel and User Memory Mirror (2 Meg) Uncached
0xbfc0_0000-0xbfc7_ffff		BIOS Mirror (512K) Uncached
*/


enum MemoryState{MEM_NO_CHANGES, MEM_PUSHED_CHANGES, MEM_UNPUSHED_CHANGES, MEM_SILENCED};

typedef struct {
	int startindex;
	int length;
	int key;
	int state;
	char* value;
	pthread_mutex_t lock;
	char* last;
	char newonly;
	char buffer;
} MemoryHook;


MemoryHook* hooks;
int numHooks;
int hookMin;
int hookMax;
int flushMemory = 0;

pthread_mutex_t memPipeLock;
char speakerPipeName[64];
int memPipe;
int isRecordingMemory;

pthread_t SpeakerOfTheMemoryThread;


void silenceMemoryNotification(int keyv){
	int k = -1;
	for (int i=0; i<numHooks; i++){
		if (hooks[i].key == keyv){
			k = i;
			break;
		}
	}
	if (k == -1) return;
	pthread_mutex_lock(&(hooks[k].lock));
	hooks[k].state = MEM_SILENCED;
	pthread_mutex_unlock(&(hooks[k].lock));
}

void unsilenceMemoryNotification(int keyv){
	int k = -1;
	for (int i=0; i<numHooks; i++){
		if (hooks[i].key == keyv){
			k = i;
			break;
		}
	}
	if (k == -1) return;
	pthread_mutex_lock(&(hooks[k].lock));
	hooks[k].state = MEM_NO_CHANGES;
	pthread_mutex_unlock(&(hooks[k].lock));
}


void writeStatusNotification(int type){

	char keyv[1];
	keyv[0] = (char) type;
	pthread_mutex_lock(&memPipeLock);
	write(memPipe, keyv, sizeof(char));
	pthread_mutex_unlock(&memPipeLock);
}

void writeMemoryNotification(int i){

	int length = hooks[i].length;
	char* memory = hooks[i].value;

	char keyv[2];
	pthread_mutex_lock(&memPipeLock);

	keyv[0] = (char) 0;
	keyv[1] = (char) hooks[i].key;

	write(memPipe, keyv, 2*sizeof(char));
  write(memPipe, memory, length);
	pthread_mutex_unlock(&memPipeLock);
}

void* SpeakerOfMemoryFunction(void){


	while(flushMemory==1){
		for (int i=0; i<numHooks; i++){
			MemoryHook h = hooks[i];
			if (h.state != MEM_UNPUSHED_CHANGES) continue;
			pthread_mutex_lock(&(hooks[i].lock));
			writeMemoryNotification(i);
			hooks[i].state = MEM_NO_CHANGES;
			pthread_mutex_unlock(&(hooks[i].lock));
		}
		usleep(1000*400);

	}
	pthread_exit(NULL);
}


int psxMemInit(int* inputHooks, int nHooks, char* uniquePipeName) {
	int i;

	printf("Init Memory\n");

	psxMemRLUT = (u8 **)malloc(0x10000 * sizeof(void *));
	psxMemWLUT = (u8 **)malloc(0x10000 * sizeof(void *));
	memset(psxMemRLUT, 0, 0x10000 * sizeof(void *));
	memset(psxMemWLUT, 0, 0x10000 * sizeof(void *));

	isRecordingMemory = 1;
	if (strcmp(uniquePipeName, "none") == 0) isRecordingMemory = 0;

	if (isRecordingMemory){
		sprintf(speakerPipeName, "%s-mem", uniquePipeName);
	  int k = mkfifo(speakerPipeName, 0666);
	  memPipe = open(speakerPipeName, O_WRONLY);
		printf("[C] Attatched Memory Callback Pipe\n");
		pthread_mutex_init(&memPipeLock, NULL);

		char* p;
		char* ptr;
		u32 t;
		hookMin = 2097136;
		hookMax = 0;
		hooks = malloc(nHooks * sizeof(MemoryHook));
		numHooks = nHooks;
		int* vHook = inputHooks;
		for (int i=0; i<nHooks; i++){
			int st = vHook[0], len = vHook[1];
			unsigned char key = ((char*) (vHook+2))[0];
			unsigned char newonly = ((char*) (vHook+2))[1];
			unsigned char allowbuffer = ((char*) (vHook+2))[2];
			int state = ((key>>7)==(char)1)?MEM_SILENCED:MEM_NO_CHANGES;
			hooks[i].startindex = st;
			hooks[i].length = len;
			hooks[i].last = (newonly==1)?malloc(len*sizeof(char)):NULL;
			hooks[i].key = (int) ((key)&((char) 127));
			hooks[i].newonly = newonly;
			hooks[i].buffer = allowbuffer;
			printf("Added memhook: (%i, %i, %i, %i) buffer:%i newonly:%i\n", st, len, hooks[i].key, state, allowbuffer, newonly);
			hooks[i].state = MEM_NO_CHANGES;
			pthread_mutex_init(&(hooks[i].lock), NULL);
			hooks[i].value = NULL;

			if (st < hookMin) hookMin = st;
			if ((st+len) > hookMax) hookMax = st+len;
			vHook+=3;
		}

		flushMemory = 1;
		pthread_create(&SpeakerOfTheMemoryThread, NULL, SpeakerOfMemoryFunction, NULL);
	}


	psxM = mmap(0, 0x00220000,
		PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	psxP = &psxM[0x200000];
	psxH = &psxM[0x210000];

	psxR = (s8 *)malloc(0x00080000);

	if (psxMemRLUT == NULL || psxMemWLUT == NULL ||
		psxM == NULL || psxP == NULL || psxH == NULL) {
		SysMessage("%s", _("Error allocating memory!"));
		return -1;
	}

// MemR
	for (i = 0; i < 0x80; i++) psxMemRLUT[i + 0x0000] = (u8 *)&psxM[(i & 0x1f) << 16];

	memcpy(psxMemRLUT + 0x8000, psxMemRLUT, 0x80 * sizeof(void *));
	memcpy(psxMemRLUT + 0xa000, psxMemRLUT, 0x80 * sizeof(void *));

	psxMemRLUT[0x1f00] = (u8 *)psxP;
	psxMemRLUT[0x1f80] = (u8 *)psxH;

	for (i = 0; i < 0x08; i++) psxMemRLUT[i + 0x1fc0] = (u8 *)&psxR[i << 16];

	memcpy(psxMemRLUT + 0x9fc0, psxMemRLUT + 0x1fc0, 0x08 * sizeof(void *));
	memcpy(psxMemRLUT + 0xbfc0, psxMemRLUT + 0x1fc0, 0x08 * sizeof(void *));

// MemW
	for (i = 0; i < 0x80; i++) psxMemWLUT[i + 0x0000] = (u8 *)&psxM[(i & 0x1f) << 16];

	memcpy(psxMemWLUT + 0x8000, psxMemWLUT, 0x80 * sizeof(void *));
	memcpy(psxMemWLUT + 0xa000, psxMemWLUT, 0x80 * sizeof(void *));

	psxMemWLUT[0x1f00] = (u8 *)psxP;
	psxMemWLUT[0x1f80] = (u8 *)psxH;

	return 0;
}

void psxMemReset() {
	FILE *f = NULL;
	char bios[1024] = { '\0' };
	//char temp[1024] = { '\0' };

	memset(psxM, 0, 0x00200000);
	memset(psxP, 0, 0x00010000);

	// Load BIOS
	if (strcmp(Config.Bios, "HLE") != 0) {

/*
		sprintf(temp, "%s/%s", Config.BiosDir, Config.Bios);

		strcat( strcat( bios, GetAppPath() ), temp );
		f = fopen(bios, "rb");
		*/

	   //AppPath's priority is high.
		const char* apppath = GetAppPath();
		if( strlen(apppath) > 0 )
			strcat( strcat( strcat( bios, GetAppPath() ), "bios\\"), Config.Bios );
		else
			sprintf(bios, "%s/%s", Config.BiosDir, Config.Bios);
		f = fopen(bios, "rb");

		if (f == NULL) {
			SysMessage(_("Could not open BIOS:\"%s\". Enabling HLE Bios!\n"), bios);
			memset(psxR, 0, 0x80000);
			Config.HLE = TRUE;
		} else {
			fread(psxR, 1, 0x80000, f);
			fclose(f);
			Config.HLE = FALSE;

			SysPrintf(_("Loaded BIOS: %s\n"), bios );
		}
	} else Config.HLE = TRUE;
}

void psxMemShutdown() {
	munmap(psxM, 0x00220000);

	flushMemory = 0;

	if (isRecordingMemory){
		printf("Closing memory pipe.\n");
		pthread_join(SpeakerOfTheMemoryThread, NULL);
	  close(memPipe);
	  unlink(speakerPipeName);
	}

	printf("Close Memory...\n");

	free(psxR);
	free(psxMemRLUT);
	free(psxMemWLUT);
}

static int writeok = 1;

u8 psxMemRead8(u32 mem) {
	char *p;
	u32 t;


	psxRegs.cycle += 0;


	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return psxHu8(mem);
		else
			return psxHwRead8(mem);
	} else {
		p = (char *)(psxMemRLUT[t]);
		if (p != NULL) {
			if (Config.Debug)
				DebugCheckBP((mem & 0xffffff) | 0x80000000, BR1);
			return *(u8 *)(p + (mem & 0xffff));
		} else {
#ifdef PSXMEM_LOG
			PSXMEM_LOG("err lb %8.8lx\n", mem);
#endif
			return 0;
		}
	}
}

u16 psxMemRead16(u32 mem) {
	char *p;
	u32 t;


	psxRegs.cycle += 1;


	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return psxHu16(mem);
		else
			return psxHwRead16(mem);
	} else {
		p = (char *)(psxMemRLUT[t]);
		if (p != NULL) {
			if (Config.Debug)
				DebugCheckBP((mem & 0xffffff) | 0x80000000, BR2);
			return SWAPu16(*(u16 *)(p + (mem & 0xffff)));
		} else {
#ifdef PSXMEM_LOG
			PSXMEM_LOG("err lh %8.8lx\n", mem);
#endif
			return 0;
		}
	}
}

u32 psxMemRead32(u32 mem) {
	char *p;
	u32 t;


	psxRegs.cycle += 1;


	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return psxHu32(mem);
		else
			return psxHwRead32(mem);
	} else {
		p = (char *)(psxMemRLUT[t]);
		if (p != NULL) {
			if (Config.Debug)
				DebugCheckBP((mem & 0xffffff) | 0x80000000, BR4);
			return SWAPu32(*(u32 *)(p + (mem & 0xffff)));
		} else {
#ifdef PSXMEM_LOG
			if (writeok) { PSXMEM_LOG("err lw %8.8lx\n", mem); }
#endif
			return 0;
		}
	}
}

FILE* logMem;

void prepareLog(){
	logMem = fopen("~/psxmemlog.txt", "w");
}

void logValue(u32 v){
	fprintf(logMem, "%i\n", v);
}

void writingTo(u32 mem, int j){
	if (isRecordingMemory){
		if ((mem <= (hookMin-j)) || (mem >= hookMax)) return;
		for (int i=0; i<numHooks; i++){
			MemoryHook h = hooks[i];
			if ((h.state == MEM_UNPUSHED_CHANGES) || (h.state == MEM_SILENCED)) continue;
			pthread_mutex_lock(&(hooks[i].lock));
			if ((mem > h.startindex-j) && (mem < (h.startindex+h.length))){
				// DEBUG: if (h.value != NULL) printf("Notification for %i state %i Comparison:%i Value=%i \n",i, h.state, memcmp(h.last, h.value, h.length), ((int*) h.value)[0]);
				if ((h.newonly == 1)&&(h.value != NULL)&&(memcmp(h.last, h.value, h.length) == 0)){
					pthread_mutex_unlock(&(hooks[i].lock));
					continue;
				}
				if (h.state == MEM_NO_CHANGES){
					if (h.buffer == 1) hooks[i].state = MEM_PUSHED_CHANGES;
					if (h.value == NULL) hooks[i].value = (char*) psxMemPointer(h.startindex);
					writeMemoryNotification(i);
				}else if (h.state == MEM_PUSHED_CHANGES){
					hooks[i].state = MEM_UNPUSHED_CHANGES;
				}
				if (h.newonly == 1) memcpy(h.last, hooks[i].value, h.length);
			}
			pthread_mutex_unlock(&(hooks[i].lock));
		}
	}
}

void psxMemWrite8(u32 mem, u8 value) {
	char *p;
	u32 t;

	psxRegs.cycle += 1;

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			psxHu8(mem) = value;
		else
			psxHwWrite8(mem, value);
	} else {
		p = (char *)(psxMemWLUT[t]);
		if (p != NULL) {
			if (Config.Debug)
				DebugCheckBP((mem & 0xffffff) | 0x80000000, BW1);
			*(u8 *)(p + (mem & 0xffff)) = value;
			writingTo(mem & 0xffffff, 1);
#ifdef PSXREC
			psxCpu->Clear((mem & (~3)), 1);
#endif
		} else {
#ifdef PSXMEM_LOG
			PSXMEM_LOG("err sb %8.8lx\n", mem);
#endif
		}
	}
}

void psxMemWrite16(u32 mem, u16 value) {
	char *p;
	u32 t;


	psxRegs.cycle += 1;



	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			psxHu16ref(mem) = SWAPu16(value);
		else
			psxHwWrite16(mem, value);
	} else {
		p = (char *)(psxMemWLUT[t]);
		if (p != NULL) {
			if (Config.Debug)
				DebugCheckBP((mem & 0xffffff) | 0x80000000, BW2);
			*(u16 *)(p + (mem & 0xffff)) = SWAPu16(value);
			writingTo(mem & 0xffffff, 2);
#ifdef PSXREC
			psxCpu->Clear((mem & (~3)), 1);
#endif
		} else {
#ifdef PSXMEM_LOG
			PSXMEM_LOG("err sh %8.8lx\n", mem);
#endif
		}
	}
}

void psxMemWrite32(u32 mem, u32 value) {
	char *p;
	u32 t;

	psxRegs.cycle += 1;


	//	if ((mem&0x1fffff) == 0x71E18 || value == 0x48088800) SysPrintf("t2fix!!\n");
	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			psxHu32ref(mem) = SWAPu32(value);
		else
			psxHwWrite32(mem, value);
	} else {
		p = (char *)(psxMemWLUT[t]);
		if (p != NULL) {
			if (Config.Debug)
				DebugCheckBP((mem & 0xffffff) | 0x80000000, BW4);
			*(u32 *)(p + (mem & 0xffff)) = SWAPu32(value);
			writingTo(mem & 0xffffff, 4);
#ifdef PSXREC
			psxCpu->Clear(mem, 1);
#endif
		} else {
			if (mem != 0xfffe0130) {
#ifdef PSXREC
				if (!writeok)
					psxCpu->Clear(mem, 1);
#endif

#ifdef PSXMEM_LOG
				if (writeok) { PSXMEM_LOG("err sw %8.8lx\n", mem); }
#endif
			} else {
				int i;

				// a0-44: used for cache flushing
				switch (value) {
					case 0x800: case 0x804:
						if (writeok == 0) break;
						writeok = 0;
						memset(psxMemWLUT + 0x0000, 0, 0x80 * sizeof(void *));
						memset(psxMemWLUT + 0x8000, 0, 0x80 * sizeof(void *));
						memset(psxMemWLUT + 0xa000, 0, 0x80 * sizeof(void *));

						psxRegs.ICache_valid = FALSE;
						break;
					case 0x00: case 0x1e988:
						if (writeok == 1) break;
						writeok = 1;
						for (i = 0; i < 0x80; i++) psxMemWLUT[i + 0x0000] = (void *)&psxM[(i & 0x1f) << 16];
						memcpy(psxMemWLUT + 0x8000, psxMemWLUT, 0x80 * sizeof(void *));
						memcpy(psxMemWLUT + 0xa000, psxMemWLUT, 0x80 * sizeof(void *));
						break;
					default:
#ifdef PSXMEM_LOG
						PSXMEM_LOG("unk %8.8lx = %x\n", mem, value);
#endif
						break;
				}
			}
		}
	}
}

void *psxMemPointer(u32 mem) {
	char *p;
	u32 t;

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return (void *)&psxH[mem];
		else
			return NULL;
	} else {
		p = (char *)(psxMemWLUT[t]);
		if (p != NULL) {
			return (void *)(p + (mem & 0xffff));
		}
		return NULL;
	}
}
