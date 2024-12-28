# APD-HW2

This is an implementation of the BitTorrent algorithm... or at least something similar to it.

## Description
The code is split in two main parts: the tracker, which handles the initialization and basically acts as a trusted middleman between all the peers, and the clients which each have owned and wanted files. The clients themselves are further split into two parts beyond the main thread- a download thread and an upload thread.

### Tracker
Being that there is only one tracker, it will handle most of the synchronity in the initialization phase, controlling the flow until eventually relinquishing control and opening the flood gates with the eventual "go signal".

> In the init phase, the tracker goes through each client, first gathering all of the owned files from each (the information of which is sent in a flattened manner here) and then gathering the wanted files from each. This latter part also handles hashInfo sharing, so that the clients will know what and where to seek.

With initialization done, the tracker sends a signal to each client, waking them and allowing them to begin their search. The tracker then switches to an infinite loop where it responds to requests from clients. These come in two types:
1) (tag 0) The client has completed a wanted file.
Once this type of message is received `n` times (where `n` is the number of wanted files in the whole cluster), the tracker will send a signal to every client telling it to close.
2) (tag 2) The client wants to sync.
This sync request merely consists of the tracker updating the client on who the owners of the requested file are. At the same time, the requesting client is added to the peer list, for other future requests from other clients.

### Client
During initalization, the client will first "understand" what its owned files are, then pass the information of these files to the tracker (hashes). Following this, the client will also pass any information of the _wanted_ files to the tracker... consisting in the filename, that is. The tracker will send the required hash information, so that the client knows where to look. (note: they send the hashes, but without sending the "data")

> Further, the client splits into two threads: an upload thread and a downlaod thread.

The upload thread is simple in its functionality, merely responding to requests. If the request comes from the tracker, it shuts down (this signal will only be sent at the end when there are no more wanted files). Otherwise, the request will be coming from another client, sporting a filename and a hash. This thread will check whether or not it owns the hash, then respond with a yes/no.

The download thread has a bit more bulk in its function. It will go through a check every 10 segments to re-sync with the tracker (see if there are any new peers in the file's swarm).
Besides that, it loops through these 4 steps while it still has wanted files:
1) Choose a file (randomly) from the ones still wanted but incomplete
2) Choose a segment (in our case, the next segment, going sequentially from 0)
3) Choose a peer (randomly) from a filtered list
The client remembers how many time it has "disturbed" each peer, and chooses one of the least disturbed peers. For example with `rank:disturbances`, the following list `[1:2, 2:2, 3:5, 4:1, 5:1]` would be trimmed down to `[4,5]`, giving us two possibile peers to request from.

4) Check response for segment
In case the response is positive (we got the segment), we only increment the disturbance amount by 1. If the response is negative, we increment said amount by a larger value, giving more time to the peer to gather segments before trying again.
If the segment completes a wanted file, the tracker is notified while the file is written to disk.

If there are no wanted files left, the download thread closes, leaving only the upload thread still up for any leeches/peers.