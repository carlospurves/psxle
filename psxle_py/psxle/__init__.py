# This file lets us communicate with the emulator on a low level
# and is the closest-to-the-metal Python in the project
import subprocess
import itertools
import os
import time
import errno
import sysv_ipc
import numpy as np
import threading
import sys
import stat
import tempfile
import struct
from PIL import Image

HOMEOVERRIDE = None #"/local/scratch/cp614/.psxpy"

def interact():
    import code
    code.InteractiveConsole(locals=globals()).interact()

class Display:
    NONE = 2
    FAST = 0
    NORMAL = 1

class Control:
    START = 3
    UP = 4
    RIGHT = 5
    DOWN = 6
    LEFT = 7
    TRIANGLE = 12
    CIRCLE = 13
    CROSS = 14
    SQUARE = 15

class Console:
    count = 0
    sharedMemoryTimeout = 3

    lastInstance = None

    def debugPrint(self, *arg):
        if self.debug:
            print(arg)

    def parseInt(self,b):
        return int.from_bytes(b, byteorder='little')

    def __init__(self, playing, start=None, gui=False, display=Display.NORMAL, debug=False):
        self.debug = debug
        self.control = False
        self.running = False
        self.playing = os.path.abspath(playing)
        self.gui = gui
        self.unique = Console.count
        Console.lastInstance = self
        Console.count += 1
        self.pid = None
        self.memoryInterest = []
        self.memoryScanRange = None
        self.listenerFile = None
        self.paused = False
        self.display = display
        self.isRecordingAudio = False
        self.isListeningToAudio = False
        self.recordingFile = None
        self.StateLoadCB = None
        self.StateSaveCB = None
        self.AudioFinishedRecordingCB = None
        self.playState = start
        self.status = [0 for _ in range(13)]
        self.statusMethods = {}
        self.createHomeConfig()
        self.sharedCommMemory = None

    def awaitNotification(self,cond, timeout=10, start=None):
        timestart = time.time()
        timeend = timestart + timeout
        rec = self.status[cond] if start is None else start
        while rec >= self.status[cond] and time.time() < timeend:
            time.sleep(0.1)
        if time.time() - timestart > 5:
            print("\n\n\nWARNING!!!! ", time.time()-timestart, "s    waiting for", cond," \n\n\n")
        return rec < self.status[cond]

    def flushAndAwait(self, cond, pipe):
        rec = self.status[cond]
        #print("Flushing...")
        pipe.flush()
        return self.awaitNotification(cond, start=rec)


    def attachControlPipe(self,pipename):
        try:
            os.mkfifo(pipename)
        except OSError as oe:
            if oe.errno != errno.EEXIST:
                raise

        self.debugPrint("Attatching Control Pipe %s ..."%pipename)
        pipe = open(pipename, "wb")
        self.debugPrint("Control Pipe %s successfully attatched..."%pipename)
        return pipe

    def attachCBPipe(self, pipename, timeout=10):
        timelimit = time.time() + timeout
        pipefound = False
        while time.time() < timelimit:
            if os.path.exists(pipename):
                pipefound = True
                break
        if not pipefound:
            self.debugPrint("Callback Pipe attach timeout: %s ..."%pipename)
            return None
        self.debugPrint("Attatching Callback Pipe %s ..."%pipename)
        pipe = open(pipename, "rb")
        self.debugPrint("Callback Pipe %s successfully attatched..."%pipename)
        return pipe

    def registerMemoryInterest(self, interest):
        interest.sort(key=lambda x: x[0][0])
        self.memoryInterest = interest
        startrange = interest[0][0][0]
        endranges = [x[0][1] for x in interest]
        endrange = max(endranges)
        self.memoryScanRange = (startrange, endrange)

    def addMemoryListener(self, start, lgth, onChange, silenceByDefault=False, newValuesOnly=True, allowBuffer=True):
        if self.running:
            print("Must register memory listeners before running an instance.")
            return
        key = len(self.memoryInterest)
        self.memoryInterest.append((key,start,lgth,onChange,silenceByDefault,newValuesOnly,allowBuffer))
        return key

    def sleepMemoryListener(self, tarkey):
        if self.running:
            self.procControlPipe.write(bytes([24]))
            self.procControlPipe.write(bytes([tarkey]))
            self.procControlPipe.flush()
        for i in range(len(self.memoryInterest)):
            if self.memoryInterest[i][0] == tarkey:
                self.memoryInterest[i][4] == True


    def wakeMemoryListener(self, tarkey):
        if self.running:
            self.procControlPipe.write(bytes([25]))
            self.procControlPipe.write(bytes([tarkey]))
            self.procControlPipe.flush()
        for i in range(len(self.memoryInterest)):
            if self.memoryInterest[i][0] == tarkey:
                self.memoryInterest[i][4] == False

    def setCallbackReceiver(self, i, fn):
        statusMethods[i] = fn

    def clearMemoryListeners(self):
        self.memoryInterest = []

    def createHomeConfig(self):
        self.CONFIGHOME = os.path.expanduser("~/.psxle") if not HOMEOVERRIDE else HOMEOVERRIDE
        if not os.path.exists(self.CONFIGHOME):
            os.mkdir(self.CONFIGHOME)
            os.mkdir(self.CONFIGHOME+"/isos")
        if not os.path.exists(self.CONFIGHOME+"/states"):
            os.mkdir(self.CONFIGHOME+"/states")

    def getStateName(self, name):
        if not os.path.exists(self.CONFIGHOME+"/states/"+self.playing):
            os.mkdir(self.CONFIGHOME+"/states/"+self.playing)
        return self.CONFIGHOME+"/states/"+self.playing+"/"+name+".state"

    def saveState(self, name, callback=None):
        if not self.running:
            return False
        if not name.isalnum():
            return False
        if self.paused:
            self.resume(block=True)
        path = self.getStateName(name)
        self.onStateSave(callback)
        self.procControlPipe.write(bytes([41, len(path)]))
        self.procControlPipe.write(path.encode("ascii"))
        self.procControlPipe.flush()

    def loadState(self, name, callback=None, block=False):
        if not name.isalnum():
            return False
        self.playState = name
        if self.running:
            if self.paused:
                self.resume(block=True)
            path = self.getStateName(name)
            if not os.path.exists(path):
                return False
            self.onStateLoad(callback)
            self.procControlPipe.write(bytes([42, len(path)]))
            self.procControlPipe.write(path.encode("ascii"))
            if block:
                return self.flushAndAwait(2, self.procControlPipe)
            else:
                self.procControlPipe.flush()
                return True


    def pause(self, block=False):
        if self.running:
            if self.paused:
                return True
            self.debugPrint("Pausing...")
            self.procControlPipe.write(bytes([2]))
            if block:
                success = self.flushAndAwait(7, self.procControlPipe)
                self.paused = success
                if not success:
                    self.debugPrint("Failed to pause!")
                return success
            else:
                self.paused = True
                self.procControlPipe.flush()
                return True
        else:
            print("Cannot pause emulator that is not running.")

    def resume(self, block=False):
        if self.running:
            if not self.paused:
                return True
            self.procControlPipe.write(bytes([3]))
            self.debugPrint("Resuming...")

            if block:
                success = self.flushAndAwait(8, self.procControlPipe)
                self.paused = not success
                if not success:
                    self.debugPrint("Failed to resume!")
                return success
            else:
                self.procControlPipe.flush()
                self.paused = False
                return True

    def run(self):
        if self.running:
            print("Console already running - Console.exit() to kill.")
            return
        if self.display == Display.FAST:
            args = ["xvfb-run", "-a", "-s", "-screen 0 1400x900x24"]
        elif self.display == Display.NONE:
            args = ["xvfb-run", "-a", "-s", "-screen 0 1400x900x24"]
        else:
            args = []
        args.append(self.CONFIGHOME+"/psxle")
        print(args)
        args.append("-cfg")
        args.append(self.CONFIGHOME+"/default.cfg")

        if self.gui:
            args.append("-gui")
            args.append("-controlPipe")
            args.append("none")
        elif self.playing is not None:
            args.append("-display")
            args.append(str(self.display))# if self.display != Display.FAST else Display.NORMAL))
            args.append("-controlPipe")
            pipeName = "{}/ml_psxemu{}".format(os.environ['TMPDIR'] if 'TMPDIR' in os.environ else "/tmp",self.unique)
            controlPipeName = pipeName+"-joy"
            proceedurePipeName = pipeName+"-proc"
            memPipeName = pipeName+"-mem"
            args.append(pipeName)
            args.append("-nMemoryListeners")
            args.append(str(len(self.memoryInterest)))
            if self.playState:
                playstatepath = self.getStateName(self.playState)
                if os.path.exists(playstatepath):
                    args.append("-loadState")
                    args.append(playstatepath)
            if os.path.isfile(self.playing):
                args.append("-play")
                args.append(self.playing)
                self.debugPrint("Playing ISO: {}".format(self.playing))
            else:
                self.debugPrint("The game ISO '{}' is not available.".format(self.playing))

        else:
            print("No Execution specified.")
            return

        self.listenerFile = tempfile.NamedTemporaryFile()

        for k in self.memoryInterest:
            keyout = (0b10000000 if k[4] else 0) | k[0]
            self.debugPrint("Sending",keyout)
            self.listenerFile.write(k[1].to_bytes(4, byteorder=sys.byteorder))
            self.listenerFile.write(k[2].to_bytes(4, byteorder=sys.byteorder))
            self.listenerFile.write(bytes([keyout,1 if k[5] else 0,1 if k[6] else 0,0]))

        self.listenerFile.seek(0)
        if self.debug:
            sub = subprocess.Popen(args, stdin=self.listenerFile)
        else:
            sub = subprocess.Popen(args, stdin=self.listenerFile)#, stdout=subprocess.PIPE)

        self.proc = sub
        self.pid = sub.pid
        self.running = True
        self.memCallbackPipe = self.attachCBPipe(memPipeName)
        self.joyControlPipe = self.attachControlPipe(controlPipeName)
        self.procControlPipe = self.attachControlPipe(proceedurePipeName)
        self.reversePipeThread = IPCThread(self)
        self.reversePipeThread.start()
        self.control = True

    def listControllerCount(self):
        return 2;

    def listOperationCount(self):
        return 16;

    def holdButton(self, button, controller=0):
        if not(self.control):
            # Ignore erroneous button presses
            print("Error, controller not initialised.")
            return
        elif button >= self.listOperationCount() or controller >= self.listControllerCount():
            print("Error, control function out of range (%i, %i)."%(button,controller))
            return
        package = (controller << 6 | button) | 32
        self.joyControlPipe.write(bytes([package]))
        self.joyControlPipe.flush()

    def addFixedControlSpacer(self, delay):
        # DELAY IS IN UNITS OF ms
        if not(self.control):
            # Ignore erroneous button presses
            print("Error, controller not initialised.")
            return
        if delay < 10:
            print("ERROR: delay too small, min 10")
            return
        if delay > 1270:
            print("ERROR: delay too large, max 1270")
            return
        delay = int(delay/10)
        package = delay | 128
        self.joyControlPipe.write(bytes([package]))
        self.joyControlPipe.flush()


    def releaseButton(self, button, controller=0):
        if not self.control:
            # Ignore erroneous button presses
            print("Error, controller not initialised.")
            return
        elif button >= self.listOperationCount() or controller >= self.listControllerCount():
            print("Error, control function out of range (%i, %i)."%(button,controller))
            return
        package = (controller << 6 | button)
        self.joyControlPipe.write(bytes([package]))
        self.joyControlPipe.flush()

    def touchButton(self, button, controller=0, sleepfor=0.2):
        self.holdButton(button, controller)
        time.sleep(sleepfor)
        self.releaseButton(button, controller)


    def onStateLoad(self, f):
        self.StateLoadCB = f
    def onStateSave(self, f):
        self.StateSaveCB = f
    def onAudioFinishedRecording(self, f):
        self.AudioFinishedRecordingCB = f

    def setSpeed(self, val, block=False):
        if not self.running:
            print("Not running - speed can only be set for running consoles.")
            return None
        self.procControlPipe.write(bytes([43]))
        self.procControlPipe.write((val).to_bytes(4, byteorder='big'))
        if block:
            return self.flushAndAwait(10, self.procControlPipe)
        else:
            self.procControlPipe.flush()
            return True

    def exit(self):
        if self.running:
            if self.paused:
                self.resume(block=True)
            self.cleanSharedMemory()
            self.procControlPipe.write(bytes([1]))
            self.procControlPipe.flush()
            self.reversePipeThread.stop()
            self.reversePipeThread.join()
            self.memCallbackPipe.close()
            self.procControlPipe.close()
            self.joyControlPipe.close()
            if self.listenerFile:
                self.listenerFile.close()
            self.running = False

    def startAudioRecording(self):
        if self.isRecordingAudio:
            print("Audio already recording.")
            return False
        self.debugPrint("Recoding audio...")
        self.recordingFile = tempfile.NamedTemporaryFile()
        path = self.recordingFile.name
        self.procControlPipe.write(bytes([31, len(path)]))
        self.procControlPipe.write(path.encode("ascii"))
        self.procControlPipe.flush()
        self.isRecordingAudio = True
        self.isListeningToAudio = True
        return True

    def stopAudioRecording(self, discard=False):
        if not self.isRecordingAudio:
            print("No audio recording.")
            return None
        self.procControlPipe.write(bytes([32]))
        self.procControlPipe.flush()
        out_ints = []
        if not discard:
            print("Audio conversion taking place...")
            outcome = self.recordingFile.read()
            if len(outcome)%2 != 0:
                print("There was a _fatal_ audio error")
            else:
                for i in range(0, len(outcome), 2):
                    val = struct.unpack( "h", outcome[i:i+2])
                    out_ints.append(val)
        self.recordingFile.close()
        print("Audio conversion done.")
        self.isRecordingAudio = False
        self.isListeningToAudio = False
        if len(out_ints) > 0:
            i = len(out_ints)-1
            while out_ints[i][0] == 0 and i > 0:
                i -= 1
            return np.array(out_ints[:i+1])
        else:
            return None

    def cleanSharedMemory(self):
        self.procControlPipe.write(bytes([12,23]))
        self.procControlPipe.flush()

    def readMemory(self, start, length):
        if not self.running:
            print("Not running - memory can only be accessed from running consoles.")
            return None
        #print("M:", start)
        self.procControlPipe.write(bytes([21]))
        self.procControlPipe.write((start).to_bytes(4, byteorder='big'))
        self.procControlPipe.write((length).to_bytes(4, byteorder='big'))
        self.procControlPipe.write(bytes([(self.unique+5)*2+1]))
        #print("Writen:", start)

        if not self.flushAndAwait(11, self.procControlPipe):
            print("Shared memory timeout..")
            return None

        if self.sharedCommMemory is None:
            startTime = time.time()
            while True:
                try:
                    self.sharedCommMemory = sysv_ipc.SharedMemory((self.unique+5)*2+1)
                except sysv_ipc.ExistentialError:
                    if time.time()-startTime < Console.sharedMemoryTimeout:
                        continue
                    else:
                        print("Shared memory timeout")
                        return None
                except Exception as ex:
                    print("Shared memory error ",type(ex))
                    return
                break

        startTime = time.time()

        try:
            output = self.sharedCommMemory.read(length)
        except:
            output = None
            print("Shared memory failed.")
        return output


    def dumpMemoryToFile(self, start, length, path, block=True):
        if not self.running:
            print("Not running - memory can only be accessed from running consoles.")
            return None
        self.procControlPipe.write(bytes([22]))
        self.procControlPipe.write((start).to_bytes(4, byteorder='big'))
        self.procControlPipe.write((length).to_bytes(4, byteorder='big'))
        self.procControlPipe.write(bytes([len(path)]))
        self.procControlPipe.write(path.encode("ascii"))
        if block:
            return self.flushAndAwait(4, self.procControlPipe)
        else:
            self.procControlPipe.flush()
            return True


    def writeByte(self, start, val, block=True):
        if not self.running:
            print("Not running - memory can only be written from running consoles.")
            return None
        self.procControlPipe.write(bytes([26]))
        self.procControlPipe.write((start).to_bytes(4, byteorder='big'))
        self.procControlPipe.write(bytes([val]))
        if block:
            return self.flushAndAwait(5, self.procControlPipe)
        else:
            self.procControlPipe.flush()
            return True


    def drill(self, start, val, times, block=True):
        if not self.running:
            print("Not running - memory can only be drilled from running consoles.")
            return None
        self.procControlPipe.write(bytes([27]))
        self.procControlPipe.write((start).to_bytes(4, byteorder='big'))
        self.procControlPipe.write(bytes([val,times]))
        if block:
            return self.flushAndAwait(6, self.procControlPipe)
        else:
            self.procControlPipe.flush()
            return True

    def snapshot(self):
        self.procControlPipe.write(bytes([11]))
        self.procControlPipe.write(bytes([(self.unique+5)*2]))
        self.procControlPipe.flush()

        startTime = time.time()
        while True:
            try:
                memory = sysv_ipc.SharedMemory((self.unique+5)*2)
            except sysv_ipc.ExistentialError:
                if time.time()-startTime < Console.sharedMemoryTimeout:
                    continue
                else:
                    self.debugPrint("Shared memory timeout!")
                    return self.snapshot()
            except Exception as ex:
                self.debugPrint("Shared memory error ",type(ex))
                return
            break

        screen_data = np.empty((480,640,3), dtype=np.uint8)

        for i in range(640*480):
            rgb = memory.read(3, i*3)
            if len(rgb) == 3:
                screen_data[479-int(i/640), i%640, 0] = rgb[0]
                screen_data[479-int(i/640), i%640, 1] = rgb[1]
                screen_data[479-int(i/640), i%640, 2] = rgb[2]
        return screen_data

    def snapshotToFile(self, path):
        data = self.snapshot()
        img = Image.fromarray(data, 'RGB')
        img.save(path)


