## 创建一个虚拟 v4l2 摄像头节点，使用 ffmpeg 持续喂数据
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

## 创建一个虚拟 alsa 输入节点，使用 ffmpeg 持续喂数据

1. **安装工具**：
```Shell
sudo apt update
#安装 alsa相关工具
sudo apt install -y \
    alsa-utils \
    libasound2-dev

#确认已经安装
aplay --version
arecord --version
```

2. **创建节点**：
```Shell
sudo modprobe snd-aloop

#检查
lsmod | grep snd_aloop
cat /proc/asound/cards
aplay -l
arecord -l
```

3. **验证**：
```Shell
#写入声音
aplay \
    -D hw:Loopback,0,0 \
    -f S16_LE \
    -r 48000 \
    -c 2 \
    test.wav

#采集声音
arecord \
    -D hw:Loopback,1,0 \
    -f S16_LE \
    -r 48000 \
    -c 2 \
    capture.wav
```

4. **使用ffmpeg喂数据**：
```Shell
#喂数据到 hw:Loopback,0,0
ffmpeg \
    -re \
    -stream_loop -1 \
    -i test-video/panasonic\(1080p-4min\).mp4 \
    -map 0:a:0 \
    -ac 2 \
    -ar 48000 \
    -f alsa \
    hw:Loopback,0,0

#从 hw:Loopback,1,0 采集数据
arecord \
    -D hw:Loopback,1,0 \
    -f S16_LE \
    -r 48000 \
    -c 2 \
    output.wav
```

## 使用ffmpeg同时喂音频和视频数据到虚拟节点
```shell
nohup ffmpeg \
    -nostdin \
    -re \
    -stream_loop -1 \
    -i "/home/yjh/work/test-video/panasonic(1080p-4min).mp4" \
    -map 0:v:0 \
    -vf "scale=1920:1080,format=yuv420p" \
    -r 30 \
    -f v4l2 \
    /dev/video10 \
    -map 0:a:0 \
    -ac 2 \
    -ar 48000 \
    -sample_fmt s16 \
    -f alsa \
    hw:Loopback,0,0 \
    > /tmp/virtual-av.log 2>&1 &

echo $! > /tmp/virtual-av.pid
```

1. **框架**：

                        test.mp4
                            │
                    one FFmpeg
                            │
                ┌───────────┴──────────┐
                │                      │
            Video                   Audio
                │                      │
            decoder                decoder
                │                      │
            scale                  resample
                │                      │
                ▼                      ▼
        /dev/video10          hw:Loopback,0,0
                │                      │
        v4l2loopback              snd-aloop
                │                      │
                ▼                      ▼
        V4L2 capture          hw:Loopback,1,0

2. **代码流程**：
```cpp
//视频
open("/dev/video10")

//音频
snd_pcm_open(..., "hw:Loopback,1,0", ...)
```