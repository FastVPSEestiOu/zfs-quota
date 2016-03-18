#!/usr/bin/python
"""
Script to emulate use activity on the host and check that quota.
"""

import collections
import itertools
import json
import multiprocessing
import os
import time
import random
import signal
import subprocess
import sys
import tempfile
import traceback

STARTING_CRED_NUM = 2000

ACT_EXIT = 0
ACT_INFO = 1
ACT_TOUCH = 2
ACT_UNLINK = 3
ACT_TOUCH_MANY = 4

if hasattr(os, 'sync'):
	sync = os.sync
else:
	import ctypes
	libc = ctypes.CDLL("libc.so.6")
	def sync():
		libc.sync()

class SetuidProcess(multiprocessing.Process):
	DEV_ZERO_FD = open("/dev/zero", "r")

	def info(self):
		print os.getuid()
		print os.getgid()

	def exit(self):
		raise StopIteration

	def touch(self, filename, size=1024*1024):
		with open(filename, "w") as fh:
			fh.write(self.DEV_ZERO_FD.read(size))
		return filename

	def unlink(self, filename):
		os.unlink(filename)

	def touch_many(self, prefix, amount=128, size=1024):
		fnames = []
		for i in range(amount):
			fname = '%s_%08d' % (prefix, i)
			with open(fname, "w") as fh:
				fh.write(self.DEV_ZERO_FD.read(size))
			fnames.append(fname)
		return fnames

	_FUNCTIONS = {
		ACT_EXIT: exit,
		ACT_INFO: info,
		ACT_TOUCH: touch,
		ACT_UNLINK: unlink,
		ACT_TOUCH_MANY: touch_many,
	}

	def __init__(self,  conn, uid=None, gid=None):
		super(SetuidProcess, self).__init__()
		self.conn = conn
		self.uid = uid
		self.gid = gid

	def process_request(self):
		cmd = self.conn.recv()

		fnum = cmd[0]
		fargs = cmd[1:]

		val = self._FUNCTIONS[fnum](self, *fargs)
		self.conn.send(val)
		return val

	def run(self):
		signal.signal(signal.SIGUSR1, signal.SIG_DFL)
		signal.signal(signal.SIGTERM, signal.SIG_DFL)
		signal.signal(signal.SIGINT, signal.SIG_IGN)

		if self.gid > 0:
			os.setgid(self.gid)

		if self.uid > 0:
			os.setuid(self.uid)

		try:
			while True:
				self.process_request()
		except StopIteration:
			pass

		self.conn.close()


class QuotaRandomAccess(object):
	def __init__(self, n_uids=10, n_gids=10, interleave_perc=50):
		uids = [random.randint(STARTING_CRED_NUM, (1<<32) - 1)
				for x in range(n_uids)]
		gids = [random.randint(STARTING_CRED_NUM, (1<<32) - 1)
				for x in range(n_gids)]
		interleave = itertools.islice(itertools.product(uids, gids), 0,
									  n_uids * n_gids * interleave_perc / 100)
		creds =  [(uid, STARTING_CRED_NUM) for uid in uids]
		creds += [(STARTING_CRED_NUM, gid) for gid in gids]
		creds += interleave

		self.creds = creds
		self.processes = {}
		self.pipes = {}
		self.files = {}

	def _start_one(self, cred):
		parent_conn, child_conn = multiprocessing.Pipe()
		process = SetuidProcess(child_conn, uid=cred[0], gid=cred[1])

		self.processes[cred] = process
		self.pipes[cred] = parent_conn
		self.files.setdefault(cred, [])

		process.start()

	def _start_for_creds(self, cred):
		if cred not in self.processes:
			self._start_one(cred)

	def _get_creds(self, exclude=None, start=True):
		cred = random.choice(self.creds)

		while exclude and cred in exclude:
			cred = random.choice(self.creds)

		if start:
			self._start_for_creds(cred)

		return cred

	def _get_file(self):
		if not self.files:
			return None, None

		cred = random.choice(self.files.keys())
		if not self.files[cred]:
			return cred, None

		return cred, random.choice(self.files[cred])

	def _touch(self):
		cred = self._get_creds()

		pipe = self.pipes[cred]
		filename = tempfile.mktemp(dir='')
		pipe.send((ACT_TOUCH, filename))

		filename = pipe.recv()
		self.files.setdefault(cred, []).append(filename)

		return True

	def _touch_many(self):
		cred = self._get_creds()

		pipe = self.pipes[cred]
		filename = tempfile.mktemp(dir='')
		pipe.send((ACT_TOUCH_MANY, filename))

		files = pipe.recv()
		self.files.setdefault(cred, []).extend(files)

		return True

	def _unlink(self):
		cred, filename = self._get_file()
		if filename is None:
			return False

		self._start_for_creds(cred)

		pipe = self.pipes[cred]
		pipe.send((ACT_UNLINK, filename))
		pipe.recv()

		self.files[cred].remove(filename)

		return True

	def _unlink_all(self):
		if not self.files:
			return False

		cred = random.choice(self.files.keys())
		if not self.files[cred]:
			return False

		for fname in self.files[cred]:
			os.unlink(fname)

		self.files[cred] = []

		return True

	def _chcred(self):
		cred, filename = self._get_file()
		if filename is None:
			return False

		cred2 = self._get_creds(exclude=(cred,), start=False)

		uid, gid = cred2
		if uid < 0: uid = cred[0]
		if gid < 0: gid = cred[1]

		os.chown(filename, uid, gid)

		self.files[cred].remove(filename)

		self.files.setdefault(cred2, []).append(filename)

		return True

	def one_action(self):
		ACTIONS = (self._touch, self._touch_many, self._unlink,
				   self._unlink_all, self._chcred)
		action = random.choice(ACTIONS)
		ret = None
		try:
			ret = action()
		except Exception as e:
			traceback.print_exc()
		return ret

	def stop_all(self):
		for pipe in self.pipes.values():
			pipe.send((0,))

		for process in self.processes.values():
			process.join()

		self.processes = {}
		self.pipes = {}


