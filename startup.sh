#!/bin/bash
# Pre FastAPI musíme použiť worker triedu uvicorn
gunicorn -w 1 -k uvicorn.workers.UvicornWorker --bind=0.0.0.0:8000 --timeout 600 server:app