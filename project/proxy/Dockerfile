FROM python:3.12.14-slim-trixie@sha256:78387bc3881b8273120a12ebe6c1ab22b018ccc2c9adf565ae1ac9b536e184ea
ENV PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 PIP_NO_CACHE_DIR=1
WORKDIR /app
COPY requirements.lock pyproject.toml ./
RUN apt-get update \
    && apt-get upgrade --yes \
    && rm -rf /var/lib/apt/lists/* \
    && python -m pip install --no-cache-dir --upgrade pip==26.2.1 \
    && python -m pip install --no-cache-dir -r requirements.lock \
    && python -m pip uninstall --yes pip setuptools
COPY src ./src
ENV PYTHONPATH=/app/src
RUN mkdir /app/data && chown 65532:65532 /app/data && chmod 700 /app/data
WORKDIR /app/data
USER 65532:65532
EXPOSE 8000
HEALTHCHECK --interval=30s --timeout=3s --start-period=30s \
  CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/readyz', timeout=2)"
CMD ["python", "-m", "uvicorn", "recorder_proxy.app:create_app", "--factory", "--host", "0.0.0.0", "--port", "8000", "--workers", "1", "--ws", "websockets", "--ws-max-size", "4096", "--ws-max-queue", "4", "--ws-ping-interval", "20", "--ws-ping-timeout", "10", "--ws-per-message-deflate", "false", "--no-access-log"]
