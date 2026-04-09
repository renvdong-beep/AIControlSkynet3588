#!/bin/bash
# 多路 RTSP 推流脚本（带自动重启）
# 使用方法: ./multi_stream.sh

INPUT="/home/topeet/rknpu2/examples/rknn_yolov5_demo/install/rknn_yolov5_demo_Linux/nainiu.h264"
PORT=8554

echo "========================================"
echo "Multi-Stream RTSP Server"
echo "========================================"

# 停止旧进程
pkill -9 -f mediamtx 2>/dev/null
pkill -9 -f "ffmpeg.*rtsp" 2>/dev/null
sleep 1

# 创建 MediaMTX 配置
cat > /tmp/mediamtx_multi.yml << 'EOF'
rtspAddress: :8554
rtmp: no
hls: no
webrtc: no
srt: no
logLevel: info
api: no
paths:
  all:
    source: publisher
EOF

# 启动 MediaMTX
echo "Starting MediaMTX..."
/usr/local/bin/mediamtx /tmp/mediamtx_multi.yml &
sleep 2

if ! pgrep -f mediamtx > /dev/null; then
    echo "Failed to start MediaMTX"
    exit 1
fi

echo "MediaMTX started on port $PORT"
echo ""

# 推流函数（带自动重启）
push_stream() {
    local id=$1
    local url="rtsp://localhost:$PORT/cow$id"
    
    while true; do
        echo "[Stream $id] Starting..."
        ffmpeg -re -stream_loop -1 -i "$INPUT" \
            -c copy \
            -fflags +genpts \
            -f rtsp \
            -rtsp_transport tcp \
            "$url" 2>&1 | grep -E "(error|Error|ERROR)" &
        
        local pid=$!
        wait $pid 2>/dev/null
        
        echo "[Stream $id] Exited, restarting in 2s..."
        sleep 2
    done
}

# 启动4路推流
for i in 0 1 2 3; do
    push_stream $i &
    sleep 0.5
done

echo ""
echo "========================================"
echo "RTSP URLs:"
echo "  rtsp://192.168.137.251:8554/cow0"
echo "  rtsp://192.168.137.251:8554/cow1"
echo "  rtsp://192.168.137.251:8554/cow2"
echo "  rtsp://192.168.137.251:8554/cow3"
echo "========================================"
echo ""
echo "Press Ctrl+C to stop all streams"

# 等待
wait
