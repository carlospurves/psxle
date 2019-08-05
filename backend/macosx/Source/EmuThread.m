//
//  EmuThread.m
//  Pcsxr
//
//  Created by Gil Pedersen on Sun Sep 21 2003.
//  Copyright (c) 2003 __MyCompanyName__. All rights reserved.
//

#import <ExceptionHandling/NSExceptionHandler.h>
#import <Cocoa/Cocoa.h>
#include <pthread.h>
#include <setjmp.h>
#import "EmuThread.h"
#include "psxcommon.h"
#include "plugins.h"
#include "misc.h"

typedef NS_OPTIONS(unsigned int, EmulationEvents) {
	EMUEVENT_NONE	= 0,
	EMUEVENT_PAUSE	= (1<<0),
	EMUEVENT_RESET	= (1<<1),
	EMUEVENT_STOP	= (1<<2)
};

EmuThread *emuThread = nil;
static NSString *defrostPath = nil;
static EmulationEvents safeEvent;
static EmuThreadPauseStatus paused;
static BOOL runbios;

static pthread_cond_t eventCond;
static pthread_mutex_t eventMutex;

@implementation EmuThread
{
	BOOL wasPaused;
	jmp_buf  restartJmp;
}

- (void)setUpThread
{
	NSAssert(![[NSThread currentThread] isEqual:[NSThread mainThread]], @"This function should not be run on the main thread!");
	
	[[NSThread currentThread] setName:@"PSX Emu Background thread"];
	NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
	[center addObserver:self
			   selector:@selector(emuWindowDidClose:)
				   name:@"emuWindowDidClose" object:nil];
	
	[center addObserver:self
			   selector:@selector(emuWindowWantPause:)
				   name:@"emuWindowWantPause" object:nil];
	
	[center addObserver:self
			   selector:@selector(emuWindowWantResume:)
				   name:@"emuWindowWantResume" object:nil];
	
	// we shouldn't change the priority, since we might depend on subthreads
	//[NSThread setThreadPriority:1.0-((1.0-[NSThread threadPriority])/4.0)];
}

- (void)EmuThreadRun:(id)anObject
{
	[self setUpThread];
	
	// Do processing here
	if (OpenPlugins() == -1)
		goto done;
	
	setjmp(restartJmp);
	
	int res = CheckCdrom();
	if (res == -1) {
		ClosePlugins();
		SysMessage("%s", _("Could not check CD-ROM!\n"));
		goto done;
	}
	
	// Auto-detect: region first, then rcnt reset
	EmuReset();
	
	LoadCdrom();
	
	if (defrostPath) {
		LoadState([defrostPath fileSystemRepresentation]);
		defrostPath = nil;
	}
	
	psxCpu->Execute();
	
done:
	emuThread = nil;
	
	return;
}

- (void)EmuThreadRunBios:(id)anObject
{
	[self setUpThread];
		
	// Do processing here
	if (OpenPlugins() == -1)
		goto done;
	
	EmuReset();
	
	psxCpu->Execute();
	
done:
	emuThread = nil;
	
	return;
}

