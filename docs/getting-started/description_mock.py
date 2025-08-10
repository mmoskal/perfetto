#!/usr/bin/env python3
import uuid

from perfetto.trace_builder.proto_builder import TraceProtoBuilder
from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import TrackEvent, TrackDescriptor, ProcessDescriptor, ThreadDescriptor


def populate_packets(builder: TraceProtoBuilder):
  """
    This function is where you will define and add your TracePackets
    to the trace. The examples in the following sections will provide
    the specific code to insert here.

    Args:
        builder: An instance of TraceProtoBuilder to add packets to.
    """
  TRUSTED_PACKET_SEQUENCE_ID = 9005

  # --- Define Track UUID ---
  described_track_uuid = uuid.uuid4().int & ((1 << 63) - 1)

  # --- 1. Define the track with a description ---
  packet = builder.add_packet()
  desc = packet.track_descriptor
  desc.uuid = described_track_uuid
  desc.name = "My Described Track"
  desc.description = "This track shows the processing stages for incoming user requests."

  # Helper to add a slice event to the track
  def add_slice_event(ts, event_type, event_track_uuid, name=None):
    packet = builder.add_packet()
    packet.timestamp = ts
    packet.track_event.type = event_type
    packet.track_event.track_uuid = event_track_uuid
    if name:
      packet.track_event.name = name
    packet.trusted_packet_sequence_id = TRUSTED_PACKET_SEQUENCE_ID

  # --- 2. Emit some events on the track ---
  add_slice_event(
      ts=1000,
      event_type=TrackEvent.TYPE_SLICE_BEGIN,
      event_track_uuid=described_track_uuid,
      name="Request #123")
  add_slice_event(
      ts=1050,
      event_type=TrackEvent.TYPE_SLICE_BEGIN,
      event_track_uuid=described_track_uuid,
      name="Validate")
  add_slice_event(
      ts=1100,
      event_type=TrackEvent.TYPE_SLICE_END,
      event_track_uuid=described_track_uuid)
  add_slice_event(
      ts=1200,
      event_type=TrackEvent.TYPE_SLICE_END,
      event_track_uuid=described_track_uuid)


def main():
  """
    Initializes the TraceProtoBuilder, calls populate_packets to fill it,
    and then writes the resulting trace to a file.
    """
  builder = TraceProtoBuilder()
  populate_packets(builder)

  output_filename = "my_custom_trace.pftrace"
  with open(output_filename, 'wb') as f:
    f.write(builder.serialize())

  print(f"Trace written to {output_filename}")
  print(f"Open with [https://ui.perfetto.dev](https://ui.perfetto.dev).")


if __name__ == "__main__":
  main()
