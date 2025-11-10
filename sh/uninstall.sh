#!/bin/bash

# stop the extension
/var/volatile/bsext/ext_npu_argus/bsext_init stop

# check that all the processes are stopped
# ps | grep bsext_npu_argus

# unmount the extension
umount /var/volatile/bsext/ext_npu_argus
# remove the extension
rm -rf /var/volatile/bsext/ext_npu_argus

# remove the extension from the system
lvremove --yes /dev/mapper/bsext_npu_argus
# if that path does not exist, you can try
lvremove --yes /dev/mapper/bsos-ext_npu_argus

rm -rf /dev/mapper/bsext_npu_argus
rm -rf /dev/mapper/bsos-ext_npu_argus

# reboot