# gNB__Hybrid

Example run cmd: sudo ./dpdk-baas -w 0000:b3:08.0 --lcores '0@3' -m 20000 --in-memory -- -p 1

##MAKE##

dpdk/build$ sudo meson configure -Dexamples=baas -Dbuildtype=debug  
sudo ninja 

