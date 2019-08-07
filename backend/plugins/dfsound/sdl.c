/* SDL Driver for P.E.Op.S Sound Plugin
 * Copyright (c) 2010, Wei Mingzhi <whistler_wmz@users.sf.net>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "stdafx.h"

#include "externals.h"
#include <SDL.h>


#define BUFFER_SIZE		22050


int* rec_switch;
char* rec_dumpLocation;
int rec_hasBegunAudio;
int rec_lastSwitchState;
FILE* rec_fd;

short			*pSndBuffer = NULL;
int				iBufSize = 0;
volatile int	iReadPos = 0, iWritePos = 0;
short* nonzeros;
short* nonzeroST;
int countNonZero;

int seen_sound = 0;

int killSound = 0;

int count_empty = 0;

void (*wsNotificationPtr)(int);

static void SOUND_FillAudio(void *unused, Uint8 *stream, int len) {
	if (killSound) return;

	short *p = (short *)stream;
	short *pcopy = (short*) malloc(len);
	short *pcopystart = pcopy;

	len /= sizeof(short);
	int lencopy = 0;


	int curr_switch = *rec_switch;

	/*
	if (curr_switch == 1){
		nonzeros = malloc(len*sizeof(short));
		countNonZero = 0;
		nonzeroST = nonzeros;
	}
	*/
	if (curr_switch^rec_lastSwitchState == 1){
		if ((curr_switch == 1) && (rec_lastSwitchState == 0)) {
			seen_sound = 0;
			count_empty = 0;
			printf("Begining recording of audio (%s).\n", rec_dumpLocation);
			rec_fd = fopen(rec_dumpLocation, "wb");
			if (rec_fd == NULL){
				printf("Error recoding!\n");
			}
		}

		if ((curr_switch == 0) && (rec_lastSwitchState == 1)) {
			printf("Ending recording of audio.\n");
			seen_sound = 0;
			fclose(rec_fd);
		}
		rec_lastSwitchState = curr_switch;
	}


	/*
	while (iReadPos != iWritePos && len > 0) {
		*p++ = pSndBuffer[iReadPos++];
		if (iReadPos >= iBufSize) iReadPos = 0;
		--len;
	}
	*/

	//short current = (short) pSndBuffer[iReadPos];

	/*
	if (curr_switch == 1){
		if (current != 0){
			*nonzeros = current;
			countNonZero += 1;
			nonzeros++;
		}
	}
	*/
	short current;
	int commit_sound;

	while (iReadPos != iWritePos && len > 0) {
		commit_sound = 1;

		if (curr_switch == 1){
			current = pSndBuffer[iReadPos++];

			if (current == 0){
				if (seen_sound==0){
					commit_sound = 0;  // We haven't seen any sound yet

					if (count_empty >= 50000){
						if (count_empty == 50000) {
							printf("    [REC] We have timed out listening.\n");
							count_empty += 1;
							(*wsNotificationPtr)(9);
						}
					}else{
						count_empty += 1; // Note a zero
					}

				}else if (seen_sound==1){ // We have already started recording
						if (count_empty >= 1500){
							commit_sound = 0; // It has been too long since we last saw sound
							seen_sound = 2;
							if (count_empty == 1500) {
								printf("    [REC] We have stopped recording now.\n");
								count_empty += 1;
							}
							count_empty = 0;
						}else{
							count_empty += 1; // Note a zero
						}
				}else{ // We have finished recording
					if (count_empty >= 2000){
						commit_sound = 0; // It has been too long since we last saw sound - give up
						if (count_empty == 2000) {
							printf("    [REC] We have stopped listening now.\n");
							count_empty += 1;
							(*wsNotificationPtr)(9);
						}
					}else{
						count_empty += 1; // Note a zero
					}
				}
			}else{
				count_empty = 0; // Reset count of empty cells
				if (seen_sound==0){ // We had not seen sound before this
					printf("    [REC] We have started recording.\n");
					seen_sound = 1; // Note that we have seen sound
				}
			}

			*p++ = current;

			if (commit_sound==1){
				*pcopy++ = current;
				lencopy += 1;
			}
		}else{
			*p++ = pSndBuffer[iReadPos++];
		}

		/*
		current = (short) pSndBuffer[iReadPos];
		if (curr_switch == 1){
			//if (current != 0){
				*nonzeros = current;
				countNonZero += 1;
				nonzeros++;
			//}
		}
		*/

		if (iReadPos >= iBufSize) iReadPos = 0;
		--len;
	}


	/*
	while (iReadPos != iWritePos && len > 0) {
		*p = (short) pSndBuffer[iReadPos++];

		if (curr_switch == 1){
			if (*p != 0){
				*nonzeros++ = *p++;
				countNonZero += 1;
			}
		}else{
			p++;
		}
		iReadPos++;


		if (iReadPos >= iBufSize) iReadPos = 0;
		--len;
	}
	*/

	// Fill remaining space with zero

	while (len > 0) {
		*p++ = 0;
		if (curr_switch == 1) *pcopy++ = 0;
		--len;
	}


	if (curr_switch == 1){
		fwrite(pcopystart, sizeof(short), lencopy, rec_fd);
		free(pcopystart);
	}



}

