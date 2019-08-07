/*  Pcsx - Pc Psx Emulator
 *  Copyright (C) 1999-2002  Pcsx Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <gtk/gtk.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../libpcsxcore/psxmem.h"
#include "../libpcsxcore/sio.h"

#include "Linux.h"
#include <fcntl.h>
#include "ConfDlg.h"

#ifdef ENABLE_NLS
#include <locale.h>
#endif

#include <X11/extensions/XTest.h>


#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#define DEBUG FALSE

typedef struct {
	size_t fsize;
	void* bytes;
} GPUShot;

enum {
	RUN = 0,
	RUN_CD,
};



gboolean UseGui = FALSE;


pthread_t ProceedurePipeThread;

int* audioRecordSwitch;
char* audioRecordPath;
char* uniquePipeValue;

int stateActionRequest = 0;
char* stateToLoad;

boolean emulationIsPaused = FALSE;


int integerStackValue(int value, int bottom){
	return (value<<8)|(bottom&0xff);
}

void* ProceedurePipeThreadF(void* pname) {
		FILE * fp = fopen ("/home/carlos/procPipeDebug.txt","w");
		if (strcmp(pname, "none") == 0) pthread_exit(NULL);
    if (DEBUG) printf("Running Proceedure Thread...\n");
		char pipename[64];
		sprintf(pipename, "%s-proc", pname);
		char insbuf[1];
		int shmid;
		int shared_memory_setup = 0;
	  char *shared_memory;
		int ins_source;
		ins_source = open((char*)pipename, O_RDONLY);
		while(1){
			int b = read(ins_source, insbuf, 1);
			if (DEBUG) printf ("Have: %i\n",b);
			if (b==0) break;
			if (DEBUG) printf("[PROC]	%i\n", insbuf[0]);
			int node = insbuf[0];
			//printf ("Enter %i\n",node);
			if (insbuf[0]==(char)1){
				// Exit emulator
				close(ins_source);
				EmuShutdown();
				ReleasePlugins();
				freeMLAdditions();
				StopDebugger();
				if (DEBUG) printf("Freeing emulator...\n");
				if (emuLog != NULL) fclose(emuLog);
				break;
			}else if (insbuf[0]==(char) 2){
				// Pause Emulation
				if (DEBUG) printf("[M]		Request Pause\n");
				if (emulationIsPaused){
					// We are already paused!
					writeStatusNotification(7);
				}
				emulationIsPaused = TRUE;
			}else if (insbuf[0]==(char) 3){
				// Resume Emulation
				if (DEBUG) printf("[M]		Request Resume\n");
				if (!emulationIsPaused){
					// We are already resumed!
					writeStatusNotification(8);
				}
				emulationIsPaused = FALSE;
			}else if (insbuf[0]==(char)11){
				// Get render
				if (DEBUG) printf("Snapshot requested.\n");
				read(ins_source, insbuf, 1);
				if (DEBUG) printf("Unique number is %i\n", insbuf[0]);
				TakeGPUSnapshot(insbuf[0]);
			}else if (insbuf[0]==(char)12){
				// Clear shared memory for render
				if (DEBUG) printf("You want to clear shared memory\n");
				TakeGPUSnapshot(0);
			}else if (insbuf[0]==(char)13){
				// Headless way of pulling framebuffer
				// Unused
			}else if (insbuf[0]==(char)21){
				// PSX memory query
				int startindex = 0;
				for (int i=0;i<4;i++){
					read(ins_source, insbuf, 1);
					startindex = integerStackValue(startindex, (int) insbuf[0]);
				}

				int length = 0;
				for (int i=0;i<4;i++){
					read(ins_source, insbuf, 1);
					//printf ("B%i\n",i);
					length = integerStackValue(length, (int) insbuf[0]);
				}


				read(ins_source, insbuf, 1);
				key_t key = insbuf[0];

				// Setup shared memory
				if (shared_memory_setup == 0){
				  if ((shmid = shmget(key, 128, IPC_CREAT | 0666)) < 0)
				  {
						perror("shmget");
				    if (DEBUG) printf("Error getting shared memory id");
						printf ("Error getting shared memory id\n",node);
						break;
				  }
					shared_memory_setup = 1;
				  // Attached shared memory
				  if ((shared_memory = shmat(shmid, NULL, 0)) == (char *) -1)
				  {
						perror("shmat");
				    if (DEBUG) printf("Error attaching shared memory id");
						printf ("Error getting shared memory id\n",node);
						break;
				  }
				}

				memcpy(shared_memory, psxMemPointer(startindex), length);
				if (DEBUG) printf("(Notified)\n");
				writeStatusNotification(11);

			}else if (insbuf[0]==(char)22){
				// Dump memory to file

				int startindex = 0;
				for (int i=0;i<4;i++){
					read(ins_source, insbuf, 1);
					startindex = integerStackValue(startindex, (int) insbuf[0]);
				}

				int length = 0;
				for (int i=0;i<4;i++){
					read(ins_source, insbuf, 1);
					length = integerStackValue(length, (int) insbuf[0]);
				}

				read(ins_source, insbuf, 1);
				char sizeofstring = insbuf[0];
				char* outputPath = malloc(sizeof(char) * (((int)sizeofstring)+1));
				read(ins_source, outputPath, sizeofstring);
				outputPath[sizeofstring] = '\0';

				if (DEBUG) printf("Dumping %i bytes to %s\n", length, outputPath);
				FILE* fd = fopen(outputPath, "w");
				if (fd == NULL) {
					if (DEBUG) printf("Failed to write to file.\n");
					continue;
				}
				int wrote = fwrite(psxMemPointer(startindex), sizeof(char), length, fd);
				if (wrote != length){
					if (DEBUG) printf("Error writing... \n");
				}
				fclose(fd);
				free(outputPath);
				writeStatusNotification(4);

			}else if (insbuf[0]==(char)23){
				// Remove shared memory used for PSX memory query
				if (shared_memory_setup == 1){
					if (DEBUG) printf("[M]		Removing shared memory...\n");
					shmdt(shmid);
					shmctl(shmid, IPC_RMID, NULL);
					shared_memory_setup = 0;
				}
			}else if (insbuf[0]==(char)24){
				// Silence Memory Listener
				read(ins_source, insbuf, 1);
				int tarkey = (int) insbuf[0];
				silenceMemoryNotification(tarkey);
			}else if (insbuf[0]==(char)25){
				// Unsilence Memory Listener
				read(ins_source, insbuf, 1);
				int tarkey = (int) insbuf[0];
				unsilenceMemoryNotification(tarkey);
			}else if (insbuf[0]==(char)26){
				// Write byte to memory

				int startindex = 0;
				for (int i=0;i<4;i++){
					read(ins_source, insbuf, 1);
					startindex = integerStackValue(startindex, (int) insbuf[0]);
				}

				read(ins_source, insbuf, 1);
				char value = insbuf[0];

				psxMemWrite8(startindex, value);
				writeStatusNotification(5);
			}else if (insbuf[0]==(char)27){
				// Drill byte to memory

				int startindex = 0;
				for (int i=0;i<4;i++){
					read(ins_source, insbuf, 1);
					startindex = integerStackValue(startindex, (int) insbuf[0]);
				}

				read(ins_source, insbuf, 1);
				char value = insbuf[0];
				read(ins_source, insbuf, 1);
				int times = (int) insbuf[0];

				for (int i=0; i<times; i++){
					psxMemWrite8(startindex, value);
					usleep(5000);
				}
				if (DEBUG) printf("Finished drilling.\n");
				writeStatusNotification(6);
			}else if (insbuf[0]==(char)31){
				// Start recording audio
				read(ins_source, insbuf, 1);
				char sizeofstring = insbuf[0];
				read(ins_source, audioRecordPath, sizeofstring);
				audioRecordPath[sizeofstring] = '\0';
				if (DEBUG) printf("Audio will record. (%s)\n", audioRecordPath);
				*audioRecordSwitch = 1;
			}else if (insbuf[0]==(char)32){
				// Stop recording audio
				if (DEBUG) printf("Audio has stopped recording.\n");
				*audioRecordSwitch = 0;
			}else if (insbuf[0]==(char)41){
				// Save state
				if (stateActionRequest != 0){
					if (DEBUG) printf("Unable to save state, state being loaded. \n");
					continue;
				}
				read(ins_source, insbuf, 1);
				char sizeofstring = insbuf[0];
				stateToLoad = malloc(sizeof(char) * (((int)sizeofstring)+1));
				read(ins_source, stateToLoad, sizeofstring);
				stateToLoad[sizeofstring] = '\0';
				stateActionRequest = 2;
			}else if (insbuf[0]==(char)42){
				// Load state
				if (stateActionRequest != 0){
					if (DEBUG) printf("Unable to load state, state being saved. \n");
					continue;
				}
				read(ins_source, insbuf, 1);
				char sizeofstring = insbuf[0];
				stateToLoad = malloc(sizeof(char) * (((int)sizeofstring)+1));
				read(ins_source, stateToLoad, sizeofstring);
				stateToLoad[sizeofstring] = '\0';
				stateActionRequest = 1;
			}else if (insbuf[0]==(char)43){
				// Set GPU speed
				int speedset = 0;
				for (int i=0;i<4;i++){
					read(ins_source, insbuf, 1);
					speedset = integerStackValue(speedset, (int) insbuf[0]);
				}
				GPU_setSpeed(speedset/100.0);
				writeStatusNotification(10);
			}else if (insbuf[0]==(char)50){
				// Debugging
				writeStatusNotification(12);
			}

		}
		fprintf (fp, "End of service.\n");
   	fclose (fp);
    pthread_exit(NULL);
}


static void CreateMemcard(char *filename, char *conf_mcd) {
	gchar *mcd;
	struct stat buf;

	mcd = g_build_filename(getenv("HOME"), MEMCARD_DIR, filename, NULL);

	strcpy(conf_mcd, mcd);

	/* Only create a memory card if an existing one does not exist */
	if (stat(mcd, &buf) == -1) {
		SysPrintf(_("Creating memory card: %s\n"), mcd);
		CreateMcd(mcd);
	}

	g_free (mcd);
}

