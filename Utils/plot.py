import matplotlib.pyplot as plt
import numpy as np

def read_execution_times(file_path):
    """
    Read the execution times from a file and return them as a list of floats.
    """
    execution_times = []
    with open(file_path, 'r') as file:
        for line in file:
            if "Execution time" in line:
                # Extract the execution time (in ms) from the line
                execution_time = float(line.split("Execution time = ")[1].split(" ms")[0])
                execution_times.append(execution_time)
    return execution_times

def plot_comparison(execution_times1, execution_times2):
    """
    Plot both sets of execution times on the same plot.
    """
    plt.figure(figsize=(10, 6))
    plt.plot(execution_times1, label="Execution Times (Without Load)", marker='o', linestyle='-', color='b')
    plt.plot(execution_times2, label="Execution Times (With Load)", marker='x', linestyle='--', color='r')
    
    plt.title('Execution Times Comparison')
    plt.xlabel('Round')
    plt.ylabel('Execution Time (ms)')
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.show()

def plot_statistics(execution_times1, execution_times2):
    """
    Plot min, mean, and variance for each of the execution time sets.
    """
    stats1 = [min(execution_times1), np.mean(execution_times1)]
    stats2 = [min(execution_times2), np.mean(execution_times2)]
    
    x_labels = ['Min', 'Mean']
    
    # Set up the figure for the bar plot
    bar_width = 0.35
    indices = np.arange(len(x_labels))
    
    # Plotting the statistics
    plt.figure(figsize=(8, 5))
    plt.bar(indices - bar_width/2, stats1, bar_width, label="Without Load", color='b')
    plt.bar(indices + bar_width/2, stats2, bar_width, label="With Load", color='r')
    
    plt.title('Execution Time Statistics')
    plt.xlabel('Statistic')
    plt.ylabel('Value')
    plt.xticks(indices, x_labels)
    plt.legend()
    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    # Read execution times from both files
    execution_times_without_load = read_execution_times('execution_times_without_load.txt')
    execution_times_with_load = read_execution_times('execution_times.txt')
    
    # Plot comparison of execution times
    plot_comparison(execution_times_without_load, execution_times_with_load)
    
    # Plot the min, mean, and variance statistics for both sets
    plot_statistics(execution_times_without_load, execution_times_with_load)
