# Convenience targets.  The firmware itself is a normal Pico SDK CMake project.
CC ?= cc

.PHONY: all host firmware panel test pidptest sshtest multostest uwm clean
all: host firmware

host: build/pdp8host
NET_SRC = net/ssh.c net/sshsrv.c net/sha256.c lib/monocypher/monocypher.c lib/monocypher/monocypher-ed25519.c
build/pdp8host: host/main.c core/pdp8.c core/pdp8.h $(NET_SRC) net/ssh.h net/sshsrv.h
	mkdir -p build
	$(CC) -O2 -Wall -Wextra -o $@ host/main.c core/pdp8.c $(NET_SRC)

firmware:
	cmake -S firmware -B build-fw -DCMAKE_BUILD_TYPE=Release
	cmake --build build-fw -j
	cp build-fw/pdp8_fruitjam.uf2 .

build/pidptest: tests/pidptest.c firmware/pidp.c firmware/pidp.h core/pdp8.c core/pdp8.h pidp8-panel/pidp_proto.h
	mkdir -p build
	$(CC) -O2 -Wall -Wextra -o $@ tests/pidptest.c firmware/pidp.c core/pdp8.c

# host build whose SSH goes through net/nina.c + net/wifi.c to a simulated ESP32
build/pdp8host-nina: host/main.c core/pdp8.c $(NET_SRC) net/nina.c net/wifi.c tests/fakeesp.c net/*.h
	mkdir -p build
	$(CC) -O2 -Wall -Wextra -DUSE_NINA -o $@ host/main.c core/pdp8.c $(NET_SRC) net/nina.c net/wifi.c tests/fakeesp.c

sshtest: build/pdp8host build/pdp8host-nina
	python3 tests/sshtest.py | tail -1
	PDP8HOST=build/pdp8host-nina python3 tests/sshtest.py | tail -1

# needs MULTOS_RK=/path/to/multos8.rk05 (skipped otherwise)
multostest: build/pdp8host
	python3 tests/multostest.py | tail -1

# needs ETOS_RK=/path/to/etosv5b-demo.rk05 (skipped otherwise)
etostest: build/pdp8host
	python3 tests/etostest.py | tail -1

pidptest: build/pidptest
	build/pidptest | tail -1

# firmware for the PiDP-8/I panel interface (Raspberry Pi Pico 2 and Pico)
panel:
	cmake -S pidp8-panel -B build-panel2 -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s -DCMAKE_BUILD_TYPE=Release
	cmake --build build-panel2 -j
	cmake -S pidp8-panel -B build-panel1 -DPICO_BOARD=pico -DPICO_PLATFORM=rp2040 -DCMAKE_BUILD_TYPE=Release
	cmake --build build-panel1 -j
	cp build-panel2/pidp8_panel.uf2 pidp8_panel_pico2.uf2
	cp build-panel1/pidp8_panel.uf2 pidp8_panel_pico.uf2

# needs python3 + pexpect and a SIMH PDP-8 binary (SIMH_PDP8=/path/to/pdp8)
test: host pidptest
	python3 tests/difftest.py | tail -1
	python3 tests/eaetest.py | tail -1
	python3 tests/tss8test.py | tail -1
	python3 tests/uwmtest.py | tail -1
	python3 tests/dtrxtest.py | tail -1

build/palbart: tools/palbart/palbart.c
	mkdir -p build
	$(CC) -O2 -w -o $@ $<

# rebuild sdcard/uwm_rf.dsk + uwm_init.bin from the UWM sources
uwm: build/palbart
	PALBART=$(CURDIR)/build/palbart python3 uwm/make_uwm_disk.py

clean:
	rm -rf build build-fw build-panel1 build-panel2 uwm/build