- (void)dealloc
{
	// remove all registered observers
	[[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)emuWindowDidClose:(NSNotification *)aNotification
{
	[EmuThread stop];
}

- (void)emuWindowWantPause:(NSNotification *)aNotification
{
	wasPaused = [EmuThread pause];
}

- (void)emuWindowWantResume:(NSNotification *)aNotification
{
	if (!wasPaused) {
		[EmuThread resume];
	}
	wasPaused = NO;
}

/* called periodically from the emulation thread */
- (void)handleEvents
{
	/* only do a trylock here, since we're not interested in blocking,
	 and we can just handle events next time round */
	if (pthread_mutex_trylock(&eventMutex) == 0) {
		while (safeEvent) {
			if (safeEvent & EMUEVENT_STOP) {
				/* signify that the emulation has stopped */
				emuThread = nil;
				paused = PauseStateIsNotPaused;
				
				/* better unlock the mutex before killing ourself */
				pthread_mutex_unlock(&eventMutex);
				
				ClosePlugins();
				SysClose();
				
				//[[NSThread currentThread] autorelease];
				[NSThread exit];
				return;
			}
			
			if (safeEvent & EMUEVENT_RESET) {
#if 0
				/* signify that the emulation has stopped */
				[emuThread autorelease];
				emuThread = nil;
				
				/* better unlock the mutex before killing ourself */
				pthread_mutex_unlock(&eventMutex);
				
				ClosePlugins();
				
				// start a new emulation thread
				[EmuThread run];
				
				//[[NSThread currentThread] autorelease];
				[NSThread exit];
				return;
#else
				safeEvent &= ~EMUEVENT_RESET;
				pthread_mutex_unlock(&eventMutex);
				
				longjmp(restartJmp, 0);
#endif
			}
			
			if (safeEvent & EMUEVENT_PAUSE) {
				paused = PauseStateIsPaused;
				/* wait until we're signalled */
				pthread_cond_wait(&eventCond, &eventMutex);
			}
		}
		pthread_mutex_unlock(&eventMutex);
	}
}

+ (void)run
{
	int err;
	
	if (emuThread) {
		[EmuThread resume];
		return;
	}
	
	if (pthread_mutex_lock(&eventMutex) != 0) {
		err = pthread_cond_init(&eventCond, NULL);
		if (err) return;
		
		err = pthread_mutex_init(&eventMutex, NULL);
		if (err) return;
		
		pthread_mutex_lock(&eventMutex);
	}
	
	safeEvent = EMUEVENT_NONE;
	paused = NO;
	runbios = NO;
	
	if (SysInit() != 0) {
		pthread_mutex_unlock(&eventMutex);
		return;
	}
	
	emuThread = [[EmuThread alloc] init];
	
	[NSThread detachNewThreadSelector:@selector(EmuThreadRun:)
							 toTarget:emuThread withObject:nil];
	
	pthread_mutex_unlock(&eventMutex);
}

+ (void)runBios
{
	int err;
	
	if (emuThread) {
		[EmuThread resume];
		return;
	}
	
	if (pthread_mutex_lock(&eventMutex) != 0) {
		err = pthread_cond_init(&eventCond, NULL);
		if (err) return;
		
		err = pthread_mutex_init(&eventMutex, NULL);
		if (err) return;
		
		pthread_mutex_lock(&eventMutex);
	}
	
	safeEvent = EMUEVENT_NONE;
	paused = PauseStateIsNotPaused;
	runbios = YES;
	
	if (SysInit() != 0) {
		pthread_mutex_unlock(&eventMutex);
		return;
	}
	
	emuThread = [[EmuThread alloc] init];
	
	[NSThread detachNewThreadSelector:@selector(EmuThreadRunBios:)
							 toTarget:emuThread withObject:nil];
	
	pthread_mutex_unlock(&eventMutex);
}

+ (void)stop
{
	pthread_mutex_lock(&eventMutex);
	safeEvent = EMUEVENT_STOP;
	pthread_mutex_unlock(&eventMutex);
	
	// wake it if it's sleeping
	pthread_cond_broadcast(&eventCond);
}

+ (BOOL)pause
{
	if (paused != PauseStateIsNotPaused || ![EmuThread active])
		return YES;
	
	pthread_mutex_lock(&eventMutex);
	safeEvent |= EMUEVENT_PAUSE;
	paused = PauseStatePauseRequested;
	pthread_mutex_unlock(&eventMutex);
	
	pthread_cond_broadcast(&eventCond);
	
	return NO;
}

+ (BOOL)pauseSafe
{
	if ((paused == PauseStateIsPaused) || ![EmuThread active])
		return YES;
	
	[EmuThread pause];
	while ([EmuThread pausedState] != PauseStateIsPaused)
		[NSThread sleepUntilDate:[[NSDate date] dateByAddingTimeInterval:0.05]];
	
	return NO;
}

+ (void)pauseSafeWithBlock:(void (^)(BOOL))theBlock
{
	dispatch_async(dispatch_get_global_queue(0, 0), ^{
		BOOL wasPaused = [self pauseSafe];
		dispatch_async(dispatch_get_main_queue(), ^{theBlock(wasPaused);});
	});
}

+ (void)resume
{
	if (paused == PauseStateIsNotPaused || ![EmuThread active])
		return;
	
	pthread_mutex_lock(&eventMutex);
	
	safeEvent &= ~EMUEVENT_PAUSE;
	paused = PauseStateIsNotPaused;
	pthread_mutex_unlock(&eventMutex);
	
	pthread_cond_broadcast(&eventCond);
}

+ (void)reset
{
	pthread_mutex_lock(&eventMutex);
	safeEvent = EMUEVENT_RESET;
	pthread_mutex_unlock(&eventMutex);
	
	pthread_cond_broadcast(&eventCond);
}

// must only be called from within the emulation thread!!!
+ (void)resetNow
{
	/* signify that the emulation has stopped */
	emuThread = nil;
	
	ClosePlugins();
	
	// start a new emulation thread
	[EmuThread run];
	
	//[[NSThread currentThread] autorelease];
	[NSThread exit];
	return;
}

+ (EmuThreadPauseStatus)pausedState
{
	return paused;
}

+ (BOOL)isPaused
{
	return paused != PauseStateIsNotPaused;
}

+ (BOOL)isRunBios
{
	return runbios;
}

+ (BOOL)active
{
	return emuThread ? YES : NO;
}

+ (void)freezeAt:(NSString *)path which:(int)num
{
	[self pauseSafeWithBlock:^(BOOL emuWasPaused) {
		int tmpNum = num;
		char Text[256];
		
		GPU_freeze(2, (GPUFreeze_t *)&tmpNum);
		int ret = SaveState([path fileSystemRepresentation]);
		
		if (!emuWasPaused) {
			[EmuThread resume];
		}
		
		if (ret == 0)
			snprintf(Text, sizeof(Text), _("*PCSXR*: Saved State %d"), num);
		else
			snprintf(Text, sizeof(Text), _("*PCSXR*: Error Saving State %d"), num);
		GPU_displayText(Text);
	}];
}

+ (BOOL)defrostAt:(NSString *)path
{
	const char *cPath = [path fileSystemRepresentation];
	if (CheckState(cPath) != 0)
		return NO;

	defrostPath = path;
	[EmuThread reset];

	GPU_displayText(_("*PCSXR*: Loaded State"));
	return YES;
}

@end