class IPCThread (threading.Thread):
    def __init__(self, owner):
        self.owner = owner
        threading.Thread.__init__(self)
        self.killed = False
    def run(self):
        while not self.killed:
            try:
                next = self.owner.memCallbackPipe.read(1)
            except Exception as e:
                if e is ValueError:
                    break
                else:
                    raise
            if len(next) == 0:
                continue
            if next[0] != 0:
                self.owner.debugPrint(("Received notif ", next[0]))
                self.owner.status[next[0]] = time.time()
                if next[0] == 2:
                    if self.owner.StateLoadCB:
                        self.owner.StateLoadCB()
                elif next[0] == 3:
                    if self.owner.StateSaveCB:
                        self.owner.StateSaveCB()
                elif next[0] == 9:
                    self.owner.isListeningToAudio = False
                    if self.owner.AudioFinishedRecordingCB:
                        self.owner.AudioFinishedRecordingCB()

                continue
            key = self.owner.memCallbackPipe.read(1)[0]
            profile = None
            memoryvalue = None
            for g in self.owner.memoryInterest:
                if g[0] == key:
                    profile = g
                    break
            if profile is None:
                continue
            bytecount = profile[2]
            memoryvalue = self.owner.memCallbackPipe.read(bytecount)
            profile[3](memoryvalue)

        self.owner.debugPrint("Stopped memory speaker.")

    def stop(self):
        self.owner.debugPrint("Stopping memory speaker...")
        self.killed = True

# Testing with:
# x.registerMemoryInterest([((700526,700527),lambda was,now: print("Changed:",int.from_bytes(was,'big'),"->",int.from_bytes(now,'big')))])


if __name__ == '__main__':
    x = Console("kula")
    x.addMemoryListener(671364, 4, lambda x: print(x))
    x.run()
    time.sleep(8)
    x.touchButton(Control.CROSS)
    time.sleep(3)
    x.touchButton(Control.CROSS)
    time.sleep(4)
    x.touchButton(Control.CROSS)
    time.sleep(3)
    x.exit()
