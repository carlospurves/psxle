# This file lets us communicate with the emulator on a low level
# and is the closest-to-the-metal Python in the project
import subprocess
import itertools
import os
import time
import cv2
import errno
import sysv_ipc
import numpy as np
import threading
import sys
import stat
import tempfile
import struct
from PIL import Image


class ISONotFoundException(Exception):
    def __init__(self,*args,**kwargs):
        Exception.__init__(self,*args,**kwargs)

class ExecutableNotFoundException(Exception):
    def __init__(self,*args,**kwargs):
        Exception.__init__(self,*args,**kwargs)

class ConfigurationChangeWhileRunningException(Exception):
    def __init__(self,*args,**kwargs):
        Exception.__init__(self,*args,**kwargs)

class MemoryListener():
    def __init__(self, key, start, length, callback, start_silent=False, fresh_only=False, use_buffer=False):
        self.start, self.length, self.callback, self.key = start, length, callback, key
        self.start_silent, self.fresh_only, self.use_buffer = start_silent, fresh_only, use_buffer
        return

class SharedPSXCallbackManager():
    def __init__(self):
        self.arrival_times = {}

    def get_last(self, cond):
        return self.arrival_times.get(cond, 0)

    def unchanged(self, cond, value):
        return self.get_last(cond) <= value

    def update(self, cond):
        self.arrival_times[cond] = time.time()

