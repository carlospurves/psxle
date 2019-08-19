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


class SharedPSXCallbackManager():
    def __init__(self):
        self.arrival_times = {}

    def get_last(self, cond):
        return self.arrival_times.get(cond, 0)
    
    def unchanged(self, cond, value):
        return self.get_last(cond) <= value

    def update(self, cond, value):
        self.arrival_times[cond] = value
    


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
    cfg_path = os.path.expanduser("~/.psxle")

    lastInstance = None

    @staticmethod
    def int32(b):
        return int.from_bytes(b, byteorder='little')

    def log(self, *arg):
        if self.debug:
            print(arg)

    def error(self, *args):
        print(*args, file=sys.stderr)

    def __init__(self, playing, start=None, gui=False, display=Display.NORMAL, debug=False):
        self.debug = debug
        self.control = False
        self.running = False
        self.playing = os.path.abspath(playing)
        self.gui = gui
        self.unique = Console.count
        Console.lastInstance = self
        Console.count += 1

        self._memory_listener_list = []
        self._memory_listener_coverage = None
        self._memory_listeners = None
        
        self.paused = False
        self.display = display

        self._fps = 100

        self.is_recording_audio = False
        
        self.recordingFile = None
        self.game_state = start
        self.cb_handler = SharedPSXCallbackManager()
        self.statusMethods = {}
        self.sharedCommMemory = None

        # Callbacks:
        self._on_state_load = None
        self._on_state_save = None
        self._on_audio_finish_record = None

    def await_cb_notification(self,cond, timeout=10, start=None):
        start_time = time.time()
        max_time = start_time + timeout
        rec = self.cb_handler.get_last(cond) if start is None else start
        # While we haven't seen changes to the timestamp of the condition
        while self.cb_handler.unchanged(cond, rec) and time.time() < max_time:
            # Wait for 0.1 seconds
            time.sleep(0.1)
        if self.cb_handler.unchanged(cond, rec):
            self.error("Failed while waiting for {} (took: {})".format(int(time.time())-start_time))
        return not self.cb_handler.unchanged(cond, rec)

    def flush_then_wait(self, cond, pipe):
        rec = self.cb_handler.get_last(cond)
        pipe.flush()
        return self.await_cb_notification(cond, start=rec)


    def attach_control_pipe(self,pipename):
        try:
            os.mkfifo(pipename)
        except OSError as oe:
            if oe.errno != errno.EEXIST:
                raise

        self.log("Attatching Control Pipe %s ..."%pipename)
        pipe = open(pipename, "wb")
        self.log("Control Pipe %s successfully attatched..."%pipename)
        return pipe

    def attach_cb_pipe(self, pipename, timeout=10):
        timelimit = time.time() + timeout
        pipefound = False
        while time.time() < timelimit:
            if os.path.exists(pipename):
                pipefound = True
                break
        if not pipefound:
            self.log("Callback Pipe attach timeout: %s ..."%pipename)
            return None
        self.log("Attatching Callback Pipe %s ..."%pipename)
        pipe = open(pipename, "rb")
        self.log("Callback Pipe %s successfully attatched..."%pipename)
        return pipe

    def register_memory_interest(self, interest):
        interest.sort(key=lambda x: x[0][0])
        self._memory_listener_list = interest
        startrange = interest[0][0][0]
        endranges = [x[0][1] for x in interest]
        endrange = max(endranges)
        self._memory_listener_coverage = (startrange, endrange)

    def add_memory_listener(self, start, lgth, onChange, silenceByDefault=False, newValuesOnly=True, allowBuffer=True):
        if self.running:
            print("Must register memory listeners before running an instance.")
            return
        key = len(self._memory_listener_list)
        self._memory_listener_list.append((key,start,lgth,onChange,silenceByDefault,newValuesOnly,allowBuffer))
        return key

    def sleep_memory_listener(self, tarkey):
        if self.running:
            self.proc_pipe.write(bytes([24]))
            self.proc_pipe.write(bytes([tarkey]))
            self.proc_pipe.flush()
        for i in range(len(self._memory_listener_list)):
            if self._memory_listener_list[i][0] == tarkey:
                self._memory_listener_list[i][4] == True

    def wake_memory_listener(self, tarkey):
        if self.running:
            self.proc_pipe.write(bytes([25]))
            self.proc_pipe.write(bytes([tarkey]))
            self.proc_pipe.flush()
        for i in range(len(self._memory_listener_list)):
            if self._memory_listener_list[i][0] == tarkey:
                self._memory_listener_list[i][4] == False

    def clear_memory_listeners(self):
        self._memory_listener_list = []

    def get_state_path(self, name):
        states_path = os.path.join(Console.cfg_path, "states", os.path.basename(self.playing))
        if not os.path.isdir(states_path):
            os.mkdir(states_path)
        state_name = "{}.state".format(name)
        return os.path.join(states_path, state_name)

    def save_game_state(self, name, callback=None):
        if not self.running:
            return False
        if not name.isalnum():
            return False
        if self.paused:
            self.resume(block=True)
        path = self.get_state_path(name)
        self.on_state_save(callback)
        self.proc_pipe.write(bytes([41, len(path)]))
        self.proc_pipe.write(path.encode("ascii"))
        self.proc_pipe.flush()

    def load_game_state(self, name, callback=None, block=False):
        if not name.isalnum():
            return False
        self.game_state = name
        if self.running:
            if self.paused:
                self.resume(block=True)
            path = self.get_state_path(name)
            if not os.path.exists(path):
                return False
            self.on_state_load(callback)
            self.proc_pipe.write(bytes([42, len(path)]))
            self.proc_pipe.write(path.encode("ascii"))
            if block:
                return self.flush_then_wait(2, self.proc_pipe)
            else:
                self.proc_pipe.flush()
                return True


    def pause(self, block=False):
        if self.running:
            if self.paused:
                return True
            self.log("Pausing...")
            self.proc_pipe.write(bytes([2]))
            if block:
                success = self.flush_then_wait(7, self.proc_pipe)
                self.paused = success
                if not success:
                    self.log("Failed to pause!")
                return success
            else:
                self.paused = True
                self.proc_pipe.flush()
                return True
        else:
            print("Cannot pause emulator that is not running.")

    def resume(self, block=False):
        if not self.running:
            return
        if not self.paused:
            return True
        self.proc_pipe.write(bytes([3]))
        self.log("Resuming...")

        if block:
            success = self.flush_then_wait(8, self.proc_pipe)
            self.paused = not success
            if not success:
                self.log("Failed to resume!")
            return success
        else:
            self.proc_pipe.flush()
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
        args.append(Console.cfg_path+"/psxle")
        print(args)
        args.append("-cfg")
        args.append(Console.cfg_path+"/default.cfg")

        if self.gui:
            args.append("-gui")
            args.append("-controlPipe")
            args.append("none")
        else:
            args.append("-display")
            args.append(str(self.display))
            args.append("-controlPipe")
            pipeName = "{}/ml_psxemu{}".format(os.environ['TMPDIR'] if 'TMPDIR' in os.environ else "/tmp",self.unique)
            controlPipeName = pipeName+"-joy"
            proceedurePipeName = pipeName+"-proc"
            memPipeName = pipeName+"-mem"
            args.append(pipeName)
            args.append("-nMemoryListeners")
            args.append(str(len(self._memory_listener_list)))
            if self.game_state:
                playstatepath = self.get_state_path(self.game_state)
                if os.path.exists(playstatepath):
                    args.append("-loadState")
                    args.append(playstatepath)
            if os.path.isfile(self.playing):
                args.append("-play")
                args.append(self.playing)
                self.log("Playing ISO: {}".format(self.playing))
            else:
                self.log("The game ISO '{}' is not available.".format(self.playing))

        self._memory_listeners = tempfile.NamedTemporaryFile()

        for k in self._memory_listener_list:
            keyout = (0b10000000 if k[4] else 0) | k[0]
            self.log("Sending",keyout)
            self._memory_listeners.write(k[1].to_bytes(4, byteorder=sys.byteorder))
            self._memory_listeners.write(k[2].to_bytes(4, byteorder=sys.byteorder))
            self._memory_listeners.write(bytes([keyout,1 if k[5] else 0,1 if k[6] else 0,0]))

        self._memory_listeners.seek(0)
        if self.debug:
            sub = subprocess.Popen(args, stdin=self._memory_listeners)
        else:
            sub = subprocess.Popen(args, stdin=self._memory_listeners, stdout=subprocess.PIPE)

        self.proc = sub
        self.running = True
        self.memory_cb_pipe = self.attach_cb_pipe(memPipeName)
        self.control_emu_pipe = self.attach_control_pipe(controlPipeName)
        self.proc_pipe = self.attach_control_pipe(proceedurePipeName)
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
        self.control_emu_pipe.write(bytes([package]))
        self.control_emu_pipe.flush()

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
        self.control_emu_pipe.write(bytes([package]))
        self.control_emu_pipe.flush()


    def releaseButton(self, button, controller=0):
        if not self.control:
            # Ignore erroneous button presses
            print("Error, controller not initialised.")
            return
        elif button >= self.listOperationCount() or controller >= self.listControllerCount():
            print("Error, control function out of range (%i, %i)."%(button,controller))
            return
        package = (controller << 6 | button)
        self.control_emu_pipe.write(bytes([package]))
        self.control_emu_pipe.flush()

    def touchButton(self, button, controller=0, sleepfor=0.2):
        self.holdButton(button, controller)
        time.sleep(sleepfor)
        self.releaseButton(button, controller)


    def on_state_load(self, f):
        self._on_state_load = f
    def on_state_save(self, f):
        self._on_state_save = f
    def on_audio_recording_stopped(self, f):
        self._on_audio_finish_record = f

    @property
    def fps(self):
        return self._fps

    @fps.setter
    def fps(self, val, block=False):
        self._fps = val
        if not self.running:
            print("Not running - speed can only be set for running consoles.")
            return None
        self.proc_pipe.write(bytes([43]))
        self.proc_pipe.write((val).to_bytes(4, byteorder='big'))
        if block:
            return self.flush_then_wait(10, self.proc_pipe)
        else:
            self.proc_pipe.flush()
            return True

    def exit(self):
        if self.running:
            if self.paused:
                self.resume(block=True)
            self.cleanSharedMemory()
            self.proc_pipe.write(bytes([1]))
            self.proc_pipe.flush()
            self.reversePipeThread.stop()
            self.reversePipeThread.join()
            self.memory_cb_pipe.close()
            self.proc_pipe.close()
            self.control_emu_pipe.close()
            if self._memory_listeners:
                self._memory_listeners.close()
            self.running = False

    def startAudioRecording(self):
        if self.is_recording_audio:
            print("Audio already recording.")
            return False
        self.log("Recoding audio...")
        self.recordingFile = tempfile.NamedTemporaryFile()
        path = self.recordingFile.name
        self.proc_pipe.write(bytes([31, len(path)]))
        self.proc_pipe.write(path.encode("ascii"))
        self.proc_pipe.flush()
        self.is_recording_audio = True
        return True

    def stopAudioRecording(self, discard=False):
        if not self.is_recording_audio:
            print("No audio recording.")
            return None
        self.proc_pipe.write(bytes([32]))
        self.proc_pipe.flush()
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
        self.is_recording_audio = False
        if len(out_ints) > 0:
            i = len(out_ints)-1
            while out_ints[i][0] == 0 and i > 0:
                i -= 1
            return np.array(out_ints[:i+1])
        else:
            return None

    def cleanSharedMemory(self):
        self.proc_pipe.write(bytes([12,23]))
        self.proc_pipe.flush()

    def readMemory(self, start, length):
        if not self.running:
            print("Not running - memory can only be accessed from running consoles.")
            return None
        self.proc_pipe.write(bytes([21]))
        self.proc_pipe.write((start).to_bytes(4, byteorder='big'))
        self.proc_pipe.write((length).to_bytes(4, byteorder='big'))
        self.proc_pipe.write(bytes([(self.unique+5)*2+1]))

        if not self.flush_then_wait(11, self.proc_pipe):
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
        self.proc_pipe.write(bytes([22]))
        self.proc_pipe.write((start).to_bytes(4, byteorder='big'))
        self.proc_pipe.write((length).to_bytes(4, byteorder='big'))
        self.proc_pipe.write(bytes([len(path)]))
        self.proc_pipe.write(path.encode("ascii"))
        if block:
            return self.flush_then_wait(4, self.proc_pipe)
        else:
            self.proc_pipe.flush()
            return True


    def writeByte(self, start, val, block=True):
        if not self.running:
            print("Not running - memory can only be written from running consoles.")
            return None
        self.proc_pipe.write(bytes([26]))
        self.proc_pipe.write((start).to_bytes(4, byteorder='big'))
        self.proc_pipe.write(bytes([val]))
        if block:
            return self.flush_then_wait(5, self.proc_pipe)
        else:
            self.proc_pipe.flush()
            return True

    def snapshot(self):
        self.proc_pipe.write(bytes([11]))
        self.proc_pipe.write(bytes([(self.unique+5)*2]))
        self.proc_pipe.flush()

        startTime = time.time()
        while True:
            try:
                memory = sysv_ipc.SharedMemory((self.unique+5)*2)
            except sysv_ipc.ExistentialError:
                if time.time()-startTime < Console.sharedMemoryTimeout:
                    continue
                else:
                    self.log("Shared memory timeout!")
                    return self.snapshot()
            except Exception as ex:
                self.log("Shared memory error ",type(ex))
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

    def snapshot_to_file(self, path):
        data = self.snapshot()
        img = Image.fromarray(data, 'RGB')
        img.save(path)