/* Create a directory under the $HOME directory, if that directory doesn't already exist */
static void CreateHomeConfigDir(char *directory) {
	struct stat buf;

	if (stat(directory, &buf) == -1) {
		gchar *dir_name = g_build_filename (getenv("HOME"), directory, NULL);
		mkdir(dir_name, S_IRWXU | S_IRWXG);
		g_free (dir_name);
	}
}

static void CheckSubDir() {
	// make sure that ~/.pcsxr exists
	CreateHomeConfigDir(PCSXR_DOT_DIR);

	CreateHomeConfigDir(BIOS_DIR);
	CreateHomeConfigDir(MEMCARD_DIR);
  CreateHomeConfigDir(MEMCARD_PERGAME_DIR);
	CreateHomeConfigDir(STATES_DIR);
	CreateHomeConfigDir(PLUGINS_DIR);
	CreateHomeConfigDir(PLUGINS_CFG_DIR);
	CreateHomeConfigDir(CHEATS_DIR);
	CreateHomeConfigDir(PATCHES_DIR);
}

static void ScanPlugins(gchar* scandir) {
	// scan for plugins and configuration tools
	DIR *dir;
	struct dirent *ent;

	gchar *linkname;
	gchar *filename;

	/* Any plugins found will be symlinked to the following directory */
	dir = opendir(scandir);
	if (dir != NULL) {
		while ((ent = readdir(dir)) != NULL) {
			filename = g_build_filename (scandir, ent->d_name, NULL);

			if (match(filename, ".*\\.so$") == 0 &&
				match(filename, ".*\\.dylib$") == 0 &&
				match(filename, "cfg.*") == 0) {
				continue;	/* Skip this file */
			} else {
				/* Create a symlink from this file to the directory ~/.pcsxr/plugin */
				linkname = g_build_filename (getenv("HOME"), PLUGINS_DIR, ent->d_name, NULL);
				symlink(filename, linkname);

				/* If it's a config tool, make one in the cfg dir as well.
				   This allows plugins with retarded cfg finding to work :- ) */
				if (match(filename, "cfg.*") == 1) {
					linkname = g_build_filename (getenv("HOME"), PLUGINS_CFG_DIR, ent->d_name, NULL);
					symlink(filename, linkname);
				}
				g_free (linkname);
			}
			g_free (filename);
		}
		closedir(dir);
	}
}

