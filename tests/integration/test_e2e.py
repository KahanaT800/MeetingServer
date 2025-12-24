import grpc
import sys
import os
import time
import random

# Ensure generated code can be imported
sys.path.append(os.path.join(os.path.dirname(__file__), 'generated'))

import common_pb2
import user_service_pb2
import user_service_pb2_grpc
import meeting_service_pb2
import meeting_service_pb2_grpc

def register_and_login(user_stub, prefix):
    # Register
    username = f"{prefix}_{int(time.time())}_{random.randint(1000,9999)}"
    password = "password123"
    
    print(f"Registering {username}...")
    try:
        reg_req = user_service_pb2.RegisterRequest(
            user_name=username,
            password=password,
            email=f"{username}@example.com",
            display_name=f"Test User {username}"
        )
        reg_resp = user_stub.Register(reg_req)
        
        if reg_resp.error.code != 0:
            print(f"❌ Registration failed: {reg_resp.error.message}")
            sys.exit(1)
        print(f"✅ User registered: {reg_resp.user.user_id}")
        
    except grpc.RpcError as e:
        print(f"❌ RPC failed during registration: {e}")
        sys.exit(1)

    # Login
    print(f"Logging in {username}...")
    try:
        login_req = user_service_pb2.LoginRequest(
            user_name=username,
            password=password
        )
        login_resp = user_stub.Login(login_req)
        
        if login_resp.error.code != 0:
            print(f"❌ Login failed: {login_resp.error.message}")
            sys.exit(1)
            
        token = login_resp.session_token
        if not token:
            print("❌ No session token received")
            sys.exit(1)
            
        print(f"✅ Logged in, token: {token[:10]}...")
        return token, reg_resp.user.user_id
        
    except grpc.RpcError as e:
        print(f"❌ RPC failed during login: {e}")
        sys.exit(1)

def run_test():
    # Connect to the server
    target = os.environ.get('SERVER_ADDRESS', 'localhost:50051')
    print(f"Connecting to {target}...")
    channel = grpc.insecure_channel(target)
    
    user_stub = user_service_pb2_grpc.UserServiceStub(channel)
    meeting_stub = meeting_service_pb2_grpc.MeetingServiceStub(channel)

    # 1. Setup Host
    print("\n[1] Setting up Host User...")
    host_token, host_id = register_and_login(user_stub, "host")

    # 2. Create Meeting (Host)
    print("\n[2] Host Creating Meeting...")
    meeting_id = None
    try:
        create_req = meeting_service_pb2.CreateMeetingRequest(
            session_token=host_token,
            topic="Integration Test Meeting",
            scheduled_start=common_pb2.Timestamp(seconds=int(time.time()) + 3600)
        )
        create_resp = meeting_stub.CreateMeeting(create_req)
        
        if create_resp.error.code != 0:
            print(f"❌ Create meeting failed: {create_resp.error.message}")
            sys.exit(1)
            
        meeting_id = create_resp.meeting.meeting_id
        print(f"✅ Meeting created: {meeting_id}")
        
    except grpc.RpcError as e:
        print(f"❌ RPC failed during create meeting: {e}")
        sys.exit(1)

    # 3. Setup Guest
    print("\n[3] Setting up Guest User...")
    guest_token, guest_id = register_and_login(user_stub, "guest")

    # 4. Join Meeting (Guest)
    print("\n[4] Guest Joining Meeting...")
    try:
        join_req = meeting_service_pb2.JoinMeetingRequest(
            session_token=guest_token,
            meeting_id=meeting_id,
            client_info="Python Integration Test Client (Guest)"
        )
        join_resp = meeting_stub.JoinMeeting(join_req)
        
        if join_resp.error.code != 0:
            print(f"❌ Join meeting failed: {join_resp.error.message}")
            sys.exit(1)
            
        print(f"✅ Guest joined meeting. Server endpoint: {join_resp.endpoint.ip}:{join_resp.endpoint.port}")
        
    except grpc.RpcError as e:
        print(f"❌ RPC failed during join meeting: {e}")
        sys.exit(1)

    # 5. Leave Meeting (Guest)
    print("\n[5] Guest Leaving Meeting...")
    try:
        leave_req = meeting_service_pb2.LeaveMeetingRequest(
            session_token=guest_token,
            meeting_id=meeting_id
        )
        leave_resp = meeting_stub.LeaveMeeting(leave_req)
        
        if leave_resp.error.code != 0:
            print(f"❌ Leave meeting failed: {leave_resp.error.message}")
            sys.exit(1)
            
        print(f"✅ Guest left meeting successfully")
        
    except grpc.RpcError as e:
        print(f"❌ RPC failed during leave meeting: {e}")
        sys.exit(1)

    # 6. Leave Meeting (Host) - Cleanup
    print("\n[6] Host Leaving Meeting...")
    try:
        leave_req = meeting_service_pb2.LeaveMeetingRequest(
            session_token=host_token,
            meeting_id=meeting_id
        )
        leave_resp = meeting_stub.LeaveMeeting(leave_req)
        
        if leave_resp.error.code != 0:
            print(f"❌ Leave meeting failed: {leave_resp.error.message}")
            sys.exit(1)
            
        print(f"✅ Host left meeting successfully")
        
    except grpc.RpcError as e:
        print(f"❌ RPC failed during leave meeting: {e}")
        sys.exit(1)

    print("\n🎉 All integration tests passed successfully!")

if __name__ == '__main__':
    run_test()
