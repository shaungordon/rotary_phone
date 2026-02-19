#!/usr/bin/env python3
"""
Voice Activity Detection (VAD) Prototype for Rotary Phone Bird Conversation Simulator

This script implements a simple energy-based VAD system to detect when a user
starts and stops speaking, triggering a "response" after a pause is detected.

Hardware target: XIAO ESP32S3 with MAX9814 preamp
This Python prototype establishes tuned constants before porting to Arduino C++.
"""

import sounddevice as sd
import soundfile as sf
import numpy as np
import time
import random
import os
from enum import Enum

# =============================================================================
# TUNABLE CONSTANTS - Easy to port to #define statements in Arduino C++
# =============================================================================

# Audio Configuration
SAMPLE_RATE = 16000          # Hz - matches phone aesthetic, ESP32S3 friendly
CHUNK_DURATION_MS = 30       # milliseconds - processing window size
CHUNK_SIZE = int(SAMPLE_RATE * CHUNK_DURATION_MS / 1000)  # samples per chunk

# Energy Threshold (RMS amplitude)
ENERGY_THRESHOLD = 0.02      # Adjust based on mic sensitivity and environment
                             # Typical range: 0.01 (sensitive) to 0.05 (less sensitive)

# Timing Constants
MIN_SPEECH_DURATION_MS = 200    # Ignore sounds shorter than this (clicks, breaths)
PAUSE_DURATION_MS = 800         # Silence duration before triggering response
POST_RESPONSE_LOCKOUT_MS = 2000 # Don't listen during bird's response

# Smoothing (optional - helps reduce false triggers)
ENERGY_SMOOTHING_FACTOR = 0.3   # 0.0 = no smoothing, 1.0 = max smoothing

# Bird Response Configuration
SQUAWK_FILES = ["squawk_1.wav", "squawk_2.wav", "squawk_3.wav", "squawk_4.wav"]
MIN_SQUAWKS_PER_RESPONSE = 1    # Minimum number of squawks in a response
MAX_SQUAWKS_PER_RESPONSE = 3    # Maximum number of squawks in a response
PAUSE_BETWEEN_SQUAWKS_MS = 300  # Pause between squawks (like words in a sentence)
PAUSE_BEFORE_RESPONSE_MS = 200  # Brief pause before bird starts talking
PAUSE_AFTER_RESPONSE_MS = 400   # Brief pause after bird finishes talking

# =============================================================================
# STATE MACHINE
# =============================================================================

class VADState(Enum):
    SILENCE = 1      # Waiting for speech to start
    SPEAKING = 2     # User is talking
    PAUSE = 3        # User stopped, waiting to see if they continue
    LOCKOUT = 4      # Bird is responding, don't listen

# =============================================================================
# VAD CLASS
# =============================================================================

