#!/usr/bin/python
"""
Script to emulate use activity on the host and check that quota.
"""

import multiprocessing
import os

class SetuidProcess(multiprocessing.Process):
	def exit(self):
		return False

	_FUNCTIONS = {
		0: exit
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

		return self._FUNCTIONS[fnum](self, *fargs)

	def run(self):
		if self.gid is not None:
			os.setgid(self.gid)

		if self.uid is not None:
			os.setuid(self.uid)

		while self.process_request():
			pass
		self.conn.close()


class QuotaRandomAccess(object):
	def __init__(self, n_uids=10, n_gids=10):
		pass
	def one(self):
		pass
	pass

def main():
	parent_conn, child_conn = multiprocessing.Pipe()
	setuidprocess = SetuidProcess(child_conn)
	setuidprocess.start()
	parent_conn.send((0,))
	setuidprocess.join()

if __name__ == "__main__":
	main()