static void ScanBios(gchar* scandir) {
	// scan for bioses
	DIR *dir;
	struct dirent *ent;

	gchar *linkname;
	gchar *filename;

	/* Any bioses found will be symlinked to the following directory */
	dir = opendir(scandir);
	if (dir != NULL) {
		while ((ent = readdir(dir)) != NULL) {
			filename = g_build_filename(scandir, ent->d_name, NULL);

			if (match(filename, ".*\\.bin$") == 0 &&
				match(filename, ".*\\.BIN$") == 0) {
				continue;	/* Skip this file */
			} else {
				/* Create a symlink from this file to the directory ~/.pcsxr/plugin */
				linkname = g_build_filename(getenv("HOME"), BIOS_DIR, ent->d_name, NULL);
				symlink(filename, linkname);

				g_free(linkname);
			}
			g_free(filename);
		}
		closedir(dir);
	}
}

static void CheckSymlinksInPath(char* dotdir) {
	DIR *dir;
	struct dirent *ent;
	struct stat stbuf;
	gchar *linkname;

	dir = opendir(dotdir);
	if (dir == NULL) {
		SysMessage(_("Could not open directory: '%s'\n"), dotdir);
		return;
	}

	/* Check for any bad links in the directory. If the remote
	   file no longer exists, remove the link */
	while ((ent = readdir(dir)) != NULL) {
		linkname = g_strconcat (dotdir, ent->d_name, NULL);

		if (stat(linkname, &stbuf) == -1) {
			/* File link is bad, remove it */
			unlink(linkname);
		}
		g_free (linkname);
	}
	closedir(dir);
}