class VoiceActivityDetector:
    def __init__(self):
        self.state = VADState.SILENCE
        self.speech_start_time = 0
        self.pause_start_time = 0
        self.lockout_start_time = 0
        self.smoothed_energy = 0
        self.response_count = 0
        self.squawk_cache = {}  # Cache loaded squawk files
        
        # Load squawk files
        self.load_squawk_files()
        
        print("=" * 60)
        print("Voice Activity Detection - Rotary Phone Prototype")
        print("=" * 60)
        print(f"Sample Rate: {SAMPLE_RATE} Hz")
        print(f"Chunk Duration: {CHUNK_DURATION_MS} ms")
        print(f"Energy Threshold: {ENERGY_THRESHOLD}")
        print(f"Min Speech Duration: {MIN_SPEECH_DURATION_MS} ms")
        print(f"Pause Duration: {PAUSE_DURATION_MS} ms")
        print(f"Post-Response Lockout: {POST_RESPONSE_LOCKOUT_MS} ms")
        print(f"Squawk files loaded: {len(self.squawk_cache)}")
        print("=" * 60)
        print("\nListening... Speak into your microphone!\n")
    
    def load_squawk_files(self):
        """Load all squawk WAV files into memory"""
        for filename in SQUAWK_FILES:
            if os.path.exists(filename):
                try:
                    data, samplerate = sf.read(filename)
                    # Convert to mono if stereo
                    if len(data.shape) > 1:
                        data = data[:, 0]
                    # Resample if needed (simple approach - just use as-is for prototype)
                    self.squawk_cache[filename] = (data, samplerate)
                    print(f"Loaded: {filename} ({len(data)} samples @ {samplerate}Hz)")
                except Exception as e:
                    print(f"Warning: Could not load {filename}: {e}")
            else:
                print(f"Warning: {filename} not found")
    
    def calculate_energy(self, audio_chunk):
        """Calculate RMS energy of audio chunk"""
        rms = np.sqrt(np.mean(audio_chunk**2))
        
        # Apply smoothing to reduce jitter
        self.smoothed_energy = (ENERGY_SMOOTHING_FACTOR * self.smoothed_energy + 
                                (1 - ENERGY_SMOOTHING_FACTOR) * rms)
        
        return self.smoothed_energy
    
    def process_chunk(self, audio_chunk, current_time_ms):
        """Process audio chunk and update state machine"""
        energy = self.calculate_energy(audio_chunk)
        
        # State machine logic
        if self.state == VADState.LOCKOUT:
            # Check if lockout period has expired
            if current_time_ms - self.lockout_start_time >= POST_RESPONSE_LOCKOUT_MS:
                print(f"[{current_time_ms:8d}ms] LOCKOUT → SILENCE (ready to listen again)")
                self.state = VADState.SILENCE
        
        elif self.state == VADState.SILENCE:
            # Waiting for speech to start
            if energy > ENERGY_THRESHOLD:
                self.speech_start_time = current_time_ms
                self.state = VADState.SPEAKING
                print(f"[{current_time_ms:8d}ms] SILENCE → SPEAKING (energy: {energy:.4f})")
        
        elif self.state == VADState.SPEAKING:
            # User is talking
            if energy <= ENERGY_THRESHOLD:
                # Energy dropped - start pause timer
                self.pause_start_time = current_time_ms
                self.state = VADState.PAUSE
                print(f"[{current_time_ms:8d}ms] SPEAKING → PAUSE (energy: {energy:.4f})")
        
        elif self.state == VADState.PAUSE:
            # Waiting to see if user continues or stops
            if energy > ENERGY_THRESHOLD:
                # User started speaking again - back to SPEAKING
                self.state = VADState.SPEAKING
                print(f"[{current_time_ms:8d}ms] PAUSE → SPEAKING (continued talking)")
            
            elif current_time_ms - self.pause_start_time >= PAUSE_DURATION_MS:
                # Pause duration exceeded - check if speech was long enough
                speech_duration = self.pause_start_time - self.speech_start_time
                
                if speech_duration >= MIN_SPEECH_DURATION_MS:
                    # Valid speech detected - trigger response!
                    self.trigger_response(current_time_ms, speech_duration)
                else:
                    # Speech too short - ignore and return to silence
                    print(f"[{current_time_ms:8d}ms] PAUSE → SILENCE (speech too short: {speech_duration}ms)")
                    self.state = VADState.SILENCE
    
    def play_bird_response(self):
        """Play a random sequence of 1-3 squawks with pauses"""
        if not self.squawk_cache:
            print("   No squawk files available to play")
            return
        
        # Determine how many squawks to play
        num_squawks = random.randint(MIN_SQUAWKS_PER_RESPONSE, MAX_SQUAWKS_PER_RESPONSE)
        
        # Select random squawks
        available_files = list(self.squawk_cache.keys())
        selected_squawks = [random.choice(available_files) for _ in range(num_squawks)]
        
        print(f"   Playing {num_squawks} squawk(s): {', '.join([os.path.basename(f) for f in selected_squawks])}")
        
        # Pause before response
        time.sleep(PAUSE_BEFORE_RESPONSE_MS / 1000.0)
        
        # Play each squawk with pauses between
        for i, squawk_file in enumerate(selected_squawks):
            data, samplerate = self.squawk_cache[squawk_file]
            
            # Play the squawk
            sd.play(data, samplerate)
            sd.wait()  # Wait for playback to finish
            
            # Pause between squawks (except after the last one)
            if i < len(selected_squawks) - 1:
                time.sleep(PAUSE_BETWEEN_SQUAWKS_MS / 1000.0)
        
        # Pause after response
        time.sleep(PAUSE_AFTER_RESPONSE_MS / 1000.0)
    
    def trigger_response(self, current_time_ms, speech_duration):
        """Trigger bird response with actual audio playback"""
        self.response_count += 1
        print("\n" + "=" * 60)
        print(f"🐦 RESPONSE TRIGGERED #{self.response_count}")
        print(f"   Time: {current_time_ms}ms")
        print(f"   Speech Duration: {speech_duration}ms")
        
        # Play the bird response
        self.play_bird_response()
        
        print("=" * 60 + "\n")
        
        # Enter lockout state
        self.lockout_start_time = current_time_ms
        self.state = VADState.LOCKOUT
    
    def get_status_string(self):
        """Return current state for display"""
        return f"State: {self.state.name:8s} | Energy: {self.smoothed_energy:.4f}"

# =============================================================================
# AUDIO CALLBACK
# =============================================================================

def audio_callback(indata, frames, time_info, status):
    """Called by sounddevice for each audio chunk"""
    if status:
        print(f"Audio status: {status}")
    
    # Convert to mono if stereo
    audio_chunk = indata[:, 0] if len(indata.shape) > 1 else indata
    
    # Get current time in milliseconds
    current_time_ms = int(time.time() * 1000)
    
    # Process the chunk
    vad.process_chunk(audio_chunk, current_time_ms)

# =============================================================================
# MAIN
# =============================================================================

def main():
    global vad
    
    # Initialize VAD
    vad = VoiceActivityDetector()
    
    try:
        # Open audio stream
        with sd.InputStream(
            samplerate=SAMPLE_RATE,
            channels=1,
            blocksize=CHUNK_SIZE,
            callback=audio_callback
        ):
            print("Press Ctrl+C to stop\n")
            
            # Keep running and display status
            while True:
                time.sleep(0.5)
                print(f"\r{vad.get_status_string()}", end="", flush=True)
    
    except KeyboardInterrupt:
        print("\n\nStopping VAD prototype...")
        print(f"Total responses triggered: {vad.response_count}")
        print("\nConstants summary for ESP32S3 port:")
        print(f"  #define SAMPLE_RATE {SAMPLE_RATE}")
        print(f"  #define CHUNK_DURATION_MS {CHUNK_DURATION_MS}")
        print(f"  #define ENERGY_THRESHOLD {ENERGY_THRESHOLD}f")
        print(f"  #define MIN_SPEECH_DURATION_MS {MIN_SPEECH_DURATION_MS}")
        print(f"  #define PAUSE_DURATION_MS {PAUSE_DURATION_MS}")
        print(f"  #define POST_RESPONSE_LOCKOUT_MS {POST_RESPONSE_LOCKOUT_MS}")
        print(f"  #define ENERGY_SMOOTHING_FACTOR {ENERGY_SMOOTHING_FACTOR}f")
    
    except Exception as e:
        print(f"\nError: {e}")
        print("\nTroubleshooting:")
        print("  - Check that your microphone is connected and accessible")
        print("  - Try: pip install sounddevice numpy")
        print("  - List available devices: python -m sounddevice")

if __name__ == "__main__":
    main()
