#!/usr/bin/python

from setup import *
import time

# TODO: find real python func
def dec_or_hex(str):
    if str.startswith("0x"):
        return int(str, 16)
    else:
        return int(str)

# remove the unused top args
sys.argv.pop(0)

params = {"kernel": None, "dtb" : None, "initrd": None}

# Try loading config from output config file
try:
    with open("boot_params.config", "r") as f:
        for line in f.readlines():
            param, base, size = line.split('\t')
            base = dec_or_hex(base)
            size = dec_or_hex(size)
            params[param] = (base, size)
            print('Found %s: base 0x%x (0x%x bytes)' % (param, base, size))

except Exception as e:
    print("No boot_params.config from qemu found (or invalid), try running in qemu's launch directory")

for i in params.keys():
    if len(sys.argv) == 0:
        break
    base = dec_or_hex(sys.argv.pop(0))
    size = dec_or_hex(sys.argv.pop(0))
    print('Found %s: base 0x%x (0x%x bytes)' % (i, base, size))
    params[i] = (base, size)

kernel_size = 32 * 1024 * 1024
kernel_base = u.memalign(2 * 1024 * 1024, kernel_size)

print("Kernel_base: 0x%x" % kernel_base)

assert not (kernel_base & 0xffff)

if params['initrd'] is not None:
    base, size = params['initrd']
    p.kboot_set_initrd(base, size)

p.smp_start_secondaries()

if p.kboot_prepare_dt(params['dtb'][0]):
    print("DT prepare failed")
    sys.exit(1)

print("Uncompressing...")

iface.dev.timeout = 40

compressed_addr, compressed_size = params['kernel']
kernel_size = p.gzdec(compressed_addr, compressed_size, kernel_base, kernel_size)
print(kernel_size)

if kernel_size < 0:
    raise Exception("Decompression error!")

print("Decompress OK...")

p.dc_cvau(kernel_base, kernel_size)
p.ic_ivau(kernel_base, kernel_size)

print("Ready to boot")

daif = u.mrs(DAIF)
daif |= 0x3c0
u.msr(DAIF, daif)
print("DAIF: %x" % daif)

p.kboot_boot(kernel_base)
iface.ttymode()