static void ScanAllPlugins (void) {
	gchar *currentdir;

	// scan some default locations to find plugins
	ScanPlugins(DEF_PLUGIN_DIR);
	ScanPlugins(DEF_PLUGIN_DIR "/lib");
	ScanPlugins(DEF_PLUGIN_DIR "/lib64");
	ScanPlugins(DEF_PLUGIN_DIR "/lib32");
	ScanPlugins(DEF_PLUGIN_DIR "/config");

	// scan some default locations to find bioses
	ScanBios(PSEMU_DATA_DIR);
	ScanBios(PSEMU_DATA_DIR "/bios");

	currentdir = g_strconcat(getenv("HOME"), "/.psemu-plugins/", NULL);
	ScanPlugins(currentdir);
	g_free(currentdir);

	currentdir = g_strconcat(getenv("HOME"), "/.psemu/", NULL);
	ScanPlugins(currentdir);
	g_free(currentdir);

	// Check for bad links in ~/.pcsxr/plugins/
	currentdir = g_build_filename(getenv("HOME"), PLUGINS_DIR, NULL);
	CheckSymlinksInPath(currentdir);
	g_free(currentdir);

	// Check for bad links in ~/.pcsxr/plugins/cfg
	currentdir = g_build_filename(getenv("HOME"), PLUGINS_CFG_DIR, NULL);
	CheckSymlinksInPath(currentdir);
	g_free(currentdir);

	// Check for bad links in ~/.pcsxr/bios
	currentdir = g_build_filename(getenv("HOME"), BIOS_DIR, NULL);
	CheckSymlinksInPath(currentdir);
	g_free(currentdir);
}

// Set the default plugin name
void set_default_plugin(char *plugin_name, char *conf_plugin_name) {
	if (strlen(plugin_name) != 0) {
		strcpy(conf_plugin_name, plugin_name);
		printf("Picking default plugin: %s\n", plugin_name);
	} else
		printf("No default plugin could be found for %s\n", conf_plugin_name);
}

