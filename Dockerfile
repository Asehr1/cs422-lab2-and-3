FROM ubuntu:24.04

# Prevent interactive prompts during apt package installation
ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

# Install Python 3, pip, venv, and certificates
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    python3-venv \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create and activate a virtual environment
ENV VIRTUAL_ENV=/opt/venv
RUN python3 -m venv $VIRTUAL_ENV
ENV PATH="$VIRTUAL_ENV/bin:$PATH"

# Set the working directory
WORKDIR /app

# Copy requirements.txt first
COPY requirements.txt .

# Upgrade pip and install dependencies
RUN pip install --no-cache-dir --upgrade pip && \
    pip install --no-cache-dir -r requirements.txt

# Copy rest of the application code
COPY . .

# Set default command (update to match your application entrypoint)
# TODO: set this to be the entrypoint of our script
CMD ["python3", "iperf_client.py"]
