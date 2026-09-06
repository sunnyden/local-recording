# An embedded audio recorder that can synced to cloud.
## Core Feature / Work outcome
* You should create an new 3P app registration on Azure Portal using command line.
* This app should be consumer oriented and can generate user token for onedrive write access. (but we should reserve capability for enterprise integration)
* For any other service deployment, e.g. function app, container web app/web app, we should deploy to resource group(create one): copilot-test
# MVP Feature
## Embedded Device Recording
* A UI on screen for local recording & sync % playback
* Audio should be stored in WAV/MP3 format(and only 16khz monochannel is supported).
* WiFi Configuration via BLE(on computer, we write a sample code on computer that can use the bluetooth to setup it, the current computer support bluetooth).
* Sync to onedrive via WiFi(auth by using the user code flow, user code will be sent to BLE and we do auth on the computer for now), after that , the auth info will be stored in ESP-32
* Recording will be stored in user's onedrive under local-recording/ folder.
## AOAI Integration for Embedded device
* User can connect to a voice agent using voice live api through azure's container app or web app (with lowest cost and scale to zero, as this is just a prototype)
* The embedded device will auth via user token of the app
* User for now trigger this by selecting a mode using physical buttons.
* User can talk to the ai and ai will speak out from the speaker
* Audio will be captured and sent in PCM 16kHz format, binary stream, on server side, we will translate it into standard AOAI realtime v1 protocol and connect to gpt-realtime-2-mini model
* For now this is just chat, but for code we should reserve capability to do other remote tool calling. (which means that the protocol will need to be extensible for more than audio payload)

# Reference
* Code reference can be found in reference folder.
* If not enough, you may search for other info or use any tools to get the necessary information.
* The current board is ATK_DNESP32S3_V1.4

# Security Consideration
* Avoid using secrets

# Code Style
* Build reusable code and avoid writing code that is too long(more than 1000 lines in a file, or more than 300 lines for a single function), try to breakdown logics and try to make interface or codes that can be reused.
* Try to document everything, particular communication protocols that may be shared across projects and even external partners.
* Always consider performance and memory footprint when you are writting embedded code, ESP32-S3 is an embedded platform and you should ALWAYS be aware of memory management, CPU resource management.

# Validation
* Use mocks, unit tests to validate the core logics as much as possible to ensure that it follows your design.
* Try to do E2E validation using the board connected to the computer, avoid interactive flow as much as possible when doing validation.