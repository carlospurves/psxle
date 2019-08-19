import os
import itertools
import subprocess
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

class ISONotFoundException(Exception):
    def __init__(self,*args,**kwargs):
        Exception.__init__(self,*args,**kwargs)

class ExecutableNotFoundException(Exception):
    def __init__(self,*args,**kwargs):
        Exception.__init__(self,*args,**kwargs)

class MemoryListener():
    def __init__(self, start, length, callback, start_silent=False, fresh_only=False, use_buffer=False):
        self.start, self.length, self.callback = start, length, callback
        self.start_silent, self.fresh_only, self.use_buffer = start_silent, fresh_only, use_buffer
        return

class IPCThread (threading.Thread):
    def __init__(self, owner):
        self.owner = owner
        threading.Thread.__init__(self)
        self.killed = False
    def run(self):
        while not self.killed:
            try:
                next = self.owner.memory_fifo.read(1)
            except Exception as e:
                if e is ValueError:
                    break
                else:
                    raise
            if len(next) == 0:
                continue
            if next[0] != 0:
                self.owner.log(("Received notif ", next[0]))
                self.owner.status[next[0]] = time.time()
                if next[0] == 2:
                    if self.owner.on_state_load:
                        self.owner.on_state_load()
                elif next[0] == 3:
                    if self.owner.on_state_save:
                        self.owner.on_state_save()
                elif next[0] == 9:
                    self.owner.isListeningToAudio = False
                    if self.owner.on_audio_stopped_recording:
                        self.owner.on_audio_stopped_recording()

                continue
            key = self.owner.memory_fifo.read(1)[0]
            profile = None
            memoryvalue = None
            for g in self.owner.memory_interest:
                if g.key == key:
                    profile = g
                    break
            if profile is None:
                continue
            bytecount = profile.length
            memoryvalue = self.owner.memory_fifo.read(bytecount)
            profile.callback(memoryvalue)

        self.owner.log("Stopped memory speaker.")

    def stop(self):
        self.owner.log("Stopping memory speaker...")
        self.killed = True


class Console():
    # This is the main console class, which wraps the PSXLE process
    unique_instance_id = itertools.count()
    install_location = os.path.expanduser("~/.psxle")
    temp_dir = os.environ['TMPDIR'] if 'TMPDIR' in os.environ else "/tmp"

    @staticmethod
    def int32(value):
        return value.to_bytes(4, "little")

    @staticmethod
    def get_resource(name):
        return os.path.join(Console.install_location, name)

    @staticmethod
    def decode_int(b):
        return int.from_bytes(b, byteorder="little")

    def log(self, *arg):
        if self.debug:
            print(arg)

    def get_pipe_path(self, type=None):
        suffix = "-{}".format(type) if type else ""
        return os.path.join(Console.temp_dir, "ml_psxemu{}{}".format(self._unique, suffix))

    def __init__(self, iso, render=False, debug=False, gui=False):

        if not os.path.isfile(Console.get_resource("psxle")):
            raise ExecutableNotFoundException()

        if not os.path.isfile(iso):
            raise ISONotFoundException(iso)

        # these should not be directly changed once instantiated
        self._iso = iso
        self._unique = next(self.unique_instance_id)
        self._process = None
        self._listeners = None
        self._paused = False # true iff pause() has been called
        self._running = False # true iff process is open

        self.debug = debug
        self.render = render
        self.gui = gui

        self.callback_status = [0 for _ in range(13)]
        self.callback_map = {}

        self.memory_listener_file = None
        self.memory_interest = []
        self.listen_range = None

        self.recording_status = None

        # event callbacks:
        self.on_state_load = None
        self.on_state_save = None
        self.on_audio_stopped_recording = None

        if not os.path.exists(Console.get_resource("isos/")):
            os.mkdir(Console.get_resource("isos/"))
        if not os.path.exists(Console.get_resource("states/")):
            os.mkdir(Console.get_resource("states/"))

    @property
    def running(self):
        return self._running
    @property
    def paused(self):
        return self._paused

    def attach_fifo(self, name):
        try:
            os.mkfifo(name)
        except OSError as oe:
            if oe.errno != errno.EEXIST:
                raise

        self.log("Attatching Control Pipe %s ..."%name)
        pipe = open(name, "wb")
        self.log("Control Pipe {} successfully attatched...".format(name))
        return pipe

    def run(self):
        if self.running:
            return

        if not os.path.isfile(self._iso):
            raise ISONotFoundException()

        execution = []

        if self.gui:
            execution += ["-gui", "-controlPipe", "none"]
            self.process = subprocess.Popen(execution, stdout=subprocess.PIPE)
            return

        if not self.render:
            execution += ["xvfb-run", "-a", "-s", "-screen 0 1400x900x24"]

        execution += [Console.get_resource("psxle"), "-cfg", Console.get_resource("default.cfg")]

        if self.debug:
            execution += ["-debug"]

        execution += ["-display", "0" if self.render else "1"]

        execution += ["-controlPipe", self.get_pipe_path()]
        execution += ["-nMemoryListeners", str(len(self.memory_interest))]
        execution += ["-play", self._iso]

        self.memory_listener_file = tempfile.NamedTemporaryFile()
        self.process = subprocess.Popen(execution, stdout=subprocess.PIPE)

        for interest in self.memory_interest:
            key = (0b10000000 if interest.start_silent else 0) | interest.key
            self.log("Sending",keyout)
            self.memory_listener_file.write(Console.int32(interest.start))
            self.memory_listener_file.write(Console.int32(interest.length))
            self.memory_listener_file.write(bytes([key,1 if interest.fresh_only else 0,1 if interest.use_buffer else 0,0]))

        self.memory_listener_file.seek(0)
        self.process = subprocess.Popen(execution, stdin=self.memory_listener_file, stdout=None if self.debug else subprocess.PIPE)

        self.memory_fifo = self.attach_fifo(self.get_pipe_path("mem"))
        self.ctl_fifo = self.attach_fifo(self.get_pipe_path("joy"))
        self.proc_fifo = self.attach_fifo(self.get_pipe_path("proc"))
        self.reverse_fifo_thread = IPCThread(self)
        self.reverse_fifo_thread.start()


    @property
    def listeners(self):
        return self._listeners

    @listeners.setter
    def listeners(self, lsn):
        lsn.sort(key=lambda x: x.start)
        self._listeners = lsn
        range_start = lsn[0].start
        end_values = [(l.start + l.length) for l in lsn]
        range_end = max(end_values)
        self.listen_range = (range_start, range_end)