int main(int argc, char *argv[]) {
	char file[MAXPATHLEN] = "";
	char path[MAXPATHLEN];
	int runcd = RUN;
	int loadst = -1;
	int i;

#ifdef ENABLE_NLS
	setlocale (LC_ALL, "");
	bindtextdomain (GETTEXT_PACKAGE, LOCALE_DIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);
#endif

	memset(&Config, 0, sizeof(PcsxConfig));

	// what is the name of the config file?
	// it may be redefined by -cfg on the command line
	strcpy(cfgfile_basename, "pcsxr.cfg");


	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-runcd")) runcd = RUN_CD;
		else if (!strcmp(argv[i], "-gui")) UseGui = TRUE;
		else if (!strcmp(argv[i], "-psxout")) Config.PsxOut = TRUE;
		else if (!strcmp(argv[i], "-slowboot")) Config.SlowBoot = TRUE;
		else if (!strcmp(argv[i], "-load")) loadst = ((argc > i+1) ? atol(argv[++i]) : 0);
		else if (!strcmp(argv[i], "-loadState")){
			loadst = -2;
			stateToLoad = argv[++i];
		}else if (!strcmp(argv[i], "-controlPipe")){
			char* value = argv[++i];
			strcpy(uniquePipeValue, value);
			validPipes = 1;
		}else if (!strcmp(argv[i], "-nMemoryListeners")){
			char* value = argv[++i];
			strcpy(memoryListenersCount, value);
			nHooks = (int) strtol(memoryListenersCount, (char **)NULL, 10);
		}else if (!strcmp(argv[i], "-cfg")) {
			if (i+1 >= argc) break;
			strncpy(cfgfile_basename, argv[++i], MAXPATHLEN-100);	/* TODO buffer overruns */
			printf("Using config file %s.\n", cfgfile_basename);
		}
		else if (!strcmp(argv[i], "-play")) {
			char isofilename[MAXPATHLEN];

			if (i+1 >= argc) break;
			strncpy(isofilename, argv[++i], MAXPATHLEN);
			isofilename[MAXPATHLEN] = '\0';
			if (DEBUG) printf("Playing %s\n", isofilename);
			SetIsoFile(isofilename);
			runcd = RUN_CD;
		}
		else if (!strcmp(argv[i], "-display")) {
			char dispval[2];

			if (i+1 >= argc) break;
			strncpy(dispval, argv[++i], 1);
			dispval[1] = '\0';
			dispMode = (int) strtol(dispval, (char **)NULL, 10);
			if (DEBUG) printf("Display mode %i\n", dispMode);
		}
		else if (!strcmp(argv[i], "-h") ||
			 !strcmp(argv[i], "-help") ||
			 !strcmp(argv[i], "--help")) {
			 if (DEBUG) printf(PACKAGE_STRING "\n");
			 if (DEBUG) printf("%s\n", _(
							" pcsxr [options] [file]\n"
							"\toptions:\n"
							"\t-runcd\t\tRuns CD-ROM\n"
							"\t-play FILE\tRuns a CD image file\n"
							"\t-gui\t\tOpen the GTK GUI\n"
							"\t-cfg FILE\tLoads desired configuration file (default: ~/.pcsxr/pcsxr.cfg)\n"
							"\t-psxout\t\tEnable PSX output\n"
							"\t-slowboot\tEnable BIOS Logo\n"
							"\t-load STATENUM\tLoads savestate STATENUM (1-9)\n"
							"\t-h -help\tDisplay this message\n"
							"\t-controlPipe NAME\tSet the name of the control pipes\n"
							"\t-display NUM\tSet the display mode, default 1\n"
							"\tfile\t\tLoads file\n"));
			 return 0;
		} else {
			strncpy(file, argv[i], MAXPATHLEN);
			if (file[0] != '/') {
				getcwd(path, MAXPATHLEN);
				if (strlen(path) + strlen(file) + 1 < MAXPATHLEN) {
					strcat(path, "/");
					strcat(path, file);
					strcpy(file, path);
				} else
					file[0] = 0;
			}
		}
	}

	if(validPipes != 1) {
		uniquePipeValue = "none";
	}
	audioRecordSwitch = malloc(sizeof(int));
	audioRecordPath = malloc(128*sizeof(char));


	if (nHooks != 0){
		inputHooks = malloc(sizeof(int)*nHooks*3);
		read(STDIN_FILENO, inputHooks, sizeof(int)*nHooks*3);
	}

	strcpy(Config.Net, "Disabled");

	if (UseGui) gtk_init(&argc, &argv);

	CheckSubDir();
	ScanAllPlugins();

	// try to load config
	// if the config file doesn't exist
	if (LoadConfig() == -1) {
		if (!UseGui) {
			printf(_("PCSXR cannot be configured without using the GUI -- you should restart without -nogui.\n"));
			return 1;
		}

		// Uh oh, no config file found, use some defaults
		Config.PsxAuto = 1;

		gchar *str_bios_dir = g_strconcat(getenv("HOME"), BIOS_DIR, NULL);
		strcpy(Config.BiosDir, str_bios_dir);
		g_free(str_bios_dir);

		gchar *str_plugin_dir = g_strconcat(getenv("HOME"), PLUGINS_DIR, NULL);
		strcpy(Config.PluginsDir, str_plugin_dir);
		g_free(str_plugin_dir);

		// Update available plugins, but not GUI
		UpdatePluginsBIOS();

		// Pick some defaults, if they're available
		set_default_plugin(GpuConfS.plist[0], Config.Gpu);
		set_default_plugin(SpuConfS.plist[0], Config.Spu);
		set_default_plugin(CdrConfS.plist[0], Config.Cdr);
#ifdef ENABLE_SIO1API
		set_default_plugin(Sio1ConfS.plist[0], Config.Sio1);
#endif
		set_default_plugin(Pad1ConfS.plist[0], Config.Pad1);
		set_default_plugin(Pad2ConfS.plist[0], Config.Pad2);
		set_default_plugin(BiosConfS.plist[0], Config.Bios);

		// create & load default memcards if they don't exist
		CreateMemcard("card1.mcd", Config.Mcd1);
		CreateMemcard("card2.mcd", Config.Mcd2);

		LoadMcds(Config.Mcd1, Config.Mcd2);

		SaveConfig();
	}

	gchar *str_patches_dir = g_strconcat(getenv("HOME"), PATCHES_DIR, NULL);
	strcpy(Config.PatchesDir,  str_patches_dir);
	g_free(str_patches_dir);

	// switch to plugin dotdir
	// this lets plugins work without modification!
	gchar *plugin_default_dir = g_build_filename(getenv("HOME"), PLUGINS_DIR, NULL);
	chdir(plugin_default_dir);
	g_free(plugin_default_dir);

	if (UseGui && runcd != RUN_CD) SetIsoFile(NULL);

	if (SysInit() == -1) return 1;

	if (UseGui && runcd != RUN_CD) {
		StartGui();
	} else {
		// the following only occurs if the gui isn't started
		if (LoadPlugins() == -1) {
			SysErrorMessage(_("Error"), _("Failed loading plugins!"));
			return 1;
		}

		if (OpenPlugins() == -1 || plugins_configured() == FALSE) {
			return 1;
		}

		CheckCdrom();

		// Auto-detect: get region first, then rcnt-bios reset
		SysReset();

		if (file[0] != '\0') {
			Load(file);
		} else {
			if (runcd == RUN_CD) {
				if (LoadCdrom() == -1) {
					ClosePlugins();
					printf(_("Could not load CD-ROM!\n"));
					return -1;
				}
			}
		}

		if (loadst==0) {
			loadst = UpdateMenuSlots() + 1;
		}
		// If a state has been specified, then load that
		if (loadst > 0) {
			SysPrintf("Loading slot %i (HLE=%i)\n", loadst, Config.HLE);
			StatesC = loadst - 1;
			gchar *state_filename = get_state_filename(StatesC);
			LoadState(state_filename);
			g_free(state_filename);
		}

		autoloadCheats();
		psxCpu->Execute();
	}


	free(uniquePipeValue);
	free(memoryListenersCount);
	if (inputHooks != NULL) free(inputHooks);

	return 0;
}

