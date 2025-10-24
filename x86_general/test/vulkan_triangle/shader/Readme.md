
# vulkan tiangle demo 

## 简介

用于测试vulkan wsi 与 native window的对接功能

## 使用注意事项

shader目录下的triangle.frag.spv和triangle.vert.spv 应 send 到开发板 /data/vulkan_test目录下

## 脚本参考

1.push shader
hdc_std shell mkdir /data/vulkan_test
hdc_std shell file send .\shader\triangle.vert.spv /data/vulkan_test
hdc_std shell file send .\shader\triangle.frag.spv /data/vulkan_test

2.可执行文件运行
hdc_std shell file send .\triangle /data/vulkan_test
hdc_std shell chmod 777 /data/vulkan_test/triangle
hdc_std shell ./data/vulkan_test/triangle