class Display:
    NONE = 0
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
    OPS_COUNT = 16
    CONTROLLERS = 2

    unique_instance_id = itertools.count()
    shared_memory_timeout = 3
    cfg_path = os.path.expanduser("~/.psxle")

    @staticmethod
    def int32(b):
        return int.from_bytes(b, byteorder='little')

    def log(self, args):
        s = str(" ".join([str(k) for k in list(args)]))
        if self.debug:
            if self.custom_log:
                self.custom_log(s)
            else:
                print(s)


    def error(self, *args):
        print(*args, file=sys.stderr)

    def __init__(self, playing, start=None, gui=False, display=Display.NORMAL, debug=False, custom_log=None):
        self.debug = debug
        self.custom_log = custom_log
        self.control = False
        self.running = False
        self._using_gui = gui
        self._iso = os.path.abspath(playing)

        if not os.path.isfile(self._iso):
            # Console object should not be created with a
            # non-existant iso
            raise ISONotFoundException(self._iso)

        self._unique = next(self.unique_instance_id)

        self._memory_listener_list = []
        self._memory_listener_coverage = None
        self._memory_listeners = None

        self.paused = False
        self.display = display

        self._speed = 100

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

    def _await_cb_notification(self,cond, timeout=10, start=None):
        start_time = time.time()
        max_time = start_time + timeout
        rec = self.cb_handler.get_last(cond) if start is None else start
        # While we haven't seen changes to the timestamp of the condition
        while self.cb_handler.unchanged(cond, rec) and time.time() < max_time:
            # Wait for 0.1 seconds
            time.sleep(0.1)
        if self.cb_handler.unchanged(cond, rec):
            self.error("Failed while waiting for {} (took: {})".format(cond, int(time.time())-start_time))
        return not self.cb_handler.unchanged(cond, rec)

    def _flush_then_wait(self, cond, pipe):
        rec = self.cb_handler.get_last(cond)
        pipe.flush()
        return self._await_cb_notification(cond, start=rec)


    def _attach_control_pipe(self,pipename):
        try:
            os.mkfifo(pipename)
        except OSError as oe:
            if oe.errno != errno.EEXIST:
                raise

        self.log("Attatching Control Pipe %s ..."%pipename)
        pipe = open(pipename, "wb")
        self.log("Control Pipe %s successfully attatched..."%pipename)
        return pipe

    def _attach_cb_pipe(self, pipename, timeout=10):
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

    def _clear_shared_memory(self):
        self.proc_pipe.write(bytes([12,23]))
        self.proc_pipe.flush()


    def _get_state_path(self, name):
        states_path = os.path.join(Console.cfg_path, "states", os.path.basename(self._iso))
        if not os.path.isdir(states_path):
            os.mkdir(states_path)
        state_name = "{}.state".format(name)
        return os.path.join(states_path, state_name)

    # ??
    def register_memory_interest(self, interest):
        interest.sort(key=lambda x: x[0][0])
        self._memory_listener_list = interest
        startrange = interest[0][0][0]
        endranges = [x[0][1] for x in interest]
        endrange = max(endranges)
        self._memory_listener_coverage = (startrange, endrange)
    # ??

    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################

    # Run and exit:

    def run(self):

        if not os.path.isfile(self._iso):
            # ISO must have moved/been deleted
            raise ISONotFoundException(self._iso)

        if self.running:
            # Do not open multiple
            return

        exc = []

        if self.display == Display.NONE:
            exc += ["xvfb-run", "-a", "-s", "-screen 0 1400x900x24"]

        exc.append(Console.cfg_path+"/psxle")
        exc.append("-cfg")
        exc.append(Console.cfg_path+"/default.cfg")

        if self._using_gui:
            exc.append("-gui")
            exc.append("-controlPipe")
            exc.append("none")
        else:
            exc.append("-display")
            exc.append(str(self.display))
            exc.append("-controlPipe")
            pipeName = "{}/ml_psxemu{}".format(os.environ['TMPDIR'] if 'TMPDIR' in os.environ else "/tmp",self._unique)
            controlPipeName = pipeName+"-joy"
            proceedurePipeName = pipeName+"-proc"
            memPipeName = pipeName+"-mem"
            exc.append(pipeName)
            exc.append("-nMemoryListeners")
            exc.append(str(len(self._memory_listener_list)))
            if self.game_state:
                playstatepath = self._get_state_path(self.game_state)
                if os.path.exists(playstatepath):
                    exc.append("-loadState")
                    exc.append(playstatepath)
            if os.path.isfile(self._iso):
                exc.append("-play")
                exc.append(self._iso)
                self.log("Playing ISO: {}".format(self._iso))
            else:
                self.log("The game ISO '{}' is not available.".format(self._iso))

        self._memory_listeners = tempfile.NamedTemporaryFile()

        for interest in self._memory_listener_list:
            key = (0b10000000 if interest.start_silent else 0) | interest.key
            self._memory_listeners.write(interest.start.to_bytes(4, "little"))
            self._memory_listeners.write(interest.length.to_bytes(4, "little"))
            self._memory_listeners.write(bytes([key,1 if interest.fresh_only else 0,1 if interest.use_buffer else 0,0]))

        self._memory_listeners.seek(0)
        if self.debug:
            sub = subprocess.Popen(exc, stdin=self._memory_listeners)
        else:
            sub = subprocess.Popen(exc, stdin=self._memory_listeners, stdout=subprocess.PIPE)

        self.process = sub
        self.running = True
        self.memory_cb_pipe = self._attach_cb_pipe(memPipeName)
        self.control_emu_pipe = self._attach_control_pipe(controlPipeName)
        self.proc_pipe = self._attach_control_pipe(proceedurePipeName)
        self.reversePipeThread = IPCThread(self)
        self.reversePipeThread.start()
        self.control = True

    def kill(self):
        if self.running:
            if self.paused:
                self.unfreeze(block=True)
            self._clear_shared_memory()
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

    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################

    # Pause and resume:

    def freeze(self, block=True):
        if self.running:
            if self.paused:
                return True
            self.log("Pausing...")
            self.proc_pipe.write(bytes([2]))
            if block:
                success = self._flush_then_wait(7, self.proc_pipe)
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

    def unfreeze(self, block=True):
        if not self.running:
            return
        if not self.paused:
            return True
        self.proc_pipe.write(bytes([3]))
        self.log("Resuming...")

        if block:
            success = self._flush_then_wait(8, self.proc_pipe)
            self.paused = not success
            if not success:
                self.log("Failed to resume!")
            return success
        else:
            self.proc_pipe.flush()
            self.paused = False
            return True

    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################

    # Controller Emulation:

    def hold_button(self, button, controller=0):
        if not(self.control):
            # Ignore erroneous button presses
            print("Error, controller not initialised.")
            return
        elif button >= Console.OPS_COUNT or controller >= Console.CONTROLLERS:
            print("Error, control function out of range (%i, %i)."%(button,controller))
            return
        package = (controller << 6 | button) | 32
        self.control_emu_pipe.write(bytes([package]))
        self.control_emu_pipe.flush()

    def release_button(self, button, controller=0):
        if not self.control:
            # Ignore erroneous button presses
            print("Error, controller not initialised.")
            return
        elif button >= Console.OPS_COUNT or controller >= Console.CONTROLLERS:
            print("Error, control function out of range (%i, %i)."%(button,controller))
            return
        package = (controller << 6 | button)
        self.control_emu_pipe.write(bytes([package]))
        self.control_emu_pipe.flush()

    def delay_button(self, delay):
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


    def touch_button(self, button, controller=0, sleepfor=0.2):
        self.hold_button(button, controller)
        time.sleep(sleepfor)
        self.release_button(button, controller)


    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################

    # Speed change:

    @property
    def speed(self):
        return self._speed

    @speed.setter
    def speed(self, val):
        self._speed = val
        if not self.running:
            print("Not running - speed can only be set for running consoles.")
            return None
        self.proc_pipe.write(bytes([43]))
        self.proc_pipe.write((val).to_bytes(4, byteorder='big'))
        return self._flush_then_wait(10, self.proc_pipe)

    # For legacy code:

    def setSpeed(self, val, block=False):
        if not self.running:
            print("Not running - speed can only be set for running consoles.")
            return None
        self.proc_pipe.write(bytes([43]))
        self.proc_pipe.write((val).to_bytes(4, byteorder='big'))
        if block:
            return self._flush_then_wait(10, self.proc_pipe)
        else:
            self.proc_pipe.flush()
            return True



    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################

    # State save / load:

    def save_state(self, name, callback=None):
        if not self.running:
            return False
        if not name.isalnum():
            return False
        if self.paused:
            self.unfreeze(block=True)
        path = self._get_state_path(name)
        self.handle_state_save(callback)
        self.proc_pipe.write(bytes([41, len(path)]))
        self.proc_pipe.write(path.encode("ascii"))
        self.proc_pipe.flush()

    def load_state(self, name, callback=None, block=False):
        if not name.isalnum():
            return False
        self.game_state = name
        if self.running:
            if self.paused:
                self.unfreeze(block=True)
            path = self._get_state_path(name)
            if not os.path.exists(path):
                return False
            self.handle_state_load(callback)
            self.proc_pipe.write(bytes([42, len(path)]))
            self.proc_pipe.write(path.encode("ascii"))
            if block:
                return self._flush_then_wait(2, self.proc_pipe)
            else:
                self.proc_pipe.flush()
                return True

    def handle_state_load(self, f):
        self._on_state_load = f
    def handle_state_save(self, f):
        self._on_state_save = f

    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################

    # Memory/RAM:

    def add_memory_listener(self, start, lgth, onChange, start_silent=False, only_new=True, buffer=True):
        if self.running:
            raise ConfigurationChangeWhileRunningException("Must register memory listeners before running an instance. Try using sleep/wake for listeners.")
        key = len(self._memory_listener_list)
        self._memory_listener_list.append(MemoryListener(key, start, lgth, onChange, start_silent, only_new, buffer))
        return key

    def clear_memory_listeners(self):
        if self.running:
            raise ConfigurationChangeWhileRunningException("Cannot remove listeners while running. Try sleep?")
        self._memory_listener_list = []

    def wake_memory_listener(self, key):
        if self.running:
            self.proc_pipe.write(bytes([25]))
            self.proc_pipe.write(bytes([key]))
            self.proc_pipe.flush()
        for i in range(len(self._memory_listener_list)):
            if self._memory_listener_list[i][0] == key:
                self._memory_listener_list[i][4] == False

    def sleep_memory_listener(self, key):
        if self.running:
            self.proc_pipe.write(bytes([24]))
            self.proc_pipe.write(bytes([key]))
            self.proc_pipe.flush()
        for i in range(len(self._memory_listener_list)):
            if self._memory_listener_list[i][0] == key:
                self._memory_listener_list[i][4] == True

    def read_bytes(self, start, length):
        if not self.running:
            print("Not running - memory can only be accessed from running consoles.")
            return None
        self.proc_pipe.write(bytes([21]))
        self.proc_pipe.write((start).to_bytes(4, byteorder='big'))
        self.proc_pipe.write((length).to_bytes(4, byteorder='big'))
        self.proc_pipe.write(bytes([(self._unique+5)*2+1]))

        if not self._flush_then_wait(11, self.proc_pipe):
            print("Shared memory timeout..")
            return None

        if self.sharedCommMemory is None:
            startTime = time.time()
            while True:
                try:
                    self.sharedCommMemory = sysv_ipc.SharedMemory((self._unique+5)*2+1)
                except sysv_ipc.ExistentialError:
                    if time.time()-startTime < Console.shared_memory_timeout:
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

    def write_byte(self, start, val, block=True):
        if not self.running:
            print("Not running - memory can only be written from running consoles.")
            return None
        self.proc_pipe.write(bytes([26]))
        self.proc_pipe.write((start).to_bytes(4, byteorder='big'))
        self.proc_pipe.write(bytes([val]))
        if block:
            return self._flush_then_wait(5, self.proc_pipe)
        else:
            self.proc_pipe.flush()
            return True

    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################

    # Audio Recording:

    def start_recording_audio(self):
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

    def stop_recording_audio(self, discard=False):
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

    def handle_audio_recording_stopped(self, f):
        self._on_audio_finish_record = f

    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################

    # Screen/GPU:

    def get_screen(self):
        self.proc_pipe.write(bytes([11]))
        self.proc_pipe.write(bytes([(self._unique+5)*2]))
        self.proc_pipe.flush()

        startTime = time.time()
        while True:
            try:
                memory = sysv_ipc.SharedMemory((self._unique+5)*2)
            except sysv_ipc.ExistentialError:
                if time.time()-startTime < Console.shared_memory_timeout:
                    continue
                else:
                    self.log("Shared memory timeout!")
                    return self.get_screen()
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

    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################
    ################################################################################################################

    # Useful functions:

    def snapshot_to_file(self, path):
        data = self.get_screen()
        img = Image.fromarray(data, 'RGB')
        img.save(path)

    def render(self):
        data = self.get_screen()
        data = cv2.cvtColor(data, cv2.COLOR_RGB2BGR)
        cv2.imshow("Running: {}".format(os.path.basename(self._iso)), data)
        cv2.waitKey(0)
        cv2.destroyAllWindows()

    def ram_to_disk(self, start, length, path, block=True):
        if not self.running:
            print("Not running - memory can only be accessed from running consoles.")
            return None
        self.proc_pipe.write(bytes([22]))
        self.proc_pipe.write((start).to_bytes(4, byteorder='big'))
        self.proc_pipe.write((length).to_bytes(4, byteorder='big'))
        self.proc_pipe.write(bytes([len(path)]))
        self.proc_pipe.write(path.encode("ascii"))
        if block:
            return self._flush_then_wait(4, self.proc_pipe)
        else:
            self.proc_pipe.flush()
            return True



################################################################################################################
################################################################################################################
#############################################  IPC THREAD  #####################################################
################################################################################################################
################################################################################################################

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
            for g in self.owner._memory_listener_list:
                if g.key == key:
                    profile = g
                    break
            if profile is None:
                continue
            bytecount = profile.length
            memoryvalue = self.owner.memory_cb_pipe.read(bytecount)
            profile.callback(memoryvalue)

        self.owner.log("Stopped memory speaker.")

    def stop(self):
        self.owner.log("Stopping memory speaker...")
        self.killed = True

################################################################################################################
################################################################################################################
############################################  \ IPC THREAD  ####################################################
################################################################################################################
################################################################################################################
