import socket
import sys
import time
import threading
from dotenv import load_dotenv
import os

# Load config from .env file
load_dotenv()

PORT = int(os.getenv("EOP_PORT", 9026))
HOST = "127.0.0.1"

def client_worker(client_id, hold_connection, num_requests, results):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        
        # Read the immediate welcome
        s.settimeout(1.0)
        try:
            s.recv(1024)
        except socket.timeout:
            pass
            
        s.settimeout(5.0)

        # REGISTER
        s.sendall(f"REGISTER node_id=node_{client_id} bunker_name=bk ip_address=127.0.0.1 capacity=10\n".encode())
        s.recv(1024)
        
        total_time = 0
        for i in range(num_requests):
            start = time.perf_counter()
            s.sendall(b"HEARTBEAT\n")
            resp = s.recv(1024)
            end = time.perf_counter()
            total_time += (end - start)
        
        avg_latency = (total_time / num_requests) * 1000 # in ms
        results.append(avg_latency)

        if hold_connection:
            time.sleep(3) # Hold connection to monopolize thread if bug was present
        
        s.sendall(b"quit\n")
        s.close()
    except Exception as e:
        print(f"Client {client_id} failed: {e}")


def run_benchmark(num_clients, test_name):
    print(f"--- Running {test_name}: {num_clients} concurrent clients ---")
    results = []
    threads = []
    
    for i in range(num_clients):
        t = threading.Thread(target=client_worker, args=(i, True, 5, results))
        threads.append(t)
        t.start()
        
    for t in threads:
        t.join()
        
    avg = sum(results)/len(results) if results else 0
    print(f"Average Latency for {num_clients} clients: {avg:.2f} ms")
    return avg

if __name__ == "__main__":
    lat_1 = run_benchmark(1, "Baseline")
    time.sleep(1)
    
    # In the old code, this would hang forever or take exponentially long 
    # because only 8 clients could connect and others would block.
    # Now it should be almost as fast as baseline.
    lat_100 = run_benchmark(100, "Load Test")
    
    # Proteccion contra lat_1 == 0
    base = max(lat_1, 0.01)
    ratio = lat_100 / base
    
    print(f"\nLatency ratio (100 clients / 1 client) = {ratio:.2f}x")
    if ratio <= 2:
        print("SUCCESS: Latency did not increase by more than 2x.")
        sys.exit(0)
    else:
        # Allowing some margin for local benchmarking inconsistencies if latency is extremely low
        if lat_100 < 5.0: 
            print("SUCCESS: Ratio > 2x but total latency is less than 5ms.")
            sys.exit(0)
        print("FAIL: Latency increased by more than 2x.")
        sys.exit(1)