class IPCThread (threading.Thread):
    def __init__(self, owner):
        self.owner = owner
        threading.Thread.__init__(self)
        self.killed = False
    def run(self):
        self.owner.log("Started memory callback listener...")
        while not self.killed:
            try:
                next = self.owner.memory_cb_pipe.read(1)
            except Exception as e:
                if e is ValueError:
                    break
                else:
                    raise
            if len(next) == 0:
                continue
            if next[0] != 0:
                self.owner.log("Received notification with id: {}".format(next[0]))
                self.owner.cb_handler.update(next[0])
                if next[0] == 2:
                    if self.owner._on_state_load:
                        self.owner._on_state_load()
                elif next[0] == 3:
                    if self.owner._on_state_save:
                        self.owner._on_state_save()
                elif next[0] == 9:
                    self.owner.is_recording_audio = False
                    if self.owner._on_audio_finish_record:
                        self.owner._on_audio_finish_record()
                continue

            key = self.owner.memory_cb_pipe.read(1)[0]
            profile = None
            memoryvalue = None
            for g in self.owner.memoryInterest:
                if g[0] == key:
                    profile = g
                    break
            if profile is None:
                continue
            bytecount = profile[2]
            memoryvalue = self.owner.memory_cb_pipe.read(bytecount)
            profile[3](memoryvalue)

        self.owner.log("Stopped memory speaker.")

    def stop(self):
        self.owner.log("Stopping memory speaker...")
        self.killed = True


if __name__ == '__main__':
    x = Console("kula")
    x.add_memory_listener(671364, 4, lambda x: print(x))
    x.run()
    time.sleep(8)
    x.touchButton(Control.CROSS)
    time.sleep(3)
    x.touchButton(Control.CROSS)
    time.sleep(4)
    x.touchButton(Control.CROSS)
    time.sleep(3)
    x.exit()
