## 创建一个虚拟 v4l2 摄像头节点，使用FFmpeg持续喂数据
1. **创建节点**：
 ``` Shell
sudo modprobe v4l2loopback \
    video_nr=10 \
    card_label="Virtual Camera" \
    exclusive_caps=1
 ```

 2. **固定开机自动创建**：
 ```Shell
 #1. 侦测
sudo vi /etc/modprobe.d/v4l2loopback.conf
#写入以下内容
options v4l2loopback video_nr=10 card_label="Virtual Camera" exclusive_caps=1

#2. 模块
sudo vi /etc/modules-load.d/v4l2loopback.conf
#写入以下内容
v4l2loopback

#下次启动执行以下命令就能看到已经自动创建了
ls -l /dev/video10
 ```
 3. **使用ffmpeg喂数据**：
```Shell
ffmpeg \
    -re \
    -stream_loop -1 \
    -i test-video/panasonic\(1080p-4min\).mp4 \
    -vf "scale=1920:1080,format=yuv420p" \
    -f v4l2 \
    /dev/video10
```

4. **v4l2-ctl一些获取信息的命令**：
```Shell
#查看格式
v4l2-ctl -d /dev/video10 --list-formats-ext

#查看当前格式
v4l2-ctl -d /dev/video10 --get-fmt-video

#抓数据
v4l2-ctl -d /dev/video10 --stream-mmap --stream-count=300 --stream-to=capture.raw

#使用ffplay播放,其中 pixel_format、video_size、framerate要跟实际 v4l2-ctl -d /dev/video10 --get-fmt-video 得到的一致
ffplay -f rawvideo -pixel_format yuv420p -video_size 1920x1080 -framerate 30 capture.raw
```