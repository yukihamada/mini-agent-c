FROM python:3.12-slim

# Set working directory
WORKDIR /app

# Copy agent.v10 binary and dependencies
# Note: You'll need to build agent.v10 for Linux before deploying
COPY agent.v10 /app/agent.v10
RUN chmod +x /app/agent.v10

# Copy bot files
COPY bot.py requirements.txt /app/

# Install Python dependencies
RUN pip install --no-cache-dir -r requirements.txt

# Expose port for LINE webhook
EXPOSE 5000

# Run bot
CMD ["python", "bot.py"]
