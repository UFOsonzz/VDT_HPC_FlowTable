# Giải phóng pagecache, dentries và inodes trong RAM
echo 3 | sudo tee /proc/sys/vm/drop_caches
# Đặt lại số lượng về 0
echo 0 | sudo tee /proc/sys/vm/nr_hugepages
