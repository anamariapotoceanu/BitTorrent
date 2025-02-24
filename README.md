# BitTorrent
To complete the assignment, three structures were used:

- The `FileInfo` structure stores all the necessary information for a file, specifically its name, the number of segments, and the hashes of each segment.

- The `PeerData` structure stores information about a peer, including how many segments it has, how many segments it wants, all the information about the files it possesses, and the names of the files it desires.

- The `Swarm` structure stores, for each file, the number of segments, the hashes of the segments, as well as the IDs of the other `peers` that have segments of the file.

### Tracker
- The tracker maintains information about other `peers` for each file using a map (`std::map<std::string, Swarm> tracker_data`).

- To ensure variation in the peer from which segments are downloaded, a map (`std::map<int, int> peer_upload`) was used to keep track of how busy a peer is. Using the `find_peer` function, a peer that is not highly loaded is selected.

### Input Handling
- The `read_input_file` function reads data from files and stores the necessary information, which is then sent to the tracker (`send_info_tracker`), specifically: the number of segments and the hash of each segment.  
The tracker receives information from the peer (`receive_file_info_from_peer`), creates a `Swarm` structure specific to each file, and finally adds to the `peers` list the peer that owns the current file.

### Downloading Mechanism
- In the `download_thread_func` function, the list of files requested by the current peer is traversed.  
  - For the current file, the tracker is queried for swarm information.  
  - The least busy peer is selected to download segments from.  
  - The downloaded segments are added to `downloaded_hashes`.  
  - If 10 segments have been downloaded, a new request is made for the updated list of peers.  
  - It checks if all segments have been downloaded. If so, the tracker is informed via `COMPLETE` that the peer now owns the full file. The file is saved for the respective client.  
  - Finally, it verifies if the client has finished downloading all desired files. The tracker is informed via `DONE` so that the download thread can be closed.

### Uploading Mechanism
- The `upload_thread_func` function is responsible for verifying whether a peer owns a requested segment.  
  - The requested segment's hash is received.  
  - The peer checks if it owns the segment and sends an appropriate message: `ACK` or `NACK`.  
  - When a message is received indicating that all clients have finished downloading the files, the upload execution thread is closed.

### Tracker Functionality
- In the `tracker` function, once all necessary information has been received, a message is sent to each peer indicating they can begin.  
- If the tracker receives a `REQUEST`, it sends information about the swarm of the file.  
- Upon receiving `COMPLETE`, after a peer has finished downloading a file, it is added to the peer list.  
- If all downloads are completed, a `STOP` message is sent to terminate the `upload` thread.  
- When a `LIST` request is received, the tracker sends the swarm information again to ensure the peer list remains updated.
