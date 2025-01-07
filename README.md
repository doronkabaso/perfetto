# Perfetto - System profiling, app tracing and trace analysis

Perfetto is a production-grade, open-source stack for performance instrumentation and trace analysis, designed for high-complexity environments like Android, Linux, and Chrome platforms. It provides unparalleled capabilities for system-wide and application-level tracing, offering deep insights into performance bottlenecks.

See https://perfetto.dev/docs or the /docs/ directory for documentation.

## Features

- **System-Level Tracing**: Capture and analyze performance metrics across Android, Linux, and Chrome.
- **Heap Profiling**: Supports native and Java heap profiling for precise memory management insights.
- **SQL-Based Trace Analysis**: Leverage SQL queries to analyze multi-GB traces with precision.
- **Advanced Visualization**: Web-based UI for exploring and visualizing traces at scale.
- **Cross-Platform Support**: Compatible with Android, Linux, Chrome, and more.
- **Customizable Instrumentation**: Extendable APIs for integrating with custom workflows.

## Tech Stack

- **Languages**: C++, Java
- **Platforms**: Android, Linux, Chrome
- **Key Tools**: SQL Analysis Engine, Web-Based UI, Heap Profilers
- **Utilities**: Trace Event APIs, Data Sources for Custom Events

## Project Structure

- **`src/`**: Core implementation files.
- **`ui/`**: Frontend code for web-based trace visualization.
- **`docs/`**: Detailed documentation and guides.
- **`tools/`**: Command-line utilities for trace recording and processing.

## Development Setup

### Prerequisites

- A Linux or macOS environment
- Python 3.x
- A supported version of Android SDK (if using Android)

### Steps to Setup Locally

1. Clone the repository:

   ```bash
   git clone https://github.com/doronkabaso/perfetto.git
   cd perfetto
   ```

2. Install dependencies:

   ```bash
   tools/install-build-deps
   ```

3. Build the project:

   ```bash
   tools/ninja -C out/debug
   ```

4. Run tests:

   ```bash
   tools/ninja -C out/debug run_tests
   ```

5. Launch the trace viewer:

   ```bash
   tools/traceconv <trace_file>
   ```

## Deployment

Perfetto can be integrated into custom workflows for performance-critical environments. For deployment details, refer to the [official documentation](https://perfetto.dev/docs).

## Complexity Highlight

Perfetto’s ability to handle multi-GB traces, integrate SQL-based analysis, and offer platform-agnostic tracing solutions makes it indispensable for performance engineers. Its use in Android, Linux, and Chrome demonstrates its reliability and scalability in diverse, complex environments.

## Feedback

For suggestions or feedback, please reach out via [GitHub Issues](https://github.com/doronkabaso/perfetto/issues).

## Lessons Learned

Perfetto showcases how performance instrumentation can be scaled to handle the demands of modern platforms. Its modular architecture, extensible APIs, and deep integration with system-level components exemplify state-of-the-art tracing tools.


