# gNB__Hybrid

Example run cmd: sudo ./dpdk-baas -w 0000:b3:08.0 --lcores '0@3' -m 20000 --in-memory -- -p 1

##MAKE##

dpdk/build$ sudo meson configure -Dexamples=baas -Dbuildtype=debug  
sudo ninja 

##Hugepage cfg##

HugePages_Total:      27
HugePages_Free:       27
HugePages_Rsvd:        0
HugePages_Surp:        0
Hugepagesize:    1048576 kB
DirectMap4k:     1405972 kB
DirectMap2M:     3450880 kB
DirectMap1G:    30408704 kB



