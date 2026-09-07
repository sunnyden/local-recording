# Feature Request - Intelligence Embedded Recording
## Functionalities
* Recording should have filename like AudioRecording_YYYYDDMM_HHMMSS.wav
* When recording is synced to onedrive, trigger the backend with necessary information.
    - Server will fetch and use Fast Transcription to do transcription and store information back to onedrive (in .json format, I am wondering if it is possible to save it using VROOM API and store it as transcript, if not then you may directly store as txt)
* For the voice agent, on proxy side, implement several tools that can search onedrive, fetch item, read item contents(text only), so that we can use this to answer user's query, we should write some system prompt to define the behavior.

## Notes
* Once sync is done, ESP32 will send a signal to backend to trigger the processing
* Since the service is scaled to zero, we should reserve enough time for warmup and avoid showing timeout to user,
* No need to change the existing protocol of ESP<->Proxy, as all LLM RAG is done on service side.
* The scope of the user token may need to be extended in order to have access to onedrive on proxy.