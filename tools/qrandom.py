#!/usr/bin/python
"""
Script to emulate use activity on the host and check that quota.
"""

import itertools
import multiprocessing
import os
import random
import tempfile

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

	_FUNCTIONS = {
		0: exit,
		1: info,
		2: touch
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
		if self.gid is not None:
			os.setgid(self.gid)

		if self.uid is not None:
			os.setuid(self.uid)

		try:
			while True:
				self.process_request()
		except StopIteration:
			pass
		self.conn.close()


class QuotaRandomAccess(object):
	def __init__(self, n_uids=10, n_gids=10, interleave_perc=50):
		uids = [random.randint(0, (1<<32) - 1) for x in range(n_uids)]
		gids = [random.randint(0, (1<<32) - 1) for x in range(n_gids)]
		interleave = itertools.islice(itertools.product(uids, gids), 0, 
									  n_uids * n_gids * interleave_perc / 100)
		creds =  [(uid, 0) for uid in uids]
		creds += [(0, gid) for gid in gids]
		creds += interleave

		self.creds = creds
		self.processes = {}
		self.pipes = {}

	def _start_one(self, cred):
		parent_conn, child_conn = multiprocessing.Pipe()
		process = SetuidProcess(child_conn, uid=cred[0], gid=cred[1])
		self.processes[cred] = process
		self.pipes[cred] = parent_conn

		process.start()

	def execute_one(self):
		cred = random.choice(self.creds)

		if cred not in self.processes:
			self._start_one(cred)

		pipe = self.pipes[cred]
		filename = tempfile.mktemp(dir='')
		pipe.send((2, filename))

	def stop_all(self):
		for pipe in self.pipes.values():
			pipe.send((0,))

def main(args):
	dirname = args[1]
	umask = os.umask(0)
	os.makedirs(dirname, 0777)
	os.chdir(dirname)
	os.umask(umask)

	qra = QuotaRandomAccess()
	qra.execute_one()
	qra.stop_all()

if __name__ == "__main__":
	import sys
	main(sys.argv)
