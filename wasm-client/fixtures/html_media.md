Raw media tags survive sanitization:

<video src="/assets/uploads/clip.mp4" controls width="640"></video>

<audio src="/assets/uploads/song.ogg" controls></audio>

<script>alert("xss")</script>

<iframe src="https://www.youtube.com/embed/abc123" width="560" height="315"></iframe>

<iframe src="https://evil.example.com/embed" width="560" height="315"></iframe>

An inline <b>bold html</b> attempt and a <span style="color:red">styled span</span>.
