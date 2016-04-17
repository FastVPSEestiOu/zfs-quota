

src: FORCE
	make -C src

install: FORCE
	make -C src modules_install
	install -o root -g root -m 0755 tools/mount.zqfs /sbin

FORCE:
