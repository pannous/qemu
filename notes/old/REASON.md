Workload -> Venus impact
Static desktop none
Scrolling web pages small
Animations moderate
Video playback irrelevant (decoder-bound)
Complex compositing significant <<<



gamescope (Valve)

Production-proven, but wrong abstraction.
	•	Vulkan-based
	•	High-performance
	•	Used in Steam Deck

Limitations:
	•	Not a general desktop compositor
	•	Depends heavily on Linux/Wayland/KMS
	•	Not reusable for Redox

Assessment:
Excellent design inspiration, poor code reuse.


	•	vkroots is the only credible Vulkan compositor reference.