class RepQuotaParser(object):
	def __init__(self, start_qid=STARTING_CRED_NUM):
		self.start_qid = start_qid

	def parse_repquota(self, *options):
		process = subprocess.Popen(("repquota",) + options,
								   stdout=subprocess.PIPE,
								   stderr=subprocess.STDOUT)
		output, x = process.communicate()
		disk_usage = {}
		disk_inodes = {}
		for line in output.split("\n"):
			if not line.startswith("#"):
				continue
			tokens = line.split()
			qid = int(tokens[0][1:])

			if qid < self.start_qid:
				continue

			disk_usage[qid] = int(tokens[2])
			disk_inodes[qid] = int(tokens[5])
		return disk_usage, disk_inodes

	def disk_usage(self):
		users_disk_usage = self.parse_repquota("-n", "/dev/simfs")
		groups_disk_usage = self.parse_repquota("-gn", "/dev/simfs")
		return users_disk_usage + groups_disk_usage

class RepQuotaEmulator(object):
	def __init__(self, dirname):
		self.dirname = dirname

	def disk_usage(self):
		users_blocks = collections.defaultdict(int)
		users_inodes = collections.defaultdict(int)
		groups_blocks = collections.defaultdict(int)
		groups_inodes = collections.defaultdict(int)
		for dirpath, dirnames, filenames in os.walk(self.dirname):
			for filename in filenames:
				filename = os.path.join(dirpath, filename)
				stat = os.stat(filename)

				users_blocks[stat.st_uid] += stat.st_blocks * 512
				groups_blocks[stat.st_gid] += stat.st_blocks * 512

				users_inodes[stat.st_uid] += 1
				groups_inodes[stat.st_gid] += 1

		for qid in users_blocks:
			users_blocks[qid] = (users_blocks[qid] + 1023) / 1024

		for qid in groups_blocks:
			groups_blocks[qid] = (groups_blocks[qid] + 1023) / 1024

		return (dict(users_blocks), dict(users_inodes),
			    dict(groups_blocks), dict(groups_inodes))


def diff_dicts(data1, data2):
	diff = {}

	for key in set(data1) | set(data2):
		d1 = data1.get(key)
		d2 = data2.get(key)
		if d1 != d2:
			diff[key] = (d1, d2)

	return diff


class Application(object):
	@staticmethod
	def get_working_dir(dirname):
		umask = os.umask(0)
		try:
			os.makedirs(dirname, 0777)
		except OSError:
			pass
		os.umask(umask)
		os.chdir(dirname)

	def __init__(self, args):
		self.working_dir = args[1]

		self.get_working_dir(self.working_dir)

		bunch_actions = 1000
		try:
			bunch_actions = int(args[2])
		except IndexError:
			pass
		self.bunch_actions = bunch_actions

		self.qra = QuotaRandomAccess()
		self.pid = os.getpid()

		self.next = False

		signal.signal(signal.SIGUSR1, self.schedule_next)
		signal.signal(signal.SIGUSR2, signal.SIG_IGN)
		signal.signal(signal.SIGTERM, self.term_handler)
		signal.signal(signal.SIGINT, self.term_handler)

	def run(self):
		while not self.next:
			signal.pause()
		self.next = False

		signal.signal(signal.SIGUSR1, signal.SIG_IGN)
		signal.signal(signal.SIGUSR2, signal.SIG_IGN)
		for i in range(self.bunch_actions):
			self.qra.one_action()

		print >>sys.stderr, "Done %d, pausing" % self.bunch_actions
		signal.signal(signal.SIGUSR1, self.schedule_next)
		signal.signal(signal.SIGUSR2, self.repquota)

	def repquota(self, *args):
		repquota_emulator = RepQuotaEmulator(self.working_dir)
		our_repquota = repquota_emulator.disk_usage()

		repquota = RepQuotaParser()
		sys_repquota = repquota.disk_usage()

		names = ("user_blocks", "user_inodes",
				 "group_blocks", "group_inodes")

		print >>sys.stderr, "Diff between our and repquota"
		for n, d1, d2 in zip(names, our_repquota, sys_repquota):
			diff = diff_dicts(d1, d2)
			if diff:
				print >>sys.stderr, n
				print >>sys.stderr, json.dumps(diff, sort_keys=True, indent=4)

	def schedule_next(self, *args):
		print >>sys.stderr, "Scheduling next %d" % self.bunch_actions
		self.next = True
		return

	def term_handler(self, *args):
		if self.pid == os.getpid():
			self.qra.stop_all()
			exit()

def main(args):
	app = Application(args)
	while True:
		app.run()

if __name__ == "__main__":
	import sys
	main(sys.argv)

# vim: set sw=4 sts=4 ts=4 noexpandtab:
