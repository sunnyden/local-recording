FROM python:3.12.12-slim
ENV PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 PIP_NO_CACHE_DIR=1
WORKDIR /app
COPY requirements.lock pyproject.toml ./
RUN pip install --no-cache-dir -r requirements.lock
COPY src ./src
ENV PYTHONPATH=/app/src
USER 65532:65532
EXPOSE 8000
HEALTHCHECK --interval=30s --timeout=3s --start-period=30s \
  CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/readyz', timeout=2)"
CMD ["python", "-m", "uvicorn", "recorder_proxy.app:create_app", "--factory", "--host", "0.0.0.0", "--port", "8000", "--workers", "1", "--ws", "websockets", "--ws-max-size", "4096", "--ws-max-queue", "4", "--ws-ping-interval", "20", "--ws-ping-timeout", "10", "--ws-per-message-deflate", "false", "--no-access-log"]
