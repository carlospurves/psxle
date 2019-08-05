//
//  EmuThread.h
//  Pcsxr
//
//  Created by Gil Pedersen on Sun Sep 21 2003.
//  Copyright (c) 2003 __MyCompanyName__. All rights reserved.
//

#import <Foundation/Foundation.h>
#include <setjmp.h>

typedef NS_ENUM(char, EmuThreadPauseStatus) {
	PauseStateIsNotPaused = 0,
	PauseStatePauseRequested,
	PauseStateIsPaused
};

@interface EmuThread : NSObject

- (void)EmuThreadRun:(id)anObject;
- (void)EmuThreadRunBios:(id)anObject;
- (void)handleEvents;

+ (void)run;
+ (void)runBios;
+ (void)stop;
+ (BOOL)pause;
+ (BOOL)pauseSafe;
+ (void)pauseSafeWithBlock:(void (^)(BOOL))theBlock;
+ (void)resume;
+ (void)resetNow;
+ (void)reset;

+ (BOOL)isPaused;
+ (EmuThreadPauseStatus)pausedState;
+ (BOOL)active;
+ (BOOL)isRunBios;

+ (void)freezeAt:(NSString *)path which:(int)num;
+ (BOOL)defrostAt:(NSString *)path;

@end

extern EmuThread *emuThread;