static void InitSDL() {
	if (killSound) return;
	if (SDL_WasInit(SDL_INIT_EVERYTHING)) {
		SDL_InitSubSystem(SDL_INIT_AUDIO);
	} else {
		SDL_Init(SDL_INIT_AUDIO | SDL_INIT_NOPARACHUTE);
	}
}

static void DestroySDL() {
	if (killSound) return;
	if (SDL_WasInit(SDL_INIT_EVERYTHING & ~SDL_INIT_AUDIO)) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	} else {
		SDL_Quit();
	}
}


void SetupSound(int* rSwitch, char* rPath, void (*wsNotification)(int)) {
	if (killSound) return;
	SDL_AudioSpec				spec;
	rec_switch = rSwitch;
	wsNotificationPtr = wsNotification;
	rec_dumpLocation = rPath;
	printf("Setting up Sound\n");

	if (pSndBuffer != NULL) return;

	InitSDL();

	spec.freq = 44100;
	//spec.freq = 10000;
	spec.format = AUDIO_S16SYS;
	spec.channels = iDisStereo ? 1 : 2;
	spec.samples = 1024;
	//spec.samples = 6;
	spec.callback = SOUND_FillAudio;

	if (SDL_OpenAudio(&spec, NULL) < 0) {
		DestroySDL();
		return;
	}

	iBufSize = BUFFER_SIZE;
	if (iDisStereo) iBufSize /= 2;

	pSndBuffer = (short *)malloc(iBufSize * sizeof(short));
	if (pSndBuffer == NULL) {
		SDL_CloseAudio();
		return;
	}

	iReadPos = 0;
	iWritePos = 0;

	SDL_PauseAudio(0);
	printf("Has finished setting up Sound\n");
}

void RemoveSound(void) {
	if (killSound) return;
	if (pSndBuffer == NULL) return;

	printf("A.\n");
	SDL_CloseAudio();
	printf("B.\n");
	DestroySDL();
	printf("C.\n");

	free(pSndBuffer);
	printf("D.\n");
	pSndBuffer = NULL;
	printf("Audio closed.\n");
}

unsigned long SoundGetBytesBuffered(void) {
	if (killSound) return 0;
	int size;

	if (pSndBuffer == NULL) return SOUNDSIZE;

	size = iReadPos - iWritePos;
	if (size <= 0) size += iBufSize;

	if (size < iBufSize / 2) return SOUNDSIZE;

	return 0;
}

void SoundFeedStreamData(unsigned char *pSound, long lBytes) {
	if (killSound) return;
	short *p = (short *)pSound;

	if (pSndBuffer == NULL) return;

	while (lBytes > 0) {
		if (((iWritePos + 1) % iBufSize) == iReadPos) break;

		pSndBuffer[iWritePos] = *p++;


		++iWritePos;
		if (iWritePos >= iBufSize) iWritePos = 0;

		lBytes -= sizeof(short);
	}
}
