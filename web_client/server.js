const express = require('express');
const bodyParser = require('body-parser');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const path = require('path');

const app = express();
const port = 3000;

// 配置
// 如果环境变量设置了 SERVER_ADDRESS 就用环境变量，否则默认连本地
// 你启动时可以这样：SERVER_ADDRESS=57.183.34.18:50051 npm start
const TARGET_SERVER = process.env.SERVER_ADDRESS || 'localhost:50051';

console.log(`Target gRPC Server: ${TARGET_SERVER}`);

app.use(bodyParser.json());
app.use(express.static('public'));

// 加载 Proto 文件
const PROTO_PATH_USER = path.join(__dirname, '../proto/user_service.proto');
const PROTO_PATH_MEETING = path.join(__dirname, '../proto/meeting_service.proto');
const PROTO_PATH_COMMON = path.join(__dirname, '../proto/common.proto');

const packageDefinition = protoLoader.loadSync(
    [PROTO_PATH_USER, PROTO_PATH_MEETING, PROTO_PATH_COMMON],
    {
        keepCase: true,
        longs: String,
        enums: String,
        defaults: true,
        oneofs: true,
        includeDirs: [path.join(__dirname, '../proto')]
    }
);

const protoDescriptor = grpc.loadPackageDefinition(packageDefinition);
const userService = new protoDescriptor.proto.user.UserService(TARGET_SERVER, grpc.credentials.createInsecure());
const meetingService = new protoDescriptor.proto.meeting.MeetingService(TARGET_SERVER, grpc.credentials.createInsecure());

// --- API 路由 ---

// 1. 注册
app.post('/api/register', (req, res) => {
    userService.Register(req.body, (err, response) => {
        if (err) {
            res.status(500).json({ error: err.message });
        } else {
            res.json(response);
        }
    });
});

// 2. 登录
app.post('/api/login', (req, res) => {
    userService.Login(req.body, (err, response) => {
        if (err) {
            res.status(500).json({ error: err.message });
        } else {
            res.json(response);
        }
    });
});

// 3. 创建会议
app.post('/api/meeting/create', (req, res) => {
    // 构造请求，添加当前时间戳作为开始时间
    const request = {
        session_token: req.body.session_token,
        topic: req.body.topic,
        scheduled_start: { seconds: Math.floor(Date.now() / 1000) }
    };
    
    meetingService.CreateMeeting(request, (err, response) => {
        if (err) {
            res.status(500).json({ error: err.message });
        } else {
            res.json(response);
        }
    });
});

// 4. 加入会议
app.post('/api/meeting/join', (req, res) => {
    meetingService.JoinMeeting(req.body, (err, response) => {
        if (err) {
            res.status(500).json({ error: err.message });
        } else {
            res.json(response);
        }
    });
});

// 5. 获取会议信息
app.post('/api/meeting/get', (req, res) => {
    meetingService.GetMeeting(req.body, (err, response) => {
        if (err) {
            res.status(500).json({ error: err.message });
        } else {
            res.json(response);
        }
    });
});

app.listen(port, () => {
    console.log(`Web Client Bridge running at http://localhost:${port}`);
});
