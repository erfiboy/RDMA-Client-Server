import subprocess
import time

def run_server_forever(server_command):
    """
    Run the server in an infinite loop. If the server terminates, restart it.
    """
    while True:
        try:
            print("Starting server...")
            # Run the server command and wait for it to terminate
            subprocess.run(server_command, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Server terminated unexpectedly: {e}. Restarting...")
            time.sleep(2)  # Wait before restarting
        except KeyboardInterrupt:
            print("Server process interrupted. Exiting...")
            break

if __name__ == "__main__":
    # Define server command with the updated parameters
    server_command = ["./server", "-p", "23458", "-s", "1004857600", "-i", "192.168.3.2"]

    try:
        run_server_forever(server_command)
    except KeyboardInterrupt:
        print("Server runner stopped.")

