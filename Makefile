

src: FORCE
	make -C src

install: FORCE
	make -C src modules_install
FORCE:
