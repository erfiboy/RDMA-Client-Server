import subprocess
import time
import re

def run_client(client_command, rounds, output_file):
    """
    Run the client command for a specified number of rounds, extract execution times,
    and save them to a text file.
    """
    execution_times = []

    for round_num in range(1, rounds + 1):
        try:
            print(f"Starting client round {round_num}...")
            # Run the client command and capture output
            result = subprocess.run(client_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

            # Extract the execution time from the client output
            match = re.search(r"Execution time: ([\d.]+) ms", result.stdout)
            if match:
                execution_time = float(match.group(1))
                execution_times.append(execution_time)
                print(f"Round {round_num}: Execution time = {execution_time} ms")
            else:
                print(f"Round {round_num}: Failed to extract execution time")

            # Wait for 1 second before the next round
            time.sleep(1)
        except subprocess.CalledProcessError as e:
            print(f"Client encountered an error: {e}")
        except KeyboardInterrupt:
            print("Client process interrupted. Exiting...")
            break

    # Write execution times to a file
    with open(output_file, 'w') as file:
        for i, exec_time in enumerate(execution_times, start=1):
            file.write(f"Round {i}: Execution time = {exec_time} ms\n")
    print(f"Execution times written to {output_file}")

if __name__ == "__main__":
    # Define client command
    client_command = ["./client", "-d", "23458", "-s", "1004857600", "-p", "12345", "-i", "192.168.9.2", "-r", "192.168.3.2"]
    
    # Output file for execution times
    output_file = "execution_times.txt"

    try:
        run_client(client_command, rounds=20, output_file=output_file)
    except KeyboardInterrupt:
        print("Client runner stopped.")