int SysInit() {
#ifdef EMU_LOG
#ifndef LOG_STDOUT
	emuLog = fopen("emuLog.txt","wb");
#else
	emuLog = stdout;
#endif
#ifdef PSXCPU_LOG
	if (Config.PsxOut) { //PSXCPU_LOG generates so much stuff that buffer is necessary
		const int BUFSZ = 20 * 1024*1024;
		void* buf = malloc(BUFSZ);
		setvbuf(emuLog, buf, _IOFBF, BUFSZ);
	} else {
		setvbuf(emuLog, NULL, _IONBF, 0u);
	}
#else
	setvbuf(emuLog, NULL, _IONBF, 0u);
#endif
#endif

	if (EmuInit() == -1) {
		printf(_("PSX emulator couldn't be initialized.\n"));
		return -1;
	}

	LoadMcds(Config.Mcd1, Config.Mcd2);	/* TODO Do we need to have this here, or in the calling main() function?? */

	if (Config.Debug) {
		StartDebugger();
	}

	return 0;
}

void SysReset() {
	EmuReset();
}


void freeMLAdditions(void){
	if (DEBUG) printf("Freeing ML additions...\n");
	free(audioRecordSwitch);
	free(audioRecordPath);
}

void SysClose() {
	EmuShutdown();
	ReleasePlugins();

	StopDebugger();

	if (emuLog != NULL) fclose(emuLog);
}

