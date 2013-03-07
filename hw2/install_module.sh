#!/bin/sh
lsmod
rmmod sys_xintegrity
insmod sys_xintegrity.ko
lsmod
