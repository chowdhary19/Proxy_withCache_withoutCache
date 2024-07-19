
![Yuvraj Singh Chowdhary](https://img.shields.io/badge/Yuvraj%20Singh%20Chowdhary-orange.svg)

# Advanced Proxy Server Implementation

![UML Diagram](pics/UML.JPG)

This project showcases two implementations of a proxy server: one with caching capabilities and another without. Both versions offer robust functionality for handling HTTP and HTTPS requests, with additional features such as bandwidth throttling, request logging, and site blocking.

## Tech Stack

![C](https://img.shields.io/badge/C-00599C?style=for-the-badge&logo=c&logoColor=white)
![OpenSSL](https://img.shields.io/badge/OpenSSL-721412?style=for-the-badge&logo=openssl&logoColor=white)
![POSIX Threads](https://img.shields.io/badge/POSIX_Threads-7D4698?style=for-the-badge&logo=linux&logoColor=white)
![Sockets](https://img.shields.io/badge/Sockets-black?style=for-the-badge&logo=socket.io&logoColor=white)

## Features

### Common Features (Both Implementations)
- HTTP/HTTPS request handling
- Multi-threading support
- Bandwidth throttling
- Request logging
- Site blocking
- Custom header injection

### Proxy with Cache
- Implements an LRU (Least Recently Used) caching mechanism
- Improves response times for frequently accessed resources

### Proxy without Cache
- Lightweight implementation
- Direct request forwarding

## Implementation Details

### Proxy with Cache
- Uses a linked list structure for cache management
- Implements functions for finding, adding, and removing cache elements
- Utilizes mutex locks for thread-safe cache operations

### Proxy without Cache
- Focuses on efficient request handling and forwarding
- Implements direct communication between client and server

## Key Components

1. **Request Parsing**: Utilizes `ParsedRequest` structure for HTTP request analysis.
2. **Error Handling**: Implements `sendErrorMessage` function for various HTTP error codes.
3. **Remote Server Connection**: `connectRemoteServer` function establishes connections to target servers.
4. **HTTPS Handling**: Special handling for HTTPS requests using OpenSSL.
5. **Bandwidth Control**: `throttle_bandwidth` function limits data transfer rates.
6. **Logging**: `log_request` function records request details.
7. **Thread Management**: Uses POSIX threads and semaphores for concurrent request handling.

## Usage

Compile the program and run it with the desired port number:

```bash
./proxy_server <port_number>
```

## Configuration

- Modify `blocked_sites` array to customize site blocking.
- Adjust `MAX_CLIENTS`, `MAX_BYTES`, and other constants as needed.

## Developer Information

**Yuvraj Singh Chowdhary**
- LinkedIn: [Yuvraj Singh Chowdhary](https://www.linkedin.com/in/yuvraj-singh-chowdhary/)
- Reddit: [SuccessfulStrain9533](https://www.reddit.com/user/SuccessfulStrain9533/)

**About Me**:
Web3 Developer | MERN Stack Developer | SQL Expert | Coder | GenAI Developer | Machine Learning Enthusiast | Blockchain Developer | Web Analyst

## Future Enhancements
- Implement HTTPS caching
- Add support for additional HTTP methods
- Enhance security features

---

This project demonstrates advanced networking concepts and provides a solid foundation for building scalable proxy servers. Feel free to contribute or use it as a learning resource!