void SysPrintf(const char *fmt, ...) {
	va_list list;
	char msg[512];

	va_start(list, fmt);
	vsprintf(msg, fmt, list);
	va_end(list);

	if (Config.PsxOut) {
		static char linestart = 1;
		int l = strlen(msg);

		printf(linestart ? " * %s" : "%s", msg);

		if (l > 0 && msg[l - 1] == '\n') {
			linestart = 1;
		} else {
			linestart = 0;
		}
	}

#ifdef EMU_LOG
#ifndef LOG_STDOUT
	if (emuLog != NULL) fprintf(emuLog, "%s", msg);
#endif
#endif
}

void *SysLoadLibrary(const char *lib) {
	return dlopen(lib, RTLD_NOW);
}

void *SysLoadSym(void *lib, const char *sym) {
	return dlsym(lib, sym);
}

const char *SysLibError() {
	return dlerror();
}

void SysCloseLibrary(void *lib) {
	dlclose(lib);
}

static void SysDisableScreenSaver() {
	static time_t fake_key_timer = 0;
	static char first_time = 1, has_test_ext = 0, t = 1;
	Display *display;
	extern unsigned long gpuDisp;

	display = (Display *)gpuDisp;

	if (first_time) {
		// check if xtest is available
		int a, b, c, d;
		has_test_ext = XTestQueryExtension(display, &a, &b, &c, &d);

		first_time = 0;
	}

	if (has_test_ext && fake_key_timer < time(NULL)) {
		XTestFakeRelativeMotionEvent(display, t *= -1, 0, 0);
		fake_key_timer = time(NULL) + 55;
	}
}


void SysUpdate() {

	if (emulationIsPaused){
		writeStatusNotification(7);
		// Need the sleep to allow value time to change
		while(1){
			usleep(100000);
			if(!emulationIsPaused) break;
		}
		writeStatusNotification(8);
	}

	PADhandleKey(PAD1_keypressed() );
	PADhandleKey(PAD2_keypressed() );
	SysDisableScreenSaver();

	if (stateActionRequest == 0) return;
	int req = stateActionRequest;
	stateActionRequest = 0;
	if (req == 1){
		state_load(stateToLoad);
		free(stateToLoad);
		if (Config.Cpu == CPU_DYNAREC) psxCpu->Execute();
	}else if (req == 2){
		state_save(stateToLoad);
		free(stateToLoad);
	}

}



/* ADB TODO Replace RunGui() with StartGui ()*/
void SysRunGui() {
	StartGui();
}
